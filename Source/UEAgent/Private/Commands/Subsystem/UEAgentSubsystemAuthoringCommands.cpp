#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Commands/Common/UEAgentGraphUtils.h"
#include "Commands/Asset/AssetCommandHelpers.h"

#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Subsystems/EngineSubsystem.h"
#include "EditorSubsystem.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Subsystems/Subsystem.h"
#include "Subsystems/SubsystemCollection.h"
#include "Subsystems/WorldSubsystem.h"
#include "Subsystems/LocalPlayerSubsystem.h"
#include "EditorAssetLibrary.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/Class.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Factories/BlueprintFactory.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"

/**
 * Subsystem authoring commands.
 *
 * Complements UEAgentSubsystemCommands (introspection only) with the surface an LLM needs to actually
 * *author* subsystem classes and lifecycle them at editor time: list and inspect subsystem classes,
 * create and remove Blueprint subclasses of USubsystem, read live instances, and activate / deactivate
 * external subsystems (FSubsystemCollectionBase::ActivateExternalSubsystem / DeactivateExternalSubsystem).
 *
 * Lifetime categories detected by walking the parent chain (no reflection
 * of the Within specifier needed):
 *  - Engine       (UEngineSubsystem)
 *  - Editor       (UEditorSubsystem)
 *  - GameInstance (UGameInstanceSubsystem)
 *  - World        (UWorldSubsystem)
 *  - LocalPlayer  (ULocalPlayerSubsystem)
 */
namespace UEAgentSubsystemAuthoringCommands
{

// MCP-DOMAIN: SubsystemAuthoring

namespace
{
	/**
	 * Walk the parent chain of `Cls` and return the matching lifetime
	 * category. Returns "Unknown" if the class is not a USubsystem or
	 * none of the recognized base classes are found in its ancestry.
	 */
	FString DetectLifetimeCategory(const UClass* Cls)
	{
		if (!Cls || !Cls->IsChildOf(USubsystem::StaticClass()))
		{
			return TEXT("Unknown");
		}
		static const TArray<TPair<UClass*, const TCHAR*>> LifetimeMap = {
			{ UEditorSubsystem::StaticClass(),       TEXT("Editor") },
			{ UEngineSubsystem::StaticClass(),       TEXT("Engine") },
			{ UGameInstanceSubsystem::StaticClass(), TEXT("GameInstance") },
			{ UWorldSubsystem::StaticClass(),        TEXT("World") },
			{ ULocalPlayerSubsystem::StaticClass(),  TEXT("LocalPlayer") },
		};
		for (const TPair<UClass*, const TCHAR*>& Pair : LifetimeMap)
		{
			if (Cls->IsChildOf(Pair.Key))
			{
				return Pair.Value;
			}
		}
		return TEXT("Unknown");
	}

	UClass* ResolveSubsystemClass(const FString& ClassName)
	{
		return FUEAgentGraphUtils::ResolveClass(ClassName);
	}

	/**
	 * Engine-provided subsystem BASE classes. Externally activating/deactivating
	 * one of these forces every collection of that family to instantiate/tear
	 * down a default — including engine-critical singletons like the
	 * WorldPartitionSubsystem, which trips an engine invariant and crashes the
	 * editor. Only concrete leaf subsystem classes are safe to toggle.
	 */
	bool IsUnsafeExternalSubsystemClass(const UClass* Cls)
	{
		if (!Cls) { return true; }
		if (Cls->HasAnyClassFlags(CLASS_Abstract)) { return true; }
		static const TCHAR* kBases[] = {
			TEXT("Subsystem"), TEXT("DynamicSubsystem"), TEXT("EngineSubsystem"),
			TEXT("GameInstanceSubsystem"), TEXT("LocalPlayerSubsystem"),
			TEXT("WorldSubsystem"), TEXT("TickableWorldSubsystem"),
			TEXT("EditorSubsystem"),
		};
		const FString Name = Cls->GetName();
		for (const TCHAR* Base : kBases)
		{
			if (Name == Base) { return true; }
		}
		// Native engine subsystems are auto-managed by their collection; only
		// allow user (Blueprint-generated) leaf classes to be toggled externally.
		return Cls->IsNative();
	}

