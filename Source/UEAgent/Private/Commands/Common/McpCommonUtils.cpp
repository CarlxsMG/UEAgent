// Copyright (c) 2026 CarlxsMG.

#include "Commands/Common/McpCommonUtils.h"
#include "Commands/Common/McpEnumLabels.h"
#include "UEAgentProjectSettings.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Misc/PackageName.h"
#include "UObject/UObjectGlobals.h"
#include "Factories/Factory.h"
#include "Misc/Paths.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "GameFramework/Actor.h"
#include "Components/ActorComponent.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "JsonObjectConverter.h"
#include "GameplayTagContainer.h"
#include "UObject/UnrealType.h"
#include "UObject/TextProperty.h"          // FTextProperty -- attributes the FText localization-only diff
#include "UObject/PropertyPortFlags.h"     // PPF_DeepComparison / PPF_None
#include "Misc/StringOutputDevice.h"       // ImportPropertyTextStrict: the parser's own error text
#include "Misc/EngineVersionComparison.h" // ENGINE_MAJOR/MINOR_VERSION for MakeRequiresEngineReply
#include "UObject/Field.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "EditorAssetLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "EdGraphSchema_K2.h"
#include "Modules/ModuleManager.h"
#include "K2Node_Event.h"
#include "K2Node_Self.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CallArrayFunction.h"      // array-library fns (MD_ArrayParam) MUST spawn as this subclass -- it carries the wildcard type-propagation
#include "K2Node_CallDataTableFunction.h"  // Datatable fns (MD_DataTablePin, e.g. GetDataTableRow) spawn as this subclass -- resolves the wildcard OutRow
#include "K2Node_Variable.h" // UK2Node_Variable base: VariableReference + SetFromProperty
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraph.h" // FGraphNodeCreator
#include "Editor.h"                                    // GEditor
#include "UEAgentBridge.h"                             // RunSubCommands dispatches through the bridge
#include "Editor/Transactor.h"                         // FTransaction::GetTransactionObjects -- the PIE-session check of a transaction
#include "Engine/GameInstance.h"                       // UGameInstance::GetWorldContext -- the same check
#include "FileHelpers.h"                               // UEditorLoadingAndSavingUtils::SavePackages -- the editor's map save
#include "Editor/LevelEditor/Public/LevelEditorSubsystem.h"  // IsInPlayInEditor/EditorRequestEndPlay -- same API stop_play_in_editor uses
#include "HAL/PlatformProcess.h"                       // Sleep, while waiting for PIE teardown
#include "HAL/FileManager.h"                           // IFileManager::IsReadOnly -- the MEASURED cause of a failed save
#include "Async/TaskGraphInterfaces.h"                 // ProcessThreadUntilIdle, pump the game thread while waiting
#include "HAL/IConsoleManager.h"                       // FAutoConsoleVariableRef / IConsoleManager -- mcp.ReadOnlyRootEnforcement
#include "EditorSupportDelegates.h"                    // PrepareToCleanseEditorObject -- the only ENGINE_API door to the Python ref purge
#include "UObject/UObjectIterator.h"                   // TObjectIterator, to sweep resident worlds / PIE packages
#include "Engine/GameInstance.h"                       // UGameInstance -- PlayLevel.cpp tests these separately
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectHash.h"                       // GetDerivedClasses
#include "GameFramework/Volume.h"
#include "ActorFactories/ActorFactory.h"               // UActorFactory::CreateBrushForVolumeActor
#include "Builders/CubeBuilder.h"

namespace mcp_common
{
    bool ExtractStringPath(
        const TSharedPtr<FJsonObject>& Params,
        const FString& FieldName,
        FString& OutPath)
    {
        return Params->TryGetStringField(FieldName, OutPath)
            && !OutPath.IsEmpty();
    }

    // Is PIE running? Same detector that is_play_in_editor_active reports -- the check
    // callers are told to use -- so the guard and that check cannot disagree.
    //
    // A save during PIE is REFUSED with the reason stated; the session is never stopped
    // to make room for it. Two reasons:
    //   - killing someone's PIE session is a side effect nobody asked for;
    //   - stopping it would not make the save work. The engine guard
    //     (Editor.AllowSavingAssetsDuringPIE=0) rejects BEFORE attempting anything, so
    //     if the teardown has not finished within a timeout, "saving anyway" saves
    //     nothing and reports success anyway.
    // The first-line refusal happens in the dispatcher
    // (UUEAgentBridge::CheckPIESafetyGuard); this covers internal paths.
    // Game thread only, like every dispatch: no lock needed.
    static bool GDiscardUndoStepRequested = false;

    void DiscardUndoStepOfThisCall()
    {
        GDiscardUndoStepRequested = true;
    }

    bool ConsumeDiscardUndoStepRequest()
    {
        const bool bRequested = GDiscardUndoStepRequested;
        GDiscardUndoStepRequested = false;
        return bRequested;
    }

    bool IsPieActive()
    {
        if (ULevelEditorSubsystem* LevelEditorSubsystem =
                GEditor ? GEditor->GetEditorSubsystem<ULevelEditorSubsystem>() : nullptr)
        {
            return LevelEditorSubsystem->IsInPlayInEditor();
        }
        return GEditor != nullptr && GEditor->PlayWorld != nullptr;
    }

    bool SaveAssetPackage(UPackage* Package, UObject* Asset, FString& OutError, bool* OutPieWasActive, bool* OutWasUnderReadOnlyRoot)
    {
        if (OutWasUnderReadOnlyRoot) { *OutWasUnderReadOnlyRoot = false; }

        if (!Package || !Asset)
        {
            OutError = TEXT("Invalid package or asset for save");
            return false;
        }

        // PIE: REFUSE -- neither stop the session nor "save anyway". With PIE running the
        // engine rejects the write before attempting anything, so carrying on does not
        // produce a worse save: it produces a SUCCESS WITHOUT A FILE, which is exactly the
        // silent failure this block exists to cut.
        const bool bPieActive = IsPieActive();
        if (OutPieWasActive) { *OutPieWasActive = bPieActive; }
        if (bPieActive)
        {
            OutError = FString::Printf(
                TEXT("REFUSED: a Play-in-Editor session is running, and the engine guard ")
                TEXT("Editor.AllowSavingAssetsDuringPIE=0 refuses the write before attempting anything ")
                TEXT("-- saving '%s' now would report success without writing a byte. ")
                TEXT("Stop PIE (stop_play_in_editor) and save again."),
                *Package->GetName());
            UE_LOG(LogTemp, Warning, TEXT("[UEAgent] %s"), *OutError);
            return false;
        }

        // A package that holds a UWorld is a map and lives in a .umap (UWorld::CreateWorld and the world's
        // serialization set PKG_ContainsMap); written to a .uasset it would be a second package of the same name
        // next to the map. Same choice as the engine's own savers (PackageAutoSaver, AssetViewUtils).
        const FString PackageName = Package->GetName();
        FString FileName;
        if (!FPackageName::TryConvertLongPackageNameToFilename(PackageName, FileName,
                Package->ContainsMap() ? FPackageName::GetMapPackageExtension() : FPackageName::GetAssetPackageExtension()))
        {
            OutError = FString::Printf(TEXT("Could not convert package name to filename: %s"), *PackageName);
            return false;
        }

        // The read-only root guard runs BEFORE MarkPackageDirty: a dirtied upstream package is
        // written by the next unrelated save, which is how in-place edits happen unnoticed.
        // A refused save is a visible, recoverable error; `mcp.ReadOnlyRootEnforcement 0` on the
        // editor console turns the refusal into a warning.
        if (IsUnderReadOnlyRoot(PackageName))
        {
            if (OutWasUnderReadOnlyRoot) { *OutWasUnderReadOnlyRoot = true; }

            if (IsReadOnlyRootSaveEnforcementEnabled())
            {
                OutError = FString::Printf(
                    TEXT("Refusing to save '%s': it lives under a read-only root. Read-only roots are ")
                    TEXT("vendored upstream content that tools must never write. ")
                    TEXT("Fork the asset into a writable root, or set the console variable ")
                    TEXT("'mcp.ReadOnlyRootEnforcement 0' if this really is intended."),
                    *PackageName);
                UE_LOG(LogTemp, Error, TEXT("[UEAgent] %s"), *OutError);
                return false;
            }

            UE_LOG(LogTemp, Warning,
                TEXT("[UEAgent] READ-ONLY ROOT WRITE: saving '%s', which is under a read-only root. ")
                TEXT("Enforcement has been turned OFF (mcp.ReadOnlyRootEnforcement 0), so the save ")
                TEXT("proceeds -- verify with 'git status' that this drift was intended."),
                *PackageName);
        }

        Package->MarkPackageDirty();

        bool bSaved = false;
        if (Package->ContainsMap())
        {
            // A map is saved the way the editor saves one (FileHelpers.cpp SaveWorld): UEditorEngine::Save runs the
            // world's pre-save (OnPreSaveWorld, the physics scene, UWorld::PreSaveRoot), and the world's own dirty
            // external packages (one file per actor) and its _BuiltData package are written with it; a newly created
            // world writes all its external packages. No other dirty package is saved. This scripting entry point
            // passes bUseDialog=false and runs as an unattended script, so a failure is logged, never a modal dialog;
            // with source control enabled it checks the files out, as any editor save does.
            const TArray<UPackage*> MapPackage{ Package };
            bSaved = UEditorLoadingAndSavingUtils::SavePackages(MapPackage, /*bOnlyDirty=*/false);
        }
        else
        {
            FSavePackageArgs SaveArgs;
            SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
            SaveArgs.SaveFlags = SAVE_NoError;
            SaveArgs.Error = GError;
            bSaved = UPackage::SavePackage(Package, Asset, *FileName, SaveArgs);
        }
        if (!bSaved)
        {
            // This is the shared save helper: hundreds of handlers derive their `saved`
            // from here, so "Failed to save package to disk" is what almost everyone writing
            // an asset sees -- and a common cause is measurable in one line: in a Git LFS repo,
            // `.uasset` files marked `lockable` in .gitattributes are left READ-ONLY on disk
            // after each commit. Without measuring it, the reader must know that.
            //
            // It is CONFIRMED or RULED OUT -- both halves save work: ruling it out keeps
            // readers from hunting a lock that is not the problem.
            const bool bFileReadOnly = IFileManager::Get().FileExists(*FileName)
                                    && IFileManager::Get().IsReadOnly(*FileName);
            OutError = FString::Printf(
                TEXT("Failed to save package to disk: %s. %s"),
                *FileName,
                bFileReadOnly
                    ? TEXT("MEASURED CAUSE: the file is READ-ONLY on disk. In an LFS repo that is a "
                           "`lockable` path you have not locked -- git-lfs restores the attribute after "
                           "every commit. Take the lock and retry:  git lfs lock <file>.")
                    : TEXT("The file is NOT read-only (measured), so an LFS lock is NOT the explanation: "
                           "look at a full disk, a permission problem, or a package the engine refused "
                           "to serialize (check the Output Log for the SavePackage error)."));
            UE_LOG(LogTemp, Error, TEXT("[UEAgent] %s"), *OutError);
            return false;
        }
        return true;
    }

    // Convenience overload for callers that don't surface the
    // error string.
    bool SaveAssetPackage(UPackage* Package, UObject* Asset)
    {
        FString Discard;
        return SaveAssetPackage(Package, Asset, Discard);
    }

    // SAFETY: this function deliberately bypasses IAssetTools::ImportAssets /
    // ImportAssetTasks. Those pump the game-thread task graph, which trips
    // a RecursionGuard assertion when called from inside the bridge's
    // FTSTicker callback.
    //
    // We go through UFactory::StaticImportObject, which is the canonical
    // public entry point. StaticImportObject is fully synchronous on the
    // calling thread -- it reads the file from disk, calls
    // Factory->ImportObject -> FactoryCreateFile -> FactoryCreateBinary, and
    // returns the new UObject. No IAssetTools, no task-graph pumping.
    //
    // (We can't call FactoryCreateBinary directly from this static helper
    // because that method is `protected` on the UFactory base; the override
    // on UTextureFactory/USoundFactory is `public` but the access check is
    // on the static type, which is UFactory* here.)
    FString FactoryCreateBinaryImport(
        const FString& SourceFilePath,
        const FString& DestinationPackagePath,
        const FString& DestinationName,
        UFactory* Factory,
        UClass* TargetClass,
        FString& OutError)
    {
        if (!Factory)
        {
            OutError = TEXT("Factory is null");
            return FString();
        }
        if (!TargetClass)
        {
            OutError = TEXT("TargetClass is null");
            return FString();
        }

        // 1) Derive the final asset name (default: source file stem).
        FString FinalName = DestinationName;
        if (FinalName.IsEmpty())
        {
            FinalName = FPaths::GetBaseFilename(SourceFilePath);
        }
        if (FinalName.IsEmpty())
        {
            OutError = TEXT("DestinationName is empty and could not be derived from source file stem");
            return FString();
        }

        // 2) Create / reuse the destination package.
        const FString PackageName = DestinationPackagePath / FinalName;
        OutError = mcp_common::AssetNameConflict(PackageName, FinalName, TargetClass);
        if (!OutError.IsEmpty())
        {
            return FString();
        }
        UPackage* Package = CreatePackage(*PackageName);
        if (!Package)
        {
            OutError = FString::Printf(TEXT("Failed to create package: %s"), *PackageName);
            return FString();
        }
        Package->FullyLoad();

        // 3) Root the factory for the duration of the call so it cannot be
        //    GC'd mid-import by any incidental collection triggered by the
        //    factory's own internal calls (texture mip generation, sound
        //    decode, etc.).
        Factory->AddToRoot();

        // 4) Drive the import synchronously via StaticImportObject (PUBLIC
        //    API, bypasses IAssetTools entirely). StaticImportObject reads
        //    the source file from disk itself, so we don't need to
        //    pre-load it.
        UObject* NewObj = UFactory::StaticImportObject(
            TargetClass,
            Package,
            FName(*FinalName),
            RF_Public | RF_Standalone,
            *SourceFilePath,
            /*Context=*/nullptr,
            /*InFactory=*/Factory,
            /*Parms=*/nullptr,
            GWarn);

        Factory->RemoveFromRoot();

        if (!NewObj)
        {
            OutError = FString::Printf(
                TEXT("StaticImportObject returned null for '%s' (class=%s, factory=%s)"),
                *SourceFilePath, *TargetClass->GetName(), *Factory->GetClass()->GetName());
            return FString();
        }

        // 5) Register the new asset, mark its package dirty, persist to disk.
        FAssetRegistryModule::AssetCreated(NewObj);
        Package->MarkPackageDirty();

        FString SaveError;
        if (!SaveAssetPackage(Package, NewObj, SaveError))
        {
            // The import itself succeeded -- the asset is in memory and in
            // the registry -- but we couldn't persist it. Surface the
            // warning but still return the path so the caller can decide
            // what to do (the asset is usable in-memory for the rest of
            // this editor session).
            UE_LOG(LogTemp, Warning,
                TEXT("FactoryCreateBinaryImport: import succeeded but save failed for '%s': %s"),
                *NewObj->GetPathName(), *SaveError);
        }

        return NewObj->GetPathName();
    }

    AActor* FindActorByName(UWorld* World, const FString& Name)
    {
        if (!World)
        {
            return nullptr;
        }
        for (TActorIterator<AActor> It(World); It; ++It)
        {
            AActor* Actor = *It;
            if (Actor && (Actor->GetName() == Name || Actor->GetActorLabel() == Name))
            {
                return Actor;
            }
        }
        return nullptr;
    }

    AActor* FindActorByLabel(UWorld* World, const FString& Label)
    {
        if (!World)
        {
            return nullptr;
        }
        for (TActorIterator<AActor> It(World); It; ++It)
        {
            if (It->GetActorLabel() == Label)
            {
                return *It;
            }
        }
        return nullptr;
    }

    FName RenameToFreeName(UObject* Object, const FString& WantedName, UObject* NewOuter, ERenameFlags Flags)
    {
        if (!WantedName.IsEmpty())
        {
            UObject* Outer = NewOuter ? NewOuter : Object->GetOuter();
            const FName Wanted(*WantedName);
            UObject* Occupant = StaticFindObjectFast(UObject::StaticClass(), Outer, Wanted);
            if (Occupant != nullptr && Occupant != Object)
            {
                Object->Rename(*MakeUniqueObjectName(Outer, Object->GetClass(), Wanted).ToString(), NewOuter, Flags);
                return Object->GetFName();
            }
        }
        Object->Rename(*WantedName, NewOuter, Flags);
        return Object->GetFName();
    }

    bool JsonToStruct(const TSharedPtr<FJsonObject>& Json, const UStruct* StructType,
                      void* Out, FString& OutError, int64 CheckFlags, int64 SkipFlags)
    {
        if (!Json.IsValid() || !StructType || !Out)
        {
            OutError = TEXT("Invalid JSON object, struct type, or output buffer");
            return false;
        }
        FText FailReason;
        if (!FJsonObjectConverter::JsonObjectToUStruct(Json.ToSharedRef(), StructType, Out,
                CheckFlags, SkipFlags, /*bStrictMode*/ false, &FailReason))
        {
            OutError = FailReason.IsEmpty()
                ? FString::Printf(TEXT("Failed to merge JSON into %s (check field names/types)"), *StructType->GetName())
                : FailReason.ToString();
            return false;
        }
        return true;
    }

    TSharedPtr<FJsonObject> StructToJson(const UStruct* StructType, const void* In,
                                         int64 CheckFlags, int64 SkipFlags)
    {
        if (!StructType || !In)
        {
            return nullptr;
        }
        TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
        if (!FJsonObjectConverter::UStructToJsonObject(StructType, In, Out, CheckFlags, SkipFlags))
        {
            return nullptr;
        }
        return Out;
    }

    void ClampNumericByMeta(const UStruct* StructType, void* Container)
    {
        if (!StructType || !Container)
        {
            return;
        }
        for (TFieldIterator<FProperty> It(StructType); It; ++It)
        {
            FProperty* Prop = *It;
            const bool bHasMin = Prop->HasMetaData(TEXT("ClampMin"));
            const bool bHasMax = Prop->HasMetaData(TEXT("ClampMax"));
            if (!bHasMin && !bHasMax)
            {
                continue;
            }
            FNumericProperty* Num = CastField<FNumericProperty>(Prop);
            if (!Num)
            {
                continue;
            }
            void* Value = Prop->ContainerPtrToValuePtr<uint8>(Container);
            if (Num->IsFloatingPoint())
            {
                double V = Num->GetFloatingPointPropertyValue(Value);
                if (bHasMin) V = FMath::Max(V, FCString::Atod(*Prop->GetMetaData(TEXT("ClampMin"))));
                if (bHasMax) V = FMath::Min(V, FCString::Atod(*Prop->GetMetaData(TEXT("ClampMax"))));
                Num->SetFloatingPointPropertyValue(Value, V);
            }
            else if (Num->IsInteger())
            {
                int64 V = Num->GetSignedIntPropertyValue(Value);
                if (bHasMin) V = FMath::Max<int64>(V, FCString::Atoi64(*Prop->GetMetaData(TEXT("ClampMin"))));
                if (bHasMax) V = FMath::Min<int64>(V, FCString::Atoi64(*Prop->GetMetaData(TEXT("ClampMax"))));
                Num->SetIntPropertyValue(Value, V);
            }
        }
    }

    bool MergeJsonWithClamp(const TSharedPtr<FJsonObject>& Json, const UStruct* StructType,
                            void* Out, FString& OutError, int64 CheckFlags, int64 SkipFlags)
    {
        if (!JsonToStruct(Json, StructType, Out, OutError, CheckFlags, SkipFlags))
        {
            return false;
        }
        ClampNumericByMeta(StructType, Out);
        return true;
    }

} // namespace mcp_common

