// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
// Shared message-log token extraction (mcp_message_log::TokenizedMessageToJson).
// Declared with the Validation domain because run_map_check reads the very same
// FTokenizedMessage objects; one implementation, no drift between the two tools.
#include "Commands/Editor/McpMessageLog.h"

// Slate notifications (Runtime/Slate, public).
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"

// Engine message log (FMessageLog lives in Runtime/Core, CORE_API).
#include "Logging/MessageLog.h"
#include "Logging/TokenizedMessage.h"

// Message log UI module (Developer/MessageLog, editor dep).
#include "MessageLogModule.h"
#include "IMessageLogListing.h"

#include "Modules/ModuleManager.h"
#include "Internationalization/Text.h"
#include "Async/Async.h"
#include "Async/TaskGraphInterfaces.h"

/**
 * Editor notifications & message log commands.
 *
 * Exposes the Slate toast/notification system and the engine message log over
 * the MCP bridge:
 *   - show_notification        FSlateNotificationManager::Get().AddNotification
 *   - print_to_output_log      UE_LOG(LogTemp, <verbosity>, ...)
 *   - add_message_log_entry    FMessageLog(LogName).Message(severity, text)
 *   - open_message_log         FMessageLogModule::OpenMessageLog(LogName)
 *   - clear_message_log        IMessageLogListing::ClearMessages()
 *   - get_message_log_entries  IMessageLogListing::GetFilteredMessages() (reader, pairs with add/clear)
 *
 * get_message_log_entries resolves each message's attached FUObjectToken/FActorToken
 * through mcp_message_log::TokenizedMessageToJson (declared in
 * Commands/Editor/McpMessageLog.h), the same extraction run_map_check uses -- one
 * implementation for both readers of the same listings.
 */
namespace UEAgentNotificationCommands
{

// MCP-DOMAIN: Editor

namespace
{
    /**
     * The listing LogName names in the editor message log (created on demand), with the MessageLog module loaded
     * first when it is not. Null on success, else the error reply.
     */
    TSharedPtr<FJsonObject> Notification_GetLogListing(const FString& LogName, TSharedPtr<IMessageLogListing>& OutListing)
    {
        if (!FModuleManager::Get().IsModuleLoaded(TEXT("MessageLog"))
            && FModuleManager::Get().LoadModule(TEXT("MessageLog")) == nullptr)
        {
            return mcp_common::MakeErrorReply(TEXT("MessageLog module is not available"));
        }

        FMessageLogModule& MessageLogModule = FModuleManager::LoadModuleChecked<FMessageLogModule>(TEXT("MessageLog"));
        OutListing = MessageLogModule.GetLogListing(FName(*LogName));
        return nullptr;
    }
}

// ---------------------------------------------------------------------------
// show_notification
// ---------------------------------------------------------------------------
// MCP:show_notification - Show a transient toast in the editor through the Slate notification manager: message, an optional completion state icon and a lifetime in seconds, after which it fades on its own. completion_state is matched without case, and anything other than Pending, Success or Fail shows no state; the reply echoes it as sent, not as applied. Each call shows another toast, so it is not idempotent, and nothing else changes: no asset, log line or undo step. shown is false when the notification manager returned no toast, and the call still succeeds. An empty or missing message is an error. For a record that stays use print_to_output_log or add_message_log_entry.
//   @param message - The text to display in the notification; empty is an error.
//   @param completion_state - Completion state, case-insensitive: "None" (default), "Pending", "Success" or "Fail"; any other value counts as "None".
//   @param duration - Notification lifetime in seconds before it fades (default 4.0); the value is not validated.
//   @returns { shown: bool, message: string, completion_state: string, duration: float }
//   @hints additive

TSharedPtr<FJsonObject> HandleShowNotification(const TSharedPtr<FJsonObject>& Params)
{
    FString Message;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("message"), Message)) { return Missing; }

    FString CompletionStateStr = TEXT("None");
    Params->TryGetStringField(TEXT("completion_state"), CompletionStateStr);

    // Map the requested completion state string to SNotificationItem::ECompletionState.
    SNotificationItem::ECompletionState CompletionState = SNotificationItem::CS_None;
    bool bSetCompletionState = false;
    if (CompletionStateStr.Equals(TEXT("Pending"), ESearchCase::IgnoreCase))
    {
        CompletionState = SNotificationItem::CS_Pending;
        bSetCompletionState = true;
    }
    else if (CompletionStateStr.Equals(TEXT("Success"), ESearchCase::IgnoreCase))
    {
        CompletionState = SNotificationItem::CS_Success;
        bSetCompletionState = true;
    }
    else if (CompletionStateStr.Equals(TEXT("Fail"), ESearchCase::IgnoreCase))
    {
        CompletionState = SNotificationItem::CS_Fail;
        bSetCompletionState = true;
    }
    else
    {
        // "None" (or anything unrecognized) leaves the default CS_None.
        CompletionState = SNotificationItem::CS_None;
        bSetCompletionState = false;
    }

    double Duration = 4.0;
    double DurationField = 0.0;
    if (Params->TryGetNumberField(TEXT("duration"), DurationField))
    {
        Duration = DurationField;
    }

    // FNotificationInfo + AddNotification must run on the game thread. The bridge
    // runs commands from its FTSTicker drain on the game thread, so we should
    // already be on it here; marshal defensively if not.
    bool bShown = false;
    auto ShowOnGameThread = [&]()
    {
        FNotificationInfo Info(FText::FromString(Message));
        Info.ExpireDuration = static_cast<float>(Duration);
        Info.bFireAndForget = true;
        Info.bUseLargeFont = false;

        TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info);
        if (Item.IsValid())
        {
            if (bSetCompletionState)
            {
                Item->SetCompletionState(CompletionState);
            }
            bShown = true;
        }
    };

    if (IsInGameThread())
    {
        ShowOnGameThread();
    }
    else
    {
        FGraphEventRef Task = FFunctionGraphTask::CreateAndDispatchWhenReady(
            [&ShowOnGameThread]() { ShowOnGameThread(); },
            TStatId(), nullptr, ENamedThreads::GameThread);
        FTaskGraphInterface::Get().WaitUntilTaskCompletes(Task);
    }

    TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
    Response->SetBoolField(TEXT("shown"), bShown);
    Response->SetStringField(TEXT("message"), Message);
    Response->SetStringField(TEXT("completion_state"), CompletionStateStr);
    Response->SetNumberField(TEXT("duration"), Duration);
    return mcp_common::MakeOkReply(Response);
}

