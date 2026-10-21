// Copyright (c) 2026 CarlxsMG.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Logging/TokenizedMessage.h"

/**
 * Shared Message Log helpers.
 *
 * run_map_check and get_message_log_entries both read the very same
 * FTokenizedMessage objects out of an IMessageLogListing, and both keep every
 * token the engine attached to them, not only {severity, text} -- which is what
 * answers "which of these warnings come from a placed library actor and which
 * from mine?" without a manual asset-by-asset join. Both call these helpers
 * (defined in UEAgentValidationCommands.cpp), so the extraction cannot drift
 * between the two tools.
 */
namespace mcp_message_log
{
    /**
     * Parses the optional `base_libraries` array of package roots.
     * Absent  -> the project's Read Only Content Roots (mcp_common::GetDefaultReadonlyRoots()).
     * Explicit [] -> empty list ("compare against nothing"), never silently
     * replaced by the default.
     * Trailing slashes are trimmed so "/Game/VendorPack/" and "/Game/VendorPack" match.
     *
     * NOTE: deliberately separate from mcp_common::ParseReadonlyRoots(Params, "base_libraries"),
     * even though both default to the same roots: base libraries are a classification input,
     * not the save guard, and this parser also de-duplicates the entries.
     */
    TArray<FString> ParseBaseLibraries(const TSharedPtr<FJsonObject>& Params);

    /**
     * Classifies a package/object path against the caller's base libraries.
     * Returns the LONGEST matching base library root when one matches (and sets
     * bOutIsBaseLibrary), otherwise the mount point of the path ("/Game",
     * "/Script", "/Engine", ...). Returns the DATA -- never a verdict/origin enum:
     * the tools of this plugin are engine-level and generic, so which roots mean
     * "upstream" is the caller's judgement.
     */
    FString ComputePathRoot(const FString& ObjectOrPackagePath, const TArray<FString>& BaseLibraries, bool& bOutIsBaseLibrary);

    /**
     * Appends the token-derived fields of one FTokenizedMessage onto an
     * existing entry object (OUT-param on purpose: run_map_check emits
     * {severity:number, text:string} and get_message_log_entries emits
     * {severity:string, message:string} -- returning a fresh object would have
     * forced one of the two live shapes to change).
     *
     * Adds: token_source, object_path, object_resolved, actor_name, actor_label,
     * actor_path, actor_class, class_asset_path, class_root,
     * class_root_is_base_library, component_name, actor_guid.
     * Fields whose value is genuinely unknown are OMITTED rather than emitted empty.
     *
     * NOTE: no `message_id` field. FTokenizedMessage::GetIdentifier() is NAME_None
     * for every Map Check message (the engine never calls SetIdentifier on them),
     * so the field would be permanently empty.
     */
    void TokenizedMessageToJson(const TSharedRef<FTokenizedMessage>& Message, const TArray<FString>& BaseLibraries, const TSharedPtr<FJsonObject>& OutEntry);
}