namespace UEAgentCommonImpl
{
    // Every command answers with the same envelope: a `success` flag, then either the reason it
    // failed (`error`) or what it produced (`data`). One builder keeps the three variants below
    // from drifting apart.
    TSharedPtr<FJsonObject> BuildEnvelope(bool bSucceeded, const FString* FailureReason, const TSharedPtr<FJsonObject>& Payload)
    {
        TSharedPtr<FJsonObject> Envelope = MakeShared<FJsonObject>();
        Envelope->SetBoolField(TEXT("success"), bSucceeded);
        if (FailureReason != nullptr)
        {
            Envelope->SetStringField(TEXT("error"), *FailureReason);
        }
        if (Payload.IsValid())
        {
            Envelope->SetObjectField(TEXT("data"), Payload);
        }
        return Envelope;
    }
}

namespace mcp_common
{
    TSharedPtr<FJsonObject> MakeErrorReply(const FString& Message)
    {
        return UEAgentCommonImpl::BuildEnvelope(false, &Message, nullptr);
    }

    TSharedPtr<FJsonObject> MakeOkReply(const TSharedPtr<FJsonObject>& Data)
    {
        return UEAgentCommonImpl::BuildEnvelope(true, nullptr, Data);
    }

    // The honest sibling of MakeOkReply (see the header): the flag comes from the
    // caller, so a payload that describes a failure is not wrapped in `success: true`.
    TSharedPtr<FJsonObject> RunSubCommand(const FString& Command, const TSharedPtr<FJsonObject>& Params)
    {
        UUEAgentBridge* Bridge = GEditor ? GEditor->GetEditorSubsystem<UUEAgentBridge>() : nullptr;
        if (!Bridge)
        {
            return MakeErrorReply(FString::Printf(TEXT("%s: the MCP bridge is not running"), *Command));
        }
        TGuardValue<int32> InnerCall(Bridge->SubCommandDepth, Bridge->SubCommandDepth + 1);
        const TSharedPtr<FJsonObject> Envelope = Bridge->DispatchSingleCommand(Command, Params.IsValid() ? Params : MakeShared<FJsonObject>());
        FString Status;
        const bool bSucceeded = Envelope.IsValid() && Envelope->TryGetStringField(TEXT("status"), Status) && Status == TEXT("success");
        const TSharedPtr<FJsonObject>* Result = nullptr;
        const bool bHasResult = Envelope.IsValid() && Envelope->TryGetObjectField(TEXT("result"), Result) && Result && Result->IsValid();
        // A handler's own failure states it (`success: false`) and goes back as it is.
        if (bHasResult && (bSucceeded || (*Result)->HasField(TEXT("success"))))
        {
            return *Result;
        }
        FString Error = TEXT("the command failed with no message");
        if (Envelope.IsValid())
        {
            Envelope->TryGetStringField(TEXT("error"), Error);
        }
        // A refusal (PIE guard, precondition) carries its detail in `result` and no `success` field: it is still a
        // failure, never the caller's success.
        return bHasResult ? MakeOutcomeReply(*Result, false, Error) : MakeErrorReply(Error);
    }

    TSharedPtr<FJsonObject> ForwardCall(const TCHAR* Tool, const FString& Command, const TSharedPtr<FJsonObject>& Params)
    {
        UUEAgentBridge* Bridge = GEditor ? GEditor->GetEditorSubsystem<UUEAgentBridge>() : nullptr;
        if (Bridge && Bridge->SubCommandDepth == 0)
        {
            if (Bridge->ForwardedBy != Tool)
            {
                Bridge->ForwardCount = 0;
            }
            Bridge->ForwardedBy = Tool;
            Bridge->ForwardedCommand = Command;
            Bridge->ForwardedParams = MakeShared<FJsonObject>();
            if (Params.IsValid())
            {
                // A copy: the dispatch rewrites aliases in place, and the contract reads the call as it was made.
                FJsonObject::Duplicate(Params, Bridge->ForwardedParams);
            }
            ++Bridge->ForwardCount;
        }
        return RunSubCommand(Command, Params);
    }

    TSharedPtr<FJsonObject> RunSubCommands(const TCHAR* Tool, const TArray<FSubCommand>& Steps, const TSharedPtr<FJsonObject>& Reply)
    {
        if (Steps.Num() == 0)
        {
            return MakeErrorReply(FString::Printf(TEXT("%s: no setting was given, so nothing changed"), Tool));
        }
        UUEAgentBridge* Bridge = GEditor ? GEditor->GetEditorSubsystem<UUEAgentBridge>() : nullptr;
        if (!Bridge)
        {
            return MakeErrorReply(FString::Printf(TEXT("%s: the MCP bridge is not running"), Tool));
        }
        TSharedPtr<FJsonObject> Applied = MakeShared<FJsonObject>();
        Reply->SetObjectField(TEXT("applied"), Applied);
        for (const FSubCommand& Step : Steps)
        {
            TGuardValue<int32> InnerCall(Bridge->SubCommandDepth, Bridge->SubCommandDepth + 1);
            const TSharedPtr<FJsonObject> Envelope = Bridge->DispatchSingleCommand(Step.Command, Step.Params.IsValid() ? Step.Params : MakeShared<FJsonObject>());
            FString Status;
            const bool bOk = Envelope.IsValid() && Envelope->TryGetStringField(TEXT("status"), Status) && Status == TEXT("success");
            const TSharedPtr<FJsonObject>* Result = nullptr;
            TSharedPtr<FJsonObject> Payload;
            if (Envelope.IsValid() && Envelope->TryGetObjectField(TEXT("result"), Result) && Result && Result->IsValid())
            {
                const TSharedPtr<FJsonObject>* Data = nullptr;
                Payload = (*Result)->TryGetObjectField(TEXT("data"), Data) && Data ? *Data : *Result;
            }
            if (bOk)
            {
                Applied->SetObjectField(Step.Label, Payload.IsValid() ? Payload : MakeShared<FJsonObject>());
                continue;
            }
            FString Error = TEXT("the step failed with no message");
            if (Envelope.IsValid())
            {
                Envelope->TryGetStringField(TEXT("error"), Error);
            }
            TSharedPtr<FJsonObject> Failed = MakeShared<FJsonObject>();
            Failed->SetStringField(TEXT("label"), Step.Label);
            Failed->SetStringField(TEXT("command"), Step.Command);
            Failed->SetStringField(TEXT("error"), Error);
            if (Payload.IsValid())
            {
                Failed->SetObjectField(TEXT("reply"), Payload);
            }
            Reply->SetObjectField(TEXT("failed"), Failed);
            return MakeOutcomeReply(Reply, false, FString::Printf(
                TEXT("%s stopped at %s (%s): %s. %s"), Tool, *Step.Label, *Step.Command, *Error,
                Applied->Values.Num() > 0 ? TEXT("The settings listed in applied were changed before it.") : TEXT("Nothing was changed.")));
        }
        return MakeOutcomeReply(Reply, true, FString());
    }

    TSharedPtr<FJsonObject> MakeOutcomeReply(const TSharedPtr<FJsonObject>& Data, bool bSuccess, const FString& ErrorMessage)
    {
        if (bSuccess)
        {
            return UEAgentCommonImpl::BuildEnvelope(true, nullptr, Data);
        }
        const FString Reason = ErrorMessage.IsEmpty() ? FString(TEXT("Operation reported failure (see data for details)")) : ErrorMessage;
        return UEAgentCommonImpl::BuildEnvelope(false, &Reason, Data);
    }

    TSharedPtr<FJsonObject> MakeSavedReply(const TSharedPtr<FJsonObject>& Data, bool bSaved, const TCHAR* Command)
    {
        if (Data.IsValid() && !Data->HasField(TEXT("saved")))
        {
            Data->SetBoolField(TEXT("saved"), bSaved);
        }
        return MakeOutcomeReply(Data, bSaved, bSaved ? FString() : FString::Printf(
            TEXT("%s: the change was applied IN MEMORY but NOT saved to disk, so it is lost when the editor closes. ")
            TEXT("See the save error in this payload for the measured cause (usually a read-only file, or one locked by source control)."),
            Command));
    }

    TSharedPtr<FJsonObject> MakeSavedReply(const TSharedPtr<FJsonObject>& Data, bool bSaved, const TCHAR* Command, const FString& SaveError)
    {
        if (Data.IsValid() && !Data->HasField(TEXT("saved")))
        {
            Data->SetBoolField(TEXT("saved"), bSaved);
        }
        if (!bSaved && Data.IsValid() && !Data->HasField(TEXT("save_error")))
        {
            Data->SetStringField(TEXT("save_error"), SaveError);
        }
        return MakeSavedReply(Data, bSaved, Command);
    }

    TSharedPtr<FJsonObject> MissingParameterReply(const TCHAR* Key)
    {
        return MakeErrorReply(FString::Printf(TEXT("Missing required parameter: %s"), Key));
    }

    TSharedPtr<FJsonObject> ReadRequiredString(const TSharedPtr<FJsonObject>& Params, const TCHAR* Key, FString& Out,
        EAllowEmpty AllowEmpty)
    {
        if (!Params->TryGetStringField(Key, Out) || (AllowEmpty == EAllowEmpty::No && Out.IsEmpty()))
        {
            return MissingParameterReply(Key);
        }
        return nullptr;
    }

    TSharedPtr<FJsonObject> ReadRequiredBool(const TSharedPtr<FJsonObject>& Params, const TCHAR* Key, bool& Out)
    {
        if (!Params->TryGetBoolField(Key, Out))
        {
            return MissingParameterReply(Key);
        }
        return nullptr;
    }

    void AddSaveOutcome(const TSharedPtr<FJsonObject>& Data, bool bSaved, const FString& SaveError)
    {
        Data->SetBoolField(TEXT("saved"), bSaved);
        if (!bSaved)
        {
            Data->SetStringField(TEXT("save_error"), SaveError);
        }
    }

    FString SavedOutcomeError(const TCHAR* Command)
    {
        return FString(Command) + TEXT(": failed. Read `saved` in this payload: false there means the\n"
            "change WAS applied in memory but did NOT reach disk (usually a read-only file or\n"
            "one locked by source control -- see the save error). If `saved`\n"
            "is true, the operation itself is what failed.");
    }

    FString MeasuredOutcomeError(const TCHAR* Command, const TCHAR* MeasuredOutcome)
    {
        return FString::Printf(TEXT("%s reported failure: the measured outcome `%s` came back false. ")
            TEXT("The payload carries what was actually read."), Command, MeasuredOutcome);
    }

    // SEPARATE THE ECHO FROM THE MEASUREMENT.
    //
    // Many commands re-emit their own input parameters at the top level of the
    // payload, in fields whose names read like observations ("rotation",
    // "location", "value"). An echo agrees with the request by construction, so
    // it can never expose a mismatch: you ask for rotation [0,0,66.8], you are
    // told rotation [0,0,66.8], and the actor is standing there with Roll=66.8.
    // The reader has no way to tell which fields were measured and which were
    // handed back.
    //
    // This does not delete the echo (that would break every consumer that reads
    // those fields); it gives the echo a NAME. Anything under "requested" is,
    // by contract, what the caller sent -- never a reading. A handler that also
    // re-reads the object emits the reading at the top level, and the two can
    // be compared.
    void AddRequestedEcho(const TSharedPtr<FJsonObject>& Data, const TSharedPtr<FJsonObject>& Params, const TArray<FString>& FieldNames)
    {
        if (!Data.IsValid() || !Params.IsValid() || FieldNames.Num() == 0)
        {
            return;
        }

        // MERGE onto an existing "requested" rather than replacing it --
        // several call sites in one handler (e.g. a helper plus the handler
        // itself) must be able to contribute without clobbering each other.
        TSharedPtr<FJsonObject> Requested;
        const TSharedPtr<FJsonObject>* ExistingRequested = nullptr;
        if (Data->TryGetObjectField(TEXT("requested"), ExistingRequested)
            && ExistingRequested != nullptr && ExistingRequested->IsValid())
        {
            Requested = *ExistingRequested;
        }
        else
        {
            Requested = MakeShared<FJsonObject>();
        }

        for (const FString& FieldName : FieldNames)
        {
            if (FieldName.IsEmpty())
            {
                continue;
            }
            // Copy the FJsonValue VERBATIM -- no conversion, no
            // re-formatting. What lands under "requested" is byte-for-byte what
            // arrived on the wire; a normalized echo would be a third thing that
            // is neither the request nor the reading.
            const TSharedPtr<FJsonValue> Value = Params->TryGetField(FieldName);
            if (Value.IsValid())
            {
                Requested->SetField(FieldName, Value);
            }
            // Absent names are skipped in silence -- a caller listing the
            // handler's whole optional param set is the normal case.
        }

        // Never emit an empty `"requested": {}`; it would claim an echo was
        // recorded when nothing was.
        if (Requested->Values.Num() > 0)
        {
            Data->SetObjectField(TEXT("requested"), Requested);
        }
    }

} // namespace mcp_common

namespace UEAgentCommonImpl
{
    // The JSON numbers of an array field, in order. Returns nullptr when the field is absent or
    // is not an array, which every reader below treats as "use the default".
    const TArray<TSharedPtr<FJsonValue>>* FindNumberList(const TSharedPtr<FJsonObject>& Owner, const FString& Field)
    {
        const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
        return Owner->TryGetArrayField(Field, Items) ? Items : nullptr;
    }

    // Element I of a number list, narrowed through float on purpose: the vector readers have always
    // produced float-precision components, and callers compare against those exact values.
    double FloatAt(const TArray<TSharedPtr<FJsonValue>>& Items, int32 Index)
    {
        return static_cast<double>(static_cast<float>(Items[Index]->AsNumber()));
    }
}

namespace mcp_common
{
    void ReadJsonFloatArray(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName, TArray<float>& OutArray)
    {
        OutArray.Reset();
        if (const TArray<TSharedPtr<FJsonValue>>* Items = UEAgentCommonImpl::FindNumberList(JsonObject, FieldName))
        {
            OutArray.Reserve(Items->Num());
            for (const TSharedPtr<FJsonValue>& Item : *Items)
            {
                OutArray.Add(static_cast<float>(Item->AsNumber()));
            }
        }
    }

    FVector2D ReadJsonVector2D(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName)
    {
        const TArray<TSharedPtr<FJsonValue>>* Items = UEAgentCommonImpl::FindNumberList(JsonObject, FieldName);
        if (Items == nullptr || Items->Num() < 2)
        {
            return FVector2D::ZeroVector;
        }
        return FVector2D(UEAgentCommonImpl::FloatAt(*Items, 0), UEAgentCommonImpl::FloatAt(*Items, 1));
    }

    FVector ReadJsonVector(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName)
    {
        const TArray<TSharedPtr<FJsonValue>>* Items = UEAgentCommonImpl::FindNumberList(JsonObject, FieldName);
        if (Items == nullptr || Items->Num() < 3)
        {
            return FVector::ZeroVector;
        }
        return FVector(UEAgentCommonImpl::FloatAt(*Items, 0), UEAgentCommonImpl::FloatAt(*Items, 1),
                       UEAgentCommonImpl::FloatAt(*Items, 2));
    }

    // THE AMBIGUITY OF THE 3-NUMBER ARRAY. The full trap is documented,
    // deliberately loudly, on the declaration in McpCommonUtils.h -- read it
    // there. The short version:
    // [0]=Pitch, [1]=Yaw, [2]=Roll matches C++'s FRotator(Pitch,Yaw,Roll) and
    // matches neither Python's unreal.Rotator(roll,pitch,yaw) nor the intuition
    // of anyone who writes [0,0,90] meaning "yaw 90" (in a LOCATION array index
    // 2 is Z, and yaw turns about Z): a Roll where a Yaw was meant.
    //
    // The array order stays, on purpose: call sites written against it would
    // all break at once and in silence. What removes the AMBIGUITY is an OBJECT
    // form with named axes, accepted here, and AddRotatorField() reports
    // rotations the same way.
    FRotator ReadJsonRotator(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName)
    {
        FRotator Result = FRotator::ZeroRotator;
        if (!JsonObject.IsValid() || !JsonObject->HasField(FieldName))
        {
            return Result;
        }

        // OBJECT form {"roll":R,"pitch":P,"yaw":Y} -- the recommended one.
        // Any subset; absent axes stay 0. FJsonObject's key lookup is
        // case-insensitive, so "Yaw"/"YAW" resolve too. Checked FIRST so that
        // the ambiguous array path is only reached when there is nothing better.
        const TSharedPtr<FJsonObject>* RotatorObject = nullptr;
        if (JsonObject->TryGetObjectField(FieldName, RotatorObject)
            && RotatorObject != nullptr && RotatorObject->IsValid())
        {
            double Component = 0.0;
            if ((*RotatorObject)->TryGetNumberField(TEXT("roll"), Component))  { Result.Roll  = static_cast<FRotator::FReal>(Component); }
            if ((*RotatorObject)->TryGetNumberField(TEXT("pitch"), Component)) { Result.Pitch = static_cast<FRotator::FReal>(Component); }
            if ((*RotatorObject)->TryGetNumberField(TEXT("yaw"), Component))   { Result.Yaw   = static_cast<FRotator::FReal>(Component); }
            return Result;
        }

        // ARRAY form -- FROZEN order [Pitch, Yaw, Roll], float precision, exactly as callers
        // have always received it. Do not "fix" this order (see above).
        const TArray<TSharedPtr<FJsonValue>>* Items = UEAgentCommonImpl::FindNumberList(JsonObject, FieldName);
        if (Items != nullptr && Items->Num() >= 3)
        {
            Result = FRotator(UEAgentCommonImpl::FloatAt(*Items, 0), UEAgentCommonImpl::FloatAt(*Items, 1),
                              UEAgentCommonImpl::FloatAt(*Items, 2));
        }
        return Result;
    }

    // The write-side twin of ReadJsonRotator's object form. A response
    // that reports a rotation as a bare 3-number array hands the reader the same
    // ambiguity that produces a Roll/Yaw mix-up, and it does it at the exact
    // moment they are trying to verify the write -- so the evidence itself is
    // unreadable. Naming the axes costs three keys and removes the guess.
    void AddRotatorField(const TSharedPtr<FJsonObject>& Obj, const FString& FieldName, const FRotator& Rotator)
    {
        if (!Obj.IsValid())
        {
            return;
        }

        TSharedPtr<FJsonObject> RotatorObject = MakeShared<FJsonObject>();
        RotatorObject->SetNumberField(TEXT("roll"),  Rotator.Roll);
        RotatorObject->SetNumberField(TEXT("pitch"), Rotator.Pitch);
        RotatorObject->SetNumberField(TEXT("yaw"),   Rotator.Yaw);
        Obj->SetObjectField(FieldName, RotatorObject);
    }

    TSharedPtr<FJsonObject> LinearColorToJsonObject(const FLinearColor& Color)
    {
        static const TCHAR* const ChannelKeys[] = { TEXT("r"), TEXT("g"), TEXT("b"), TEXT("a") };
        const float Channels[] = { Color.R, Color.G, Color.B, Color.A };
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        for (int32 Index = 0; Index < UE_ARRAY_COUNT(Channels); ++Index)
        {
            Out->SetNumberField(ChannelKeys[Index], Channels[Index]);
        }
        return Out;
    }

