// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Commands/Asset/AssetCommandHelpers.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Engine/Engine.h"
#include "Editor.h"                              // kept: UnrealEd types pulled in transitively by this TU (world resolution itself now lives in mcp_common::ResolveWorldFromParams)
#include "Engine/EngineTypes.h"                  // EWorldType
#include "Components/SkeletalMeshComponent.h"
#include "Animation/AnimInstance.h"
#include "Animation/Skeleton.h"
#include "Engine/SkeletalMesh.h"                 // USkeletalMesh::GetSkeletonMemberName (get_skeleton_info mesh_paths)
#include "Animation/AnimationAsset.h"
#include "Animation/AnimMontage.h"
#include "UObject/UObjectGlobals.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Misc/PackageName.h"
#include "UObject/UnrealType.h"
#include "UObject/PropertyPortFlags.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Blueprint/BlueprintSupport.h"
#include "Engine/Selection.h"
#include "HAL/PlatformFilemanager.h"
#include "EditorAssetLibrary.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/DateTime.h"
#include "Misc/Timespan.h"

namespace UEAgentAnimationCommands
{

// MCP-DOMAIN: Animation

namespace UEAgentAnimationImpl
{
    // get_skeleton_info lists at most this many bones; bone_count always carries the real total.
    constexpr int32 MaxListedBones = 100;

    // First USkeletalMeshComponent of the world whose object name equals ComponentName. Only
    // actors that already live in the world are walked; no asset gets loaded. FString equality
    // ignores case, so "mesh" also finds "Mesh".
    USkeletalMeshComponent* FindSkeletalMeshInWorld(UWorld* World, const FString& ComponentName)
    {
        if (World == nullptr || ComponentName.IsEmpty())
        {
            return nullptr;
        }
        for (TActorIterator<AActor> Owner(World); Owner; ++Owner)
        {
            TArray<USkeletalMeshComponent*> OwnedMeshes;
            Owner->GetComponents<USkeletalMeshComponent>(OwnedMeshes);
            USkeletalMeshComponent* const* Named = OwnedMeshes.FindByPredicate(
                [&ComponentName](const USkeletalMeshComponent* Mesh) { return Mesh != nullptr && Mesh->GetName() == ComponentName; });
            if (Named != nullptr)
            {
                return *Named;
            }
        }
        return nullptr;
    }

    // What a component-level command resolved before touching anything: the world it acts in,
    // the selector to echo back as `world`, and the mesh found there. Failure holds the error
    // response to send when either lookup came up empty.
    struct FMeshLookup
    {
        UWorld* World = nullptr;
        FString WorldLabel;
        USkeletalMeshComponent* Mesh = nullptr;
        TSharedPtr<FJsonObject> Failure;
    };

    // Component commands act on the editor world unless `world` points them elsewhere.
    FMeshLookup LookUpSkeletalMesh(const TSharedPtr<FJsonObject>& Params, const FString& ComponentName)
    {
        FMeshLookup Lookup;
        FString WhyNoWorld;
        Lookup.World = mcp_common::ResolveWorldSelector(Params, TEXT("editor"), Lookup.WorldLabel, WhyNoWorld);
        if (Lookup.World == nullptr || !Lookup.World->IsValidLowLevel())
        {
            // The resolver's own message lists the worlds that DO exist; the generic text is only
            // the fallback when it had nothing to say.
            Lookup.Failure = mcp_common::MakeErrorReply(
                WhyNoWorld.IsEmpty() ? FString(TEXT("No valid world found")) : WhyNoWorld);
            return Lookup;
        }

        Lookup.Mesh = FindSkeletalMeshInWorld(Lookup.World, ComponentName);
        if (!IsValid(Lookup.Mesh))
        {
            Lookup.Failure = mcp_common::MakeErrorReply(FString::Printf(
                TEXT("Skeletal mesh component not found or invalid: %s (world='%s')"), *ComponentName, *Lookup.WorldLabel));
        }
        return Lookup;
    }

    // Echoes the selector that was used and stamps the exact identity of that world.
    void AddWorldFields(const TSharedPtr<FJsonObject>& Out, const FMeshLookup& Lookup)
    {
        Out->SetStringField(TEXT("world"), Lookup.WorldLabel);
        mcp_common::AddWorldEvidence(Out, Lookup.World);
    }

    // Validity and class of the mesh plus what is known about its anim instance.
    void AddMeshFacts(const TSharedPtr<FJsonObject>& Out, USkeletalMeshComponent* Mesh, const bool bWithInstanceValidity)
    {
        Out->SetBoolField(TEXT("component_valid"), IsValid(Mesh));
        Out->SetStringField(TEXT("component_class"), Mesh->GetClass()->GetName());

        UAnimInstance* Instance = Mesh->GetAnimInstance();
        Out->SetBoolField(TEXT("has_anim_instance"), Instance != nullptr);
        if (Instance == nullptr)
        {
            return;
        }
        Out->SetStringField(TEXT("anim_instance_class"), Instance->GetClass()->GetName());
        if (bWithInstanceValidity)
        {
            Out->SetBoolField(TEXT("anim_instance_valid"), IsValid(Instance));
        }
    }

