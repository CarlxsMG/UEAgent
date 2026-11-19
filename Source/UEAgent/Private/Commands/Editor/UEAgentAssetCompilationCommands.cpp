// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"

#include "AssetCompilingManager.h"
#include "IAssetCompilingManager.h"   // not pulled in by AssetCompilingManager.h from UE 5.8 on
#include "ShaderCompiler.h"           // GShaderCompilingManager: the job counts behind the shader kind
#include "Engine/Texture.h"
#include "Engine/StaticMesh.h"
#include "Engine/SkinnedAsset.h"
#include "Sound/SoundWave.h"
#include "UObject/UObjectIterator.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Async/TaskGraphInterfaces.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

/**
 * Asset compilation commands: what the editor still compiles in the background (shaders, textures, static and
 * skeletal meshes, and every other asset compiling manager the engine registers), and a bounded wait for it.
 *
 * The kinds come from FAssetCompilingManager::GetRegisteredManagers(), so a manager a plugin adds shows up without a
 * change here. Requests run on the bridge's single game-thread queue, so the wait cannot let the engine tick: it pumps
 * what the engine's own tick would (FAssetCompilingManager::ProcessAsyncTasks, which reaches the shader manager too,
 * and the game thread's task queue) between short sleeps, and gives up at its timeout instead of calling the
 * unbounded FinishAllCompilation.
 */
namespace UEAgentAssetCompilationCommands
{

// MCP-DOMAIN: Editor

namespace
{
    /** The longest wait one call may ask for: past it an MCP client's own call timeout gets close. */
    constexpr double AssetCompile_MaxTimeoutSeconds = 600.0;

    /** "MeshDistanceField" -> "mesh_distance_field". */
    FString AssetCompile_CamelToSnake(const FString& In)
    {
        FString Out;
        Out.Reserve(In.Len() + 8);
        for (int32 Index = 0; Index < In.Len(); ++Index)
        {
            const TCHAR Char = In[Index];
            if (FChar::IsUpper(Char))
            {
                const bool bPrevLower = Index > 0 && (FChar::IsLower(In[Index - 1]) || FChar::IsDigit(In[Index - 1]));
                const bool bNextLower = Index + 1 < In.Len() && FChar::IsLower(In[Index + 1]);
                const bool bPrevUpper = Index > 0 && FChar::IsUpper(In[Index - 1]);
                if (!Out.IsEmpty() && (bPrevLower || (bPrevUpper && bNextLower)))
                {
                    Out.AppendChar(TEXT('_'));
                }
                Out.AppendChar(FChar::ToLower(Char));
            }
            else if (FChar::IsAlnum(Char))
            {
                Out.AppendChar(Char);
            }
            else if (!Out.IsEmpty() && !Out.EndsWith(TEXT("_")))
            {
                Out.AppendChar(TEXT('_'));
            }
        }
        return Out;
    }

    /** The kind a caller names for a manager's type name: the five common ones by their plural, the rest derived. */
    FString AssetCompile_KindOf(const FName TypeName)
    {
        const FString Type = TypeName.ToString();
        if (Type.Equals(TEXT("UE-Shader"), ESearchCase::IgnoreCase))       { return TEXT("shaders"); }
        if (Type.Equals(TEXT("UE-Texture"), ESearchCase::IgnoreCase))      { return TEXT("textures"); }
        if (Type.Equals(TEXT("UE-StaticMesh"), ESearchCase::IgnoreCase))   { return TEXT("static_meshes"); }
        if (Type.Equals(TEXT("UE-SkinnedAsset"), ESearchCase::IgnoreCase)) { return TEXT("skeletal_meshes"); }
        if (Type.Equals(TEXT("UE-SoundWave"), ESearchCase::IgnoreCase))    { return TEXT("sound_waves"); }
        FString Stripped = Type;
        Stripped.RemoveFromStart(TEXT("UE-"), ESearchCase::IgnoreCase);
        return AssetCompile_CamelToSnake(Stripped);
    }

    struct FAssetCompileKind
    {
        FString Kind;
        FName TypeName;
        IAssetCompilingManager* Manager = nullptr;
    };