    TSharedPtr<FJsonObject> VectorToJsonObject(const FVector& Vector)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        Out->SetNumberField(TEXT("x"), Vector.X);
        Out->SetNumberField(TEXT("y"), Vector.Y);
        Out->SetNumberField(TEXT("z"), Vector.Z);
        return Out;
    }

    TSharedPtr<FJsonObject> RotatorToJsonObject(const FRotator& Rotator)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        Out->SetNumberField(TEXT("pitch"), Rotator.Pitch);
        Out->SetNumberField(TEXT("yaw"), Rotator.Yaw);
        Out->SetNumberField(TEXT("roll"), Rotator.Roll);
        return Out;
    }

    TArray<TSharedPtr<FJsonValue>> StringsToJson(const TArray<FString>& Strings)
    {
        TArray<TSharedPtr<FJsonValue>> Out;
        Out.Reserve(Strings.Num());
        for (const FString& Value : Strings)
        {
            Out.Add(MakeShared<FJsonValueString>(Value));
        }
        return Out;
    }

    TArray<TSharedPtr<FJsonValue>> GameplayTagsToJson(const FGameplayTagContainer& Tags)
    {
        TArray<TSharedPtr<FJsonValue>> Out;
        for (const FGameplayTag& Tag : Tags)
        {
            Out.Add(MakeShared<FJsonValueString>(Tag.ToString()));
        }
        return Out;
    }

    FString InvalidPackageName(const FString& PackageName)
    {
        if (PackageName.Contains(TEXT("//")))
        {
            return FString::Printf(TEXT("Invalid asset path '%s': it contains '//'. Pass the folder without a trailing ")
                TEXT("slash, and the asset name without a path."), *PackageName);
        }
        // The package name becomes an FName, which holds fewer than NAME_SIZE characters: a folder and a name that join
        // to more would hit FName's checkf (UnrealNames.cpp) in CreatePackage or the load that resolves the path.
        const int32 PackageLength = FPackageName::ObjectPathToPackageName(PackageName).Len();
        if (PackageLength >= NAME_SIZE)
        {
            return FString::Printf(TEXT("Invalid asset path '%s...': its package name is %d characters long, and a package name is ")
                TEXT("an FName, which holds at most %d characters (the engine aborts the editor on a longer one). Use a shorter ")
                TEXT("folder or asset name."), *PackageName.Left(64), PackageLength, NAME_SIZE - 1);
        }
        // The objects inside the asset are named after its object path, and the transaction buffer makes an FName of the
        // full path of each one it records: their paths, not the package name, reach the limit first.
        FString AssetName;
        int32 Dot = INDEX_NONE;
        if (PackageName.FindChar(TEXT('.'), Dot))
        {
            AssetName = PackageName.Mid(Dot + 1);
            // A subobject path ("Asset:Sub", "Asset.Sub") is measured by its asset.
            int32 Cut = AssetName.Len();
            for (int32 Index = 0; Index < AssetName.Len(); ++Index)
            {
                if (AssetName[Index] == TEXT(':') || AssetName[Index] == TEXT('.'))
                {
                    Cut = Index;
                    break;
                }
            }
            AssetName.LeftInline(Cut);
        }
        else
        {
            AssetName = FPackageName::GetShortName(PackageName);
        }
        const int32 ObjectPathLength = PackageLength + 1 + AssetName.Len();
        if (ObjectPathLength + ObjectPathSubobjectMargin >= NAME_SIZE)
        {
            return FString::Printf(TEXT("Invalid asset path '%s...': its object path (package.asset) is %d characters long, and the ")
                TEXT("engine makes an FName of the path of each object it creates inside the asset, up to %d characters longer (a ")
                TEXT("state machine of an anim blueprint, an actor and its component in a World Partition map, a sequence's track and ")
                TEXT("section); an FName holds at most %d characters and the engine aborts the editor on a longer one. The longest ")
                TEXT("object path accepted is %d. Use a shorter folder or asset name."),
                *PackageName.Left(64), ObjectPathLength, ObjectPathSubobjectMargin, NAME_SIZE - 1,
                NAME_SIZE - 1 - ObjectPathSubobjectMargin);
        }
        if (ObjectPathLength + AssetName.Len() + NamedSubobjectPathMargin >= NAME_SIZE)
        {
            return FString::Printf(TEXT("Invalid asset path '%s...': its object path (package.asset) is %d characters long and its ")
                TEXT("asset name %d, and the engine names some objects inside an asset after the asset too ('<object path>:<asset>")
                TEXT("EditorOnlyData' in a material, '<object path>:PersistentLevel.<asset>...' for a map's level blueprint), up to the ")
                TEXT("asset name plus %d characters past the object path; an FName holds at most %d characters and the engine aborts ")
                TEXT("the editor on a longer one. With this asset name the longest object path accepted is %d. Use a shorter folder ")
                TEXT("or asset name."),
                *PackageName.Left(64), ObjectPathLength, AssetName.Len(), NamedSubobjectPathMargin, NAME_SIZE - 1,
                NAME_SIZE - 1 - NamedSubobjectPathMargin - AssetName.Len());
        }
        return FString();
    }

    TSharedPtr<FJsonObject> InvalidPackageNameReply(const FString& PackageName)
    {
        const FString Invalid = InvalidPackageName(PackageName);
        if (Invalid.IsEmpty())
        {
            return nullptr;
        }
        return MakeErrorReply(Invalid);
    }

    FString AssetNameConflict(const FString& PackageName, const FString& ObjectName, const UClass* ObjectClass)
    {
        if (FString Invalid = InvalidPackageName(PackageName); !Invalid.IsEmpty())
        {
            return Invalid;
        }
        if (!ObjectClass || ObjectName.IsEmpty())
        {
            return FString();
        }
        const FString ObjectPath = FPackageName::ObjectPathToPackageName(PackageName) + TEXT(".") + ObjectName;
        const auto Taken = [&ObjectPath, ObjectClass](const FString& ExistingClass)
        {
            return FString::Printf(TEXT("'%s' already exists as a %s, so a %s cannot be created with that name. ")
                TEXT("Choose another name, or delete or rename the existing asset first."),
                *ObjectPath, *ExistingClass, *ObjectClass->GetName());
        };

        if (const UObject* Existing = FindObject<UObject>(nullptr, *ObjectPath))
        {
            return Existing->GetClass()->IsChildOf(ObjectClass) ? FString() : Taken(Existing->GetClass()->GetName());
        }
        const FAssetData OnDisk = IAssetRegistry::GetChecked().GetAssetByObjectPath(FSoftObjectPath(ObjectPath), /*bIncludeOnlyOnDiskAssets*/ true);
        if (OnDisk.IsValid())
        {
            // A class that is not loaded (a Blueprint's, say) is compared by path.
            const UClass* DiskClass = OnDisk.GetClass();
            if (DiskClass ? !DiskClass->IsChildOf(ObjectClass) : OnDisk.AssetClassPath != ObjectClass->GetClassPathName())
            {
                return Taken(OnDisk.AssetClassPath.GetAssetName().ToString());
            }
        }
        return FString();
    }

    UPackage* CreateAssetPackage(const FString& PackageName, const FString& AssetName, const UClass* Class, FString& OutError)
    {
        OutError = AssetNameConflict(PackageName, AssetName, Class);
        if (!OutError.IsEmpty())
        {
            return nullptr;
        }
        UPackage* Package = CreatePackage(*PackageName);
        if (!Package)
        {
            OutError = FString::Printf(TEXT("Failed to create package: %s"), *PackageName);
        }
        return Package;
    }

    UObject* CreateOrLoadAsset(const FString& PackageName, UClass* Class, FString& OutError, bool* bOutCreated)
    {
        if (bOutCreated)
        {
            *bOutCreated = false;
        }
        const FString AssetName = FPackageName::GetShortName(PackageName);
        if (!Class || AssetName.IsEmpty() || FPackageName::GetLongPackagePath(PackageName).IsEmpty())
        {
            OutError = FString::Printf(TEXT("'%s' is not an asset path (/Game/Folder/AssetName)."), *PackageName);
            return nullptr;
        }
        OutError = AssetNameConflict(PackageName, AssetName, Class);
        if (!OutError.IsEmpty())
        {
            return nullptr;
        }

        const FString ObjectPath = PackageName + TEXT(".") + AssetName;
        if (UObject* Existing = FindObject<UObject>(nullptr, *ObjectPath))
        {
            return Existing;
        }
        if (FPackageName::DoesPackageExist(PackageName))
        {
            if (UObject* Loaded = StaticLoadObject(Class, nullptr, *ObjectPath))
            {
                return Loaded;
            }
        }

        UPackage* Package = CreatePackage(*PackageName);
        if (!Package)
        {
            OutError = FString::Printf(TEXT("Could not create the package '%s'."), *PackageName);
            return nullptr;
        }
        Package->FullyLoad();
        UObject* Asset = NewObject<UObject>(Package, Class, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
        FAssetRegistryModule::AssetCreated(Asset);
        Asset->MarkPackageDirty();
        if (bOutCreated)
        {
            *bOutCreated = true;
        }
        return Asset;
    }

    UBlueprint* CreateOrLoadBlueprint(UClass* ParentClass, const FString& PackagePath, const FString& Name,
        bool& bOutAlreadyExisted, FString& OutError)
    {
        bOutAlreadyExisted = false;
        const FString FullPath = FString::Printf(TEXT("%s/%s"), *PackagePath, *Name);
        OutError = InvalidPackageName(FullPath);
        if (!OutError.IsEmpty())
        {
            return nullptr;
        }

        if (UEditorAssetLibrary::DoesAssetExist(FullPath))
        {
            UBlueprint* Existing = LoadObject<UBlueprint>(nullptr, *FullPath);
            if (!Existing)
            {
                OutError = FString::Printf(TEXT("Asset exists but failed to load as Blueprint: %s"), *FullPath);
                return nullptr;
            }
            bOutAlreadyExisted = true;
            return Existing;
        }

        UPackage* Package = CreatePackage(*FullPath);
        if (!Package)
        {
            OutError = TEXT("Failed to create package");
            return nullptr;
        }

        UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
            ParentClass, Package, FName(*Name), BPTYPE_Normal,
            UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(), NAME_None);
        if (!Blueprint)
        {
            OutError = TEXT("FKismetEditorUtilities::CreateBlueprint returned null");
            return nullptr;
        }

        FAssetRegistryModule::AssetCreated(Blueprint);
        Blueprint->MarkPackageDirty();
        FKismetEditorUtilities::CompileBlueprint(Blueprint);
        if (!SaveAssetPackage(Package, Blueprint, OutError))
        {
            return nullptr;
        }
        return Blueprint;
    }

} // namespace mcp_common

namespace UEAgentCommonImpl
{
    TArray<TSharedPtr<FJsonValue>> NumberTriple(double First, double Second, double Third)
    {
        TArray<TSharedPtr<FJsonValue>> Triple;
        Triple.Reserve(3);
        for (const double Component : {First, Second, Third})
        {
            Triple.Add(MakeShared<FJsonValueNumber>(Component));
        }
        return Triple;
    }

    // The summary every actor listing shares: identity plus world placement, with rotation as the
    // [pitch, yaw, roll] array listings have always used.
    TSharedPtr<FJsonObject> DescribeActor(const AActor& Actor)
    {
        const FVector Where = Actor.GetActorLocation();
        const FRotator Facing = Actor.GetActorRotation();
        const FVector Size = Actor.GetActorScale3D();

        TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
        Summary->SetStringField(TEXT("name"), Actor.GetName());
        Summary->SetStringField(TEXT("class"), Actor.GetClass()->GetName());
        Summary->SetArrayField(TEXT("location"), NumberTriple(Where.X, Where.Y, Where.Z));
        Summary->SetArrayField(TEXT("rotation"), NumberTriple(Facing.Pitch, Facing.Yaw, Facing.Roll));
        Summary->SetArrayField(TEXT("scale"), NumberTriple(Size.X, Size.Y, Size.Z));
        return Summary;
    }
}

namespace mcp_common
{
    TSharedPtr<FJsonValue> DescribeActorValue(AActor* Actor)
    {
        if (Actor == nullptr)
        {
            return MakeShared<FJsonValueNull>();
        }
        return MakeShared<FJsonValueObject>(UEAgentCommonImpl::DescribeActor(*Actor));
    }

    // bDetailed is part of the signature callers use; the summary carries the same fields either way.
    TSharedPtr<FJsonObject> DescribeActorObject(AActor* Actor, bool bDetailed)
    {
        return Actor != nullptr ? UEAgentCommonImpl::DescribeActor(*Actor) : nullptr;
    }

    UBlueprint* ResolveBlueprint(const FString& BlueprintName)
    {
        return ResolveBlueprintByName(BlueprintName);
    }

    // A full asset path (/Game/... or /Engine/...) is honored as-is, never prefixed with
    // /Game/Blueprints/: that would produce "/Game/Blueprints//Game/Path/Asset.Asset",
    // which trips CreatePackage's double-slash check (UObjectGlobals.cpp,
    // fatal). Full paths are the unambiguous contract from create_anim_blueprint /
    // get_blueprint_info asset_path fields, so callers expect them to resolve.
    UBlueprint* ResolveBlueprintByName(const FString& BlueprintName)
    {
        if (BlueprintName.StartsWith(TEXT("/")))
        {
            UBlueprint* DirectBP = LoadObject<UBlueprint>(nullptr, *BlueprintName);
            if (DirectBP)
            {
                return DirectBP;
            }
            // Fallback: AssetRegistry can resolve the package even when LoadObject needs
            // the ".AssetName" suffix the caller may not have appended.
            FAssetData DirectAD = mcp_common::FindAssetDataByPath(BlueprintName);
            if (DirectAD.IsValid())
            {
                return Cast<UBlueprint>(DirectAD.GetAsset());
            }
            UE_LOG(LogTemp, Error, TEXT("ResolveBlueprintByName: Failed to find or load blueprint at absolute path: %s"), *BlueprintName);
            return nullptr;
        }

        // A bare name follows the /Game/Blueprints convention, where the object path repeats the
        // asset name. Three ways to reach it there, cheapest first: load it, ask the registry (it
        // knows assets created this session), then look for a package that only lives in memory.
        const FString Conventional = FString::Printf(TEXT("/Game/Blueprints/%s.%s"), *BlueprintName, *BlueprintName);
        if (UBlueprint* Loaded = LoadObject<UBlueprint>(nullptr, *Conventional))
        {
            return Loaded;
        }

        IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
        if (const FAssetData Registered = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(Conventional)); Registered.IsValid())
        {
            if (UBlueprint* FromRegistry = Cast<UBlueprint>(Registered.GetAsset()))
            {
                return FromRegistry;
            }
        }

        if (UBlueprint* InMemory = FindObject<UBlueprint>(nullptr, *(FString(TEXT("/Game/Blueprints/")) + BlueprintName)))
        {
            return InMemory;
        }

        // ADDITIVE FALLBACK: project-wide short-name scan. Reached ONLY when the bare name did NOT
        // resolve under /Game/Blueprints, so a lookup that resolves there keeps its result.
        // Mirrors analysis::LoadBlueprintFlexible: recursive over /Game, recursive UBlueprint
        // subclasses (AnimBP / WidgetBP / EditorUtilityBP all derive from UBlueprint -- exactly what
        // the LoadObject<UBlueprint> fast paths above already accepted).
        {
            FARFilter Filter;
            Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
            Filter.bRecursiveClasses = true;
            Filter.bRecursivePaths = true;
            Filter.PackagePaths.Add(TEXT("/Game"));

            TArray<FAssetData> Candidates;
            AssetRegistry.GetAssets(Filter, Candidates);

            TArray<FAssetData> Matches;
            for (const FAssetData& AD : Candidates)
            {
                if (AD.AssetName.ToString() == BlueprintName)
                {
                    Matches.Add(AD);
                }
            }

            if (Matches.Num() > 0)
            {
                if (Matches.Num() > 1)
                {
                    // The return type can't carry an "ambiguous" signal and every caller expects a bare
                    // UBlueprint*, so pick DETERMINISTICALLY (lexically-first package) and warn loudly.
                    Matches.Sort([](const FAssetData& A, const FAssetData& B)
                    {
                        return A.PackageName.ToString() < B.PackageName.ToString();
                    });
                    FString CandidateList;
                    for (const FAssetData& AD : Matches)
                    {
                        CandidateList += TEXT("\n    ") + AD.PackageName.ToString();
                    }
                    UE_LOG(LogTemp, Warning,
                        TEXT("ResolveBlueprintByName: short name '%s' is AMBIGUOUS (%d matches); returning '%s'. ")
                        TEXT("Pass a full /Game path to disambiguate. Candidates:%s"),
                        *BlueprintName, Matches.Num(), *Matches[0].PackageName.ToString(), *CandidateList);
                }

                if (UBlueprint* Blueprint = Cast<UBlueprint>(Matches[0].GetAsset()))
                {
                    return Blueprint;
                }
            }
        }

        UE_LOG(LogTemp, Error, TEXT("ResolveBlueprintByName: no Blueprint resolves from '%s'"), *BlueprintName);
        return nullptr;
    }

    UEdGraph* FindOrCreateEventGraph(UBlueprint* Blueprint)
    {
        if (Blueprint == nullptr)
        {
            return nullptr;
        }

        // The first ubergraph page that carries the default name, a renamed copy ("EventGraph_1") included. A
        // Blueprint without one gets it the way FKismetEditorUtilities::CreateBlueprint makes it.
        const FString DefaultName = UEdGraphSchema_K2::GN_EventGraph.ToString();
        const TObjectPtr<UEdGraph>* Existing = Blueprint->UbergraphPages.FindByPredicate(
            [&DefaultName](const TObjectPtr<UEdGraph>& Page) { return Page && Page->GetName().Contains(DefaultName); });
        if (Existing != nullptr)
        {
            return *Existing;
        }
        UEdGraph* Created = FBlueprintEditorUtils::CreateNewGraph(
            Blueprint, UEdGraphSchema_K2::GN_EventGraph, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
        FBlueprintEditorUtils::AddUbergraphPage(Blueprint, Created);
        return Created;
    }

    UK2Node_Event* SpawnEventNode(UEdGraph* Graph, const FString& EventName, const FVector2D& Position)
    {
        UBlueprint* Blueprint = Graph ? FBlueprintEditorUtils::FindBlueprintForGraph(Graph) : nullptr;
        if (Blueprint == nullptr)
        {
            return nullptr;
        }

        const FName EventFName(*EventName);

        // Make sure interface event stubs exist / are resolvable before we look them up (idempotent).
        FBlueprintEditorUtils::ConformImplementedInterfaces(Blueprint);

        // Resolve the overridable function AND the class that DECLARES it (an implemented interface OR a parent
        // in the hierarchy): an event node must reference the DECLARING class with bOverrideFunction=true.
        // Referencing this blueprint's OWN generated class with bOverrideFunction=false would, for an
        // interface event, produce a dangling custom event that the real interface stub eclipses -- it
        // compiles clean but never runs (Execute_<Iface> runs the empty stub, not the user's graph).
        // Mirrors UBlueprintEditorLibrary::AddEventOverride / the editor's "Add Event" action.
        UFunction* OverrideFunc = nullptr;
        UClass* OverrideFuncClass = FBlueprintEditorUtils::GetOverrideFunctionClass(Blueprint, EventFName, &OverrideFunc);
        if (!OverrideFunc || !OverrideFuncClass)
        {
            UE_LOG(LogTemp, Error,
                TEXT("SpawnEventNode: '%s' is not an overridable event on '%s' (use custom_event for a new event, or add_blueprint_function with override=true for a function with a return value)"),
                *EventName, *Blueprint->GetName());
            return nullptr;
        }

        // Event-shape only: a function with a return value / outputs cannot be an event node. Fail clearly rather
        // than leaving a broken node behind -- such a function belongs in a function graph (add_blueprint_function with override=true).
        if (!UEdGraphSchema_K2::FunctionCanBePlacedAsEvent(OverrideFunc))
        {
            UE_LOG(LogTemp, Error,
                TEXT("SpawnEventNode: '%s' has a return value / outputs and cannot be an event; use add_blueprint_function with override=true"),
                *EventName);
            return nullptr;
        }

        // Idempotent: an override event for a given function can only exist once per blueprint. This also catches
        // an existing node on another ubergraph page (returning it beats spawning a duplicate the compiler rejects).
        if (UK2Node_Event* Existing = FBlueprintEditorUtils::FindOverrideForFunction(Blueprint, OverrideFuncClass, EventFName))
        {
            UE_LOG(LogTemp, Verbose, TEXT("SpawnEventNode: '%s' already has its override event (%s)"),
                *EventName, *Existing->NodeGuid.ToString());
            return Existing;
        }

        FGraphNodeCreator<UK2Node_Event> Creator(*Graph);
        UK2Node_Event* EventNode = Creator.CreateNode();
        EventNode->EventReference.SetExternalMember(EventFName, OverrideFuncClass);
        EventNode->bOverrideFunction = true;
        EventNode->NodePosX = static_cast<int32>(Position.X);
        EventNode->NodePosY = static_cast<int32>(Position.Y);
        Creator.Finalize(); // allocates default pins, runs PostPlacedNewNode, assigns a fresh NodeGuid
        UE_LOG(LogTemp, Verbose, TEXT("SpawnEventNode: placed the override event '%s' of '%s' (%s)"),
            *EventName, *OverrideFuncClass->GetName(), *EventNode->NodeGuid.ToString());
        return EventNode;
    }

} // namespace mcp_common

