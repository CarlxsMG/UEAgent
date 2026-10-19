#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"

#include "HAL/IConsoleManager.h"
#include "Scalability.h"
#include "Interfaces/IPluginManager.h"   // set_engine_ini_value plugin_name
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"
#include "PluginDescriptor.h"

#define LOCTEXT_NAMESPACE "UEAgentCVar"

/**
 * Console-variable, scalability and engine INI commands.
 *
 * They operate on the canonical engine APIs (unlike execute_console_command, which just Execs a raw
 * string), so values are read back directly from the live state:
 *   - cvar commands        -> IConsoleManager::Get()
 *   - scalability commands -> Scalability:: API (FQualityLevels)
 *   - INI commands         -> GConfig
 *
 * Setters verify by reading the value back (set_cvars_from_json only reports what it applied); a
 * missing cvar produces an honest error.
 */
namespace UEAgentCVarCommands
{

// MCP-DOMAIN: Editor

// ---------------------------------------------------------------------------
// Helpers (file-local)
// ---------------------------------------------------------------------------
namespace
{
    /** The plugin identity set_engine_ini_value reports with plugin_name. FPluginDescriptor has no bEnabled and IPlugin
     *  no IsLoaded(), so "loaded" and "enabled" both report IsEnabled(). */
    void AddPluginIdentity(const TSharedPtr<FJsonObject>& Out, const IPlugin& Plugin)
    {
        const FPluginDescriptor& Descriptor = Plugin.GetDescriptor();
        const bool bEnabled = Plugin.IsEnabled();
        Out->SetStringField(TEXT("plugin_status"), bEnabled ? TEXT("Enabled") : TEXT("Disabled"));
        Out->SetStringField(TEXT("plugin_version"), Descriptor.VersionName);
        Out->SetStringField(TEXT("plugin_description"), Descriptor.Description);
        Out->SetStringField(TEXT("plugin_author"), Descriptor.CreatedBy);
        Out->SetStringField(TEXT("plugin_category"), Descriptor.Category);
        Out->SetBoolField(TEXT("plugin_loaded"), bEnabled);
        Out->SetBoolField(TEXT("plugin_enabled"), bEnabled);
        Out->SetStringField(TEXT("plugin_path"), Plugin.GetBaseDir());
        Out->SetStringField(TEXT("plugin_file"), Plugin.GetDescriptorFileName());
    }

    /** Build a JSON payload describing a single console variable's live state. */
    void FillCVarState(const TSharedPtr<FJsonObject>& Out, const FString& Name, IConsoleVariable* CVar)
    {
        const FString StringValue = CVar->GetString();
        Out->SetStringField(TEXT("name"), Name);
        Out->SetStringField(TEXT("value"), StringValue);
        Out->SetStringField(TEXT("string_value"), StringValue);
        Out->SetNumberField(TEXT("int_value"), CVar->GetInt());
        Out->SetNumberField(TEXT("float_value"), CVar->GetFloat());
        Out->SetBoolField(TEXT("bool_value"), CVar->GetBool());

        const TCHAR* Help = CVar->GetHelp();
        Out->SetStringField(TEXT("help"), Help ? FString(Help) : FString());

        const EConsoleVariableFlags Flags = CVar->GetFlags();
        Out->SetNumberField(TEXT("flags"), static_cast<int32>(Flags));
        const TCHAR* SetBy = GetConsoleVariableSetByName(Flags);
        Out->SetStringField(TEXT("set_by"), SetBy ? FString(SetBy) : FString());
    }