    // A plain LoadObject first; on a miss, one retry through FSoftObjectPath, which also
    // understands the quoted export-text spelling (Class'/Path.Object').
    UAnimMontage* LoadMontage(const FString& Path)
    {
        if (UAnimMontage* Loaded = LoadObject<UAnimMontage>(nullptr, *Path))
        {
            return Loaded;
        }
        const FString Normalized = FSoftObjectPath(Path).ToString();
        return LoadObject<UAnimMontage>(nullptr, *Normalized);
    }

    // Registry identity of a resolved asset. The name/path/class keys carry a per-command prefix.
    void AddAssetIdentity(const TSharedPtr<FJsonObject>& Out, const FString& RequestedName, const FAssetData& Asset,
                          const TCHAR* NameKey, const TCHAR* PathKey, const TCHAR* ClassKey)
    {
        Out->SetStringField(NameKey, RequestedName);
        Out->SetStringField(PathKey, Asset.GetObjectPathString());
        Out->SetStringField(ClassKey, Asset.AssetClassPath.ToString());
        Out->SetStringField(TEXT("package_name"), Asset.PackageName.ToString());
        Out->SetBoolField(TEXT("is_loaded"), Asset.IsAssetLoaded());
    }

    // Size and timestamp of the .uasset behind a package. Nothing is written when the package
    // does not map to a file that exists on disk.
    void AddPackageFileStats(const TSharedPtr<FJsonObject>& Out, const FName PackageName)
    {
        FString OnDisk;
        if (!FPackageName::TryConvertLongPackageNameToFilename(PackageName.ToString(), OnDisk, TEXT(".uasset")))
        {
            return;
        }
        IPlatformFile& Disk = FPlatformFileManager::Get().GetPlatformFile();
        if (!Disk.FileExists(*OnDisk))
        {
            return;
        }
        const int64 Bytes = Disk.FileSize(*OnDisk);
        Out->SetNumberField(TEXT("file_size_bytes"), Bytes);
        Out->SetNumberField(TEXT("file_size_kb"), Bytes / 1024.0);
        Out->SetNumberField(TEXT("file_size_mb"), Bytes / (1024.0 * 1024.0));
        Out->SetStringField(TEXT("last_modified"), Disk.GetTimeStamp(*OnDisk).ToString());
    }

    // load_full_data facts of a Skeleton; a null pointer means the asset did not load as one.
    void AddSkeletonDetails(const TSharedPtr<FJsonObject>& Out, USkeleton* Skeleton)
    {
        Out->SetBoolField(TEXT("loaded_successfully"), Skeleton != nullptr);
        if (Skeleton == nullptr)
        {
            Out->SetStringField(TEXT("load_error"), TEXT("Failed to cast to USkeleton"));
            return;
        }
        Out->SetBoolField(TEXT("is_valid"), IsValid(Skeleton));

        const FReferenceSkeleton& Reference = Skeleton->GetReferenceSkeleton();
        const TArray<FMeshBoneInfo>& BoneInfos = Reference.GetRefBoneInfo();
        const int32 TotalBones = Reference.GetNum();
        const int32 ListedBones = FMath::Min(TotalBones, MaxListedBones);

        TArray<TSharedPtr<FJsonValue>> BoneEntries;
        BoneEntries.Reserve(ListedBones);
        for (int32 Slot = 0; Slot < ListedBones; ++Slot)
        {
            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("name"), BoneInfos[Slot].Name.ToString());
            Entry->SetNumberField(TEXT("index"), Slot);
            Entry->SetNumberField(TEXT("parent_index"), BoneInfos[Slot].ParentIndex);
            BoneEntries.Add(MakeShared<FJsonValueObject>(Entry));
        }

        Out->SetNumberField(TEXT("bone_count"), TotalBones);
        Out->SetArrayField(TEXT("bones"), BoneEntries);
        Out->SetBoolField(TEXT("bones_truncated"), TotalBones > MaxListedBones);
        Out->SetNumberField(TEXT("socket_count"), Skeleton->Sockets.Num());
        Out->SetNumberField(TEXT("slot_group_count"), Skeleton->GetSlotGroups().Num());