namespace UEAgentCommonImpl
{
    // The editor's own order for a node created outside the palette: position it, hand it to the
    // graph as a user action, give it an addressable GUID, let it react to being placed, then build
    // its pins. Returns the node so the creators below can stay one expression long.
    template <typename NodeType>
    NodeType* PlaceNewNode(UEdGraph& Graph, NodeType* Node, const FVector2D& Position)
    {
        Node->NodePosX = static_cast<int32>(Position.X);
        Node->NodePosY = static_cast<int32>(Position.Y);
        Graph.AddNode(Node, true);
        Node->CreateNewGuid();
        Node->PostPlacedNewNode();
        Node->AllocateDefaultPins();
        return Node;
    }
}

namespace mcp_common
{
    UK2Node_Self* SpawnSelfNode(UEdGraph* Graph, const FVector2D& Position)
    {
        return Graph != nullptr
            ? UEAgentCommonImpl::PlaceNewNode(*Graph, NewObject<UK2Node_Self>(Graph), Position)
            : nullptr;
    }

    UK2Node_CallFunction* CreateFunctionCallNode(UEdGraph* Graph, UFunction* Function, const FVector2D& Position)
    {
        if (Graph == nullptr || Function == nullptr)
        {
            return nullptr;
        }

        // Mirror UBlueprintFunctionNodeSpawner's node-class selection: array-library functions
        // (Array_Add / Array_Append / Array_Contains / ... -- tagged MD_ArrayParam) MUST spawn as
        // UK2Node_CallArrayFunction, and DataTable functions (MD_DataTablePin, e.g. GetDataTableRow)
        // as UK2Node_CallDataTableFunction -- NOT a plain UK2Node_CallFunction. All three expose the
        // wildcard pins the metadata implies (TargetArray/NewItem, OutRow), but ONLY the subclass
        // carries the type-propagation (NotifyPinConnectionListChanged -> PropagateArrayTypeInfo). On a
        // plain CallFunction those pins stay PC_Wildcard after a connect_pins -> compile blocks with
        // "The type of Target Array/New Item is undetermined", and NO ReconstructNode can fix it (the
        // propagation logic simply is not on that class). Picking the right subclass makes headless
        // array-append / datatable-row authoring behave exactly like the editor palette.
        UClass* NodeClass = UK2Node_CallFunction::StaticClass();
        if (Function->HasMetaData(FBlueprintMetadata::MD_DataTablePin))
        {
            NodeClass = UK2Node_CallDataTableFunction::StaticClass();
        }
        else if (Function->HasMetaData(FBlueprintMetadata::MD_ArrayParam))
        {
            NodeClass = UK2Node_CallArrayFunction::StaticClass();
        }

        UK2Node_CallFunction* CallNode = NewObject<UK2Node_CallFunction>(Graph, NodeClass);
        CallNode->SetFromFunction(Function); // before placement: the pins are built from it
        return UEAgentCommonImpl::PlaceNewNode(*Graph, CallNode, Position);
    }


    // Shared resolver for CreateVariableGet/SetNode.
    // Configures the VariableReference on `Node` (the common UK2Node_Variable base of
    // both the get- and set-variant) from one of three sources, chosen by `Scope`:
    //   local  : a function-local variable declared in this graph's function.
    //   member : a member on the blueprint (self) OR on the supplied TargetClass.
    //   auto   : local wins first (UE shadowing) when the graph is a function graph
    //            that declares it, else self member, else -- only when TargetClass is
    //            given -- a non-self member on TargetClass.
    // Returns true when configured (caller then adds the node + CreateNewGuid). On
    // not-found returns false and fills OutError with an actionable message; the
    // caller discards the node WITHOUT adding it (so no zero-GUID node is left).
    //
    // Unique file-scope name (not an anonymous-namespace helper) to stay ODR-safe
    // under -DisableAdaptiveUnity.
    static bool ConfigureMcpVariableNodeReference(
        UK2Node_Variable* Node, UEdGraph* Graph, UBlueprint* Blueprint,
        const FName VarName, UClass* TargetClass, const FString& Scope, FString& OutError)
    {
        const FString S = Scope.IsEmpty() ? TEXT("auto") : Scope.ToLower();

        // --- self member (GeneratedClass -> Skeleton -> not-yet-compiled NewVariableIndex) ---
        auto TrySelfMember = [&]() -> bool
        {
            FProperty* Property = FindFProperty<FProperty>(Blueprint->GeneratedClass, VarName);
            if (!Property && Blueprint->SkeletonGeneratedClass)
            {
                Property = FindFProperty<FProperty>(Blueprint->SkeletonGeneratedClass, VarName);
            }
            if (Property)
            {
                // Self-context member var: SetFromProperty(bSelfContext=true) points/hides the node's
                // `self` pin. A NOT-self reference here would leave a visible, unwired self pin ->
                // compile purges it ("Variable node uses an invalid target ... got purged"). Silent:
                // the compile still succeeds; only compile_blueprint's messages (return_messages) flag it.
                Node->SetFromProperty(Property, /*bSelfContext=*/true, Property->GetOwnerClass());
                return true;
            }
            if (FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, VarName) != INDEX_NONE)
            {
                // Declared on the blueprint but not yet compiled into the generated class.
                Node->VariableReference.SetSelfMember(VarName);
                return true;
            }
            return false;
        };

        // --- non-self member on an explicit TargetClass (member-access "target" pin) ---
        auto TryTargetMember = [&]() -> bool
        {
            if (!TargetClass) { return false; }
            FProperty* Property = FindFProperty<FProperty>(TargetClass, VarName);
            if (Property)
            {
                // bSelfContext=false leaves a typed member-access target pin; OwnerClass is the class
                // that actually declares the property (may be a super of TargetClass).
                Node->SetFromProperty(Property, /*bSelfContext=*/false, Property->GetOwnerClass());
                return true;
            }
            return false;
        };

        // --- function-local variable (nothing on an event/ubergraph: FindLocalVariable gates on
        //     DoesSupportLocalVariables + resolves the top-level function graph internally) ---
        auto TryLocal = [&]() -> bool
        {
            FBPVariableDescription* LocalVar = FBlueprintEditorUtils::FindLocalVariable(Blueprint, Graph, VarName);
            if (LocalVar)
            {
                UEdGraph* TopGraph = FBlueprintEditorUtils::GetTopLevelGraph(Graph);
                const FString ScopeName = TopGraph ? TopGraph->GetName() : Graph->GetName();
                // Engine-canonical local-var wiring (BlueprintVariableNodeSpawner / K2Node_LocalVariable):
                // reference by name + owning function-graph name + the local's stable VarGuid.
                Node->VariableReference.SetLocalMember(VarName, ScopeName, LocalVar->VarGuid);
                return true;
            }
            return false;
        };

        if (S == TEXT("local"))
        {
            if (TryLocal()) { return true; }
            OutError = FString::Printf(
                TEXT("Local variable '%s' not found in this function graph (scope='local'). ")
                TEXT("Declare it with add_function_local_variable, or drop scope to fall back to member variables."),
                *VarName.ToString());
            return false;
        }

        if (S == TEXT("member"))
        {
            if (TargetClass) { if (TryTargetMember()) { return true; } }
            if (TrySelfMember()) { return true; }
            OutError = TargetClass
                ? FString::Printf(TEXT("Variable '%s' not found on target_class '%s' nor as a member of the blueprint (scope='member')."),
                    *VarName.ToString(), *TargetClass->GetName())
                : FString::Printf(TEXT("Member variable '%s' not found on the blueprint (scope='member'). ")
                    TEXT("Declare it with add_blueprint_variable, or pass target_class to reference another class's member."),
                    *VarName.ToString());
            return false;
        }