// ---------------------------------------------------------------------------
// print_to_output_log
// ---------------------------------------------------------------------------
// MCP:print_to_output_log - Write one line to the editor Output Log under the LogTemp category, at Log, Warning or Error verbosity; verbosity is matched without case, anything else logs at Log, and the reply's verbosity is the level actually used. An empty or missing message is an error. Each call appends another line, so it is not idempotent, and nothing else changes: no asset, no undo step, no Message Log entry; like any UE_LOG line it also reaches the editor log file. Use it to mark a point in the log between other calls; read lines back with get_output_log_lines; a listing in Window > Message Log is add_message_log_entry, an on-screen toast show_notification.
//   @param message - The text to log; empty is an error.
//   @param verbosity - UE_LOG verbosity: "Log" (default), "Warning", "Error"; matched without case, and any other value logs at Log.
//   @returns { logged: bool, message: string, verbosity: string }
//   @hints additive

TSharedPtr<FJsonObject> HandlePrintToOutputLog(const TSharedPtr<FJsonObject>& Params)
{
    FString Message;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("message"), Message)) { return Missing; }

    FString Verbosity = TEXT("Log");
    Params->TryGetStringField(TEXT("verbosity"), Verbosity);

    // UE_LOG's verbosity is a compile-time token, so branch on the requested
    // string and emit the matching macro. Default to Log for unknown values.
    FString AppliedVerbosity;
    if (Verbosity.Equals(TEXT("Warning"), ESearchCase::IgnoreCase))
    {
        UE_LOG(LogTemp, Warning, TEXT("%s"), *Message);
        AppliedVerbosity = TEXT("Warning");
    }
    else if (Verbosity.Equals(TEXT("Error"), ESearchCase::IgnoreCase))
    {
        UE_LOG(LogTemp, Error, TEXT("%s"), *Message);
        AppliedVerbosity = TEXT("Error");
    }
    else
    {
        UE_LOG(LogTemp, Log, TEXT("%s"), *Message);
        AppliedVerbosity = TEXT("Log");
    }

    TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
    Response->SetBoolField(TEXT("logged"), true);
    Response->SetStringField(TEXT("message"), Message);
    Response->SetStringField(TEXT("verbosity"), AppliedVerbosity);
    return mcp_common::MakeOkReply(Response);
}