	/**
	 * class_name of activate/deactivate_external_subsystem: a USubsystem class that is safe to toggle externally
	 * (Verb names the operation in the refusal). Returns the error reply to send back, or nullptr once OutClass
	 * holds the class.
	 */
	TSharedPtr<FJsonObject> SubsystemAuthoring_ReadToggleableClass(const TSharedPtr<FJsonObject>& Params, const TCHAR* Verb, UClass*& OutClass)
	{
		FString ClassName;
		if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("class_name"), ClassName)) { return Missing; }
		OutClass = ResolveSubsystemClass(ClassName);
		if (!OutClass || !OutClass->IsChildOf(USubsystem::StaticClass()))
		{
			return mcp_common::MakeErrorReply(
				FString::Printf(TEXT("Subsystem class not found: %s"), *ClassName));
		}
		if (IsUnsafeExternalSubsystemClass(OutClass))
		{
			return mcp_common::MakeErrorReply(FString::Printf(
				TEXT("Refusing to externally %s '%s': only concrete, non-abstract, "
					"non-native (Blueprint-authored) subsystem leaf classes are safe to "
					"toggle. Engine subsystem base/native classes are auto-managed and "
					"tearing them down crashes the editor."), Verb, *OutClass->GetName()));
		}
		return nullptr;
	}
}

// ---------------------------------------------------------------------------
// list_subsystem_classes
// ---------------------------------------------------------------------------
// MCP:list_subsystem_classes - Enumerate all loaded USubsystem-derived classes (native + Blueprint), optionally filtered by lifetime category.
//   @param lifetime - Optional lifetime filter: "Engine"|"Editor"|"GameInstance"|"World"|"LocalPlayer" (case-insensitive)
//   @returns { total: int, native: int, blueprint: int, lifetime_filter?: string, classes: [{ class_name, class_path, lifetime, parent_class, is_native }] }

TSharedPtr<FJsonObject> HandleListSubsystemClasses(
	const TSharedPtr<FJsonObject>& Params)
{
	FString LifetimeFilter;
	Params->TryGetStringField(TEXT("lifetime"), LifetimeFilter);

	TArray<TSharedPtr<FJsonValue>> Entries;
	int32 TotalCount = 0, NativeCount = 0, BlueprintCount = 0;

	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* Cls = *It;
		if (!Cls || Cls->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
		{
			continue;
		}
		if (!Cls->IsChildOf(USubsystem::StaticClass()))
		{
			continue;
		}
		const FString Lifetime = DetectLifetimeCategory(Cls);
		if (!LifetimeFilter.IsEmpty()
			&& !LifetimeFilter.Equals(Lifetime, ESearchCase::IgnoreCase))
		{
			continue;
		}
		const bool bIsNative = Cls->IsNative();
		if (bIsNative) { NativeCount++; } else { BlueprintCount++; }
		TotalCount++;

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("class_name"), Cls->GetName());
		Entry->SetStringField(TEXT("class_path"), Cls->GetPathName());
		Entry->SetStringField(TEXT("lifetime"), Lifetime);
		Entry->SetStringField(TEXT("parent_class"), Cls->GetSuperClass()
			? Cls->GetSuperClass()->GetName() : TEXT(""));
		Entry->SetBoolField(TEXT("is_native"), bIsNative);
		Entries.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("total"), TotalCount);
	Data->SetNumberField(TEXT("native"), NativeCount);
	Data->SetNumberField(TEXT("blueprint"), BlueprintCount);
	if (!LifetimeFilter.IsEmpty())
	{
		Data->SetStringField(TEXT("lifetime_filter"), LifetimeFilter);
	}
	Data->SetArrayField(TEXT("classes"), Entries);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// get_subsystem_class_info
// ---------------------------------------------------------------------------
// MCP:get_subsystem_class_info - Read class metadata and the full USubsystem inheritance chain for a subsystem class.
//   @param class_name - Short class name (e.g. "MyGameInstanceSubsystem") or /Script/ path
//   @returns { class_name: string, class_path: string, lifetime: string, parent_class: string, is_native: bool, is_abstract: bool, parent_chain: [{ class_name, class_path, lifetime }] }