        // auto (default): local shadows member; self is the default member; TargetClass is the
        // fallback for accessing another class's member.
        if (TryLocal()) { return true; }
        if (TrySelfMember()) { return true; }
        if (TryTargetMember()) { return true; }
        OutError = FString::Printf(
            TEXT("Variable '%s' not found: not a local variable of this graph, not a member of the blueprint%s. ")
            TEXT("Declare it (add_blueprint_variable / add_function_local_variable), or pass target_class for another class's member."),
            *VarName.ToString(),
            TargetClass ? *FString::Printf(TEXT(", and not a member of target_class '%s'"), *TargetClass->GetName()) : TEXT(""));
        return false;
    }

    UK2Node_VariableGet* SpawnVariableGetNode(UEdGraph* Graph, UBlueprint* Blueprint, const FString& VariableName, const FVector2D& Position,
        UClass* TargetClass, const FString& Scope, FString* OutError)
    {
        if (!Graph || !Blueprint)
        {
            if (OutError) { *OutError = TEXT("Invalid graph or blueprint"); }
            return nullptr;
        }

        UK2Node_VariableGet* VariableGetNode = NewObject<UK2Node_VariableGet>(Graph);

        FString LocalError;
        if (!ConfigureMcpVariableNodeReference(VariableGetNode, Graph, Blueprint, FName(*VariableName), TargetClass, Scope, LocalError))
        {
            if (OutError) { *OutError = LocalError; }
            return nullptr; // node never added -> no zero-GUID node left behind
        }

        return UEAgentCommonImpl::PlaceNewNode(*Graph, VariableGetNode, Position);
    }

    UK2Node_VariableSet* SpawnVariableSetNode(UEdGraph* Graph, UBlueprint* Blueprint, const FString& VariableName, const FVector2D& Position,
        UClass* TargetClass, const FString& Scope, FString* OutError)
    {
        if (!Graph || !Blueprint)
        {
            if (OutError) { *OutError = TEXT("Invalid graph or blueprint"); }
            return nullptr;
        }

        UK2Node_VariableSet* VariableSetNode = NewObject<UK2Node_VariableSet>(Graph);

        FString LocalError;
        if (!ConfigureMcpVariableNodeReference(VariableSetNode, Graph, Blueprint, FName(*VariableName), TargetClass, Scope, LocalError))
        {
            if (OutError) { *OutError = LocalError; }
            return nullptr; // node never added -> no zero-GUID node left behind
        }

        return UEAgentCommonImpl::PlaceNewNode(*Graph, VariableSetNode, Position);
    }




    // Canonical typed import: a single route for every
    // FProperty kind. FJsonObjectConverter::JsonValueToUProperty writes
    // scalars, enums (by name or value), structs, arrays, sets and
    // maps from nested JSON straight onto the value pointer and
    // reports a precise reason on failure.
    // A JSON string where a number or a bool is expected. FJsonObjectConverter reads it with AsNumber / AsBool, which
    // turn text they cannot parse into 0 or false without an error ("abc" -> 0, "12abc" -> 12, "maybe" -> false).
    // Walks structs, arrays, sets and map values the way the converter does and refuses such a string, before anything
    // is written; JSON numbers and bools, and enum names, pass untouched.
    static bool ValidateScalarStringsInJson(const FProperty* Property, const TSharedPtr<FJsonValue>& Value, const FString& Path,
        FString& OutError)
    {
        if (!Property || !Value.IsValid() || Value->Type == EJson::Null)
        {
            return true;
        }
        if (const FNumericProperty* Numeric = CastField<FNumericProperty>(Property))
        {
            if (Numeric->IsEnum() || Value->Type != EJson::String)
            {
                return true;
            }
            // The whole text must be a number: [sign] digits, and for a decimal type an optional fraction, exponent
            // and trailing f.
            const FString Text = Value->AsString().TrimStartAndEnd();
            const bool bDecimal = Numeric->IsFloatingPoint();
            int32 At = 0;
            const auto Digits = [&Text, &At]()
            {
                const int32 From = At;
                while (At < Text.Len() && FChar::IsDigit(Text[At])) { ++At; }
                return At - From;
            };
            if (At < Text.Len() && (Text[At] == TCHAR('+') || Text[At] == TCHAR('-'))) { ++At; }
            int32 Mantissa = Digits();
            if (bDecimal && At < Text.Len() && Text[At] == TCHAR('.')) { ++At; Mantissa += Digits(); }
            bool bNumber = Mantissa > 0;
            if (bNumber && bDecimal && At < Text.Len() && (Text[At] == TCHAR('e') || Text[At] == TCHAR('E')))
            {
                ++At;
                if (At < Text.Len() && (Text[At] == TCHAR('+') || Text[At] == TCHAR('-'))) { ++At; }
                bNumber = Digits() > 0;
            }
            if (bNumber && bDecimal && At < Text.Len() && (Text[At] == TCHAR('f') || Text[At] == TCHAR('F'))) { ++At; }
            if (!bNumber || At != Text.Len())
            {
                OutError = FString::Printf(TEXT("'%s' is not a %s number (%s is %s)"), *Text,
                    Numeric->IsFloatingPoint() ? TEXT("decimal") : TEXT("whole"), *Path, *Property->GetCPPType());
                return false;
            }
            return true;
        }
        if (CastField<FBoolProperty>(Property))
        {
            if (Value->Type != EJson::String)
            {
                return true;
            }
            const FString Text = Value->AsString().TrimStartAndEnd();
            for (const TCHAR* Word : { TEXT("true"), TEXT("false"), TEXT("1"), TEXT("0"), TEXT("yes"), TEXT("no"), TEXT("on"), TEXT("off") })
            {
                if (Text.Equals(Word, ESearchCase::IgnoreCase))
                {
                    return true;
                }
            }
            OutError = FString::Printf(TEXT("'%s' is not a bool (%s): use true or false"), *Text, *Path);
            return false;
        }
        if (const FStructProperty* StructProp = CastField<FStructProperty>(Property))
        {
            const TSharedPtr<FJsonObject>* Fields = nullptr;
            if (!Value->TryGetObject(Fields) || !Fields || !Fields->IsValid())
            {
                return true;
            }
            for (TFieldIterator<FProperty> It(StructProp->Struct); It; ++It)
            {
                // FJsonObject keys compare without case, as the converter's field lookup does.
                const TSharedPtr<FJsonValue> Field = (*Fields)->TryGetField(It->GetName());
                if (Field.IsValid() && !ValidateScalarStringsInJson(*It, Field, Path + TEXT(".") + It->GetName(), OutError))
                {
                    return false;
                }
            }
            return true;
        }
        const FProperty* Element = nullptr;
        if (const FArrayProperty* ArrayProp = CastField<FArrayProperty>(Property)) { Element = ArrayProp->Inner; }
        else if (const FSetProperty* SetProp = CastField<FSetProperty>(Property)) { Element = SetProp->ElementProp; }
        if (Element)
        {
            const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
            if (!Value->TryGetArray(Items) || !Items)
            {
                return true;
            }
            for (int32 Index = 0; Index < Items->Num(); ++Index)
            {
                if (!ValidateScalarStringsInJson(Element, (*Items)[Index], FString::Printf(TEXT("%s[%d]"), *Path, Index), OutError))
                {
                    return false;
                }
            }
            return true;
        }
        if (const FMapProperty* MapProp = CastField<FMapProperty>(Property))
        {
            const TSharedPtr<FJsonObject>* Entries = nullptr;
            if (!Value->TryGetObject(Entries) || !Entries || !Entries->IsValid())
            {
                return true;
            }
            for (const auto& Entry : (*Entries)->Values)
            {
                if (!ValidateScalarStringsInJson(MapProp->ValueProp, Entry.Value, FString::Printf(TEXT("%s[%s]"), *Path, *Entry.Key), OutError))
                {
                    return false;
                }
            }
        }
        return true;
    }

    TSharedPtr<FJsonObject> MakeRequiresEngineReply(const TCHAR* Tool, const TCHAR* MinVersion, const TCHAR* Why)
    {
        return MakeErrorReply(FString::Printf(
            TEXT("%s needs Unreal Engine %s or later (%s); this editor runs Unreal Engine %d.%d. Nothing was changed."),
            Tool, MinVersion, Why, ENGINE_MAJOR_VERSION, ENGINE_MINOR_VERSION));
    }

    bool TransactionHoldsPieSessionObjects(const FTransaction* Transaction)
    {
        if (!Transaction)
        {
            return false;
        }
        TArray<UObject*> Objects;
        Transaction->GetTransactionObjects(Objects);
        for (const UObject* Object : Objects)
        {
            for (const UObject* Outer = Object; Outer; Outer = Outer->GetOuter())
            {
                if (Outer->GetOutermost()->HasAnyPackageFlags(PKG_PlayInEditor))
                {
                    return true;
                }
                const UGameInstance* GameInstance = Cast<UGameInstance>(Outer);
                const FWorldContext* Context = GameInstance ? GameInstance->GetWorldContext() : nullptr;
                if (Context && Context->WorldType == EWorldType::PIE)
                {
                    return true;
                }
            }
        }
        return false;
    }

    bool ResetUndoHistoryHoldingPieSession()
    {
        UTransactor* Trans = GEditor ? GEditor->Trans.Get() : nullptr;
        if (!Trans)
        {
            return false;
        }
        for (int32 Index = 0; Index < Trans->GetQueueLength(); ++Index)
        {
            if (TransactionHoldsPieSessionObjects(Trans->GetTransaction(Index)))
            {
                GEditor->ResetTransaction(NSLOCTEXT("UEAgent", "PieSessionInUndoHistory",
                    "An object of the ending Play-in-Editor session was in the undo history"));
                return true;
            }
        }
        return false;
    }

    bool ImportPropertyTextStrict(const FProperty* Property, const FString& Text, void* ValuePtr, UObject* Owner,
        FString& OutError)
    {
        if (!Property || !ValuePtr)
        {
            OutError = TEXT("null property or value pointer");
            return false;
        }
        const FString Trimmed = Text.TrimStartAndEnd();
        if (Trimmed.IsEmpty() && (Property->IsA<FNumericProperty>() || Property->IsA<FBoolProperty>()))
        {
            OutError = FString::Printf(TEXT("an empty value is not a %s"), *Property->GetCPPType());
            return false;
        }

        void* Scratch = Property->AllocateAndInitializeValue();
        Property->CopyCompleteValue(Scratch, ValuePtr);
        FStringOutputDevice ParserErrors;
        const TCHAR* End = Property->ImportText_Direct(*Trimmed, Scratch, Owner, PPF_None, &ParserErrors);
        bool bImported = End != nullptr;
        if (!bImported)
        {
            OutError = ParserErrors.IsEmpty()
                ? FString::Printf(TEXT("'%s' is not a valid %s"), *Trimmed, *Property->GetCPPType())
                : FString::Printf(TEXT("'%s' is not a valid %s: %s"), *Trimmed, *Property->GetCPPType(), *ParserErrors);
        }
        else
        {
            while (FChar::IsWhitespace(*End))
            {
                ++End;
            }
            if (*End != TCHAR('\0'))
            {
                bImported = false;
                const int32 Read = UE_PTRDIFF_TO_INT32(End - *Trimmed);
                OutError = Read == 0
                    ? FString::Printf(TEXT("'%s' is not a valid %s"), *Trimmed, *Property->GetCPPType())
                    : FString::Printf(TEXT("'%s' is not a valid %s: only '%s' could be read"), *Trimmed,
                        *Property->GetCPPType(), *Trimmed.Left(Read));
            }
        }
        if (bImported)
        {
            Property->CopyCompleteValue(ValuePtr, Scratch);
        }
        Property->DestroyAndFreeValue(Scratch);
        return bImported;
    }

    bool SetObjectProperty(UObject* Object, const FString& PropertyName,
                           const TSharedPtr<FJsonValue>& Value, FString& OutErrorMessage)
    {
        // Resolve the target slot first; both ways of missing it are reported, never guessed.
        FProperty* Property = (Object != nullptr) ? Object->GetClass()->FindPropertyByName(*PropertyName) : nullptr;
        if (Property == nullptr)
        {
            OutErrorMessage = (Object == nullptr)
                ? FString(TEXT("Invalid object"))
                : FString::Printf(TEXT("Property not found: %s"), *PropertyName);
            return false;
        }
        void* PropertyAddr = Property->ContainerPtrToValuePtr<void>(Object);

        // Ergonomic sugar: a FGameplayTagContainer property accepts a simple
        // JSON array of tag-name strings (["A.B","C.D"]) -- the verbose struct form that
        // JsonValueToUProperty expects is gnarly for callers to produce. Every other type
        // (scalars, enums, structs, struct-arrays, maps) falls through to the canonical
        // JsonValueToUProperty path below.
        if (Value.IsValid() && Value->Type == EJson::Array)
        {
            if (const FStructProperty* StructProp = CastField<FStructProperty>(Property))
            {
                if (StructProp->Struct == FGameplayTagContainer::StaticStruct())
                {
                    // Every name must be a registered tag, as for a single FGameplayTag below: one that is not is
                    // refused with the container untouched, instead of being left out of it.
                    FGameplayTagContainer Container;
                    TArray<FString> Unregistered;
                    for (const TSharedPtr<FJsonValue>& Entry : Value->AsArray())
                    {
                        const FString TagName = Entry.IsValid() ? Entry->AsString().TrimStartAndEnd() : FString();
                        if (TagName.IsEmpty()) { continue; }
                        const FGameplayTag Tag = FGameplayTag::RequestGameplayTag(FName(*TagName), /*ErrorIfNotFound=*/ false);
                        if (Tag.IsValid()) { Container.AddTag(Tag); }
                        else { Unregistered.Add(TagName); }
                    }
                    if (Unregistered.Num() > 0)
                    {
                        OutErrorMessage = FString::Printf(
                            TEXT("Unregistered gameplay tag(s) %s for property '%s' (not in the tag registry). ")
                            TEXT("Register them (create_gameplay_tag / a tag table) or fix the spelling; nothing was written."),
                            *FString::Join(Unregistered, TEXT(", ")), *PropertyName);
                        return false;
                    }
                    *static_cast<FGameplayTagContainer*>(PropertyAddr) = MoveTemp(Container);
                    return true;
                }
            }
        }

        // A single FGameplayTag property is VALIDATED (mirror of the container
        // branch above). Accept EITHER a bare string ("A.B.C") OR the struct form
        // {"TagName":"A.B.C"}. JsonValueToUProperty would otherwise write the tag's internal
        // FName WITHOUT registry validation (a typo'd/unregistered tag is dead-set silently),
        // and a bare tag string does not reliably construct the tag. Resolve through the tag
        // registry and refuse to dead-set an unregistered tag. FGameplayTagContainer is not
        // handled here (array branch above / JsonValueToUProperty below).
        if (const FStructProperty* TagProp = CastField<FStructProperty>(Property))
        {
            if (TagProp->Struct == FGameplayTag::StaticStruct())
            {
                FString TagName;
                if (Value.IsValid() && Value->Type == EJson::String)
                {
                    TagName = Value->AsString();
                }
                else if (Value.IsValid() && Value->Type == EJson::Object)
                {
                    const TSharedPtr<FJsonObject> TagObj = Value->AsObject();
                    if (TagObj.IsValid()) { TagObj->TryGetStringField(TEXT("TagName"), TagName); }
                }
                TagName.TrimStartAndEndInline();

                if (TagName.IsEmpty())
                {
                    // Empty string / empty struct clears the tag to an (empty, valid) FGameplayTag.
                    *static_cast<FGameplayTag*>(PropertyAddr) = FGameplayTag();
                    return true;
                }

                const FGameplayTag Tag = FGameplayTag::RequestGameplayTag(FName(*TagName), /*ErrorIfNotFound=*/ false);
                if (!Tag.IsValid())
                {
                    OutErrorMessage = FString::Printf(
                        TEXT("Unregistered gameplay tag '%s' for property '%s' (not in the tag registry). ")
                        TEXT("Register it (create_gameplay_tag / a tag table) or fix the spelling; refusing to dead-set an invalid tag."),
                        *TagName, *PropertyName);
                    return false;
                }
                *static_cast<FGameplayTag*>(PropertyAddr) = Tag;
                return true;
            }
        }

        // RECURSIVE tag guardrail. The two branches above only protect a
        // TOP-LEVEL FGameplayTag / FGameplayTagContainer; a tag nested one level
        // down -- inside a struct, or inside a TArray<FMyStruct> -- would fall
        // through to JsonValueToUProperty, which writes the tag's internal FName
        // WITHOUT consulting the registry, dead-setting a typo'd tag in silence.
        // Validate the whole reachable tree before writing anything; refuse as a
        // unit rather than leaving a half-written value behind.
        {
            FString TagError;
            int32 TagsChecked = 0;
            if (!ValidateGameplayTagsInJson(Property, Value, TagError, &TagsChecked))
            {
                OutErrorMessage = FString::Printf(TEXT("Failed to set '%s': %s"), *PropertyName, *TagError);
                return false;
            }
        }
        {
            FString ScalarError;
            if (!ValidateScalarStringsInJson(Property, Value, PropertyName, ScalarError))
            {
                OutErrorMessage = FString::Printf(TEXT("Failed to set '%s': %s; nothing was written"), *PropertyName, *ScalarError);
                return false;
            }
        }

        FText FailReason;
        if (!FJsonObjectConverter::JsonValueToUProperty(Value, Property, PropertyAddr, /*CheckFlags=*/0, /*SkipFlags=*/0, /*bStrictMode=*/false, &FailReason))
        {
            OutErrorMessage = FString::Printf(TEXT("Failed to set '%s' (type %s): %s"),
                *PropertyName, *Property->GetClass()->GetName(), *FailReason.ToString());
            return false;
        }

        return true;
    }

    UWorld* GetEditorWorld()
    {
        return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    }

    TSharedPtr<FJsonObject> RequireEditorWorld(UWorld*& OutWorld, const TCHAR* Message)
    {
        OutWorld = GetEditorWorld();
        if (OutWorld)
        {
            return nullptr;
        }
        return MakeErrorReply(Message);
    }

    UWorld* GetFirstContextWorld()
    {
        if (GEngine == nullptr || GEngine->GetWorldContexts().Num() == 0)
        {
            return nullptr;
        }
        return GEngine->GetWorldContexts()[0].World();
    }

    bool FindPackageFileOnDisk(const FString& PackageOrObjectPath, FString& OutFile)
    {
        OutFile.Reset();
        const FString PackageName = FPackageName::ObjectPathToPackageName(PackageOrObjectPath);
        if (!FPackageName::IsValidLongPackageName(PackageName))
        {
            return false;
        }
        return FPackageName::DoesPackageExist(PackageName, &OutFile);
    }

    FAssetData FindAssetDataByPath(const FString& PackageOrObjectPath)
    {
        if (PackageOrObjectPath.IsEmpty())
        {
            return FAssetData();
        }
        IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
        const FTopLevelAssetPath PackageClassPath = UPackage::StaticClass()->GetClassPathName();
        const bool bPackagePath = !PackageOrObjectPath.Contains(TEXT(".")) && FPackageName::IsValidLongPackageName(PackageOrObjectPath);
        const FString ObjectPath = bPackagePath
            ? PackageOrObjectPath + TEXT(".") + FPackageName::GetLongPackageAssetName(PackageOrObjectPath)
            : PackageOrObjectPath;
        const FAssetData Direct = Registry.GetAssetByObjectPath(FSoftObjectPath(ObjectPath));
        if (Direct.IsValid() && Direct.AssetClassPath != PackageClassPath)
        {
            return Direct;
        }
        if (bPackagePath)
        {
            TArray<FAssetData> InPackage;
            Registry.GetAssetsByPackageName(FName(*PackageOrObjectPath), InPackage);
            for (const FAssetData& Candidate : InPackage)
            {
                if (Candidate.AssetClassPath != PackageClassPath)
                {
                    return Candidate;
                }
            }
        }
        return FAssetData();
    }

    // See header doc. GEngine (not GEditor) is the entry point so this
    // also resolves correctly in cooked/packaged builds, where GEditor is
    // always null but GEngine and its world contexts still exist.
    UWorld* GetActiveWorld()
    {
        UWorld* GameWorld = nullptr;
        if (GEngine)
        {
            for (const FWorldContext& Context : GEngine->GetWorldContexts())
            {
                if (Context.WorldType == EWorldType::PIE && Context.World())
                {
                    return Context.World();
                }
                if (!GameWorld && Context.WorldType == EWorldType::Game && Context.World())
                {
                    GameWorld = Context.World();
                }
            }
        }
        if (GameWorld)
        {
            return GameWorld;
        }
        return GetEditorWorld();
    }

    // =====================================================================
    // CANONICAL PIE WORLD RESOLUTION -- see header contract.
    //
    // One resolver for every command domain. "The first PIE context" cannot
    // tell a server from a client: raycast_scene can answer from UEDPIE_0 while
    // get_actor_properties_by_class reads UEDPIE_1 -- two CORRECT readings of two
    // DIFFERENT worlds, which is indistinguishable from a game bug -- and
    // diagnose_navmesh can report "no navmesh" because it resolved the
    // client, where the navmesh (server-authoritative) legitimately isn't.
    // =====================================================================

    namespace
    {
        // FWorldContext::PIEInstance for a given world, or INDEX_NONE.
        // NOT derived from the world's name: in a World Partition map the UWorld
        // is named after the map and only the PACKAGE carries UEDPIE_N.
        int32 McpGetPieInstance(const UWorld* World)
        {
            if (!World || !GEngine)
            {
                return INDEX_NONE;
            }
            if (const FWorldContext* Context = GEngine->GetWorldContextFromWorld(World))
            {
                return Context->PIEInstance;
            }
            return INDEX_NONE;
        }

        // One line of identity per world, for the not-found error. An error
        // that only says "no PIE world" makes the caller guess; one that lists
        // what DOES exist makes the mistake self-evident.
        FString McpDescribeWorld(const UWorld* World)
        {
            if (!World)
            {
                return TEXT("<null>");
            }
            const UPackage* Outermost = World->GetOutermost();
            return FString::Printf(
                TEXT("{name=%s, package=%s, type=%s, net_mode=%s, pie_instance=%d}"),
                *World->GetName(),
                Outermost ? *Outermost->GetName() : TEXT("<none>"),
                LexToString(World->WorldType.GetValue()),
                mcp_common::NetModeLabel(World->GetNetMode()),
                McpGetPieInstance(World));
        }

        // Every live PIE world, in world-context order (the order "the first
        // PIE context" refers to, so "pie" stays stable).
        void McpCollectPieWorlds(TArray<UWorld*>& OutWorlds)
        {
            OutWorlds.Reset();
            if (!GEngine)
            {
                return;
            }
            for (const FWorldContext& Context : GEngine->GetWorldContexts())
            {
                if (Context.WorldType == EWorldType::PIE && Context.World())
                {
                    OutWorlds.Add(Context.World());
                }
            }
        }

        FString McpDescribeAllWorlds()
        {
            TArray<FString> Lines;
            if (GEngine)
            {
                for (const FWorldContext& Context : GEngine->GetWorldContexts())
                {
                    if (Context.World())
                    {
                        Lines.Add(McpDescribeWorld(Context.World()));
                    }
                }
            }
            if (Lines.Num() == 0)
            {
                return TEXT("(no worlds)");
            }
            return FString::Join(Lines, TEXT(", "));
        }
    }

    UWorld* ResolveWorldFromParams(const TSharedPtr<FJsonObject>& Params, FString& OutError, const FString& FieldName)
    {
        FString Selector;
        if (Params.IsValid())
        {
            Params->TryGetStringField(FieldName, Selector);
        }
        Selector = Selector.TrimStartAndEnd();

        // --- absent | "editor" -------------------------------------------------
        if (Selector.IsEmpty() || Selector.Equals(TEXT("editor"), ESearchCase::IgnoreCase))
        {
            UWorld* EditorWorld = GetEditorWorld();
            if (!EditorWorld)
            {
                OutError = FString::Printf(
                    TEXT("Requested the editor world ('%s'='%s'), but there is none (GEditor is null or has no edit context). Worlds present: %s"),
                    *FieldName, Selector.IsEmpty() ? TEXT("<absent>") : *Selector, *McpDescribeAllWorlds());
            }
            return EditorWorld;
        }

        TArray<UWorld*> PieWorlds;
        McpCollectPieWorlds(PieWorlds);

        // --- "pie" : ambiguous -------------------------------------------------
        // PlayWorld first (that is the instance the editor considers "the" play world),
        // then the first PIE context. With 2+ players this picks ONE of them and
        // does not say which -- that is exactly the trap; AddWorldEvidence is
        // what makes the choice visible instead of silent.
        if (Selector.Equals(TEXT("pie"), ESearchCase::IgnoreCase))
        {
            if (GEditor && GEditor->PlayWorld)
            {
                return GEditor->PlayWorld;
            }
            if (PieWorlds.Num() > 0)
            {
                return PieWorlds[0];
            }
            OutError = FString::Printf(
                TEXT("Requested a PIE world ('%s'='pie'), but no PIE session is running. Worlds present: %s"),
                *FieldName, *McpDescribeAllWorlds());
            return nullptr;
        }

        // --- "server" ----------------------------------------------------------
        // Authority is NetMode != NM_Client (Standalone, ListenServer and
        // DedicatedServer are all "some variety of server" -- EngineBaseTypes.h
        // says so on NM_Client itself). This is the resolution that
        // diagnose_navmesh and anything else reading authoritative state needs.
        if (Selector.Equals(TEXT("server"), ESearchCase::IgnoreCase))
        {
            for (UWorld* World : PieWorlds)
            {
                if (World->GetNetMode() != NM_Client)
                {
                    return World;
                }
            }
            OutError = FString::Printf(
                TEXT("Requested the authoritative PIE world ('%s'='server'), but no PIE world has authority (none with GetNetMode() != NM_Client). PIE worlds: %s"),
                *FieldName,
                PieWorlds.Num() > 0 ? *McpDescribeAllWorlds() : TEXT("(no PIE session running)"));
            return nullptr;
        }

        // --- "client" ----------------------------------------------------------
        if (Selector.Equals(TEXT("client"), ESearchCase::IgnoreCase))
        {
            for (UWorld* World : PieWorlds)
            {
                if (World->GetNetMode() == NM_Client)
                {
                    return World;
                }
            }
            OutError = FString::Printf(
                TEXT("Requested a client PIE world ('%s'='client'), but no PIE world is a client (none with GetNetMode() == NM_Client -- a single-player PIE session is Standalone, i.e. a server). PIE worlds: %s"),
                *FieldName,
                PieWorlds.Num() > 0 ? *McpDescribeAllWorlds() : TEXT("(no PIE session running)"));
            return nullptr;
        }

        // --- "pie:N" -----------------------------------------------------------
        if (Selector.StartsWith(TEXT("pie:"), ESearchCase::IgnoreCase))
        {
            const FString InstanceToken = Selector.RightChop(4).TrimStartAndEnd();
            if (InstanceToken.IsEmpty() || !InstanceToken.IsNumeric())
            {
                OutError = FString::Printf(
                    TEXT("Could not parse '%s'='%s': expected 'pie:N' with N an integer PIE instance id."),
                    *FieldName, *Selector);
                return nullptr;
            }
            const int32 WantedInstance = FCString::Atoi(*InstanceToken);
            for (UWorld* World : PieWorlds)
            {
                if (McpGetPieInstance(World) == WantedInstance)
                {
                    return World;
                }
            }
            OutError = FString::Printf(
                TEXT("Requested PIE instance %d ('%s'='%s'), but no PIE world has that PIEInstance. PIE worlds: %s"),
                WantedInstance, *FieldName, *Selector,
                PieWorlds.Num() > 0 ? *McpDescribeAllWorlds() : TEXT("(no PIE session running)"));
            return nullptr;
        }

        OutError = FString::Printf(
            TEXT("Unknown value for '%s': '%s'. Accepted: <absent>, 'editor', 'pie' (ambiguous), 'server', 'client', 'pie:N'. Worlds present: %s"),
            *FieldName, *Selector, *McpDescribeAllWorlds());
        return nullptr;
    }

    UWorld* ResolveWorldSelector(const TSharedPtr<FJsonObject>& Params, const TCHAR* DefaultSelector,
        FString& OutSelector, FString& OutError)
    {
        FString Selector;
        if (Params.IsValid())
        {
            Params->TryGetStringField(TEXT("world"), Selector);
        }
        Selector = Selector.TrimStartAndEnd();
        if (Selector.IsEmpty())
        {
            Selector = DefaultSelector;
        }
        else if (Selector.Equals(TEXT("game"), ESearchCase::IgnoreCase))
        {
            Selector = TEXT("pie");
        }
        OutSelector = Selector.ToLower();

        const TSharedPtr<FJsonObject> WorldParams = MakeShared<FJsonObject>();
        WorldParams->SetStringField(TEXT("world"), Selector);
        return ResolveWorldFromParams(WorldParams, OutError);
    }

    void AddWorldEvidence(const TSharedPtr<FJsonObject>& Data, UWorld* World)
    {
        if (!Data.IsValid())
        {
            return;
        }

        if (!World)
        {
            // Emit the fields as null rather than omitting them -- an absent
            // field is indistinguishable from "this tool doesn't stamp evidence",
            // which is precisely the confusion this helper exists to end.
            Data->SetField(TEXT("world_name"),    MakeShared<FJsonValueNull>());
            Data->SetField(TEXT("world_package"), MakeShared<FJsonValueNull>());
            Data->SetField(TEXT("world_type"),    MakeShared<FJsonValueNull>());
            Data->SetField(TEXT("net_mode"),      MakeShared<FJsonValueNull>());
            Data->SetNumberField(TEXT("pie_instance"), INDEX_NONE);
            return;
        }

        Data->SetStringField(TEXT("world_name"), World->GetName());

        // The PACKAGE, not the world name. In a World Partition map the
        // UWorld keeps the map's name in PIE and the UEDPIE_N prefix lives only
        // on the outermost package -- reading GetName() there tells you nothing
        // about which instance you are on, so two tools can report
        // contradictory state from two different PIE instances.
        const UPackage* Outermost = World->GetOutermost();
        Data->SetStringField(TEXT("world_package"), Outermost ? Outermost->GetName() : FString());

        Data->SetStringField(TEXT("world_type"), LexToString(World->WorldType.GetValue()));
        Data->SetStringField(TEXT("net_mode"), mcp_common::NetModeLabel(World->GetNetMode()));
        Data->SetNumberField(TEXT("pie_instance"), McpGetPieInstance(World));
    }

    // Custom export override for FJsonObjectConverter::UPropertyToJsonValue (checked
    // first, before every engine-default case -- see the Execute contract on
    // FJsonObjectConverter::CustomExportCallback). Returning an invalid TSharedPtr
    // falls through to the engine's own default behavior for everything else
    // (numerics/bools/strings/enums/arrays/sets/maps/other structs), so this only
    // needs to special-case the two conventions the engine default gets wrong for
    // MCP's purposes.
    TSharedPtr<FJsonValue> McpPropertyExportOverride(FProperty* Property, const void* Value)
    {
        if (FStructProperty* StructProp = CastField<FStructProperty>(Property))
        {
            if (StructProp->Struct == FGameplayTag::StaticStruct())
            {
                return MakeShared<FJsonValueString>(static_cast<const FGameplayTag*>(Value)->ToString());
            }
            if (StructProp->Struct == FGameplayTagContainer::StaticStruct())
            {
                TArray<FGameplayTag> Tags;
                static_cast<const FGameplayTagContainer*>(Value)->GetGameplayTagArray(Tags);
                TArray<TSharedPtr<FJsonValue>> Out;
                Out.Reserve(Tags.Num());
                for (const FGameplayTag& Tag : Tags)
                {
                    Out.Add(MakeShared<FJsonValueString>(Tag.ToString()));
                }
                return MakeShared<FJsonValueArray>(Out);
            }
        }
        // soft-ref/soft-class -> stored path, loaded or NOT. FObjectPropertyBase::
        // GetObjectPropertyValue() returns null for an UNLOADED soft-ref, which the
        // branch below would then serialize as "None", silently LOSING the path. Read
        // the FSoftObjectPtr directly so the asset path round-trips whether or not the
        // target is in memory. FSoftObjectProperty derives from FObjectPropertyBase and
        // FSoftClassProperty derives from FSoftObjectProperty, so one CastField catches
        // both; it must run BEFORE the FObjectPropertyBase branch.
        else if (FSoftObjectProperty* SoftProp = CastField<FSoftObjectProperty>(Property))
        {
            const FString Path = SoftProp->GetPropertyValue(Value).ToString();
            return MakeShared<FJsonValueString>(Path.IsEmpty() ? TEXT("None") : Path);
        }
        else if (FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Property))
        {
            // Always a resolvable object_path string -- never inline the referenced
            // object's properties by value (the engine default does this for
            // Instanced/EditInlineNew properties, which would break the "object_path
            // is always a plain resolvable string" convention every other MCP command
            // relies on). "None" (not JSON null) matches ExportTextItem's existing
            // null-object text, which callers across the toolset already handle.
            UObject* Obj = ObjProp->GetObjectPropertyValue(Value);
            return MakeShared<FJsonValueString>(Obj ? Obj->GetPathName() : TEXT("None"));
        }
        return TSharedPtr<FJsonValue>();
    }

    TSharedPtr<FJsonValue> PropertyValueToJson(FProperty* Property, const void* ValuePtr)
    {
        if (!Property || !ValuePtr)
        {
            return MakeShared<FJsonValueNull>();
        }

        static const FJsonObjectConverter::CustomExportCallback ExportCb =
            FJsonObjectConverter::CustomExportCallback::CreateStatic(&McpPropertyExportOverride);

        // SkipStandardizeCase: by default the engine lower-cases the first letter of
        // every nested struct field name (UStructToJsonAttributesWithContainer calls
        // StandardizeCaseInPlace on each FProperty::GetAuthoredName()), so a field
        // SequenceId inside an array of structs would come back as "sequenceId".
        // Keep the authored name at every nesting level: it matches set_properties'
        // param names and the top-level keys of every reflection command
        // (Property->GetName() verbatim), one casing rule everywhere.
        TSharedPtr<FJsonValue> Result = FJsonObjectConverter::UPropertyToJsonValue(
            Property, ValuePtr, /*CheckFlags=*/0, /*SkipFlags=*/0, &ExportCb,
            /*OuterProperty=*/nullptr, EJsonObjectConversionFlags::SkipStandardizeCase);
        return Result.IsValid() ? Result : MakeShared<FJsonValueNull>();
    }

    // No phantom UPackage; see the header for
    // the full rationale. StaticLoadObject on a path whose package is neither in
    // memory nor on disk runs ResolveName with Create=true and CreatePackage()s
    // unconditionally, leaving an empty UPackage that breaks the next create_X at
    // the same path. Gate the load behind two side-effect-free probes.
    UObject* ResolveObjectByPath(const FString& ObjectPath, UClass* ExpectedClass)
    {
        if (ObjectPath.IsEmpty())
        {
            return nullptr;
        }
        UClass* const SearchClass = ExpectedClass ? ExpectedClass : UObject::StaticClass();

        // Normalize a bare package path ("/Game/Foo") to its primary-asset object
        // path ("/Game/Foo.Foo"), mirroring StaticLoadObject's own package->asset
        // resolution. Without this, StaticFindObject on a bare (already-loaded)
        // package path returns the UPackage itself, not the contained asset, so a
        // typed Cast<> downstream fails ("not found") the moment the package is
        // loaded. Dotted paths (object paths, or the "Actor.Component" convenience
        // form) and non-package names (e.g. a bare actor label) are left untouched.
        FString ResolvedPath = ObjectPath;
        if (!ObjectPath.Contains(TEXT(".")) && FPackageName::IsValidLongPackageName(ObjectPath))
        {
            const FString LeafName = FPackageName::GetLongPackageAssetName(ObjectPath);
            if (!LeafName.IsEmpty())
            {
                ResolvedPath = FString::Printf(TEXT("%s.%s"), *ObjectPath, *LeafName);
            }
        }
        // The same for a sub-object named under a bare package path: "/Game/M_Rock:MaterialExpressionConstant_0" is
        // "/Game/M_Rock.M_Rock:MaterialExpressionConstant_0".
        FString PackagePart, SubObjectPart;
        if (ObjectPath.Split(TEXT(":"), &PackagePart, &SubObjectPart) && !PackagePart.Contains(TEXT("."))
            && FPackageName::IsValidLongPackageName(PackagePart) && !SubObjectPart.IsEmpty())
        {
            ResolvedPath = FString::Printf(TEXT("%s.%s:%s"), *PackagePart, *FPackageName::GetLongPackageAssetName(PackagePart), *SubObjectPart);
        }

        // 1) In-memory first -- zero side effects. Resolves already-loaded assets
        //    by object path and live objects by their full GetPathName().
        if (UObject* Found = StaticFindObject(SearchClass, nullptr, *ResolvedPath))
        {
            return Found;
        }

        // 2) On-disk -- only attempt the load (which has the CreatePackage side
        //    effect on a miss) when the package genuinely exists. The package is
        //    the substring before the first '.' ("/Game/Foo.Foo" -> "/Game/Foo").
        FString PackageName = ResolvedPath;
        int32 DotIndex = INDEX_NONE;
        if (ResolvedPath.FindChar(TEXT('.'), DotIndex))
        {
            PackageName = ResolvedPath.Left(DotIndex);
        }
        if (FPackageName::IsValidLongPackageName(PackageName) &&
            FPackageName::DoesPackageExist(PackageName))
        {
            return StaticLoadObject(SearchClass, nullptr, *ResolvedPath);
        }

        // 3) Genuine miss: no live object, no on-disk package. Return nullptr
        //    WITHOUT creating a phantom package.
        return nullptr;
    }

    // Resolve a UClass from an FAssetData's AssetClassPath ("/Script/Module.ClassName") without
    // forcing a load when the class is already resident (the common case -- asset registry class
    // paths are almost always native UObject classes, e.g. UBlueprint/UStaticMesh/UMaterial,
    // which are loaded for the lifetime of the process). Mirrors the resolution order already
    // used by GC_ResolveCameraNodeClass (UEAgentCameraCommands.cpp) for camera-node class
    // names: TryFindTypeSlow first (side-effect-free, handles class redirects), LoadObject as a
    // fallback for the rare case the class itself needs loading.
    static UClass* McpResolveAssetDataClass(const FTopLevelAssetPath& AssetClassPath)
    {
        const FString PathString = AssetClassPath.ToString();
        if (PathString.IsEmpty())
        {
            return nullptr;
        }
        if (UClass* Found = UClass::TryFindTypeSlow<UClass>(PathString))
        {
            return Found;
        }
        return LoadObject<UClass>(nullptr, *PathString);
    }

    // Generic (class-parameterized) sibling of ResolveBlueprintByName -- see header for the full
    // contract. Unlike ResolveBlueprintByName, this NEVER silently resolves ambiguity: every
    // matching FAssetData is returned in OutAssets and the caller decides what to do with >1.
    bool ResolveAssetFlexible(
        const FString& NameOrPath,
        UClass* ExpectedClass,
        const FString& SearchRoot,
        TArray<FAssetData>& OutAssets,
        FString& OutError)
    {
        OutAssets.Reset();
        OutError.Reset();

        if (NameOrPath.IsEmpty())
        {
            OutError = TEXT("ResolveAssetFlexible: NameOrPath is empty");
            return false;
        }

        FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

        const bool bLooksLikePath = NameOrPath.Contains(TEXT("/"));

        // 1) Full object/package path fast path -- a single, side-effect-free AssetRegistry
        //    lookup. Only short-circuits on a HIT whose class also matches (or ExpectedClass is
        //    unset); a class mismatch or registry miss falls through to the step-2 scan below
        //    (the scan's own exact AssetName match will simply not find a "/Game/..." string as
        //    an asset name, so it degrades to "no match" rather than a false positive).
        if (bLooksLikePath)
        {
            const FTopLevelAssetPath PackageClassPath = UPackage::StaticClass()->GetClassPathName();
            auto ClassOk = [ExpectedClass, &PackageClassPath](const FAssetData& Candidate)
            {
                // A UPackage is never the asset a caller means. The registry hands one back when
                // it is asked for a bare package path whose package is in memory, so it is
                // skipped here even with no class filter.
                if (!Candidate.IsValid() || Candidate.AssetClassPath == PackageClassPath)
                {
                    return false;
                }
                if (ExpectedClass == nullptr)
                {
                    return true;
                }
                UClass* ResolvedClass = McpResolveAssetDataClass(Candidate.AssetClassPath);
                return ResolvedClass && ResolvedClass->IsChildOf(ExpectedClass);
            };

            // A package path names its primary asset ("/Game/A/B" -> "/Game/A/B.B"), the way
            // StaticLoadObject resolves it.
            const bool bPackagePath = !NameOrPath.Contains(TEXT(".")) && FPackageName::IsValidLongPackageName(NameOrPath);
            const FString ObjectPath = bPackagePath
                ? NameOrPath + TEXT(".") + FPackageName::GetLongPackageAssetName(NameOrPath)
                : NameOrPath;
            const FAssetData DirectAssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(ObjectPath));
            if (ClassOk(DirectAssetData))
            {
                OutAssets.Add(DirectAssetData);
                return true;
            }

            // A package whose asset is not named after it: every asset the package holds.
            if (bPackagePath)
            {
                TArray<FAssetData> InPackage;
                AssetRegistry.GetAssetsByPackageName(FName(*NameOrPath), InPackage);
                for (const FAssetData& Candidate : InPackage)
                {
                    if (ClassOk(Candidate))
                    {
                        OutAssets.Add(Candidate);
                    }
                }
                if (OutAssets.Num() > 0)
                {
                    return true;
                }
            }
        }

        // 2) Project-wide recursive scan, scoped to SearchRoot (default "/Game"). bRecursiveClasses
        //    means subclasses of ExpectedClass match too -- e.g. ExpectedClass=UBlueprint also
        //    matches UAnimBlueprint/UWidgetBlueprint (unlike an exact-class filter, which would miss
        //    them). Manual exact AssetName match; NOT a substring match.
        const FString EffectiveRoot = SearchRoot.IsEmpty() ? TEXT("/Game") : SearchRoot;

        FARFilter Filter;
        Filter.PackagePaths.Add(FName(*EffectiveRoot));
        Filter.bRecursivePaths = true;
        if (ExpectedClass)
        {
            Filter.ClassPaths.Add(ExpectedClass->GetClassPathName());
            Filter.bRecursiveClasses = true;
        }

        TArray<FAssetData> Candidates;
        AssetRegistry.GetAssets(Filter, Candidates);

        for (const FAssetData& AssetData : Candidates)
        {
            if (AssetData.AssetName.ToString() == NameOrPath)
            {
                OutAssets.Add(AssetData);
            }
        }

        if (OutAssets.Num() == 0)
        {
            OutError = FString::Printf(
                TEXT("ResolveAssetFlexible: no asset named '%s' found%s under '%s'%s"),
                *NameOrPath,
                ExpectedClass ? *FString::Printf(TEXT(" (class '%s' or a subclass)"), *ExpectedClass->GetName()) : TEXT(""),
                *EffectiveRoot,
                bLooksLikePath ? TEXT(" (also tried as a direct object path)") : TEXT(""));
            return false;
        }

        return true;
    }

    // =====================================================================
    // READ-ONLY ROOT GUARD -- see header for the contract.
    //
    // The parse below mirrors the one fixup_redirectors does inline
    // (UEAgentSystemCommands.cpp). Same default, same trailing-slash trim,
    // same case-insensitive prefix match -- so that call site keeps working
    // identically if it forwards here.
    // =====================================================================

    namespace
    {
        // Trim trailing '/' as the fixup_redirectors parse does (never
        // trimming a lone "/", so a root of "/" stays a valid -- if extreme -- root).
        void McpTrimTrailingSlashes(FString& InOutPath)
        {
            while (InOutPath.Len() > 1 && InOutPath.EndsWith(TEXT("/")))
            {
                InOutPath.LeftChopInline(1);
            }
        }

        // Set only by SetDefaultReadonlyRoots (tests); otherwise the roots come from the
        // project settings (UUEAgentProjectSettings::ReadOnlyContentRoots).
        TOptional<TArray<FString>>& McpReadonlyRootsOverride()
        {
            static TOptional<TArray<FString>> Override;
            return Override;
        }

        // Whether saves under a read-only root are refused (1, the default) or only logged (0). The
        // console variable references this int32, so the console writes it and
        // IsReadOnlyRootSaveEnforcementEnabled() reads it; constant-initialized, it is 1 before any
        // dynamic initializer runs.
        int32 GMcpReadOnlyRootEnforcement = 1;

        // The switch is reachable without an MCP command or a rebuild: `mcp.ReadOnlyRootEnforcement 0`
        // in the editor console lowers the guard for the rest of the session.
        FAutoConsoleVariableRef CVarMcpReadOnlyRootEnforcement(
            TEXT("mcp.ReadOnlyRootEnforcement"),
            GMcpReadOnlyRootEnforcement,
            TEXT("1 (default) = REFUSE any MCP tool save under a read-only root (Project Settings > Plugins > UEAgent > Read Only Content Roots). ")
            TEXT("0 = report-only: log a warning and save anyway."),
            ECVF_Default);
    }

    TArray<FString> ParseReadonlyRoots(const TSharedPtr<FJsonObject>& Params, const FString& FieldName)
    {
        TArray<FString> ReadOnlyRoots;

        const TArray<TSharedPtr<FJsonValue>>* ReadOnlyRootsJson = nullptr;
        if (Params.IsValid() && Params->TryGetArrayField(FieldName, ReadOnlyRootsJson) && ReadOnlyRootsJson)
        {
            for (const TSharedPtr<FJsonValue>& Value : *ReadOnlyRootsJson)
            {
                FString Root;
                if (Value.IsValid() && Value->TryGetString(Root) && !Root.IsEmpty())
                {
                    McpTrimTrailingSlashes(Root);
                    ReadOnlyRoots.Add(Root);
                }
            }
            // An EXPLICIT [] means "no read-only roots" -- the same thing the
            // inline parse produces (the loop adds nothing). Do NOT substitute the
            // default here; that would silently ignore a caller's opt-out.
            return ReadOnlyRoots;
        }

        ReadOnlyRoots = GetDefaultReadonlyRoots();
        return ReadOnlyRoots;
    }

    TArray<FString> GetDefaultReadonlyRoots()
    {
        const TOptional<TArray<FString>>& Override = McpReadonlyRootsOverride();
        if (Override.IsSet())
        {
            return Override.GetValue();
        }
        return GetDefault<UUEAgentProjectSettings>()->GetReadOnlyContentRoots();
    }

    void SetDefaultReadonlyRoots(const TArray<FString>& Roots)
    {
        TArray<FString> Trimmed;
        for (const FString& Root : Roots)
        {
            if (Root.IsEmpty())
            {
                continue;
            }
            FString Entry = Root;
            McpTrimTrailingSlashes(Entry);
            Trimmed.Add(Entry);
        }
        McpReadonlyRootsOverride() = MoveTemp(Trimmed);
    }

    void ClearDefaultReadonlyRootsOverride()
    {
        McpReadonlyRootsOverride().Reset();
    }

    bool IsUnderReadOnlyRoot(const FString& PackageOrObjectPath, const TArray<FString>& ReadOnlyRoots)
    {
        if (PackageOrObjectPath.IsEmpty() || ReadOnlyRoots.Num() == 0)
        {
            return false;
        }

        // Reduce object / subobject paths to their package name.
        // "/Game/VendorPack/X.X:Comp" and "/Game/VendorPack/X.X" both -> "/Game/VendorPack/X".
        FString PackageName = PackageOrObjectPath;
        int32 DotIndex = INDEX_NONE;
        if (PackageName.FindChar(TEXT('.'), DotIndex))
        {
            PackageName.LeftInline(DotIndex, EAllowShrinking::No);
        }
        McpTrimTrailingSlashes(PackageName);

        for (const FString& Root : ReadOnlyRoots)
        {
            if (Root.IsEmpty())
            {
                continue;
            }
            // FString::operator== and StartsWith are case-insensitive by
            // default -- deliberately, it is what fixup_redirectors does.
            if (PackageName == Root || PackageName.StartsWith(Root + TEXT("/")))
            {
                return true;
            }
        }
        return false;
    }

    bool IsUnderReadOnlyRoot(const FString& PackageOrObjectPath)
    {
        return IsUnderReadOnlyRoot(PackageOrObjectPath, GetDefaultReadonlyRoots());
    }

    bool IsReadOnlyRootSaveEnforcementEnabled()
    {
        return GMcpReadOnlyRootEnforcement != 0;
    }

    // =====================================================================
    // PROPERTY VALUE COMPARATOR -- see header for the contract.
    // =====================================================================

    namespace
    {
        // Does this property's TYPE tree contain an FText leaf / an object
        // reference leaf? Used only to ATTRIBUTE a residual export-text
        // difference once the engine has already declared the values equal --
        // never to decide equality itself. Depth-bounded and struct-visited-set
        // guarded because USTRUCTs can reference each other cyclically through
        // pointers/arrays.
        void McpScanPropertyTypeTree(const FProperty* Property, bool& bOutHasText, bool& bOutHasObjectRef,
                                     TSet<const UStruct*>& VisitedStructs, int32 Depth)
        {
            if (!Property || Depth > 12 || (bOutHasText && bOutHasObjectRef))
            {
                return;
            }

            if (CastField<FTextProperty>(Property))
            {
                bOutHasText = true;
                return;
            }
            if (CastField<FObjectPropertyBase>(Property))
            {
                // Covers FObjectProperty/FClassProperty/FSoftObjectProperty/
                // FSoftClassProperty/FWeakObjectProperty/FLazyObjectProperty.
                bOutHasObjectRef = true;
                return;
            }
            if (const FArrayProperty* ArrayProp = CastField<FArrayProperty>(Property))
            {
                McpScanPropertyTypeTree(ArrayProp->Inner, bOutHasText, bOutHasObjectRef, VisitedStructs, Depth + 1);
                return;
            }
            if (const FSetProperty* SetProp = CastField<FSetProperty>(Property))
            {
                McpScanPropertyTypeTree(SetProp->ElementProp, bOutHasText, bOutHasObjectRef, VisitedStructs, Depth + 1);
                return;
            }
            if (const FMapProperty* MapProp = CastField<FMapProperty>(Property))
            {
                McpScanPropertyTypeTree(MapProp->KeyProp, bOutHasText, bOutHasObjectRef, VisitedStructs, Depth + 1);
                McpScanPropertyTypeTree(MapProp->ValueProp, bOutHasText, bOutHasObjectRef, VisitedStructs, Depth + 1);
                return;
            }
            if (const FStructProperty* StructProp = CastField<FStructProperty>(Property))
            {
                const UStruct* StructType = StructProp->Struct;
                if (!StructType || VisitedStructs.Contains(StructType))
                {
                    return;
                }
                VisitedStructs.Add(StructType);
                for (TFieldIterator<FProperty> It(StructType); It; ++It)
                {
                    McpScanPropertyTypeTree(*It, bOutHasText, bOutHasObjectRef, VisitedStructs, Depth + 1);
                    if (bOutHasText && bOutHasObjectRef)
                    {
                        return;
                    }
                }
            }
        }

        FString McpExportPropertyText(const FProperty* Property, const void* ValuePtr)
        {
            if (!Property || !ValuePtr)
            {
                return FString();
            }
            FString Out;
            // Defaults=nullptr + PPF_None == exactly what
            // UEAgentReflectionCommands::GetPropertyValueAsString does, so
            // the "export text" side of the classification is directly comparable
            // to that reader's output.
            Property->ExportTextItem_Direct(Out, ValuePtr, /*DefaultValue=*/nullptr, /*Parent=*/nullptr, PPF_None);
            return Out;
        }
    }

    FPropertyDiffResult ComparePropertyValues(
        const FProperty* PropertyA,
        const void* ContainerA,
        const FProperty* PropertyB,
        const void* ContainerB,
        uint32 PortFlags)
    {
        FPropertyDiffResult Result;

        if (!PropertyA || !ContainerA)
        {
            Result.Kind = EPropertyDiffKind::Invalid;
            Result.Note = TEXT("Null property or null container on side A -- nothing was compared.");
            return Result;
        }
        if (!PropertyB)
        {
            PropertyB = PropertyA;
        }

        if (!PropertyA->SameType(PropertyB))
        {
            Result.Kind = EPropertyDiffKind::TypeMismatch;
            Result.bDiffers = true;
            Result.Note = FString::Printf(
                TEXT("Property types differ (%s vs %s) -- values were NOT compared."),
                *PropertyA->GetCPPType(), *PropertyB->GetCPPType());
            Result.ValueA = McpExportPropertyText(PropertyA, PropertyA->ContainerPtrToValuePtr<void>(ContainerA));
            if (ContainerB)
            {
                Result.ValueB = McpExportPropertyText(PropertyB, PropertyB->ContainerPtrToValuePtr<void>(ContainerB));
            }
            return Result;
        }

        // A C-style static array (UPROPERTY int32 Foo[4]) is ONE FProperty
        // with ArrayDim == 4; FProperty::Identical only ever looks at the element
        // it is handed, so we must walk every index or we would silently compare
        // element 0 alone (a real over-report/under-report source).
        const int32 NumElements = FMath::Max(1, FMath::Min(PropertyA->ArrayDim, PropertyB->ArrayDim));

        bool bAllIdentical = true;
        bool bAllExportEqual = true;
        TArray<FString> ExportsA;
        TArray<FString> ExportsB;
        ExportsA.Reserve(NumElements);
        ExportsB.Reserve(NumElements);

        for (int32 Index = 0; Index < NumElements; ++Index)
        {
            const void* ValueA = PropertyA->ContainerPtrToValuePtr<void>(ContainerA, Index);
            const void* ValueB = ContainerB ? PropertyB->ContainerPtrToValuePtr<void>(ContainerB, Index) : nullptr;

            // Identical() takes TNotNull for A (it check()s on null) but a
            // plain pointer for B -- a null B legitimately means "no baseline",
            // which every FProperty::Identical override handles.
            const bool bIdentical = ValueA ? PropertyA->Identical(ValueA, ValueB, PortFlags) : false;
            bAllIdentical = bAllIdentical && bIdentical;

            const FString ExportA = McpExportPropertyText(PropertyA, ValueA);
            const FString ExportB = ValueB ? McpExportPropertyText(PropertyB, ValueB) : FString();
            bAllExportEqual = bAllExportEqual && (ValueB != nullptr) && ExportA.Equals(ExportB, ESearchCase::CaseSensitive);
            ExportsA.Add(ExportA);
            ExportsB.Add(ExportB);
        }

        Result.bIdenticalDeep = bAllIdentical;
        Result.bExportTextEqual = bAllExportEqual;
        Result.ValueA = FString::Join(ExportsA, TEXT(","));
        Result.ValueB = ContainerB ? FString::Join(ExportsB, TEXT(",")) : FString();

        if (!ContainerB)
        {
            Result.Kind = EPropertyDiffKind::RealOverride;
            Result.bDiffers = true;
            Result.Note = TEXT("No baseline container was supplied -- the value is reported as authored, not compared.");
            return Result;
        }

        if (!bAllIdentical)
        {
            Result.Kind = EPropertyDiffKind::RealOverride;
            Result.bDiffers = true;
            Result.Note = TEXT("FProperty::Identical reports the values differ.");
            return Result;
        }

        if (bAllExportEqual)
        {
            Result.Kind = EPropertyDiffKind::Identical;
            Result.bDiffers = false;
            Result.Note = TEXT("Equal by engine comparison and by exported text.");
            return Result;
        }

        // Equal for the engine, different as text. Attribute the noise --
        // unattributed, this case reads as a self-contradictory
        // 'Values differ' with value_1 == value_2 == "Player".
        TSet<const UStruct*> Visited;
        McpScanPropertyTypeTree(PropertyA, Result.bTypeContainsText, Result.bTypeContainsObjectRef, Visited, 0);

        Result.bDiffers = false;
        if (Result.bTypeContainsText)
        {
            Result.Kind = EPropertyDiffKind::TextLocalizationMetadataOnly;
            Result.Note = TEXT("Equal per FTextProperty::Identical (display-string compare in editor); the exported ")
                          TEXT("text differs only in localization identity (namespace/key/source). Not an override.");
        }
        else if (Result.bTypeContainsObjectRef)
        {
            Result.Kind = EPropertyDiffKind::ObjectOuterOnly;
            Result.Note = TEXT("Equal per FObjectPropertyBase::Identical with PPF_DeepComparison (same class, same name, ")
                          TEXT("identical contents); the exported paths differ only by outer. Not an override.");
        }
        else
        {
            Result.Kind = EPropertyDiffKind::ExportTextOnly;
            Result.Note = TEXT("Equal per FProperty::Identical but the exported text differs, and this helper could not ")
                          TEXT("attribute the difference to FText localization or to object outers. Reported, not hidden.");
        }
        return Result;
    }

    FPropertyDiffResult ComparePropertyValues(
        const FProperty* Property,
        const void* ContainerA,
        const void* ContainerB,
        uint32 PortFlags)
    {
        return ComparePropertyValues(Property, ContainerA, Property, ContainerB, PortFlags);
    }

    const TCHAR* PropertyDiffKindToString(EPropertyDiffKind Kind)
    {
        switch (Kind)
        {
        case EPropertyDiffKind::Identical:                    return TEXT("identical");
        case EPropertyDiffKind::RealOverride:                 return TEXT("real_override");
        case EPropertyDiffKind::TextLocalizationMetadataOnly: return TEXT("text_localization_metadata_only");
        case EPropertyDiffKind::ObjectOuterOnly:              return TEXT("object_outer_only");
        case EPropertyDiffKind::ExportTextOnly:               return TEXT("export_text_only");
        case EPropertyDiffKind::TypeMismatch:                 return TEXT("type_mismatch");
        case EPropertyDiffKind::Invalid:                      return TEXT("invalid");
        default:                                              return TEXT("invalid");
        }
    }

    TSharedPtr<FJsonObject> ComparePropertyValuesToJson(
        FProperty* PropertyA,
        const void* ContainerA,
        FProperty* PropertyB,
        const void* ContainerB,
        uint32 PortFlags)
    {
        const FPropertyDiffResult Diff = ComparePropertyValues(PropertyA, ContainerA, PropertyB, ContainerB, PortFlags);

        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        Out->SetStringField(TEXT("kind"), PropertyDiffKindToString(Diff.Kind));
        Out->SetBoolField(TEXT("differs"), Diff.bDiffers);
        Out->SetBoolField(TEXT("identical_deep"), Diff.bIdenticalDeep);
        Out->SetBoolField(TEXT("export_text_equal"), Diff.bExportTextEqual);
        Out->SetBoolField(TEXT("type_contains_text"), Diff.bTypeContainsText);
        Out->SetBoolField(TEXT("type_contains_object_ref"), Diff.bTypeContainsObjectRef);
        Out->SetStringField(TEXT("export_text_a"), Diff.ValueA);
        Out->SetStringField(TEXT("export_text_b"), Diff.ValueB);
        Out->SetStringField(TEXT("note"), Diff.Note);

        if (PropertyA && ContainerA)
        {
            Out->SetField(TEXT("value_a"), PropertyValueToJson(PropertyA, PropertyA->ContainerPtrToValuePtr<void>(ContainerA)));
        }
        else
        {
            Out->SetField(TEXT("value_a"), MakeShared<FJsonValueNull>());
        }

        FProperty* EffectiveB = PropertyB ? PropertyB : PropertyA;
        if (EffectiveB && ContainerB)
        {
            Out->SetField(TEXT("value_b"), PropertyValueToJson(EffectiveB, EffectiveB->ContainerPtrToValuePtr<void>(ContainerB)));
        }
        else
        {
            Out->SetField(TEXT("value_b"), MakeShared<FJsonValueNull>());
        }

        return Out;
    }

    // =====================================================================
    // VERIFIED RESPONSES -- see header for the contract.
    // =====================================================================

    const TCHAR* McpVerificationToString(EMcpVerification Verification)
    {
        switch (Verification)
        {
        case EMcpVerification::Verified:      return TEXT("verified");
        case EMcpVerification::Failed:        return TEXT("failed");
        case EMcpVerification::NotVerifiable: return TEXT("not_verifiable");
        case EMcpVerification::NotAttempted:  return TEXT("not_attempted");
        default:                              return TEXT("not_attempted");
        }
    }

    namespace
    {
        // worst-status-wins ranking. A single failed check must not be
        // hidden behind four passing ones -- that is exactly the "hardcoded
        // success" failure mode this whole envelope exists to kill.
        int32 McpVerificationSeverity(EMcpVerification Verification)
        {
            switch (Verification)
            {
            case EMcpVerification::Failed:        return 3;
            case EMcpVerification::NotVerifiable: return 2;
            case EMcpVerification::NotAttempted:  return 1;
            case EMcpVerification::Verified:      return 0;
            default:                              return 1;
            }
        }

        EMcpVerification McpVerificationFromString(const FString& Status)
        {
            if (Status == TEXT("verified"))       { return EMcpVerification::Verified; }
            if (Status == TEXT("failed"))         { return EMcpVerification::Failed; }
            if (Status == TEXT("not_verifiable")) { return EMcpVerification::NotVerifiable; }
            return EMcpVerification::NotAttempted;
        }
    }

    void AddVerificationEvidence(
        const TSharedPtr<FJsonObject>& Data,
        EMcpVerification Verification,
        const FString& Method,
        const FString& Detail)
    {
        if (!Data.IsValid())
        {
            return;
        }

        TSharedPtr<FJsonObject> Check = MakeShared<FJsonObject>();
        Check->SetStringField(TEXT("status"), McpVerificationToString(Verification));
        Check->SetBoolField(TEXT("verified"), Verification == EMcpVerification::Verified);
        Check->SetStringField(TEXT("method"), Method);
        Check->SetStringField(TEXT("detail"), Detail);

        TArray<TSharedPtr<FJsonValue>> Checks;
        const TArray<TSharedPtr<FJsonValue>>* ExistingChecks = nullptr;
        if (Data->TryGetArrayField(TEXT("verification_checks"), ExistingChecks) && ExistingChecks)
        {
            Checks = *ExistingChecks;
        }
        Checks.Add(MakeShared<FJsonValueObject>(Check));
        Data->SetArrayField(TEXT("verification_checks"), Checks);

        // Recompute the aggregate from the accumulated checks, so calling
        // this N times can only ever make the reported status worse, never better.
        // The summary must be captioned by the check that PRODUCED the aggregate
        // status, not by whichever call happened to be last. Pairing a "failed" verdict
        // with the method/detail of a check that passed is exactly the self-contradictory
        // response this envelope exists to eliminate (a caller reading only
        // verification.method/detail is told the wrong reason for the failure).
        // Seeding from the current call leaves a single check captioned by itself.
        EMcpVerification Aggregate = EMcpVerification::Verified;
        FString WorstMethod = Method;
        FString WorstDetail = Detail;
        for (const TSharedPtr<FJsonValue>& Entry : Checks)
        {
            const TSharedPtr<FJsonObject>* EntryObj = nullptr;
            if (!Entry.IsValid() || !Entry->TryGetObject(EntryObj) || !EntryObj)
            {
                continue;
            }
            FString Status;
            (*EntryObj)->TryGetStringField(TEXT("status"), Status);
            const EMcpVerification EntryVerification = McpVerificationFromString(Status);
            if (McpVerificationSeverity(EntryVerification) > McpVerificationSeverity(Aggregate))
            {
                Aggregate = EntryVerification;
                FString EntryMethod;
                FString EntryDetail;
                if ((*EntryObj)->TryGetStringField(TEXT("method"), EntryMethod))
                {
                    WorstMethod = EntryMethod;
                }
                if ((*EntryObj)->TryGetStringField(TEXT("detail"), EntryDetail))
                {
                    WorstDetail = EntryDetail;
                }
            }
        }

        TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
        Summary->SetStringField(TEXT("status"), McpVerificationToString(Aggregate));
        Summary->SetBoolField(TEXT("verified"), Aggregate == EMcpVerification::Verified);
        Summary->SetNumberField(TEXT("check_count"), Checks.Num());
        Summary->SetStringField(TEXT("method"), WorstMethod);
        Summary->SetStringField(TEXT("detail"), WorstDetail);
        Data->SetObjectField(TEXT("verification"), Summary);
    }

    TSharedPtr<FJsonObject> MakeVerifiedReply(
        const TSharedPtr<FJsonObject>& Data,
        bool bSuccess,
        EMcpVerification Verification,
        const FString& VerificationMethod,
        const FString& VerificationDetail,
        const FString& ErrorMessage)
    {
        // Never mutate the caller's object in place -- some handlers reuse
        // the payload they pass in. Shallow-copy the fields into a fresh object.
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        if (Data.IsValid())
        {
            for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Data->Values)
            {
                Payload->SetField(Pair.Key, Pair.Value);
            }
        }

        AddVerificationEvidence(Payload, Verification, VerificationMethod, VerificationDetail);
        return MakeOutcomeReply(Payload, bSuccess, ErrorMessage);
    }

    TSharedPtr<FJsonObject> MakeVerifiedReply(
        const TSharedPtr<FJsonObject>& Data,
        bool bSuccess,
        bool bVerified,
        const FString& VerificationMethod,
        const FString& VerificationDetail,
        const FString& ErrorMessage)
    {
        return MakeVerifiedReply(
            Data,
            bSuccess,
            bVerified ? EMcpVerification::Verified : EMcpVerification::Failed,
            VerificationMethod,
            VerificationDetail,
            ErrorMessage);
    }

    // =====================================================================
    // RECURSIVE FGameplayTag GUARDRAIL -- see header for the contract.
    // =====================================================================

    namespace
    {
        bool McpValidateTagString(const FString& RawTagName, const FString& JsonPath, FString& OutError, int32& InOutChecked)
        {
            FString TagName = RawTagName;
            TagName.TrimStartAndEndInline();

            ++InOutChecked;

            if (TagName.IsEmpty())
            {
                // Empty clears the tag to the (valid) empty FGameplayTag --
                // same semantics as the top-level branch in SetObjectProperty.
                return true;
            }

            const FGameplayTag Tag = FGameplayTag::RequestGameplayTag(FName(*TagName), /*ErrorIfNotFound=*/ false);
            if (!Tag.IsValid())
            {
                OutError = FString::Printf(
                    TEXT("Unregistered gameplay tag '%s' at '%s' (not in the tag registry). ")
                    TEXT("Register it (create_gameplay_tag / a tag table) or fix the spelling; ")
                    TEXT("refusing to dead-set an invalid tag."),
                    *TagName, *JsonPath);
                return false;
            }
            return true;
        }

        // Pull the tag string out of either accepted JSON form.
        // Returns false only when the shape is not tag-shaped at all (in which
        // case there is nothing this guardrail can assert about it).
        bool McpExtractTagString(const TSharedPtr<FJsonValue>& Value, FString& OutTagName)
        {
            if (!Value.IsValid())
            {
                return false;
            }
            if (Value->Type == EJson::String)
            {
                OutTagName = Value->AsString();
                return true;
            }
            if (Value->Type == EJson::Object)
            {
                const TSharedPtr<FJsonObject> Obj = Value->AsObject();
                if (Obj.IsValid())
                {
                    if (Obj->TryGetStringField(TEXT("TagName"), OutTagName))
                    {
                        return true;
                    }
                    if (Obj->Values.Num() == 0)
                    {
                        // Empty object == clear the tag.
                        OutTagName.Reset();
                        return true;
                    }
                }
            }
            return false;
        }

        bool McpValidateTagsRecursive(
            const FProperty* Property,
            const TSharedPtr<FJsonValue>& Value,
            const FString& JsonPath,
            int32 Depth,
            FString& OutError,
            int32& InOutChecked)
        {
            if (!Property || !Value.IsValid())
            {
                return true;
            }
            if (Depth > 16)
            {
                // Depth bound. Deliberately silent-pass rather than an
                // error: refusing a legitimate deep write would be worse
                // than not validating at depth 17.
                return true;
            }

            if (const FStructProperty* StructProp = CastField<FStructProperty>(Property))
            {
                if (StructProp->Struct == FGameplayTag::StaticStruct())
                {
                    FString TagName;
                    if (!McpExtractTagString(Value, TagName))
                    {
                        return true;
                    }
                    return McpValidateTagString(TagName, JsonPath, OutError, InOutChecked);
                }

                if (StructProp->Struct == FGameplayTagContainer::StaticStruct())
                {
                    // Accepted forms: ["A.B", ...] and {"GameplayTags":["A.B", ...]}.
                    const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
                    if (Value->Type == EJson::Array)
                    {
                        Entries = &Value->AsArray();
                    }
                    else if (Value->Type == EJson::Object)
                    {
                        const TSharedPtr<FJsonObject> Obj = Value->AsObject();
                        if (Obj.IsValid())
                        {
                            Obj->TryGetArrayField(TEXT("GameplayTags"), Entries);
                        }
                    }
                    if (!Entries)
                    {
                        return true;
                    }
                    for (int32 Index = 0; Index < Entries->Num(); ++Index)
                    {
                        FString TagName;
                        if (!McpExtractTagString((*Entries)[Index], TagName))
                        {
                            continue;
                        }
                        const FString ElementPath = FString::Printf(TEXT("%s[%d]"), *JsonPath, Index);
                        if (!McpValidateTagString(TagName, ElementPath, OutError, InOutChecked))
                        {
                            return false;
                        }
                    }
                    return true;
                }

                // Plain struct: descend field by field.
                if (Value->Type != EJson::Object || !StructProp->Struct)
                {
                    return true;
                }
                const TSharedPtr<FJsonObject> Obj = Value->AsObject();
                if (!Obj.IsValid())
                {
                    return true;
                }
                const UStruct* StructType = StructProp->Struct;
                for (TFieldIterator<FProperty> It(StructType); It; ++It)
                {
                    const FProperty* Field = *It;
                    if (!Field)
                    {
                        continue;
                    }
                    // TMap<FString,...> lookups in FJsonObject are
                    // case-insensitive, so GetName() covers the casing variants
                    // FJsonObjectConverter accepts; GetAuthoredName() additionally
                    // covers Blueprint struct members whose FName carries a GUID.
                    TSharedPtr<FJsonValue> FieldValue = Obj->TryGetField(Field->GetName());
                    if (!FieldValue.IsValid())
                    {
                        FieldValue = Obj->TryGetField(Field->GetAuthoredName());
                    }
                    if (!FieldValue.IsValid())
                    {
                        continue;
                    }
                    const FString FieldPath = FString::Printf(TEXT("%s.%s"), *JsonPath, *Field->GetAuthoredName());
                    if (!McpValidateTagsRecursive(Field, FieldValue, FieldPath, Depth + 1, OutError, InOutChecked))
                    {
                        return false;
                    }
                }
                return true;
            }

            if (const FArrayProperty* ArrayProp = CastField<FArrayProperty>(Property))
            {
                if (Value->Type != EJson::Array)
                {
                    return true;
                }
                const TArray<TSharedPtr<FJsonValue>>& Entries = Value->AsArray();
                for (int32 Index = 0; Index < Entries.Num(); ++Index)
                {
                    const FString ElementPath = FString::Printf(TEXT("%s[%d]"), *JsonPath, Index);
                    if (!McpValidateTagsRecursive(ArrayProp->Inner, Entries[Index], ElementPath, Depth + 1, OutError, InOutChecked))
                    {
                        return false;
                    }
                }
                return true;
            }

            if (const FSetProperty* SetProp = CastField<FSetProperty>(Property))
            {
                if (Value->Type != EJson::Array)
                {
                    return true;
                }
                const TArray<TSharedPtr<FJsonValue>>& Entries = Value->AsArray();
                for (int32 Index = 0; Index < Entries.Num(); ++Index)
                {
                    const FString ElementPath = FString::Printf(TEXT("%s[%d]"), *JsonPath, Index);
                    if (!McpValidateTagsRecursive(SetProp->ElementProp, Entries[Index], ElementPath, Depth + 1, OutError, InOutChecked))
                    {
                        return false;
                    }
                }
                return true;
            }

            if (const FMapProperty* MapProp = CastField<FMapProperty>(Property))
            {
                if (Value->Type != EJson::Object)
                {
                    return true;
                }
                const TSharedPtr<FJsonObject> Obj = Value->AsObject();
                if (!Obj.IsValid())
                {
                    return true;
                }
                for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Obj->Values)
                {
                    // A map KEY arrives as the JSON field name (a string), so
                    // an FGameplayTag key is validated as a bare tag string.
                    if (const FStructProperty* KeyStruct = CastField<FStructProperty>(MapProp->KeyProp))
                    {
                        if (KeyStruct->Struct == FGameplayTag::StaticStruct())
                        {
                            const FString KeyPath = FString::Printf(TEXT("%s{key:%s}"), *JsonPath, *Pair.Key);
                            if (!McpValidateTagString(Pair.Key, KeyPath, OutError, InOutChecked))
                            {
                                return false;
                            }
                        }
                    }
                    const FString ValuePath = FString::Printf(TEXT("%s[%s]"), *JsonPath, *Pair.Key);
                    if (!McpValidateTagsRecursive(MapProp->ValueProp, Pair.Value, ValuePath, Depth + 1, OutError, InOutChecked))
                    {
                        return false;
                    }
                }
                return true;
            }

            return true;
        }
    }

    bool ValidateGameplayTagsInJson(
        const FProperty* Property,
        const TSharedPtr<FJsonValue>& Value,
        FString& OutError,
        int32* OutTagsChecked)
    {
        int32 Checked = 0;
        const FString RootPath = Property ? Property->GetAuthoredName() : FString(TEXT("<value>"));
        const bool bOk = McpValidateTagsRecursive(Property, Value, RootPath, /*Depth=*/0, OutError, Checked);
        if (OutTagsChecked) { *OutTagsChecked = Checked; }
        return bOk;
    }

    // =====================================================================
    // SCRIPT-REFERENCE PURGE BEFORE WORLD TEARDOWN
    // The full rationale and the three engine holes this plugs are documented on the
    // declarations in McpCommonUtils.h -- not repeated here.
    // =====================================================================

    void PurgeScriptRefs(UObject* Object)
    {
        if (Object)
        {
            // Broadcast, never call FPyReferenceCollector directly: PurgeUnrealObjectReferences
            // is private (PyReferenceCollector.h) and linking against it is LNK2019.
            // FPythonScriptPlugin listens at PythonScriptPlugin.cpp and purges with
            // bIncludeInners=true.
            //
            // LISTENER CENSUS (engine tree). A broadcast wakes EVERY listener, so this
            // matters. Those 6, and only those 6, subscribe anywhere in the engine tree:
            //   ACTIVE:
            //     * FPythonScriptPlugin (PythonScriptPlugin.cpp) -- the one we
            //       want: purges with bIncludeInners=true.
            //     * FBlutilityModule (BlutilityModule.cpp) -- built-in editor module, so
            //       always live. Also a PURGER (drops refs held by Editor Utility instances).
            //       Same semantics as the above; harmless.
            //   NOT PURGERS, and only live when their plugin is enabled:
            //     * SSequenceRecorder::HandleMapUnload (SSequenceRecorder.cpp) -- mutates UI.
            //     * VCamCore  (Util/DeferredCleanupHandler.cpp) -- deinitializes UVCamComponents.
            //     * nDisplay  (SDisplayClusterOperatorToolbar.cpp).
            //     * RemoteSession (Widgets/SRemoteSessionStream.cpp).
            // The engine itself broadcasts this for a ULevel at EditorLevelUtils.cpp, so
            // the pattern is the engine's own, not an invention.
            // !! If SequenceRecorder, VCamCore, nDisplay or RemoteSession are enabled in your
            //    project, review their handlers before trusting these call sites.
            FEditorSupportDelegates::PrepareToCleanseEditorObject.Broadcast(Object);
        }
    }

    int32 PurgeWorldsBeforeLoadMap(const FString& TargetPackageName)
    {
        if (!GEditor)
        {
            return 0;
        }

        int32 Purged = 0;
        UWorld* CurrentEditorWorld = GEditor->GetEditorWorldContext().World();

        // Purge the world AND its package: the engine's purge walks inners only
        // (ForEachObjectWithOuter), so a handle held on the UPackage itself survives --
        // and CheckForWorldGCLeaks (EditorServer.cpp) tests exactly that package.
        auto PurgeWorldAndPackage = [&Purged](UWorld* World)
        {
            if (!World || !IsValid(World))
            {
                return;
            }
            PurgeScriptRefs(World);
            if (UPackage* Pkg = World->GetOutermost())
            {
                if (Pkg != GetTransientPackage())
                {
                    PurgeScriptRefs(Pkg);
                }
            }
            ++Purged;
        };

        PurgeWorldAndPackage(CurrentEditorWorld);

        for (TObjectIterator<UWorld> It; It; ++It)
        {
            UWorld* World = *It;
            if (!World || !IsValid(World) || World == CurrentEditorWorld)
            {
                continue;
            }

            // Preview worlds are exempt in the engine's own leak check (bIsPersistantWorldType)
            // and every open asset editor / thumbnail render makes one. Never the leak.
            const EWorldType::Type WorldType = World->WorldType;
            if (WorldType == EWorldType::EditorPreview || WorldType == EWorldType::GamePreview)
            {
                continue;
            }

            UPackage* Pkg = World->GetOutermost();
            if (!Pkg || Pkg == GetTransientPackage())
            {
                continue;
            }

            // The engine's own reuse predicate, copied exactly (EditorServer.cpp,
            // IsWorldValidForReuse): an uninitialized world at the destination package is the
            // one Map_Load REUSES as NewWorld. It is not destroyed, so it must not be purged.
            if (Pkg->GetName() == TargetPackageName && !World->HasEverBeenInitialized())
            {
                continue;
            }

            PurgeWorldAndPackage(World);
        }

        return Purged;
    }

    int32 PurgePIEScriptRefs()
    {
        // The set below is exactly what PlayLevel.cpp tests before its lethal checkf
        // at :552-553. We are called from EndPIE (PlayLevel.cpp), 247 lines earlier and
        // in the same tick, with the engine's CollectGarbage still ahead of us.
        int32 PurgedRoots = 0;

        for (TObjectIterator<UPackage> It; It; ++It)
        {
            UPackage* Pkg = *It;
            if (!Pkg || !Pkg->HasAnyPackageFlags(PKG_PlayInEditor))
            {
                continue;
            }
            // Emit for the PACKAGE, not per actor: the purge walks inners, so one broadcast
            // covers world + levels + actors in a single pass. The package is also the one
            // thing the engine's own purge can never reach (it only ever walks inners).
            PurgeScriptRefs(Pkg);
            ++PurgedRoots;

            if (UWorld* PieWorld = UWorld::FindWorldInPackage(Pkg))
            {
                PurgeScriptRefs(PieWorld);
            }
        }

        // A UGameInstance is outered to the engine, not to a PIE package, so the loop above
        // cannot reach it -- yet PlayLevel.cpp tests it just the same.
        for (TObjectIterator<UGameInstance> It; It; ++It)
        {
            UGameInstance* GI = *It;
            if (GI && GI->HasAnyInternalFlags(EInternalObjectFlags::Garbage))
            {
                PurgeScriptRefs(GI);
                ++PurgedRoots;
            }
        }

        return PurgedRoots;
    }

    UObject* LoadAssetOrSoftPath(const FString& Path, UClass* Class)
    {
        if (Path.IsEmpty())
        {
            return nullptr;
        }
        UObject* Asset = StaticLoadObject(Class, nullptr, *Path);
        if (!Asset)
        {
            FSoftObjectPath SoftPath(Path);
            if (SoftPath.IsValid())
            {
                UObject* Loaded = SoftPath.TryLoad();
                Asset = Loaded && Loaded->IsA(Class) ? Loaded : nullptr;
            }
        }
        return Asset;
    }

    UObject* NewAssetInPackage(UClass* Class, const FString& PackageName, const FString& AssetName,
        const TCHAR* CreateFailedError, FString& OutError, EObjectFlags Flags)
    {
        UPackage* Package = CreateAssetPackage(PackageName, AssetName, Class, OutError);
        if (!Package)
        {
            return nullptr;
        }
        Package->FullyLoad();

        UObject* Asset = NewObject<UObject>(Package, Class, *AssetName, Flags);
        if (!Asset)
        {
            OutError = CreateFailedError;
        }
        return Asset;
    }

    FString NormalizeFolder(FString Folder, const TCHAR* DefaultFolder)
    {
        Folder.TrimStartAndEndInline();
        while (Folder.EndsWith(TEXT("/")))
        {
            Folder.LeftChopInline(1);
        }
        if (Folder.IsEmpty())
        {
            Folder = DefaultFolder;
        }
        return Folder;
    }

    AActor* FindActorByObjectName(UWorld* World, const FString& ObjectName)
    {
        if (World == nullptr)
        {
            return nullptr;
        }
        for (TActorIterator<AActor> It(World); It; ++It)
        {
            AActor* Candidate = *It;
            if (Candidate != nullptr && Candidate->GetName() == ObjectName)
            {
                return Candidate;
            }
        }
        return nullptr;
    }

    AActor* FindActorByUniqueLabel(UWorld* World, const FString& Label, int32* OutMatches)
    {
        int32 Matches = 0;
        AActor* Found = nullptr;
        if (World != nullptr && !Label.IsEmpty())
        {
            for (TActorIterator<AActor> It(World); It; ++It)
            {
                if (*It != nullptr && It->GetActorLabel() == Label)
                {
                    Found = *It;
                    ++Matches;
                }
            }
        }
        if (OutMatches)
        {
            *OutMatches = Matches;
        }
        return Matches == 1 ? Found : nullptr;
    }

    UActorComponent* FindComponentByNameOrClass(AActor* Actor, const FString& Name, TArray<FString>* OutAmbiguous)
    {
        if (OutAmbiguous)
        {
            OutAmbiguous->Reset();
        }
        if (Actor == nullptr || Name.IsEmpty())
        {
            return nullptr;
        }
        TArray<UActorComponent*> Components;
        Actor->GetComponents(Components);
        for (UActorComponent* Component : Components)
        {
            if (Component != nullptr && Component->GetName() == Name)
            {
                return Component;
            }
        }
        // By class: "USkyLightComponent" and "SkyLightComponent" name the same class.
        const FString ClassName = Name.Len() > 1 && Name[0] == TEXT('U') && FChar::IsUpper(Name[1]) ? Name.RightChop(1) : Name;
        TArray<UActorComponent*> OfClass;
        for (UActorComponent* Component : Components)
        {
            for (const UClass* Class = Component ? Component->GetClass() : nullptr; Class != nullptr; Class = Class->GetSuperClass())
            {
                if (Class->GetName() == ClassName)
                {
                    OfClass.Add(Component);
                    break;
                }
                if (Class == UActorComponent::StaticClass())
                {
                    break;
                }
            }
        }
        if (OfClass.Num() == 1)
        {
            return OfClass[0];
        }
        if (OutAmbiguous)
        {
            for (const UActorComponent* Component : OfClass)
            {
                OutAmbiguous->Add(Component->GetName());
            }
        }
        return nullptr;
    }

    UWorld* ActiveGameWorld()
    {
        UWorld* Active = GetActiveWorld();
        return Active && Active->WorldType == EWorldType::Game ? Active : nullptr;
    }

    TArray<TSharedPtr<FJsonValue>> ConcreteSubclassesToJson(UClass* BaseClass)
    {
        TArray<UClass*> Derived;
        GetDerivedClasses(BaseClass, Derived, /*bRecursive=*/true);
        Derived.Sort([](const UClass& A, const UClass& B) { return A.GetName().Compare(B.GetName()) < 0; });

        TArray<TSharedPtr<FJsonValue>> Types;
        for (UClass* Class : Derived)
        {
            if (!Class || Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
            {
                continue;
            }
            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("name"), Class->GetName());
            Entry->SetStringField(TEXT("path"), Class->GetPathName());
            Types.Add(MakeShared<FJsonValueObject>(Entry));
        }
        return Types;
    }

    FLinearColor LinearColorFromJsonArray(const TArray<TSharedPtr<FJsonValue>>* Values, const FLinearColor& Default)
    {
        FLinearColor Color = Default;
        if (Values)
        {
            for (int32 Channel = 0; Channel < 4 && Channel < Values->Num(); ++Channel)
            {
                Color.Component(Channel) = (float)(*Values)[Channel]->AsNumber();
            }
        }
        return Color;
    }

    TSharedPtr<FJsonObject> ReadJsonLinearColor(const TSharedPtr<FJsonObject>& Params, const TCHAR* Key,
        const FLinearColor& Default, FLinearColor& OutColor)
    {
        const TSharedPtr<FJsonValue> Value = Params.IsValid() ? Params->TryGetField(Key) : nullptr;
        if (!Value.IsValid() || Value->IsNull())
        {
            OutColor = Default;
            return nullptr;
        }

        // A channel is stored as a float: AsNumber would lex a string, read a bool as 0 or 1 and anything else as 0,
        // and a number past the float range would become infinite.
        auto IsChannel = [](const TSharedPtr<FJsonValue>& Item)
        {
            return Item.IsValid() && Item->Type == EJson::Number && FMath::IsFinite(Item->AsNumber())
                && FMath::Abs(Item->AsNumber()) <= static_cast<double>(TNumericLimits<float>::Max());
        };
        // What a value that is not a color, or an element that is not a channel, is.
        auto Describe = [](const TSharedPtr<FJsonValue>& Item) -> FString
        {
            if (!Item.IsValid() || Item->IsNull())
            {
                return TEXT("null");
            }
            switch (Item->Type)
            {
            case EJson::String:  return TEXT("a string");
            case EJson::Boolean: return TEXT("a boolean");
            case EJson::Object:  return TEXT("an object");
            case EJson::Array:
            {
                const int32 Count = Item->AsArray().Num();
                return FString::Printf(TEXT("an array of %d element%s"), Count, Count == 1 ? TEXT("") : TEXT("s"));
            }
            case EJson::Number:
            {
                const double Number = Item->AsNumber();
                return FMath::IsNaN(Number) ? TEXT("NaN") : !FMath::IsFinite(Number) ? TEXT("an infinite number")
                    : FMath::Abs(Number) > static_cast<double>(TNumericLimits<float>::Max()) ? TEXT("a number outside the float range")
                    : TEXT("a number");
            }
            default:             return TEXT("a value of no JSON type");
            }
        };

        FString Problem;
        const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
        if (!Value->TryGetArray(Items) || !Items || Items->Num() < 3 || Items->Num() > 4)
        {
            Problem = TEXT("got ") + Describe(Value);
        }
        for (int32 Index = 0; Problem.IsEmpty() && Index < Items->Num(); ++Index)
        {
            if (!IsChannel((*Items)[Index]))
            {
                Problem = FString::Printf(TEXT("element %d is %s"), Index, *Describe((*Items)[Index]));
            }
        }
        if (!Problem.IsEmpty())
        {
            return MakeErrorReply(FString::Printf(
                TEXT("%s must be an array of 3 or 4 finite numbers, [R, G, B] or [R, G, B, A] (%s). Nothing was changed."),
                Key, *Problem));
        }
        OutColor = LinearColorFromJsonArray(Items, Default);
        return nullptr;
    }

    TSharedPtr<FJsonObject> BlueprintNotFoundReply(const FString& Requested)
    {
        return MakeErrorReply(FString::Printf(TEXT("Blueprint not found: %s"), *Requested));
    }

    bool BuildCubeBrushForVolume(AActor* Actor, const FVector& HalfExtent)
    {
        AVolume* Volume = Cast<AVolume>(Actor);
        if (!Volume)
        {
            return false;
        }
        UCubeBuilder* Builder = NewObject<UCubeBuilder>();
        if (!Builder)
        {
            return false;
        }
        Builder->X = static_cast<float>(HalfExtent.X * 2.0);
        Builder->Y = static_cast<float>(HalfExtent.Y * 2.0);
        Builder->Z = static_cast<float>(HalfExtent.Z * 2.0);
        UActorFactory::CreateBrushForVolumeActor(Volume, Builder);
        return true;
    }

    IAssetRegistry& AssetRegistry()
    {
        return FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
    }
}