        // Virtual bones, in two fields: the flat names (the shape remove_virtual_bone also returns) and the
        // per-bone detail with its source and target bones.
        TArray<TSharedPtr<FJsonValue>> VirtualBoneNames;
        TArray<TSharedPtr<FJsonValue>> VirtualBones;
        for (const FVirtualBone& VirtualBone : Skeleton->GetVirtualBones())
        {
            VirtualBoneNames.Add(MakeShared<FJsonValueString>(VirtualBone.VirtualBoneName.ToString()));
            TSharedPtr<FJsonObject> VirtualEntry = MakeShared<FJsonObject>();
            VirtualEntry->SetStringField(TEXT("name"), VirtualBone.VirtualBoneName.ToString());
            VirtualEntry->SetStringField(TEXT("source_bone"), VirtualBone.SourceBoneName.ToString());
            VirtualEntry->SetStringField(TEXT("target_bone"), VirtualBone.TargetBoneName.ToString());
            VirtualBones.Add(MakeShared<FJsonValueObject>(VirtualEntry));
        }
        Out->SetNumberField(TEXT("virtual_bone_count"), VirtualBones.Num());
        Out->SetArrayField(TEXT("virtual_bone_names"), VirtualBoneNames);
        Out->SetArrayField(TEXT("virtual_bones"), VirtualBones);
    }

    // The skeletal meshes bound to a skeleton, from the Asset Registry tag USkeletalMesh::Skeleton (the lookup
    // USkeleton::FindCompatibleMesh makes): no asset is loaded.
    void AddSkeletonMeshes(const TSharedPtr<FJsonObject>& Out, const FAssetData& SkeletonAsset)
    {
        FARFilter Filter;
        Filter.bRecursivePaths = true;
        Filter.bRecursiveClasses = true;
        Filter.ClassPaths.Add(USkeletalMesh::StaticClass()->GetClassPathName());
        Filter.TagsAndValues.Add(USkeletalMesh::GetSkeletonMemberName(), SkeletonAsset.GetExportTextName());

        TArray<FAssetData> Meshes;
        mcp_common::AssetRegistry().GetAssets(Filter, Meshes);

        TArray<TSharedPtr<FJsonValue>> MeshPaths;
        MeshPaths.Reserve(Meshes.Num());
        for (const FAssetData& Mesh : Meshes)
        {
            MeshPaths.Add(MakeShared<FJsonValueString>(Mesh.GetObjectPathString()));
        }
        Out->SetNumberField(TEXT("mesh_count"), Meshes.Num());
        Out->SetBoolField(TEXT("shared"), Meshes.Num() > 1);
        Out->SetArrayField(TEXT("mesh_paths"), MeshPaths);
    }
}

// MCP:set_skeletal_mesh_component_tick - Switch the tick of a skeletal mesh component in the editor world (or the one the world param picks): despite the name, it only toggles component ticking and tick-when-paused. Returns the previous and current tick flags; nothing is marked dirty or saved.
//   @param component_name - Object name of the USkeletalMeshComponent, e.g. "CharacterMesh0", case-insensitive; the first actor in the world that has one with that name wins
//   @param event_settings - Tick mode, any case: 'enable' (tick on, not while paused), 'disable' (tick off) or 'custom' (tick on, paused setting kept)
//   @param world - Optional world selector: absent/'editor' (default) | 'pie' (ambiguous: PlayWorld, else the first PIE context) | 'server' (the PIE world with authority) | 'client' | 'pie:N' | 'game' (alias of 'pie').
//   @returns { component_name: string, requested_settings: string, events_configured: bool, previous_tick_enabled: bool, previous_tick_when_paused: bool, current_tick_enabled: bool, current_tick_when_paused: bool, component_valid: bool, component_class: string, anim_instance_class: string?, has_anim_instance: bool, world: string, world_name: string|null, world_package: string|null, world_type: string|null, net_mode: string|null, pie_instance: number }