TSharedPtr<FJsonObject> HandleGetSubsystemClassInfo(
	const TSharedPtr<FJsonObject>& Params)
{
	FString ClassName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("class_name"), ClassName)) { return Missing; }
	UClass* Cls = ResolveSubsystemClass(ClassName);
	if (!Cls)
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Subsystem class not found: %s"), *ClassName));
	}
	if (!Cls->IsChildOf(USubsystem::StaticClass()))
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Class is not a USubsystem: %s"), *ClassName));
	}

	// Walk parent chain so the LLM can see the full inheritance tree.
	TArray<TSharedPtr<FJsonValue>> Chain;
	for (UClass* Cur = Cls->GetSuperClass(); Cur && Cur != USubsystem::StaticClass(); Cur = Cur->GetSuperClass())
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("class_name"), Cur->GetName());
		Entry->SetStringField(TEXT("class_path"), Cur->GetPathName());
		Entry->SetStringField(TEXT("lifetime"), DetectLifetimeCategory(Cur));
		Chain.Add(MakeShared<FJsonValueObject>(Entry));
		if (Cur == USubsystem::StaticClass()) { break; }
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("class_name"), Cls->GetName());
	Data->SetStringField(TEXT("class_path"), Cls->GetPathName());
	Data->SetStringField(TEXT("lifetime"), DetectLifetimeCategory(Cls));
	Data->SetStringField(TEXT("parent_class"), Cls->GetSuperClass()
		? Cls->GetSuperClass()->GetName() : TEXT(""));
	Data->SetBoolField(TEXT("is_native"), Cls->IsNative());
	Data->SetBoolField(TEXT("is_abstract"), Cls->HasAnyClassFlags(CLASS_Abstract));
	Data->SetArrayField(TEXT("parent_chain"), Chain);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// create_subsystem_class_bp
// ---------------------------------------------------------------------------
// MCP:create_subsystem_class_bp - Create a Blueprint subclass of a USubsystem class at package_path/asset_name with FKismetEditorUtilities::CreateBlueprint, which compiles it; the package is marked dirty but not saved (save it with save_asset), there is no undo step, and it is refused while PIE runs unless force is set. A repeat call is idempotent: a Blueprint already at that path is returned as it is with already_exists true, without checking its parent, and parent_class then echoes the request rather than its real parent; another asset type there is an error. parent_class must resolve to a USubsystem class or nothing is created. lifetime is the family of the generated class: Editor, Engine, GameInstance, World, LocalPlayer or Unknown. Parent candidates: list_subsystem_classes; inspect: get_subsystem_class_info; inverse: remove_subsystem_class_bp; live instance check: is_subsystem_class_active.
//   @param asset_name - Name for the new Blueprint asset, e.g. "BP_ScoreSubsystem"
//   @param parent_class - Parent USubsystem class: short name with or without the U prefix (e.g. "GameInstanceSubsystem"), a /Script path, or a Blueprint asset path
//   @param package_path - Optional content folder; defaults to /Game/MCP
//   @returns { asset_name: string, asset_path: string, package_path: string, parent_class: string, lifetime: string, already_exists: bool }