// ---------------------------------------------------------------------------
// add_message_log_entry
// ---------------------------------------------------------------------------
// MCP:add_message_log_entry - Add one message to a named editor Message Log listing (for example "PIE", "MapCheck" or a new name, created on demand) at Info, Warning or Error severity; an unrecognised severity becomes Info, and a missing log_name or message is an error. Entries stay in memory for the editor session and are never saved. Use it for diagnostics meant for Window > Message Log; print_to_output_log writes to the Output Log instead. message_count is the listing's message count after the add. Read entries back with get_message_log_entries, show the window with open_message_log, empty the listing with clear_message_log.
//   @param log_name - The message log listing name (e.g. "PIE", "MapCheck", or a custom name).
//   @param message - The message text to add.
//   @param severity - EMessageSeverity token: "Info" (default), "Warning", "Error".
//   @returns { added: bool, log_name: string, message: string, severity: string, message_count: int }

TSharedPtr<FJsonObject> HandleAddMessageLogEntry(const TSharedPtr<FJsonObject>& Params)
{
    FString LogName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("log_name"), LogName)) { return Missing; }

    FString Message;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("message"), Message)) { return Missing; }

    FString SeverityStr = TEXT("Info");
    Params->TryGetStringField(TEXT("severity"), SeverityStr);

    // EMessageSeverity: Error=1, PerformanceWarning=2, Warning=3, Info=4.
    EMessageSeverity::Type Severity = EMessageSeverity::Info;
    FString AppliedSeverity;
    if (SeverityStr.Equals(TEXT("Error"), ESearchCase::IgnoreCase))
    {
        Severity = EMessageSeverity::Error;
        AppliedSeverity = TEXT("Error");
    }
    else if (SeverityStr.Equals(TEXT("Warning"), ESearchCase::IgnoreCase))
    {
        Severity = EMessageSeverity::Warning;
        AppliedSeverity = TEXT("Warning");
    }
    else
    {
        Severity = EMessageSeverity::Info;
        AppliedSeverity = TEXT("Info");
    }

    // FMessageLog buffers messages; Flush() pushes them to the listing and the
    // output log. The listing is created on demand if it does not yet exist.
    {
        // Named intermediate avoids the most-vexing-parse (FName(*LogName) with the
        // '*' would otherwise be read as a pointer-parameter -> function declaration).
        const FName LogFName(*LogName);
        FMessageLog MsgLog(LogFName);
        MsgLog.Message(Severity, FText::FromString(Message));
        MsgLog.Flush();
    }

    // Read back the listing message count so the caller has a real effect to
    // verify (the listing is registered/created lazily by the module).
    int32 MessageCount = 0;
    if (FModuleManager::Get().IsModuleLoaded(TEXT("MessageLog")))
    {
        FMessageLogModule& MessageLogModule = FModuleManager::LoadModuleChecked<FMessageLogModule>(TEXT("MessageLog"));
        TSharedRef<IMessageLogListing> Listing = MessageLogModule.GetLogListing(FName(*LogName));
        MessageCount = Listing->GetFilteredMessages().Num();
    }

    TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
    Response->SetBoolField(TEXT("added"), true);
    Response->SetStringField(TEXT("log_name"), LogName);
    Response->SetStringField(TEXT("message"), Message);
    Response->SetStringField(TEXT("severity"), AppliedSeverity);
    Response->SetNumberField(TEXT("message_count"), MessageCount);
    return mcp_common::MakeOkReply(Response);
}