    TArray<FAssetCompileKind> AssetCompile_AllKinds()
    {
        TArray<FAssetCompileKind> Kinds;
        for (IAssetCompilingManager* Manager : FAssetCompilingManager::Get().GetRegisteredManagers())
        {
            if (Manager)
            {
                FAssetCompileKind& Entry = Kinds.AddDefaulted_GetRef();
                Entry.TypeName = Manager->GetAssetTypeName();
                Entry.Kind = AssetCompile_KindOf(Entry.TypeName);
                Entry.Manager = Manager;
            }
        }
        Kinds.Sort([](const FAssetCompileKind& A, const FAssetCompileKind& B) { return A.Kind < B.Kind; });
        return Kinds;
    }

    /**
     * The kinds the optional `kinds` param selects (all registered ones when it is absent, null or empty), each
     * matched case-insensitively against a kind ("textures") or a type name ("UE-Texture"). Null on success, else the
     * error reply naming what did not match and the kinds that exist.
     */
    TSharedPtr<FJsonObject> AssetCompile_SelectKinds(const TSharedPtr<FJsonObject>& Params, TArray<FAssetCompileKind>& OutKinds)
    {
        const TArray<FAssetCompileKind> All = AssetCompile_AllKinds();
        const TArray<TSharedPtr<FJsonValue>>* Requested = nullptr;
        const bool bSelects = Params->TryGetArrayField(TEXT("kinds"), Requested) && Requested != nullptr && Requested->Num() > 0;
        if (!bSelects)
        {
            OutKinds = All;
            return nullptr;
        }

        TArray<FString> Available;
        for (const FAssetCompileKind& Entry : All)
        {
            Available.Add(Entry.Kind);
        }
        for (const TSharedPtr<FJsonValue>& Value : *Requested)
        {
            FString Name;
            if (!Value.IsValid() || !Value->TryGetString(Name) || Name.TrimStartAndEnd().IsEmpty())
            {
                return mcp_common::MakeErrorReply(FString::Printf(
                    TEXT("kinds holds an entry that is not a non-empty string. Kinds: %s"), *FString::Join(Available, TEXT(", "))));
            }
            Name = Name.TrimStartAndEnd();
            const FAssetCompileKind* Match = All.FindByPredicate([&Name](const FAssetCompileKind& Entry)
            {
                return Entry.Kind.Equals(Name, ESearchCase::IgnoreCase)
                    || Entry.TypeName.ToString().Equals(Name, ESearchCase::IgnoreCase);
            });
            if (!Match)
            {
                return mcp_common::MakeErrorReply(FString::Printf(
                    TEXT("Unknown compilation kind '%s'. Kinds: %s"), *Name, *FString::Join(Available, TEXT(", "))));
            }
            if (!OutKinds.ContainsByPredicate([Match](const FAssetCompileKind& Entry) { return Entry.Manager == Match->Manager; }))
            {
                OutKinds.Add(*Match);
            }
        }
        return nullptr;
    }

    template <typename AssetType>
    void AssetCompile_CollectCompiling(int32 MaxAssets, TArray<TSharedPtr<FJsonValue>>& OutPaths, int32& OutTotal)
    {
        for (TObjectIterator<AssetType> It; It; ++It)
        {
            const AssetType* Asset = *It;
            if (Asset && !Asset->HasAnyFlags(RF_ClassDefaultObject) && Asset->IsCompiling())
            {
                ++OutTotal;
                if (OutPaths.Num() < MaxAssets)
                {
                    OutPaths.Add(MakeShared<FJsonValueString>(Asset->GetPathName()));
                }
            }
        }
    }

    /** Whether AssetCompile_AddAssetList can name the assets of Kind. */
    bool AssetCompile_IsListable(const FString& Kind)
    {
        return Kind == TEXT("textures") || Kind == TEXT("static_meshes") || Kind == TEXT("skeletal_meshes")
            || Kind == TEXT("sound_waves");
    }