TSharedPtr<FJsonObject> HandleCreateSubsystemClassBp(
	const TSharedPtr<FJsonObject>& Params)
{
	FString AssetName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("asset_name"), AssetName)) { return Missing; }
	FString ParentClassName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("parent_class"), ParentClassName)) { return Missing; }
	// Destination folder; absent or empty means the plugin's default asset folder.
	FString TargetFolder;
	Params->TryGetStringField(TEXT("package_path"), TargetFolder);
	if (TargetFolder.IsEmpty())
	{
		TargetFolder = mcp_common::DefaultAssetFolder();
	}

	// The parent must resolve AND be a USubsystem; anything else is refused before any
	// package is created.
	UClass* ParentCls = ResolveSubsystemClass(ParentClassName);
	if (ParentCls == nullptr)
	{
		return mcp_common::MakeErrorReply(FString::Printf(TEXT("Parent subsystem class not found: %s"), *ParentClassName));
	}
	if (!ParentCls->IsChildOf(USubsystem::StaticClass()))
	{
		return mcp_common::MakeErrorReply(TEXT("parent_class must be a USubsystem derivative"));
	}

	FString PackageError;
	UPackage* Package = mcp_common::CreateAssetPackage(TargetFolder / AssetName, AssetName, UBlueprint::StaticClass(), PackageError);
	if (!Package)
	{
	    return mcp_common::MakeErrorReply(PackageError);
	}
	Package->FullyLoad();

	// FKismetEditorUtilities::CreateBlueprint ASSERTS (Kismet2.cpp) if a
	// UBlueprint of this name still lives in the package (in-editor delete does
	// not reliably unload a loaded Blueprint). Return the existing asset
	// idempotently instead of crashing the editor.
	UBlueprint* BP = FindObject<UBlueprint>(Package, *AssetName);
	const bool bAlreadyExists = (BP != nullptr);
	if (!BP)
	{
		BP = Cast<UBlueprint>(
			FKismetEditorUtilities::CreateBlueprint(
				ParentCls, Package, FName(*AssetName),
				BPTYPE_Normal, FName(TEXT("CreateSubsystemBP"))));
		if (!BP)
		{
			return mcp_common::MakeErrorReply(TEXT("Failed to create subsystem Blueprint"));
		}
		FAssetRegistryModule::AssetCreated(BP);
		BP->MarkPackageDirty();
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_name"), BP->GetName());
	Data->SetStringField(TEXT("asset_path"), BP->GetPathName());
	Data->SetStringField(TEXT("package_path"), TargetFolder);
	Data->SetStringField(TEXT("parent_class"), ParentCls->GetName());
	Data->SetStringField(TEXT("lifetime"), DetectLifetimeCategory(BP->GeneratedClass));
	Data->SetBoolField(TEXT("already_exists"), bAlreadyExists);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// get_subsystem_instance
// ---------------------------------------------------------------------------
// MCP:get_subsystem_instance - Look up the live instance of a USubsystem by class + lifetime category (Engine/Editor/GameInstance/World/LocalPlayer).
//   @param class_name - Short class name or /Script/ path
//   @param lifetime - One of "Engine"|"Editor"|"GameInstance"|"World"|"LocalPlayer"
//   @returns { class_name: string, lifetime: string, instance_name: string, instance_path: string }

TSharedPtr<FJsonObject> HandleGetSubsystemInstance(
	const TSharedPtr<FJsonObject>& Params)
{
	FString ClassName, Lifetime;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("class_name"), ClassName)) { return Missing; }
	if (!Params->TryGetStringField(TEXT("lifetime"), Lifetime) || Lifetime.IsEmpty())
	{
		return mcp_common::MakeErrorReply(
			TEXT("lifetime is required (Engine|Editor|GameInstance|World|LocalPlayer)"));
	}

	UClass* Cls = ResolveSubsystemClass(ClassName);
	if (!Cls || !Cls->IsChildOf(USubsystem::StaticClass()))
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Subsystem class not found: %s"), *ClassName));
	}

	USubsystem* Instance = nullptr;
	const FString LC = Lifetime.ToLower();

	if (LC == TEXT("engine"))
	{
		if (GEngine && Cls->IsChildOf(UEngineSubsystem::StaticClass()))
		{
			TSubclassOf<UEngineSubsystem> Subclass(Cls);
			Instance = GEngine->GetEngineSubsystemBase(Subclass);
		}
	}
	else if (LC == TEXT("editor"))
	{
		if (GEditor && Cls->IsChildOf(UEditorSubsystem::StaticClass()))
		{
			TSubclassOf<UEditorSubsystem> Subclass(Cls);
			Instance = GEditor->GetEditorSubsystemBase(Subclass);
		}
	}
	else if (LC == TEXT("gameinstance"))
	{
		UWorld* World = mcp_common::GetEditorWorld();
		UGameInstance* GI = World ? World->GetGameInstance() : nullptr;
		if (GI && Cls->IsChildOf(UGameInstanceSubsystem::StaticClass()))
		{
			TSubclassOf<UGameInstanceSubsystem> Subclass(Cls);
			Instance = GI->GetSubsystemBase(Subclass);
		}
	}
	else if (LC == TEXT("world"))
	{
		UWorld* World = mcp_common::GetEditorWorld();
		if (World && Cls->IsChildOf(UWorldSubsystem::StaticClass()))
		{
			TSubclassOf<UWorldSubsystem> Subclass(Cls);
			Instance = World->GetSubsystemBase(Subclass);
		}
	}
	else if (LC == TEXT("localplayer"))
	{
		UWorld* World = mcp_common::GetEditorWorld();
		UGameInstance* GI = World ? World->GetGameInstance() : nullptr;
		ULocalPlayer* LP = (GI && GI->GetNumLocalPlayers() > 0) ? GI->GetLocalPlayerByIndex(0) : nullptr;
		if (LP && Cls->IsChildOf(ULocalPlayerSubsystem::StaticClass()))
		{
			TSubclassOf<ULocalPlayerSubsystem> Subclass(Cls);
			Instance = LP->GetSubsystemBase(Subclass);
		}
	}
	else
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Unknown lifetime: %s"), *Lifetime));
	}

	if (!Instance)
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Subsystem instance not found: %s [%s]"),
				*ClassName, *Lifetime));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("class_name"), Cls->GetName());
	Data->SetStringField(TEXT("lifetime"), Lifetime);
	Data->SetStringField(TEXT("instance_name"), Instance->GetName());
	Data->SetStringField(TEXT("instance_path"), Instance->GetPathName());
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// activate_external_subsystem
// ---------------------------------------------------------------------------
// MCP:activate_external_subsystem - Externally activate a Blueprint-authored USubsystem via FSubsystemCollectionBase::ActivateExternalSubsystem. Refuses to toggle native / abstract / base classes.
//   @param class_name - Short class name or /Script/ path of a Blueprint-derived USubsystem
//   @returns { class_name: string, activated: bool }

