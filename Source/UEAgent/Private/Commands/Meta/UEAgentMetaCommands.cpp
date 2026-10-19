#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "UEAgentBridge.h"
#include "Editor.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// Bridge-control-flow commands (`execute_batch`): they touch no engine subsystem, they run other commands
// through the bridge's own dispatch.
namespace UEAgentMetaCommands
{

// MCP-DOMAIN: Meta

// MCP:execute_batch - Run SEVERAL commands back-to-back inside the SAME game-thread pass, with no intermediate MCP round-trips -- the way to cut N calls to 1 when you are authoring in bulk. Any command the bridge dispatches can go in the list. By default every item runs even if an earlier one failed, and each result is reported individually; set stop_on_error to abort at the first failure, and transaction to make the whole run a single undo step.
//   @param commands [array!] - [{command_type: string, params?: object}, ...] (required, non-empty)
//   @param stop_on_error [bool] - Stop after the first failing sub-command (default false)
//   @param transaction [bool] - Wrap the whole run in one undo transaction, so a single undo reverts all of it (begin_transaction / end_transaction, or cancel_transaction instead when stop_on_error tripped on an error). Default false.
//   @returns { success: bool, results: [{index: int, command_type: string, ok: bool, payload?: any, error?: string}], stopped_early: bool }
TSharedPtr<FJsonObject> HandleBatch(const TSharedPtr<FJsonObject>& Params)
{
    // Every item runs through UUEAgentBridge::DispatchSingleCommand, on this thread and in this pass: never
    // through the queue, which is drained by the very call stack that runs the batch.
    UUEAgentBridge* Bridge = GEditor ? GEditor->GetEditorSubsystem<UUEAgentBridge>() : nullptr;
    if (!Bridge)
    {
        return mcp_common::MakeErrorReply(TEXT("execute_batch: the MCP bridge is not running"));
    }

    const TArray<TSharedPtr<FJsonValue>>* CommandsArray = nullptr;
    if (!Params.IsValid() || !Params->TryGetArrayField(TEXT("commands"), CommandsArray) || !CommandsArray || CommandsArray->Num() == 0)
    {
        return mcp_common::MakeErrorReply(TEXT("execute_batch requires a non-empty 'commands' array"));
    }

    bool bStopOnError = false;
    Params->TryGetBoolField(TEXT("stop_on_error"), bStopOnError);
    bool bTransaction = false;
    Params->TryGetBoolField(TEXT("transaction"), bTransaction);

    if (bTransaction)
    {
        TSharedPtr<FJsonObject> BeginParams = MakeShared<FJsonObject>();
        BeginParams->SetStringField(TEXT("description"), TEXT("MCP batch"));
        Bridge->DispatchSingleCommand(TEXT("begin_transaction"), BeginParams);
    }

    TArray<TSharedPtr<FJsonValue>> ResultsArray;
    bool bStoppedEarly = false;
    bool bAnyError = false;

    for (int32 Index = 0; Index < CommandsArray->Num(); ++Index)
    {
        const TSharedPtr<FJsonObject> Item = (*CommandsArray)[Index].IsValid() ? (*CommandsArray)[Index]->AsObject() : nullptr;
        TSharedPtr<FJsonObject> ItemResult = MakeShared<FJsonObject>();
        ItemResult->SetNumberField(TEXT("index"), Index);

        FString SubCommandType;
        if (!Item.IsValid() || !Item->TryGetStringField(TEXT("command_type"), SubCommandType) || SubCommandType.IsEmpty())
        {
            bAnyError = true;
            ItemResult->SetStringField(TEXT("command_type"), FString());
            ItemResult->SetBoolField(TEXT("ok"), false);
            ItemResult->SetStringField(TEXT("error"), TEXT("batch item missing required string field 'command_type'"));
            ResultsArray.Add(MakeShared<FJsonValueObject>(ItemResult));
            if (bStopOnError) { bStoppedEarly = true; break; }
            continue;
        }

        ItemResult->SetStringField(TEXT("command_type"), SubCommandType);

        // Disallow nesting: a batch item recursing into another batch adds nothing (its
        // sub-items would run in this same pass anyway) and complicates stop_on_error /
        // transaction semantics for no benefit.
        if (SubCommandType == TEXT("execute_batch"))
        {
            bAnyError = true;
            ItemResult->SetBoolField(TEXT("ok"), false);
            ItemResult->SetStringField(TEXT("error"), TEXT("nested execute_batch is not supported"));
            ResultsArray.Add(MakeShared<FJsonValueObject>(ItemResult));
            if (bStopOnError) { bStoppedEarly = true; break; }
            continue;
        }

        const TSharedPtr<FJsonObject>* SubParamsPtr = nullptr;
        TSharedPtr<FJsonObject> SubParams;
        if (Item->TryGetObjectField(TEXT("params"), SubParamsPtr) && SubParamsPtr && SubParamsPtr->IsValid())
        {
            SubParams = *SubParamsPtr;
        }
        else
        {
            SubParams = MakeShared<FJsonObject>();
        }

        const TSharedPtr<FJsonObject> SubResponse = Bridge->DispatchSingleCommand(SubCommandType, SubParams);
        FString Status;
        const bool bItemOk = SubResponse.IsValid() && SubResponse->TryGetStringField(TEXT("status"), Status) && Status == TEXT("success");
        ItemResult->SetBoolField(TEXT("ok"), bItemOk);

        if (bItemOk)
        {
            const TSharedPtr<FJsonValue> Payload = SubResponse->TryGetField(TEXT("result"));
            if (Payload.IsValid())
            {
                ItemResult->SetField(TEXT("payload"), Payload);
            }
        }
        else
        {
            bAnyError = true;
            FString ErrorMessage = TEXT("unknown error");
            if (SubResponse.IsValid())
            {
                SubResponse->TryGetStringField(TEXT("error"), ErrorMessage);
            }
            ItemResult->SetStringField(TEXT("error"), ErrorMessage);
        }

        ResultsArray.Add(MakeShared<FJsonValueObject>(ItemResult));

        if (!bItemOk && bStopOnError)
        {
            bStoppedEarly = true;
            break;
        }
    }

    if (bTransaction)
    {
        TSharedPtr<FJsonObject> EmptyParams = MakeShared<FJsonObject>();
        if (bStoppedEarly && bAnyError)
        {
            Bridge->DispatchSingleCommand(TEXT("cancel_transaction"), EmptyParams);
        }
        else
        {
            Bridge->DispatchSingleCommand(TEXT("end_transaction"), EmptyParams);
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetArrayField(TEXT("results"), ResultsArray);
    Result->SetBoolField(TEXT("stopped_early"), bStoppedEarly);
    return Result;
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("execute_batch"), &HandleBatch },
};
} // namespace UEAgentMetaCommands