    /** Map a scalability quality token ("low"/"medium"/.../0..4) to an int level. */
    bool ParseQualityLevel(const TSharedPtr<FJsonObject>& Params, int32& OutLevel, FString& OutError)
    {
        // Numeric form first.
        double NumberValue = 0.0;
        if (Params->TryGetNumberField(TEXT("quality"), NumberValue))
        {
            OutLevel = FMath::Clamp(static_cast<int32>(NumberValue), 0, 4);
            return true;
        }

        FString Token;
        if (Params->TryGetStringField(TEXT("quality"), Token))
        {
            const FString Lower = Token.ToLower();
            // Allow a numeric string too.
            if (Lower.IsNumeric())
            {
                OutLevel = FMath::Clamp(FCString::Atoi(*Lower), 0, 4);
                return true;
            }
            if (Lower == TEXT("low"))       { OutLevel = 0; return true; }
            if (Lower == TEXT("medium"))    { OutLevel = 1; return true; }
            if (Lower == TEXT("high"))      { OutLevel = 2; return true; }
            if (Lower == TEXT("epic"))      { OutLevel = 3; return true; }
            if (Lower == TEXT("cinematic")) { OutLevel = 4; return true; }
            OutError = FString::Printf(TEXT("Unknown quality token '%s' (expected 0-4 or Low/Medium/High/Epic/Cinematic)"), *Token);
            return false;
        }

        OutError = TEXT("Missing required 'quality' parameter (0-4 or Low/Medium/High/Epic/Cinematic)");
        return false;
    }

    /** Serialize the full set of scalability levels into a JSON object. */
    void FillScalabilityLevels(const TSharedPtr<FJsonObject>& Out, const Scalability::FQualityLevels& Q)
    {
        Out->SetNumberField(TEXT("ResolutionQuality"), Q.ResolutionQuality);
        Out->SetNumberField(TEXT("ViewDistanceQuality"), Q.ViewDistanceQuality);
        Out->SetNumberField(TEXT("AntiAliasingQuality"), Q.AntiAliasingQuality);
        Out->SetNumberField(TEXT("ShadowQuality"), Q.ShadowQuality);
        Out->SetNumberField(TEXT("GlobalIlluminationQuality"), Q.GlobalIlluminationQuality);
        Out->SetNumberField(TEXT("ReflectionQuality"), Q.ReflectionQuality);
        Out->SetNumberField(TEXT("PostProcessQuality"), Q.PostProcessQuality);
        Out->SetNumberField(TEXT("TextureQuality"), Q.TextureQuality);
        Out->SetNumberField(TEXT("EffectsQuality"), Q.EffectsQuality);
        Out->SetNumberField(TEXT("FoliageQuality"), Q.FoliageQuality);
        Out->SetNumberField(TEXT("ShadingQuality"), Q.ShadingQuality);
        Out->SetNumberField(TEXT("LandscapeQuality"), Q.LandscapeQuality);
    }
}

// ---------------------------------------------------------------------------
// set_cvar
// ---------------------------------------------------------------------------
// MCP:set_cvar - Sets a console variable via IConsoleManager and reads the value back.  Uses IConsoleManager::Get().FindConsoleVariable(name)->Set(value, ECVF_SetByConsole). Honest error if the named cvar does not exist (unlike a raw console Exec which silently no-ops).
//   @param name - Console variable name, e.g. "r.ScreenPercentage", "r.Shadow.MaxResolution".
//   @param value - New value (string, number, or bool; coerced to the cvar's type).
//   @returns { set: bool, requested_value: string, name: string, value: string, string_value: string, int_value: int, float_value: float, bool_value: bool, help: string, flags: int, set_by: string }

TSharedPtr<FJsonObject> HandleSetCVar(const TSharedPtr<FJsonObject>& Params)
{
    FString Name;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("name"), Name)) { return Missing; }

    // Value may arrive as a string, number, or bool. Normalise to a string for Set().
    FString Value;
    if (!Params->TryGetStringField(TEXT("value"), Value))
    {
        double NumberValue = 0.0;
        bool BoolValue = false;
        if (Params->TryGetNumberField(TEXT("value"), NumberValue))
        {
            Value = FString::SanitizeFloat(NumberValue);
            // Preserve integer-looking values without a trailing ".0".
            if (FMath::IsNearlyEqual(NumberValue, FMath::RoundToDouble(NumberValue)))
            {
                Value = FString::Printf(TEXT("%lld"), static_cast<int64>(FMath::RoundToDouble(NumberValue)));
            }
        }
        else if (Params->TryGetBoolField(TEXT("value"), BoolValue))
        {
            Value = BoolValue ? TEXT("1") : TEXT("0");
        }
        else
        {
            return mcp_common::MakeErrorReply(TEXT("Missing required 'value' parameter"));
        }
    }

    IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(*Name);
    if (!CVar)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Console variable '%s' not found"), *Name));
    }

    CVar->Set(*Value, ECVF_SetByConsole);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("set"), true);
    Data->SetStringField(TEXT("requested_value"), Value);
    FillCVarState(Data, Name, CVar); // read-back: value/int_value/float_value/...
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// get_cvar
// ---------------------------------------------------------------------------
// MCP:get_cvar - Reads a console variable's live value, help text, and flags.
//   @param name - Console variable name, e.g. "r.ScreenPercentage".
//   @returns { name: string, value: string, string_value: string, int_value: int, float_value: float, bool_value: bool, help: string, flags: int, set_by: string }