TSharedPtr<FJsonObject> HandleConfigureAnimationEvents(const TSharedPtr<FJsonObject>& Params)
{
    FString TargetName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("component_name"), TargetName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString Mode;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("event_settings"), Mode, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TargetName.IsEmpty())
    {
        return AssetCommandHelpers::EmptyParamError(TEXT("component_name"));
    }
    if (Mode.IsEmpty())
    {
        return AssetCommandHelpers::EmptyParamError(TEXT("event_settings"));
    }

    // FString comparison ignores case: "Enable" and "ENABLE" select the same mode.
    const bool bModeEnable = (Mode == TEXT("enable"));
    const bool bModeDisable = (Mode == TEXT("disable"));
    const bool bModeCustom = (Mode == TEXT("custom"));
    if (!(bModeEnable || bModeDisable || bModeCustom))
    {
        return mcp_common::MakeErrorReply(TEXT("Parameter 'event_settings' must be 'enable', 'disable', or 'custom'"));
    }

    const UEAgentAnimationImpl::FMeshLookup Lookup = UEAgentAnimationImpl::LookUpSkeletalMesh(Params, TargetName);
    if (Lookup.Failure.IsValid())
    {
        return Lookup.Failure;
    }
    USkeletalMeshComponent* Mesh = Lookup.Mesh;

    const bool bTickWasOn = Mesh->IsComponentTickEnabled();
    const bool bPausedTickWasOn = Mesh->PrimaryComponentTick.bTickEvenWhenPaused;

    // "disable" switches ticking off; "enable" and "custom" both switch it on, and "enable"
    // additionally stops the component from ticking while the game is paused.
    Mesh->SetComponentTickEnabled(!bModeDisable);
    if (bModeEnable)
    {
        Mesh->SetTickableWhenPaused(false);
    }

    TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
    Report->SetStringField(TEXT("component_name"), TargetName);
    Report->SetStringField(TEXT("requested_settings"), Mode);
    Report->SetBoolField(TEXT("events_configured"), true);
    Report->SetBoolField(TEXT("previous_tick_enabled"), bTickWasOn);
    Report->SetBoolField(TEXT("previous_tick_when_paused"), bPausedTickWasOn);
    Report->SetBoolField(TEXT("current_tick_enabled"), Mesh->IsComponentTickEnabled());
    Report->SetBoolField(TEXT("current_tick_when_paused"), Mesh->PrimaryComponentTick.bTickEvenWhenPaused);
    UEAgentAnimationImpl::AddMeshFacts(Report, Mesh, /*bWithInstanceValidity=*/false);
    UEAgentAnimationImpl::AddWorldFields(Report, Lookup);
    return Report;
}

// MCP:set_animation_speed - Set the play rate of a skeletal mesh component, found by object name in the editor world (or the world the world param selects), and switch its component tick on. Only a component in Use Animation Asset (single node) mode takes it: in Animation Blueprint mode nothing changes and actual_speed reads 0, so check speed_set_successfully. Transient: nothing is saved or undoable. A speed outside 0-10 or no such component is an error. For montages use play_animation with play_rate, or set_properties (rate_scale) on the asset.
//   @param component_name - Object name of the USkeletalMeshComponent, e.g. "CharacterMesh0", case-insensitive; the first actor in the world that has one with that name wins
//   @param speed - Playback speed multiplier; must be between 0.0 and 10.0
//   @param world - Optional world selector: absent/'editor' (default) | 'pie' (ambiguous: PlayWorld, else the first PIE context) | 'server' (the PIE world with authority) | 'client' | 'pie:N' | 'game' (alias of 'pie').
//   @returns { component_name: string, requested_speed: number, previous_speed: number, actual_speed: number, speed_set_successfully: bool, previous_tick_enabled: bool, current_tick_enabled: bool, component_valid: bool, component_class: string, anim_instance_class: string?, has_anim_instance: bool, anim_instance_valid: bool?, warning: string?, world: string, world_name: string|null, world_package: string|null, world_type: string|null, net_mode: string|null, pie_instance: number }

TSharedPtr<FJsonObject> HandleSetAnimationSpeed(const TSharedPtr<FJsonObject>& Params)
{
    FString TargetName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("component_name"), TargetName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    double RequestedRate = 0.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("speed"), RequestedRate)) { return Missing; }
    if (TargetName.IsEmpty())
    {
        return AssetCommandHelpers::EmptyParamError(TEXT("component_name"));
    }
    if (RequestedRate < 0.0 || RequestedRate > 10.0)
    {
        return mcp_common::MakeErrorReply(TEXT("Parameter 'speed' must be between 0.0 and 10.0"));
    }

    const UEAgentAnimationImpl::FMeshLookup Lookup = UEAgentAnimationImpl::LookUpSkeletalMesh(Params, TargetName);
    if (Lookup.Failure.IsValid())
    {
        return Lookup.Failure;
    }
    USkeletalMeshComponent* Mesh = Lookup.Mesh;

    const float RateBefore = Mesh->GetPlayRate();
    const bool bTickWasOn = Mesh->IsComponentTickEnabled();

    // The component stores the rate as a float; the read-back below is compared at that precision.
    const float AppliedRate = static_cast<float>(RequestedRate);
    Mesh->SetPlayRate(AppliedRate);
    Mesh->SetComponentTickEnabled(true);
    const float RateAfter = Mesh->GetPlayRate();
    const bool bRateMatches = FMath::IsNearlyEqual(RateAfter, AppliedRate, 0.001f);

    TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
    Report->SetStringField(TEXT("component_name"), TargetName);
    Report->SetNumberField(TEXT("requested_speed"), RequestedRate);
    Report->SetNumberField(TEXT("previous_speed"), RateBefore);
    Report->SetNumberField(TEXT("actual_speed"), RateAfter);
    Report->SetBoolField(TEXT("speed_set_successfully"), bRateMatches);
    Report->SetBoolField(TEXT("previous_tick_enabled"), bTickWasOn);
    Report->SetBoolField(TEXT("current_tick_enabled"), Mesh->IsComponentTickEnabled());
    UEAgentAnimationImpl::AddMeshFacts(Report, Mesh, /*bWithInstanceValidity=*/true);
    if (!bRateMatches)
    {
        Report->SetStringField(TEXT("warning"), FString::Printf(
            TEXT("Speed was requested as %.2f but actual value is %.2f"), RequestedRate, RateAfter));
    }
    UEAgentAnimationImpl::AddWorldFields(Report, Lookup);
    return Report;
}