    /**
     * Adds assets, assets_total and assets_truncated to Entry for a kind whose assets can be listed (textures, static
     * and skeletal meshes, sound waves); false for another kind, which only has a count.
     */
    bool AssetCompile_AddAssetList(const FString& Kind, int32 MaxAssets, const TSharedPtr<FJsonObject>& Entry)
    {
        TArray<TSharedPtr<FJsonValue>> Paths;
        int32 Total = 0;
        if (Kind == TEXT("textures"))             { AssetCompile_CollectCompiling<UTexture>(MaxAssets, Paths, Total); }
        else if (Kind == TEXT("static_meshes"))   { AssetCompile_CollectCompiling<UStaticMesh>(MaxAssets, Paths, Total); }
        else if (Kind == TEXT("skeletal_meshes")) { AssetCompile_CollectCompiling<USkinnedAsset>(MaxAssets, Paths, Total); }
        else if (Kind == TEXT("sound_waves"))     { AssetCompile_CollectCompiling<USoundWave>(MaxAssets, Paths, Total); }
        else
        {
            return false;
        }
        Entry->SetArrayField(TEXT("assets"), Paths);
        Entry->SetNumberField(TEXT("assets_total"), Total);
        Entry->SetBoolField(TEXT("assets_truncated"), Total > Paths.Num());
        return true;
    }

    /** The optional include_assets / max_assets pair: false and 50 by default, max_assets clamped to 1..1000. */
    void AssetCompile_ReadAssetListParams(const TSharedPtr<FJsonObject>& Params, bool& bOutInclude, int32& OutMax)
    {
        bOutInclude = false;
        Params->TryGetBoolField(TEXT("include_assets"), bOutInclude);
        double Max = 50.0;
        Params->TryGetNumberField(TEXT("max_assets"), Max);
        OutMax = FMath::Clamp(FMath::RoundToInt(Max), 1, 1000);
    }

    /** {outstanding, pending, remaining, compiling} of the shader compiling manager, or null without one. */
    TSharedPtr<FJsonObject> AssetCompile_ShaderJobs()
    {
        if (GShaderCompilingManager == nullptr)
        {
            return nullptr;
        }
        TSharedPtr<FJsonObject> Jobs = MakeShared<FJsonObject>();
        Jobs->SetNumberField(TEXT("outstanding"), GShaderCompilingManager->GetNumOutstandingJobs());
        Jobs->SetNumberField(TEXT("pending"), GShaderCompilingManager->GetNumPendingJobs());
        Jobs->SetNumberField(TEXT("remaining"), GShaderCompilingManager->GetNumRemainingJobs());
        Jobs->SetBoolField(TEXT("compiling"), GShaderCompilingManager->IsCompiling());
        return Jobs;
    }