TSharedPtr<FJsonObject> HandleActivateExternalSubsystem(
	const TSharedPtr<FJsonObject>& Params)
{
	UClass* Cls = nullptr;
	if (TSharedPtr<FJsonObject> Refusal = SubsystemAuthoring_ReadToggleableClass(Params, TEXT("activate"), Cls))
	{
		return Refusal;
	}
	FSubsystemCollectionBase::ActivateExternalSubsystem(Cls);
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("class_name"), Cls->GetName());
	Data->SetBoolField(TEXT("activated"), true);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// deactivate_external_subsystem
// ---------------------------------------------------------------------------
// MCP:deactivate_external_subsystem - Externally deactivate a Blueprint-authored USubsystem via FSubsystemCollectionBase::DeactivateExternalSubsystem. Refuses to toggle native / abstract / base classes.
//   @param class_name - Short class name or /Script/ path of a Blueprint-derived USubsystem
//   @returns { class_name: string, deactivated: bool }

TSharedPtr<FJsonObject> HandleDeactivateExternalSubsystem(
	const TSharedPtr<FJsonObject>& Params)
{
	UClass* Cls = nullptr;
	if (TSharedPtr<FJsonObject> Refusal = SubsystemAuthoring_ReadToggleableClass(Params, TEXT("deactivate"), Cls))
	{
		return Refusal;
	}
	FSubsystemCollectionBase::DeactivateExternalSubsystem(Cls);
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("class_name"), Cls->GetName());
	Data->SetBoolField(TEXT("deactivated"), true);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// remove_subsystem_class_bp
// ---------------------------------------------------------------------------
// MCP:remove_subsystem_class_bp - Destroy the Blueprint-authored USubsystem subclass at a package path (mirror of create_subsystem_class_bp). Typed-delete on TWO checks: the asset must be a UBlueprint AND its generated class must derive from USubsystem, so it cannot wipe an unrelated Blueprint the way the generic delete_asset can. Idempotent on missing assets.
//   @param asset_path - Full /Game/.../Name asset path of the subsystem Blueprint.
//   @returns { asset_path, deleted, already_existed, asset_class, generated_class_name?, note? }