// ---------------------------------------------------------------------------
// open_message_log
// ---------------------------------------------------------------------------
// MCP:open_message_log - Open or focus the editor Message Log window on one listing (PIE by default), creating the listing empty when it does not exist yet and enabling message log display; no message is changed. opened only says the request was made: without an editor UI nothing shows. module_available says whether the MessageLog module loaded. To read the messages use get_message_log_entries rather than the window; add or empty them with add_message_log_entry and clear_message_log.
//   @param log_name - Listing to surface (default "PIE").
//   @returns { opened: bool, log_name: string, module_available: bool }
//   @hints additive idempotent

TSharedPtr<FJsonObject> HandleOpenMessageLog(const TSharedPtr<FJsonObject>& Params)
{
    FString LogName = TEXT("PIE");
    Params->TryGetStringField(TEXT("log_name"), LogName);

    bool bOpened = false;
    bool bModuleAvailable = FModuleManager::Get().IsModuleLoaded(TEXT("MessageLog"))
        || FModuleManager::Get().LoadModule(TEXT("MessageLog")) != nullptr;

    if (bModuleAvailable)
    {
        FMessageLogModule& MessageLogModule = FModuleManager::LoadModuleChecked<FMessageLogModule>(TEXT("MessageLog"));

        // Ensure the listing exists (created on demand) and that display is enabled,
        // then attempt to surface the message log window/tab.
        MessageLogModule.GetLogListing(FName(*LogName));
        MessageLogModule.EnableMessageLogDisplay(true);
        MessageLogModule.OpenMessageLog(FName(*LogName));
        bOpened = true;
    }

    TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
    // OpenMessageLog requests the UI to surface the tab; this can be a no-op in a
    // headless/no-UI session, hence we report whether the open was requested and
    // whether the module was available to honor it.
    Response->SetBoolField(TEXT("opened"), bOpened);
    Response->SetStringField(TEXT("log_name"), LogName);
    Response->SetBoolField(TEXT("module_available"), bModuleAvailable);
    return mcp_common::MakeOkReply(Response);
}

// ---------------------------------------------------------------------------
// clear_message_log
// ---------------------------------------------------------------------------
// MCP:clear_message_log - Clear all messages from a named engine Message Log listing.
//   @param log_name - The message log listing name (e.g. "PIE", "MapCheck", or a custom name).
//   @returns { cleared: bool, log_name: string, cleared_count: int, messages_after: int }

TSharedPtr<FJsonObject> HandleClearMessageLog(const TSharedPtr<FJsonObject>& Params)
{
    FString LogName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("log_name"), LogName)) { return Missing; }

    // The same listing add_message_log_entry reads back. IMessageLogListing::ClearMessages()
    // empties the current page in place.
    TSharedPtr<IMessageLogListing> Listing;
    if (TSharedPtr<FJsonObject> Error = Notification_GetLogListing(LogName, Listing)) { return Error; }

    // Snapshot the pre-clear count so the caller has a concrete effect to verify;
    // clearing an already-empty listing is a valid no-op (cleared_count == 0).
    const int32 ClearedCount = Listing->GetFilteredMessages().Num();
    Listing->ClearMessages();
    const int32 MessagesAfter = Listing->GetFilteredMessages().Num();

    TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
    Response->SetBoolField(TEXT("cleared"), true);
    Response->SetStringField(TEXT("log_name"), LogName);
    Response->SetNumberField(TEXT("cleared_count"), ClearedCount);
    Response->SetNumberField(TEXT("messages_after"), MessagesAfter);
    return mcp_common::MakeOkReply(Response);
}

// ---------------------------------------------------------------------------
// get_message_log_entries
// ---------------------------------------------------------------------------
// MCP:get_message_log_entries - Read back the messages currently in a named engine Message Log listing (IMessageLogListing::GetFilteredMessages), each resolved back to the actor/component the engine attached to it. The reader that pairs with add_message_log_entry/clear_message_log.
//   @param log_name - The message log listing name (e.g. "PIE", "MapCheck", or a custom name). Required.
//   @param severity - Optional severity filter: "Error"|"PerformanceWarning"|"Warning"|"Info" (case-insensitive). Omit for all.
//   @param base_libraries [array] - Package roots to classify each entry's class against (optional, default: the project's Read Only Content Roots). An explicit [] disables the classification. Reported as data in class_root/class_root_is_base_library; the tool never judges which root is "upstream".
//   @returns { log_name: string, severity_filter: string|null, base_libraries: [string], entries: [{message: string, severity: string, token_source: string, object_path: string, object_resolved: bool, actor_name: string, actor_label: string, actor_path: string, actor_class: string, actor_guid: string, component_name: string, class_asset_path: string, class_root: string, class_root_is_base_library: bool}], count: int }