    int32 AssetCompile_SumRemaining(const TArray<FAssetCompileKind>& Kinds)
    {
        int32 Sum = 0;
        for (const FAssetCompileKind& Entry : Kinds)
        {
            Sum += FMath::Max(0, Entry.Manager->GetNumRemainingAssets());
        }
        return Sum;
    }
}

// MCP:get_asset_compilation_status - What the editor is still compiling in the background, per kind: shaders, textures, static_meshes, skeletal_meshes, sound_waves and every other asset compiling manager the engine registers (mesh distance fields, mesh cards, animation sequences, Niagara systems...), with the count left for each and, on request, which assets. A count of 0 everywhere means rendering, cooking and measurements see final data; wait_for_asset_compilation blocks until it is so.
//   @param kinds [array] - Optional list of kinds to report, by kind ("textures") or engine type name ("UE-Texture"), case-insensitive; default all. An unknown kind is an error that lists the kinds there are.
//   @param include_assets - Optional bool (default false): also list the object paths still compiling for textures, static_meshes, skeletal_meshes and sound_waves (the other kinds only have a count).
//   @param max_assets - Optional int (default 50, clamped 1..1000): most paths listed per kind; assets_total still counts them all.
//   @returns { compiling: bool, total_remaining: int, kinds: [{ kind: string, type_name: string, remaining: int, assets_listable: bool, assets?: [string], assets_total?: int, assets_truncated?: bool }], shader_jobs?: { outstanding: int, pending: int, remaining: int, compiling: bool } }
//   @hints read-only
TSharedPtr<FJsonObject> HandleGetAssetCompilationStatus(const TSharedPtr<FJsonObject>& Params)
{
    TArray<FAssetCompileKind> Kinds;
    if (TSharedPtr<FJsonObject> Error = AssetCompile_SelectKinds(Params, Kinds))
    {
        return Error;
    }
    bool bIncludeAssets = false;
    int32 MaxAssets = 50;
    AssetCompile_ReadAssetListParams(Params, bIncludeAssets, MaxAssets);

    TArray<TSharedPtr<FJsonValue>> KindsJson;
    int32 Total = 0;
    for (const FAssetCompileKind& Entry : Kinds)
    {
        const int32 Remaining = FMath::Max(0, Entry.Manager->GetNumRemainingAssets());
        Total += Remaining;
        TSharedPtr<FJsonObject> KindJson = MakeShared<FJsonObject>();
        KindJson->SetStringField(TEXT("kind"), Entry.Kind);
        KindJson->SetStringField(TEXT("type_name"), Entry.TypeName.ToString());
        KindJson->SetNumberField(TEXT("remaining"), Remaining);
        KindJson->SetBoolField(TEXT("assets_listable"), AssetCompile_IsListable(Entry.Kind));
        if (bIncludeAssets)
        {
            AssetCompile_AddAssetList(Entry.Kind, MaxAssets, KindJson);
        }
        KindsJson.Add(MakeShared<FJsonValueObject>(KindJson));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("compiling"), Total > 0);
    Data->SetNumberField(TEXT("total_remaining"), Total);
    Data->SetArrayField(TEXT("kinds"), KindsJson);
    if (TSharedPtr<FJsonObject> ShaderJobs = AssetCompile_ShaderJobs())
    {
        Data->SetObjectField(TEXT("shader_jobs"), ShaderJobs);
    }
    return mcp_common::MakeOkReply(Data);
}

// MCP:wait_for_asset_compilation - Block until the editor has finished compiling the chosen kinds of assets (shaders, textures, static_meshes, skeletal_meshes... as get_asset_compilation_status names them), or until timeout_seconds, and report what finished and what remains per kind. success is false when it timed out with work left; the data still says how far it got.
//   @para While it waits it does what the engine's tick would: it pumps every asset compiling manager (all kinds, since shaders wait on textures) and the game thread's task queue between short sleeps. The editor does not redraw and no other MCP call runs meanwhile, so keep the timeout short and call it again rather than asking for minutes; do not call it while run_automation_test or run_functional_test_in_pie has a test running, since it blocks the ticks the test needs.
//   @param timeout_seconds - Optional float (default 60, clamped 0..600): upper bound on the wait. 0 pumps once and reports.
//   @param kinds [array] - Optional list of kinds to wait for, by kind ("shaders") or engine type name ("UE-Shader"), case-insensitive; default all. An unknown kind is an error that lists the kinds there are.
//   @param poll_interval_seconds - Optional float (default 0.05, clamped 0.01..1): sleep between pumps.
//   @param include_assets - Optional bool (default false): list the object paths still compiling after the wait, for textures, static_meshes, skeletal_meshes and sound_waves.
//   @param max_assets - Optional int (default 50, clamped 1..1000): most paths listed per kind.
//   @returns { completed: bool, timed_out: bool, elapsed_seconds: float, pumps: int, remaining_before: int, remaining_after: int, kinds: [{ kind: string, type_name: string, remaining_before: int, remaining_after: int, finished: bool, assets?: [string], assets_total?: int, assets_truncated?: bool }], shader_jobs?: { outstanding: int, pending: int, remaining: int, compiling: bool } }
//   @hints read-only
TSharedPtr<FJsonObject> HandleWaitForAssetCompilation(const TSharedPtr<FJsonObject>& Params)
{
    TArray<FAssetCompileKind> Kinds;
    if (TSharedPtr<FJsonObject> Error = AssetCompile_SelectKinds(Params, Kinds))
    {
        return Error;
    }
    bool bIncludeAssets = false;
    int32 MaxAssets = 50;
    AssetCompile_ReadAssetListParams(Params, bIncludeAssets, MaxAssets);

    double TimeoutSeconds = 60.0;
    double PollSeconds = 0.05;
    double Value = 0.0;
    if (Params->TryGetNumberField(TEXT("timeout_seconds"), Value))
    {
        TimeoutSeconds = FMath::Clamp(Value, 0.0, AssetCompile_MaxTimeoutSeconds);
    }
    if (Params->TryGetNumberField(TEXT("poll_interval_seconds"), Value))
    {
        PollSeconds = FMath::Clamp(Value, 0.01, 1.0);
    }

    TArray<int32> Before;
    for (const FAssetCompileKind& Entry : Kinds)
    {
        Before.Add(FMath::Max(0, Entry.Manager->GetNumRemainingAssets()));
    }
    const int32 RemainingBefore = AssetCompile_SumRemaining(Kinds);

    FAssetCompilingManager& CompilingManager = FAssetCompilingManager::Get();
    const double Start = FPlatformTime::Seconds();
    int32 Pumps = 0;
    bool bTimedOut = false;
    for (;;)
    {
        // Every manager is pumped, not only the selected ones: a kind can wait on another (shaders on textures).
        CompilingManager.ProcessAsyncTasks(/*bLimitExecutionTime=*/true);
        FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
        ++Pumps;

        if (AssetCompile_SumRemaining(Kinds) == 0)
        {
            break;
        }
        if (FPlatformTime::Seconds() - Start >= TimeoutSeconds)
        {
            bTimedOut = true;
            break;
        }
        FPlatformProcess::Sleep(static_cast<float>(PollSeconds));
    }
    const double Elapsed = FPlatformTime::Seconds() - Start;

    TArray<TSharedPtr<FJsonValue>> KindsJson;
    for (int32 Index = 0; Index < Kinds.Num(); ++Index)
    {
        const FAssetCompileKind& Entry = Kinds[Index];
        const int32 After = FMath::Max(0, Entry.Manager->GetNumRemainingAssets());
        TSharedPtr<FJsonObject> KindJson = MakeShared<FJsonObject>();
        KindJson->SetStringField(TEXT("kind"), Entry.Kind);
        KindJson->SetStringField(TEXT("type_name"), Entry.TypeName.ToString());
        KindJson->SetNumberField(TEXT("remaining_before"), Before[Index]);
        KindJson->SetNumberField(TEXT("remaining_after"), After);
        KindJson->SetBoolField(TEXT("finished"), After == 0);
        if (bIncludeAssets)
        {
            AssetCompile_AddAssetList(Entry.Kind, MaxAssets, KindJson);
        }
        KindsJson.Add(MakeShared<FJsonValueObject>(KindJson));
    }
    const int32 RemainingAfter = AssetCompile_SumRemaining(Kinds);
    const bool bCompleted = RemainingAfter == 0;

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("completed"), bCompleted);
    Data->SetBoolField(TEXT("timed_out"), bTimedOut && !bCompleted);
    Data->SetNumberField(TEXT("elapsed_seconds"), Elapsed);
    Data->SetNumberField(TEXT("pumps"), Pumps);
    Data->SetNumberField(TEXT("remaining_before"), RemainingBefore);
    Data->SetNumberField(TEXT("remaining_after"), RemainingAfter);
    Data->SetArrayField(TEXT("kinds"), KindsJson);
    if (TSharedPtr<FJsonObject> ShaderJobs = AssetCompile_ShaderJobs())
    {
        Data->SetObjectField(TEXT("shader_jobs"), ShaderJobs);
    }
    return mcp_common::MakeOutcomeReply(Data, bCompleted, bCompleted ? FString() : FString::Printf(
        TEXT("wait_for_asset_compilation: %d compilation(s) still pending after %.1f s; call it again or raise timeout_seconds"),
        RemainingAfter, Elapsed));
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("get_asset_compilation_status"), &HandleGetAssetCompilationStatus },
    { TEXT("wait_for_asset_compilation"), &HandleWaitForAssetCompilation },
};
} // namespace UEAgentAssetCompilationCommands