// MCP:get_skeleton_info - Find a Skeleton asset in the Asset Registry and report its object path, class path, package, whether it is loaded, the size and timestamp of its .uasset (left out when no file is on disk), and the skeletal meshes bound to it (mesh_count, mesh_paths, shared when more than one), all without loading it; nothing is changed. skeleton_name takes an object path, a package path or a short name searched under /Game; no match is an error, and so is a short name several skeletons share, with the candidates listed. Only with load_full_data true is the asset loaded, adding bone_count, the first 100 bones (name, index, parent_index) with bones_truncated, socket_count, slot_group_count and every virtual bone with its source and target. One bone's reference pose: get_bone_transform; a skeletal mesh: get_skeletal_mesh_info.
//   @param skeleton_name - Object path, package path, or bare short name of the Skeleton asset (resolved via mcp_common::ResolveAssetFlexible; an ambiguous short name errors out listing every candidate instead of picking one silently)
//   @param skeleton_path - Alias of skeleton_name.
//   @param load_full_data - When true, loads the asset and reports bone list (capped at 100), sockets, slot groups and virtual bones (optional, default false)
//   @returns { skeleton_name: string, skeleton_path: string, skeleton_class: string, package_name: string, is_loaded: bool, file_size_bytes: number?, file_size_kb: number?, file_size_mb: number?, last_modified: string?, mesh_count: int, shared: bool, mesh_paths: [string], loaded_successfully: bool?, is_valid: bool?, bone_count: number?, bones: [{ name, index, parent_index }]?, bones_truncated: bool?, socket_count: number?, slot_group_count: number?, virtual_bone_count: number?, virtual_bone_names: [string]?, virtual_bones: [{ name, source_bone, target_bone }]?, load_error: string? }

TSharedPtr<FJsonObject> HandleGetSkeletonInfo(const TSharedPtr<FJsonObject>& Params)
{
    FString Requested;
    if (!Params->TryGetStringField(TEXT("skeleton_name"), Requested) && !Params->TryGetStringField(TEXT("skeleton_path"), Requested))
    {
        return mcp_common::MissingParameterReply(TEXT("skeleton_name"));
    }
    bool bWithDetails = false;
    Params->TryGetBoolField(TEXT("load_full_data"), bWithDetails);
    if (Requested.IsEmpty())
    {
        return AssetCommandHelpers::EmptyParamError(TEXT("skeleton_name"));
    }

    // Resolution goes through the shared flexible resolver (object path, package path or short
    // name). A registry lookup keyed on FSoftObjectPath(Name) is NOT equivalent: handed a package
    // path it returns the entry of the PACKAGE itself (class "Package"), and the load then fails.
    TArray<FAssetData> Matches;
    FString LookupError;
    if (!mcp_common::ResolveAssetFlexible(Requested, USkeleton::StaticClass(), TEXT("/Game"), Matches, LookupError))
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Skeleton not found: %s (%s)"), *Requested, *LookupError));
    }
    if (Matches.Num() > 1)
    {
        // Two skeletons sharing a short name can differ in every bone, socket and slot group, so
        // the command refuses to guess and lists the candidates instead.
        TArray<FString> MatchPaths;
        for (const FAssetData& Match : Matches)
        {
            MatchPaths.Add(Match.GetObjectPathString());
        }
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Ambiguous skeleton_name '%s': %d matches (%s). Pass a full object path to disambiguate."),
            *Requested, Matches.Num(), *FString::Join(MatchPaths, TEXT(", "))));
    }

    const FAssetData& Found = Matches[0];
    TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
    UEAgentAnimationImpl::AddAssetIdentity(Report, Requested, Found,
        TEXT("skeleton_name"), TEXT("skeleton_path"), TEXT("skeleton_class"));
    UEAgentAnimationImpl::AddPackageFileStats(Report, Found.PackageName);
    UEAgentAnimationImpl::AddSkeletonMeshes(Report, Found);
    if (bWithDetails)
    {
        UEAgentAnimationImpl::AddSkeletonDetails(Report, Cast<USkeleton>(Found.GetAsset()));
    }
    return Report;
}