TSharedPtr<FJsonObject> HandleGetCVar(const TSharedPtr<FJsonObject>& Params)
{
    FString Name;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("name"), Name)) { return Missing; }

    IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(*Name);
    if (!CVar)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Console variable '%s' not found"), *Name));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    FillCVarState(Data, Name, CVar);
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// list_cvars
// ---------------------------------------------------------------------------
// MCP:list_cvars - Lists registered console variables whose name contains a substring, or starts with prefix.  Iterates IConsoleManager::Get().ForEachConsoleObjectThatContains(filter) (ForEachConsoleObjectThatStartsWith(prefix) when prefix is given), collecting only console *variables* unless include_commands also lists console commands, capped at max_results. Read-only.
//   @param filter - Substring filter for cvar names (empty matches all); with prefix, a name must match both.
//   @param prefix - Optional name prefix, case-insensitive, e.g. "r.Shadow." or "t."; when given only names starting with it are listed.
//   @param include_commands - Optional bool (default false): also list console commands, entries with a name and help but no value.
//   @param max_results - Cap on results (default 200, min 1).
//   @returns { filter: string, prefix?: string, include_commands: bool, cvars: [{name, value?, help}], count: int, total_matches: int, max_results: int, truncated: bool }

TSharedPtr<FJsonObject> HandleListCVars(const TSharedPtr<FJsonObject>& Params)
{
    FString Filter;
    Params->TryGetStringField(TEXT("filter"), Filter); // empty allowed -> matches "" substring
    FString Prefix;
    Params->TryGetStringField(TEXT("prefix"), Prefix);
    bool bIncludeCommands = false;
    Params->TryGetBoolField(TEXT("include_commands"), bIncludeCommands);

    int32 MaxResults = 200;
    double MaxNumber = 0.0;
    if (Params->TryGetNumberField(TEXT("max_results"), MaxNumber))
    {
        MaxResults = FMath::Max(1, static_cast<int32>(MaxNumber));
    }

    TArray<TSharedPtr<FJsonValue>> CVarArray;
    bool bTruncated = false;
    int32 TotalMatches = 0;

    auto Visitor = [&CVarArray, &bTruncated, &TotalMatches, &Filter, &Prefix, bIncludeCommands, MaxResults](
        const TCHAR* VarName, IConsoleObject* Obj)
    {
        // Console *variables*, plus commands when asked (unregistered objects are skipped).
        IConsoleVariable* AsVar = Obj ? Obj->AsVariable() : nullptr;
        if (!AsVar && !(bIncludeCommands && Obj && Obj->AsCommand()))
        {
            return;
        }
        // With a prefix the engine walks the names starting with it, so the filter is applied here.
        const FString Name(VarName);
        if (!Prefix.IsEmpty() && (!Name.StartsWith(Prefix, ESearchCase::IgnoreCase)
            || (!Filter.IsEmpty() && !Name.Contains(Filter, ESearchCase::IgnoreCase))))
        {
            return;
        }
        ++TotalMatches;
        if (CVarArray.Num() >= MaxResults)
        {
            bTruncated = true;
            return;
        }
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("name"), Name);
        if (AsVar)
        {
            Entry->SetStringField(TEXT("value"), AsVar->GetString());
        }
        const TCHAR* Help = Obj->GetHelp();
        Entry->SetStringField(TEXT("help"), Help ? FString(Help) : FString());
        CVarArray.Add(MakeShared<FJsonValueObject>(Entry));
    };

    // The engine's "contains" walk matches nothing for an empty string, so no filter and no prefix walks every name
    // (an empty prefix matches all).
    if (Prefix.IsEmpty() && !Filter.IsEmpty())
    {
        IConsoleManager::Get().ForEachConsoleObjectThatContains(
            FConsoleObjectVisitor::CreateLambda(Visitor), *Filter);
    }
    else
    {
        IConsoleManager::Get().ForEachConsoleObjectThatStartsWith(
            FConsoleObjectVisitor::CreateLambda(Visitor), *Prefix);
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("filter"), Filter);
    if (!Prefix.IsEmpty())
    {
        Data->SetStringField(TEXT("prefix"), Prefix);
    }
    Data->SetBoolField(TEXT("include_commands"), bIncludeCommands);
    Data->SetArrayField(TEXT("cvars"), CVarArray);
    Data->SetNumberField(TEXT("count"), CVarArray.Num());
    Data->SetNumberField(TEXT("total_matches"), TotalMatches);
    Data->SetNumberField(TEXT("max_results"), MaxResults);
    Data->SetBoolField(TEXT("truncated"), bTruncated);
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_scalability_quality
// ---------------------------------------------------------------------------
// MCP:set_scalability_quality - Sets engine scalability quality via the Scalability API and reads it back.  Modifies a Scalability::FQualityLevels (starting from the current live state), then applies it with Scalability::SetQualityLevels + ApplyCachedQualityLevels.
//   @param quality - Level 0-3(/4) or one of "Low"/"Medium"/"High"/"Epic"/"Cinematic".
//   @param category - Optional category name: "ViewDistance", "AntiAliasing", "Shadows", "GlobalIllumination", "Reflection", "PostProcess", "Texture", "Effects", "Foliage", "Shading", "Landscape", or "all" (default all).
//   @returns { set: bool, requested_level: int, category: string, ResolutionQuality: int, ViewDistanceQuality: int, AntiAliasingQuality: int, ShadowQuality: int, GlobalIlluminationQuality: int, ReflectionQuality: int, PostProcessQuality: int, TextureQuality: int, EffectsQuality: int, FoliageQuality: int, ShadingQuality: int, LandscapeQuality: int }