TSharedPtr<FJsonObject> HandleGetMessageLogEntries(const TSharedPtr<FJsonObject>& Params)
{
    FString LogName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("log_name"), LogName)) { return Missing; }

    FString SeverityFilterStr;
    const bool bHasSeverityFilter = Params->TryGetStringField(TEXT("severity"), SeverityFilterStr) && !SeverityFilterStr.IsEmpty();
    EMessageSeverity::Type SeverityFilter = EMessageSeverity::Info;
    if (bHasSeverityFilter)
    {
        if (SeverityFilterStr.Equals(TEXT("Error"), ESearchCase::IgnoreCase))                    { SeverityFilter = EMessageSeverity::Error; }
        else if (SeverityFilterStr.Equals(TEXT("PerformanceWarning"), ESearchCase::IgnoreCase))   { SeverityFilter = EMessageSeverity::PerformanceWarning; }
        else if (SeverityFilterStr.Equals(TEXT("Warning"), ESearchCase::IgnoreCase))              { SeverityFilter = EMessageSeverity::Warning; }
        else if (SeverityFilterStr.Equals(TEXT("Info"), ESearchCase::IgnoreCase))                 { SeverityFilter = EMessageSeverity::Info; }
        else
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Unknown severity filter: %s"), *SeverityFilterStr));
        }
    }

    // Optional package roots the per-entry class path is classified against.
    const TArray<FString> BaseLibraries = mcp_message_log::ParseBaseLibraries(Params);

    TSharedPtr<IMessageLogListing> Listing;
    if (TSharedPtr<FJsonObject> Error = Notification_GetLogListing(LogName, Listing)) { return Error; }

    TArray<TSharedPtr<FJsonValue>> Entries;
    for (const TSharedRef<FTokenizedMessage>& Message : Listing->GetFilteredMessages())
    {
        const EMessageSeverity::Type ThisSeverity = Message->GetSeverity();
        if (bHasSeverityFilter && ThisSeverity != SeverityFilter)
        {
            continue;
        }

        FString SeverityStr;
        switch (ThisSeverity)
        {
            case EMessageSeverity::Error:               SeverityStr = TEXT("Error"); break;
            case EMessageSeverity::PerformanceWarning:  SeverityStr = TEXT("PerformanceWarning"); break;
            case EMessageSeverity::Warning:              SeverityStr = TEXT("Warning"); break;
            default:                                     SeverityStr = TEXT("Info"); break;
        }

        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("message"), Message->ToText().ToString());
        Entry->SetStringField(TEXT("severity"), SeverityStr);
        // Resolve the FUObjectToken/FActorToken the engine attached to the entry
        // (shared with run_map_check) instead of throwing it away.
        mcp_message_log::TokenizedMessageToJson(Message, BaseLibraries, Entry);
        Entries.Add(MakeShared<FJsonValueObject>(Entry));
    }

    TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
    Response->SetStringField(TEXT("log_name"), LogName);
    if (bHasSeverityFilter)
    {
        Response->SetStringField(TEXT("severity_filter"), SeverityFilterStr);
    }
    else
    {
        Response->SetField(TEXT("severity_filter"), MakeShared<FJsonValueNull>());
    }
    Response->SetArrayField(TEXT("base_libraries"), mcp_common::StringsToJson(BaseLibraries));
    Response->SetArrayField(TEXT("entries"), Entries);
    Response->SetNumberField(TEXT("count"), Entries.Num());
    return mcp_common::MakeOkReply(Response);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("add_message_log_entry"), &HandleAddMessageLogEntry },
    { TEXT("clear_message_log"), &HandleClearMessageLog },
    { TEXT("get_message_log_entries"), &HandleGetMessageLogEntries },
    { TEXT("open_message_log"), &HandleOpenMessageLog },
    { TEXT("print_to_output_log"), &HandlePrintToOutputLog },
    { TEXT("show_notification"), &HandleShowNotification },
};
} // namespace UEAgentNotificationCommands