// MCP:play_animation - Play an AnimMontage on a skeletal mesh component with UAnimInstance::Montage_Play at play_rate, then jump to start_section if playback started. Editor world unless the world param says otherwise; the component needs an anim instance. playing/success mean Montage_Play returned a length above 0.
//   @param component_name - Object name of the USkeletalMeshComponent, e.g. "CharacterMesh0", case-insensitive; the first actor in the world that has one with that name wins
//   @param montage_path - Full asset path to the AnimMontage (e.g. '/Game/Animations/MyMontage.MyMontage')
//   @param play_rate - Playback rate multiplier (optional, default 1.0)
//   @param start_section - Name of the section to jump to after play starts (optional)
//   @param world - Optional world selector: absent/'editor' (default) | 'pie' (ambiguous: PlayWorld, else the first PIE context) | 'server' (the PIE world with authority) | 'client' | 'pie:N' | 'game' (alias of 'pie').
//   @returns { component_name: string, montage_path: string, montage_name: string, playing: bool, play_rate: number, montage_length: number, return_value: number, success: bool, start_section: string?, world: string, world_name: string|null, world_package: string|null, world_type: string|null, net_mode: string|null, pie_instance: number }

TSharedPtr<FJsonObject> HandlePlayAnimation(const TSharedPtr<FJsonObject>& Params)
{
    FString TargetName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("component_name"), TargetName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString AssetPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("montage_path"), AssetPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    const UEAgentAnimationImpl::FMeshLookup Lookup = UEAgentAnimationImpl::LookUpSkeletalMesh(Params, TargetName);
    if (Lookup.Failure.IsValid())
    {
        return Lookup.Failure;
    }
    UAnimInstance* Instance = Lookup.Mesh->GetAnimInstance();
    if (Instance == nullptr)
    {
        return mcp_common::MakeErrorReply(TEXT("Component has no anim instance"));
    }
    UAnimMontage* Montage = UEAgentAnimationImpl::LoadMontage(AssetPath);
    if (Montage == nullptr)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("AnimMontage not found: %s. Use full path like '/Game/Animations/MyMontage.MyMontage'"), *AssetPath));
    }

    float Rate = 1.0f;
    Params->TryGetNumberField(TEXT("play_rate"), Rate);
    // An absent or empty start_section becomes NAME_None, i.e. "start from the beginning".
    FString SectionText;
    Params->TryGetStringField(TEXT("start_section"), SectionText);
    const FName Section(*SectionText);

    const float Length = Montage->GetPlayLength();
    // Montage_Play returns the montage length when playback started, and 0 when it did not.
    const float PlayResult = Instance->Montage_Play(Montage, Rate);
    const bool bStarted = PlayResult > 0.0f;
    if (bStarted && !Section.IsNone())
    {
        Instance->Montage_JumpToSection(Section, Montage);
    }

    TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
    Report->SetStringField(TEXT("component_name"), TargetName);
    Report->SetStringField(TEXT("montage_path"), AssetPath);
    Report->SetStringField(TEXT("montage_name"), Montage->GetName());
    Report->SetNumberField(TEXT("play_rate"), Rate);
    Report->SetNumberField(TEXT("montage_length"), Length);
    Report->SetNumberField(TEXT("return_value"), PlayResult);
    Report->SetBoolField(TEXT("playing"), bStarted);
    Report->SetBoolField(TEXT("success"), bStarted);
    if (!Section.IsNone())
    {
        Report->SetStringField(TEXT("start_section"), Section.ToString());
    }
    UEAgentAnimationImpl::AddWorldFields(Report, Lookup);
    return Report;
}

// MCP:stop_animation - Stop montage playback on a skeletal mesh component with Montage_Stop and a 0.25 s blend-out: only montage_path when given, otherwise every active montage. Editor world unless the world param says otherwise; an unknown montage_path is an error. was_playing and playing cover any montage.
//   @param component_name - Object name of the USkeletalMeshComponent, e.g. "CharacterMesh0", case-insensitive; the first actor in the world that has one with that name wins
//   @param montage_path - Full asset path of a specific AnimMontage to stop; if omitted, stops all montages (optional)
//   @param world - Optional world selector: absent/'editor' (default) | 'pie' (ambiguous: PlayWorld, else the first PIE context) | 'server' (the PIE world with authority) | 'client' | 'pie:N' | 'game' (alias of 'pie').
//   @returns { component_name: string, was_playing: bool, playing: bool, success: bool, montage_path: string?, montage_name: string?, world: string, world_name: string|null, world_package: string|null, world_type: string|null, net_mode: string|null, pie_instance: number }