TSharedPtr<FJsonObject> HandleSetScalabilityQuality(const TSharedPtr<FJsonObject>& Params)
{
    int32 Level = 3;
    FString ParseError;
    if (!ParseQualityLevel(Params, Level, ParseError))
    {
        return mcp_common::MakeErrorReply(ParseError);
    }

    FString Category;
    Params->TryGetStringField(TEXT("category"), Category);
    const FString CategoryLower = Category.ToLower();

    // Start from the current live state so a single-category change leaves the rest intact.
    Scalability::FQualityLevels Quality = Scalability::GetQualityLevels();

    FString AppliedCategory;
    if (Category.IsEmpty() || CategoryLower == TEXT("all"))
    {
        Quality.SetFromSingleQualityLevel(Level);
        AppliedCategory = TEXT("all");
    }
    else if (CategoryLower == TEXT("viewdistance"))            { Quality.SetViewDistanceQuality(Level);        AppliedCategory = TEXT("ViewDistance"); }
    else if (CategoryLower == TEXT("antialiasing"))           { Quality.SetAntiAliasingQuality(Level);        AppliedCategory = TEXT("AntiAliasing"); }
    else if (CategoryLower == TEXT("shadows") || CategoryLower == TEXT("shadow")) { Quality.SetShadowQuality(Level); AppliedCategory = TEXT("Shadows"); }
    else if (CategoryLower == TEXT("globalillumination") || CategoryLower == TEXT("gi")) { Quality.SetGlobalIlluminationQuality(Level); AppliedCategory = TEXT("GlobalIllumination"); }
    else if (CategoryLower == TEXT("reflection") || CategoryLower == TEXT("reflections")) { Quality.SetReflectionQuality(Level); AppliedCategory = TEXT("Reflection"); }
    else if (CategoryLower == TEXT("postprocess") || CategoryLower == TEXT("postprocessing")) { Quality.SetPostProcessQuality(Level); AppliedCategory = TEXT("PostProcess"); }
    else if (CategoryLower == TEXT("texture") || CategoryLower == TEXT("textures")) { Quality.SetTextureQuality(Level); AppliedCategory = TEXT("Texture"); }
    else if (CategoryLower == TEXT("effects"))                { Quality.SetEffectsQuality(Level);             AppliedCategory = TEXT("Effects"); }
    else if (CategoryLower == TEXT("foliage"))                { Quality.SetFoliageQuality(Level);             AppliedCategory = TEXT("Foliage"); }
    else if (CategoryLower == TEXT("shading"))                { Quality.SetShadingQuality(Level);             AppliedCategory = TEXT("Shading"); }
    else if (CategoryLower == TEXT("landscape"))              { Quality.SetLandscapeQuality(Level);           AppliedCategory = TEXT("Landscape"); }
    else
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Unknown scalability category '%s'"), *Category));
    }

    // Canonical way to push the new state (drives the underlying sg.* cvars).
    Scalability::SetQualityLevels(Quality, /*bForce=*/true);
    Scalability::ApplyCachedQualityLevels();

    // Read back the live state to confirm.
    const Scalability::FQualityLevels Verified = Scalability::GetQualityLevels();

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("set"), true);
    Data->SetNumberField(TEXT("requested_level"), Level);
    Data->SetStringField(TEXT("category"), AppliedCategory);
    FillScalabilityLevels(Data, Verified);
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// get_scalability_levels
// ---------------------------------------------------------------------------
// MCP:get_scalability_levels - Reads the current engine scalability quality levels.  Calls Scalability::GetQualityLevels().
//   @returns { ResolutionQuality: int, ViewDistanceQuality: int, AntiAliasingQuality: int, ShadowQuality: int, GlobalIlluminationQuality: int, ReflectionQuality: int, PostProcessQuality: int, TextureQuality: int, EffectsQuality: int, FoliageQuality: int, ShadingQuality: int, LandscapeQuality: int, single_quality_level: int }