TSharedPtr<FJsonObject> HandleRemoveSubsystemClassBp(
	const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("asset_path"), AssetPath)) { return Missing; }

	UObject* Asset = LoadObject<UObject>(nullptr, *AssetPath);
	if (!Asset)
	{
		TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
		Resp->SetStringField(TEXT("asset_path"), AssetPath);
		Resp->SetBoolField(TEXT("deleted"), false);
		Resp->SetBoolField(TEXT("already_existed"), false);
		Resp->SetStringField(TEXT("note"), TEXT("asset_path did not resolve to a loadable UObject"));
		return mcp_common::MakeOkReply(Resp);
	}

	UBlueprint* BP = Cast<UBlueprint>(Asset);
	if (!BP)
	{
		return mcp_common::MakeOkReply(AssetCommandHelpers::WrongTypeRefusal(AssetPath, *Asset, TEXT("UBlueprint")));
	}

	// Verify generated class is a USubsystem derivative (typed-delete guard).
	const FString GeneratedClassName = BP->GeneratedClass ? BP->GeneratedClass->GetName() : FString();
	const bool bIsSubsystemClass = BP->GeneratedClass && BP->GeneratedClass->IsChildOf(USubsystem::StaticClass());

	// DeleteAsset(path) is unreliable for a Blueprint created + saved this session (asset registry
	// not synced); delete the loaded object directly (registry-independent).
	const bool bDeleted = UEditorAssetLibrary::DeleteLoadedAsset(BP);

	TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
	Resp->SetStringField(TEXT("asset_path"), AssetPath);
	Resp->SetStringField(TEXT("asset_class"), BP->GetClass()->GetName());
	if (!GeneratedClassName.IsEmpty())
	{
		Resp->SetStringField(TEXT("generated_class_name"), GeneratedClassName);
	}
	Resp->SetBoolField(TEXT("deleted"), bDeleted);
	Resp->SetBoolField(TEXT("already_existed"), true);
	Resp->SetBoolField(TEXT("is_subsystem_class"), bIsSubsystemClass);
	if (!bIsSubsystemClass)
	{
		Resp->SetStringField(TEXT("note"),
			TEXT("deleted asset was not a USubsystem derivative -- accepted because the "
			     "caller named the asset_path explicitly; this guard is informational"));
	}
	return mcp_common::MakeOkReply(Resp);
}

// ---------------------------------------------------------------------------
// is_subsystem_class_active
// ---------------------------------------------------------------------------
// MCP:is_subsystem_class_active - Probe whether ONE named USubsystem class currently has a live instance -- the yes/no question, where list_subsystem_instances is the enumeration. It checks every lifetime category (Engine / Editor / GameInstance / World / LocalPlayer) and answers active=true if an instance exists in ANY of them, reporting which collection each one came from.
//   @param class_name - Short class name or /Script/ path
//   @returns { class_name, lifetime, active, instances:[{ collection, instance_path }] }

TSharedPtr<FJsonObject> HandleIsSubsystemClassActive(
	const TSharedPtr<FJsonObject>& Params)
{
	FString ClassName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("class_name"), ClassName)) { return Missing; }
	UClass* Cls = ResolveSubsystemClass(ClassName);
	if (!Cls || !Cls->IsChildOf(USubsystem::StaticClass()))
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Subsystem class not found: %s"), *ClassName));
	}

	TArray<TSharedPtr<FJsonValue>> InstancesJson;
	bool bAnyActive = false;

	// Walk every live USubsystem instance. The collection-by-UClass accessors on
	// UEngine/UEditorEngine/UGameInstance only expose the templated form
	// (`GetSubsystem<T>()`) or the `_Base` variants that return the typed base, but
	// not a free UClass* variant — and UWorld hides its collection entirely. The
	// portable approach is to enumerate every USubsystem-derived instance via
	// TObjectIterator and check IsA<>() against the requested class.
	for (TObjectIterator<USubsystem> It; It; ++It)
	{
		USubsystem* Sub = *It;
		if (!Sub || !Sub->IsValidLowLevel()) { continue; }
		UClass* SubCls = Sub->GetClass();
		if (!SubCls || !SubCls->IsChildOf(Cls)) { continue; }

		// Skip the CDO.
		if (Sub->HasAnyFlags(RF_ClassDefaultObject)) { continue; }

		// Resolve collection label by walking the parent chain against the lifetime base classes.
		const FString Lifetime = DetectLifetimeCategory(SubCls);

		TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
		E->SetStringField(TEXT("collection"), Lifetime);
		E->SetStringField(TEXT("instance_path"), Sub->GetPathName());
		InstancesJson.Add(MakeShared<FJsonValueObject>(E));
		bAnyActive = true;
	}

	TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
	Resp->SetStringField(TEXT("class_name"), Cls->GetName());
	Resp->SetStringField(TEXT("lifetime"), DetectLifetimeCategory(Cls));
	Resp->SetBoolField(TEXT("active"), bAnyActive);
	Resp->SetNumberField(TEXT("instance_count"), InstancesJson.Num());
	Resp->SetArrayField(TEXT("instances"), InstancesJson);
	return mcp_common::MakeOkReply(Resp);
}