TSharedPtr<FJsonObject> HandleStopAnimation(const TSharedPtr<FJsonObject>& Params)
{
    FString TargetName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("component_name"), TargetName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    const UEAgentAnimationImpl::FMeshLookup Lookup = UEAgentAnimationImpl::LookUpSkeletalMesh(Params, TargetName);
    if (Lookup.Failure.IsValid())
    {
        return Lookup.Failure;
    }
    UAnimInstance* Instance = Lookup.Mesh->GetAnimInstance();
    if (Instance == nullptr)
    {
        return mcp_common::MakeErrorReply(TEXT("Component has no anim instance"));
    }

    // montage_path narrows the stop to one montage; without it every active montage stops. A path
    // that does not load is an error -- a null montage would ask Montage_Stop to stop them ALL.
    FString AssetPath;
    UAnimMontage* OnlyThis = nullptr;
    if (Params->TryGetStringField(TEXT("montage_path"), AssetPath) && !AssetPath.IsEmpty())
    {
        OnlyThis = UEAgentAnimationImpl::LoadMontage(AssetPath);
        if (OnlyThis == nullptr)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("AnimMontage not found: %s"), *AssetPath));
        }
    }

    const bool bAnyWasPlaying = Instance->IsAnyMontagePlaying();
    Instance->Montage_Stop(0.25f, OnlyThis);

    TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
    Report->SetStringField(TEXT("component_name"), TargetName);
    Report->SetBoolField(TEXT("was_playing"), bAnyWasPlaying);
    Report->SetBoolField(TEXT("playing"), Instance->IsAnyMontagePlaying());
    Report->SetBoolField(TEXT("success"), true);
    if (OnlyThis != nullptr)
    {
        Report->SetStringField(TEXT("montage_path"), AssetPath);
        Report->SetStringField(TEXT("montage_name"), OnlyThis->GetName());
    }
    UEAgentAnimationImpl::AddWorldFields(Report, Lookup);
    return Report;
}

// get_active_montages helpers -----------------------------------------------

// Resolve an actor by object name, editor label, OR full object path within World.
static AActor* FindActorFlexible_Anim(UWorld* World, const FString& Name)
{
    if (!World || Name.IsEmpty())
    {
        return nullptr;
    }
    for (TActorIterator<AActor> It(World); It; ++It)
    {
        AActor* Actor = *It;
        if (Actor && (Actor->GetName() == Name || Actor->GetActorLabel() == Name || Actor->GetPathName() == Name))
        {
            return Actor;
        }
    }
    return nullptr;
}