TSharedPtr<FJsonObject> HandleGetScalabilityLevels(const TSharedPtr<FJsonObject>& Params)
{
    const Scalability::FQualityLevels Quality = Scalability::GetQualityLevels();

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    FillScalabilityLevels(Data, Quality);
    // The single overall level if all categories agree (-1 means "custom").
    Data->SetNumberField(TEXT("single_quality_level"), Quality.GetSingleQualityLevel());
    return mcp_common::MakeOkReply(Data);
}

// ===========================================================================
// Engine settings: bulk cvar set and direct INI access.

// MCP:set_cvars_from_json - Apply many cvars in one call. JSON shape: {"name":"value", ...}
//   @param cvars - JSON object string {"cvar":"value", ...}
//   @returns { applied, requested, errors: [{name, error}] }
TSharedPtr<FJsonObject> HandleSetCVarsFromJson(const TSharedPtr<FJsonObject>& Params)
{
    FString CvarsJson;
    if (!Params->TryGetStringField(TEXT("cvars"), CvarsJson))
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter: cvars (JSON object string)"));
    }
    TSharedPtr<FJsonObject> JsonObj;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(CvarsJson);
    if (!FJsonSerializer::Deserialize(Reader, JsonObj) || !JsonObj.IsValid())
    {
        return mcp_common::MakeErrorReply(TEXT("cvars: invalid JSON"));
    }

    int32 Applied = 0;
    TArray<TSharedPtr<FJsonValue>> ErrArr;
    IConsoleManager& CM = IConsoleManager::Get();
    for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : JsonObj->Values)
    {
        IConsoleVariable* V = CM.FindConsoleVariable(*Pair.Key);
        if (!V)
        {
            TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
            E->SetStringField(TEXT("name"), Pair.Key);
            E->SetStringField(TEXT("error"), TEXT("cvar not found"));
            ErrArr.Add(MakeShared<FJsonValueObject>(E));
            continue;
        }
        FString ValueStr;
        if (!Pair.Value->TryGetString(ValueStr))
        {
            ValueStr = Pair.Value->AsString();
        }
        V->Set(*ValueStr, ECVF_SetByCode);
        ++Applied;
    }
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetNumberField(TEXT("applied"), Applied);
    Data->SetNumberField(TEXT("requested"), JsonObj->Values.Num());
    Data->SetArrayField(TEXT("errors"), ErrArr);
    return mcp_common::MakeOkReply(Data);
}