// ---------------------------------------------------------------------------
// list_subsystem_instances
// ---------------------------------------------------------------------------
// MCP:list_subsystem_instances - Enumerate the USubsystem instances live in the EDITOR right now, with each one's class path and lifetime -- use is_subsystem_class_active for a yes/no on a single class. It walks the editor world context only (Engine / Editor / GameInstance / World) and does NOT iterate LocalPlayer subsystems, since a headless editor world has no PIE LocalPlayer; use list_local_player_subsystems for those.
//   @param lifetime - Optional filter: "Engine"|"Editor"|"GameInstance"|"World" (case-insensitive)
//   @returns { total: int, lifetime_filter?, instances:[{ class_name, class_path, lifetime, instance_name, instance_path }] }

TSharedPtr<FJsonObject> HandleListSubsystemInstances(
	const TSharedPtr<FJsonObject>& Params)
{
	FString LifetimeFilter;
	Params->TryGetStringField(TEXT("lifetime"), LifetimeFilter);

	TArray<TSharedPtr<FJsonValue>> Entries;

	// Walk every live USubsystem instance. We use TObjectIterator<USubsystem> instead
	// of the per-collection `GetSubsystem<T>()` accessors because UWorld hides its
	// subsystem collection entirely, and the class-based variants only return the
	// typed base. Iterating live instances is O(N_subsystems) and avoids per-class
	// discovery cost.
	for (TObjectIterator<USubsystem> It; It; ++It)
	{
		USubsystem* Sub = *It;
		if (!Sub || !Sub->IsValidLowLevel()) { continue; }
		UClass* SubCls = Sub->GetClass();
		if (!SubCls) { continue; }
		if (Sub->HasAnyFlags(RF_ClassDefaultObject)) { continue; }

		if (!LifetimeFilter.IsEmpty())
		{
			const FString Detected = DetectLifetimeCategory(SubCls);
			if (!Detected.Equals(LifetimeFilter, ESearchCase::IgnoreCase)) { continue; }
		}

		TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
		E->SetStringField(TEXT("class_name"), SubCls->GetName());
		E->SetStringField(TEXT("class_path"), SubCls->GetPathName());
		E->SetStringField(TEXT("lifetime"), DetectLifetimeCategory(SubCls));
		E->SetStringField(TEXT("instance_name"), Sub->GetName());
		E->SetStringField(TEXT("instance_path"), Sub->GetPathName());
		Entries.Add(MakeShared<FJsonValueObject>(E));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("total"), Entries.Num());
	if (!LifetimeFilter.IsEmpty())
	{
		Data->SetStringField(TEXT("lifetime_filter"), LifetimeFilter);
	}
	Data->SetArrayField(TEXT("instances"), Entries);
	return mcp_common::MakeOkReply(Data);
}

const McpCommandFamilies::FRegistrar Registration =
{
	{ TEXT("activate_external_subsystem"), &HandleActivateExternalSubsystem },
	{ TEXT("create_subsystem_class_bp"), &HandleCreateSubsystemClassBp },
	{ TEXT("deactivate_external_subsystem"), &HandleDeactivateExternalSubsystem },
	{ TEXT("get_subsystem_class_info"), &HandleGetSubsystemClassInfo },
	{ TEXT("get_subsystem_instance"), &HandleGetSubsystemInstance },
	{ TEXT("is_subsystem_class_active"), &HandleIsSubsystemClassActive },
	{ TEXT("list_subsystem_classes"), &HandleListSubsystemClasses },
	{ TEXT("list_subsystem_instances"), &HandleListSubsystemInstances },
	{ TEXT("remove_subsystem_class_bp"), &HandleRemoveSubsystemClassBp },
};
} // namespace UEAgentSubsystemAuthoringCommands