// MCP:get_active_montages - Report the AnimMontage(s) currently playing on a live PIE actor's skeletal mesh (useful for blend-out debugging).
//   @param actor - Name, label, or full path of a live actor (resolved in the PIE world by default).
//   @param component - Optional skeletal mesh component name; defaults to the actor's primary USkeletalMeshComponent.
//   @param world - Which world to resolve the actor in: absent/'pie' (default, ambiguous: PlayWorld, else the first PIE context) | 'server' (the PIE world with authority) | 'client' | 'pie:N' | 'editor' | 'game' (alias of 'pie').
//   @returns { actor, actor_name, component, world, anim_instance_class, montage_count, is_any_montage_playing, top_montage_id, enumeration, montages: [{ index, montage_path, montage_name, is_top, current_section, position, normalized_position, length, play_rate, blend_weight, desired_weight, blend_time, is_playing, is_active, instance_id, slots:[string] }], world_name: string|null, world_package: string|null, world_type: string|null, net_mode: string|null, pie_instance: number }
TSharedPtr<FJsonObject> HandleGetActiveMontages(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor"), ActorName)) { return Missing; }
    FString ComponentName;
    Params->TryGetStringField(TEXT("component"), ComponentName);

    // World -- canonical resolver. Default stays 'pie' (this command has always meant
    // "the live session"), but 'server' / 'client' / 'pie:N' are now addressable, and the
    // failure message lists the worlds that DO exist instead of a bare "PIE not running".
    FString WorldMode, WorldError;
    UWorld* World = mcp_common::ResolveWorldSelector(Params, TEXT("pie"), WorldMode, WorldError);
    if (!World)
    {
        return mcp_common::MakeErrorReply(WorldError.IsEmpty()
            ? TEXT("No world could be resolved for get_active_montages")
            : WorldError);
    }

    AActor* Actor = FindActorFlexible_Anim(World, ActorName);
    if (!Actor)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Actor not found in %s world: %s"), *WorldMode, *ActorName));
    }

    // Skeletal mesh component: named match if a component name was given, else the primary one.
    USkeletalMeshComponent* SkelComp = nullptr;
    if (!ComponentName.IsEmpty())
    {
        TArray<USkeletalMeshComponent*> Comps;
        Actor->GetComponents<USkeletalMeshComponent>(Comps);
        for (USkeletalMeshComponent* C : Comps)
        {
            if (C && C->GetName() == ComponentName)
            {
                SkelComp = C;
                break;
            }
        }
        if (!SkelComp)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Skeletal mesh component '%s' not found on actor '%s'"), *ComponentName, *ActorName));
        }
    }
    else
    {
        SkelComp = Actor->FindComponentByClass<USkeletalMeshComponent>();
        if (!SkelComp)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Actor '%s' has no USkeletalMeshComponent"), *ActorName));
        }
    }

    UAnimInstance* AnimInstance = SkelComp->GetAnimInstance();
    if (!AnimInstance)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Skeletal mesh component '%s' has no anim instance"), *SkelComp->GetName()));
    }

    // UE 5.8 exposes UAnimInstance::MontageInstances publicly, so we iterate the full
    // set (the top-most PLUS any blending-out) -- not just GetActiveMontageInstance().
    const FAnimMontageInstance* TopInstance = AnimInstance->GetActiveMontageInstance();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actor"), Actor->GetActorLabel());
    Result->SetStringField(TEXT("actor_name"), Actor->GetName());
    Result->SetStringField(TEXT("component"), SkelComp->GetName());
    Result->SetStringField(TEXT("world"), WorldMode);
    Result->SetStringField(TEXT("anim_instance_class"), AnimInstance->GetClass()->GetName());
    Result->SetBoolField(TEXT("is_any_montage_playing"), AnimInstance->IsAnyMontagePlaying());
    Result->SetStringField(TEXT("enumeration"),
        TEXT("all_active_instances (UAnimInstance::MontageInstances -- top-most + blending-out)"));

    TArray<TSharedPtr<FJsonValue>> MontagesArr;
    int32 EmittedIndex = 0;
    for (const FAnimMontageInstance* Inst : AnimInstance->MontageInstances)
    {
        if (!Inst || !Inst->Montage)
        {
            continue;
        }
        UAnimMontage* Montage = Inst->Montage;

        TSharedPtr<FJsonObject> M = MakeShared<FJsonObject>();
        M->SetNumberField(TEXT("index"), EmittedIndex++);
        M->SetStringField(TEXT("montage_path"), Montage->GetPathName());
        M->SetStringField(TEXT("montage_name"), Montage->GetName());
        M->SetBoolField(TEXT("is_top"), Inst == TopInstance);

        M->SetStringField(TEXT("current_section"), Inst->GetCurrentSection().ToString());

        const float Position = Inst->GetPosition();
        const float Length = Montage->GetPlayLength();
        M->SetNumberField(TEXT("position"), Position);
        M->SetNumberField(TEXT("length"), Length);
        M->SetNumberField(TEXT("normalized_position"), Length > 0.f ? (Position / Length) : 0.f);

        M->SetNumberField(TEXT("play_rate"), Inst->GetPlayRate());
        M->SetNumberField(TEXT("blend_weight"), Inst->GetWeight());
        M->SetNumberField(TEXT("desired_weight"), Inst->GetDesiredWeight());
        M->SetNumberField(TEXT("blend_time"), Inst->GetBlendTime());
        M->SetBoolField(TEXT("is_playing"), Inst->IsPlaying());
        M->SetBoolField(TEXT("is_active"), Inst->IsActive());
        M->SetNumberField(TEXT("instance_id"), Inst->GetInstanceID());

        // Slot name(s) this montage plays on (from the montage's slot tracks).
        TArray<TSharedPtr<FJsonValue>> SlotsArr;
        for (const FSlotAnimationTrack& Slot : Montage->SlotAnimTracks)
        {
            SlotsArr.Add(MakeShared<FJsonValueString>(Slot.SlotName.ToString()));
        }
        M->SetArrayField(TEXT("slots"), SlotsArr);

        MontagesArr.Add(MakeShared<FJsonValueObject>(M));
    }

    Result->SetNumberField(TEXT("montage_count"), MontagesArr.Num());
    Result->SetArrayField(TEXT("montages"), MontagesArr);
    Result->SetStringField(TEXT("top_montage_id"),
        (TopInstance && TopInstance->Montage) ? TopInstance->Montage->GetPathName() : FString());

    // With 2+ PIE instances "world":"pie" says nothing -- these five fields say
    // EXACTLY which world was read (UEDPIE_N lives in world_package, not world_name).
    mcp_common::AddWorldEvidence(Result, World);

    return Result;
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("set_skeletal_mesh_component_tick"), &HandleConfigureAnimationEvents },
    { TEXT("get_active_montages"), &HandleGetActiveMontages },
    { TEXT("get_skeleton_info"), &HandleGetSkeletonInfo },
    { TEXT("play_animation"), &HandlePlayAnimation },
    { TEXT("set_animation_speed"), &HandleSetAnimationSpeed },
    { TEXT("stop_animation"), &HandleStopAnimation },
};
} // namespace UEAgentAnimationCommands