// MCP:list_engine_ini_sections - List the section names of one INI in the merged in-memory config (GConfig), like get_engine_ini_value reads it: count, sections, found (false when GConfig does not know that INI) and file_path. Read-only.
//   @param config_file - INI base name, e.g. "Engine" (default), "Game", "Input", "Editor", "Scalability"; a trailing ".ini" is dropped.
//   @param include_base - Ignored: the code reads it but never uses it, so Base.ini sections are neither added nor removed.
//   @returns { count, sections: [string], found: bool, config_file, file_path }
TSharedPtr<FJsonObject> HandleListEngineIniSections(const TSharedPtr<FJsonObject>& Params)
{
    FString ConfigFile = TEXT("Engine");
    Params->TryGetStringField(TEXT("config_file"), ConfigFile);
    bool bIncludeBase = false;
    Params->TryGetBoolField(TEXT("include_base"), bIncludeBase);

    if (!GConfig)
    {
        return mcp_common::MakeErrorReply(TEXT("GConfig unavailable"));
    }
    // GConfig resolves a known config (Engine, Game, ...) only by its BARE name: a name ending in
    // ".ini" is looked up among the other files instead and finds nothing (see get_engine_ini_value).
    ConfigFile.RemoveFromEnd(TEXT(".ini"), ESearchCase::IgnoreCase);
    const FString FilePath = FPaths::GeneratedConfigDir() / ConfigFile + TEXT(".ini");

    TArray<FString> SectionNames;
    const bool bFound = GConfig->GetSectionNames(ConfigFile, SectionNames);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetNumberField(TEXT("count"), SectionNames.Num());
    Data->SetBoolField(TEXT("found"), bFound);
    Data->SetStringField(TEXT("config_file"), ConfigFile);
    Data->SetStringField(TEXT("file_path"), FilePath);
    Data->SetArrayField(TEXT("sections"), mcp_common::StringsToJson(SectionNames));
    return mcp_common::MakeOkReply(Data);
}

// MCP:get_engine_ini_value - Read one key from the merged in-memory config (GConfig) of an INI such as Engine or Game. Read-only; returns value and found (false, with an empty value, when the section or key is missing). Pass config_file without .ini. Writer: set_engine_ini_value.
//   @param section - Section name (e.g. "/Script/Engine.RendererSettings")
//   @param key - Key inside the section, e.g. "r.DefaultFeature.AntiAliasing".
//   @param config_file - Config base name without extension, e.g. "Engine" (default), "Game", "Input", "Editor", "Scalability"; "Engine.ini" is not resolved.
//   @returns { section, key, value, found }
TSharedPtr<FJsonObject> HandleGetEngineIniValue(const TSharedPtr<FJsonObject>& Params)
{
    FString Section, Key, ConfigFile = TEXT("Engine");
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("section"), Section)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("key"), Key)) { return Missing; }
    Params->TryGetStringField(TEXT("config_file"), ConfigFile);

    if (!GConfig)
    {
        return mcp_common::MakeErrorReply(TEXT("GConfig unavailable"));
    }
    // GConfig/FConfigCacheIni::FindConfigFile only recognizes the BARE
    // KnownFiles name ("Engine", "Game", "Input", "Scalability", ...); a name with
    // ".ini" appended falls through to the (empty) OtherFiles branch and silently
    // fails to resolve the real config, so GetString would report found=false even
    // when the key genuinely exists.
    FString Value;
    bool bFound = GConfig->GetString(*Section, *Key, Value, ConfigFile);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("section"), Section);
    Data->SetStringField(TEXT("key"), Key);
    Data->SetStringField(TEXT("value"), Value);
    Data->SetBoolField(TEXT("found"), bFound);
    return mcp_common::MakeOkReply(Data);
}

// MCP:set_engine_ini_value - Write one key into the GConfig copy of an INI (Engine by default), flush it to the saved config under Saved/Config/<Platform>/, and verify by reading it back (applied). It does not edit Config/Default*.ini, and objects that already loaded their config are not reloaded. The value may be text, a bool or a number; with plugin_name it writes a plugin's setting, the section defaulting to the plugin's /Script/<Plugin>.<Plugin>.
//   @param section - INI section, e.g. "/Script/Engine.RendererSettings"; created when missing; surrounding brackets are dropped. Required unless plugin_name is given.
//   @param key - Key inside the section, e.g. "r.DefaultFeature.AntiAliasing"; its current value is replaced.
//   @param value - Value to write: a string written verbatim (an empty string is allowed), a JSON bool written as GConfig writes one ("True"/"False"), or a JSON number written as its text (an integer without decimals).
//   @param config_file - Config base name without extension, e.g. "Engine" (default), "Game", "Input"; "Engine.ini" is not resolved.
//   @param plugin_name - Optional plugin, as its .uplugin is named: an unknown one fails with nothing written; a known one adds its identity to the reply and makes section default to /Script/<Plugin>.<Plugin>.
//   @returns { applied, section, key, value, value_type: "string"|"bool"|"number", readback_value, config_file, config_file_path, plugin_name?, plugin_status?, plugin_version?, plugin_description?, plugin_author?, plugin_category?, plugin_loaded?, plugin_enabled?, plugin_path?, plugin_file? }
TSharedPtr<FJsonObject> HandleSetEngineIniValue(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params->HasField(TEXT("value")))
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter 'value' (a string, bool or number)"));
    }
    FString Section, Key, ConfigFile = TEXT("Engine"), PluginName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("key"), Key)) { return Missing; }
    Params->TryGetStringField(TEXT("section"), Section);
    Params->TryGetStringField(TEXT("config_file"), ConfigFile);
    Params->TryGetStringField(TEXT("plugin_name"), PluginName);

    // The value as the INI text it becomes. The JSON type is read, not guessed from the text, so the
    // string "true" is still written verbatim.
    const TSharedPtr<FJsonValue> ValueField = Params->TryGetField(TEXT("value"));
    FString Value;
    const TCHAR* ValueType = TEXT("string");
    switch (ValueField.IsValid() ? ValueField->Type : EJson::None)
    {
    case EJson::String:
        Value = ValueField->AsString();
        break;
    case EJson::Boolean:
        // What GConfig->SetBool writes.
        Value = ValueField->AsBool() ? TEXT("True") : TEXT("False");
        ValueType = TEXT("bool");
        break;
    case EJson::Number:
    {
        // An integral number is written without a decimal part, so an int setting parses back.
        const double Number = ValueField->AsNumber();
        const bool bIntegral = FMath::IsFinite(Number) && FMath::Abs(Number) < 9.0e15 && FMath::FloorToDouble(Number) == Number;
        Value = bIntegral ? FString::Printf(TEXT("%lld"), static_cast<int64>(Number)) : FString::SanitizeFloat(Number);
        ValueType = TEXT("number");
        break;
    }
    default:
        return mcp_common::MakeErrorReply(TEXT("Parameter 'value' must be a string, bool or number; nothing was written"));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    if (!PluginName.IsEmpty())
    {
        const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(PluginName);
        Data->SetStringField(TEXT("plugin_name"), PluginName);
        if (!Plugin.IsValid())
        {
            Data->SetBoolField(TEXT("applied"), false);
            Data->SetStringField(TEXT("plugin_status"), TEXT("Not Found"));
            return mcp_common::MakeOutcomeReply(Data, false, FString::Printf(
                TEXT("set_engine_ini_value: no plugin named '%s' is known to the plugin manager; nothing was written."), *PluginName));
        }
        AddPluginIdentity(Data, *Plugin);
        if (Section.IsEmpty())
        {
            Section = FString::Printf(TEXT("/Script/%s.%s"), *PluginName, *PluginName);
        }
    }
    // GConfig wants the section bare; a bracketed "[...]" form is tolerated.
    Section.RemoveFromStart(TEXT("["));
    Section.RemoveFromEnd(TEXT("]"));
    if (Section.IsEmpty())
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter 'section' (it defaults only when plugin_name is given)"));
    }

    if (!GConfig)
    {
        return mcp_common::MakeErrorReply(TEXT("GConfig unavailable"));
    }
    // Pass the BARE config file name to GConfig, same as get_engine_ini_value
    // above -- ConfigFile + ".ini" resolves (or fails to resolve) an OtherFiles entry
    // instead of the real KnownFiles config, so SetString would be a silent no-op
    // (if (!File) return;) while the handler would report applied:true.
    GConfig->SetString(*Section, *Key, *Value, ConfigFile);
    GConfig->Flush(false, ConfigFile);

    // Verify the write really took effect (read-back) before reporting applied:true.
    FString ReadBackValue;
    const bool bApplied = GConfig->GetString(*Section, *Key, ReadBackValue, ConfigFile)
        && ReadBackValue == Value;

    Data->SetBoolField(TEXT("applied"), bApplied);
    Data->SetStringField(TEXT("section"), Section);
    Data->SetStringField(TEXT("key"), Key);
    Data->SetStringField(TEXT("value"), Value);
    Data->SetStringField(TEXT("value_type"), ValueType);
    Data->SetStringField(TEXT("readback_value"), ReadBackValue);
    Data->SetStringField(TEXT("config_file"), ConfigFile);
    // Where the flush writes: the saved (generated) config of this platform.
    Data->SetStringField(TEXT("config_file_path"),
        FConfigCacheIni::GetDestIniFilename(*ConfigFile, nullptr, *FPaths::GeneratedConfigDir()));
    return mcp_common::MakeOkReply(Data);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("get_cvar"), &HandleGetCVar },
    { TEXT("get_engine_ini_value"), &HandleGetEngineIniValue },
    { TEXT("get_scalability_levels"), &HandleGetScalabilityLevels },
    { TEXT("list_cvars"), &HandleListCVars },
    { TEXT("list_engine_ini_sections"), &HandleListEngineIniSections },
    { TEXT("set_cvar"), &HandleSetCVar },
    { TEXT("set_cvars_from_json"), &HandleSetCVarsFromJson },
    { TEXT("set_engine_ini_value"), &HandleSetEngineIniValue },
    { TEXT("set_scalability_quality"), &HandleSetScalabilityQuality },
};
} // namespace UEAgentCVarCommands

#undef LOCTEXT_NAMESPACE
