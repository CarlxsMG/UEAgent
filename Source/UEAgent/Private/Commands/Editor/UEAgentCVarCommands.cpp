#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"

#include "HAL/IConsoleManager.h"
#include "Scalability.h"

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
// MCP:list_cvars - Lists registered console variables whose name contains a substring.  Iterates IConsoleManager::Get().ForEachConsoleObjectThatContains(filter), collecting only console *variables* (commands are skipped), capped at max_results.
//   @param filter - Substring filter for cvar names (empty matches all).
//   @param max_results - Cap on results (default 200, min 1).
//   @returns { filter: string, cvars: [{name, value, help}], count: int, total_matches: int, max_results: int, truncated: bool }

TSharedPtr<FJsonObject> HandleListCVars(const TSharedPtr<FJsonObject>& Params)
{
    FString Filter;
    Params->TryGetStringField(TEXT("filter"), Filter); // empty allowed -> matches "" substring

    int32 MaxResults = 200;
    double MaxNumber = 0.0;
    if (Params->TryGetNumberField(TEXT("max_results"), MaxNumber))
    {
        MaxResults = FMath::Max(1, static_cast<int32>(MaxNumber));
    }

    TArray<TSharedPtr<FJsonValue>> CVarArray;
    bool bTruncated = false;
    int32 TotalMatches = 0;

    auto Visitor = [&CVarArray, &bTruncated, &TotalMatches, MaxResults](const TCHAR* VarName, IConsoleObject* Obj)
    {
        // Only collect console *variables* (skip commands / unregistered objects).
        IConsoleVariable* AsVar = Obj ? Obj->AsVariable() : nullptr;
        if (!AsVar)
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
        Entry->SetStringField(TEXT("name"), VarName);
        Entry->SetStringField(TEXT("value"), AsVar->GetString());
        const TCHAR* Help = AsVar->GetHelp();
        Entry->SetStringField(TEXT("help"), Help ? FString(Help) : FString());
        CVarArray.Add(MakeShared<FJsonValueObject>(Entry));
    };

    IConsoleManager::Get().ForEachConsoleObjectThatContains(
        FConsoleObjectVisitor::CreateLambda(Visitor), *Filter);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("filter"), Filter);
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
// Engine settings: cvar search, bulk cvar set and direct INI access.

// ---------------------------------------------------------------------------
// search_cvars
// ---------------------------------------------------------------------------

// MCP:search_cvars - Search cvar names by substring (read-only).
//   @param term - Substring to search (case-insensitive)
//   @param max - Max results (default 50)
//   @returns { count, cvars: [{name, value, help}] }
TSharedPtr<FJsonObject> HandleSearchCVars(const TSharedPtr<FJsonObject>& Params)
{
    FString Term;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("term"), Term)) { return Missing; }
    int32 Max = 50;
    Params->TryGetNumberField(TEXT("max"), Max);
    Max = FMath::Max(1, Max);

    TArray<TSharedPtr<FJsonValue>> Out;
    IConsoleManager& CM = IConsoleManager::Get();
    CM.ForEachConsoleObjectThatContains(FConsoleObjectVisitor::CreateLambda(
        [&](const TCHAR* Name, IConsoleObject* Obj)
        {
            if (Out.Num() >= Max) { return; }
            const FString N(Name);
            if (!N.Contains(Term, ESearchCase::IgnoreCase)) { return; }
            TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
            O->SetStringField(TEXT("name"), N);
            if (IConsoleVariable* V = CM.FindConsoleVariable(Name))
            {
                O->SetStringField(TEXT("value"), V->GetString());
            }
            Out.Add(MakeShared<FJsonValueObject>(O));
        }), *Term);
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetNumberField(TEXT("count"), Out.Num());
    Data->SetArrayField(TEXT("cvars"), Out);
    return mcp_common::MakeOkReply(Data);
}

// MCP:list_cvars_with_prefix - List cvars whose name starts with prefix (read-only).
//   @param prefix - Required prefix
//   @param max - Max results (default 100)
//   @returns { count, cvars: [...] }
TSharedPtr<FJsonObject> HandleListCVarsWithPrefix(const TSharedPtr<FJsonObject>& Params)
{
    FString Prefix;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("prefix"), Prefix)) { return Missing; }
    int32 Max = 100;
    Params->TryGetNumberField(TEXT("max"), Max);
    Max = FMath::Max(1, Max);

    TArray<TSharedPtr<FJsonValue>> Out;
    IConsoleManager& CM = IConsoleManager::Get();
    CM.ForEachConsoleObjectThatStartsWith(FConsoleObjectVisitor::CreateLambda(
        [&](const TCHAR* Name, IConsoleObject* Obj)
        {
            if (Out.Num() >= Max) { return; }
            const FString N(Name);
            if (!N.StartsWith(Prefix, ESearchCase::IgnoreCase)) { return; }
            TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
            O->SetStringField(TEXT("name"), N);
            if (IConsoleVariable* V = CM.FindConsoleVariable(Name))
            {
                O->SetStringField(TEXT("value"), V->GetString());
            }
            Out.Add(MakeShared<FJsonValueObject>(O));
        }), *Prefix);
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetNumberField(TEXT("count"), Out.Num());
    Data->SetArrayField(TEXT("cvars"), Out);
    return mcp_common::MakeOkReply(Data);
}

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

// MCP:list_engine_ini_sections - List section names in an INI file.
//   @param config_file - One of "Engine", "Game", "Input", "Editor", "Scalability" (default "Engine")
//   @param include_base - Whether to include Base.ini (default false)
//   @returns { count, sections: [...] }
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
    const FString FilePath = FPaths::GeneratedConfigDir() / ConfigFile + TEXT(".ini");

    TArray<FString> SectionNames;
    GConfig->GetSectionNames(ConfigFile + TEXT(".ini"), SectionNames);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetNumberField(TEXT("count"), SectionNames.Num());
    Data->SetStringField(TEXT("config_file"), ConfigFile);
    Data->SetStringField(TEXT("file_path"), FilePath);
    Data->SetArrayField(TEXT("sections"), mcp_common::StringsToJson(SectionNames));
    return mcp_common::MakeOkReply(Data);
}

// MCP:get_engine_ini_value - Read a value from a specific INI section.
//   @param section - Section name (e.g. "/Script/Engine.RendererSettings")
//   @param key - Key name
//   @param config_file - INI file base name (default "Engine")
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

// MCP:set_engine_ini_value - Write a value to a specific INI section, verified by read-back.
//   @param section - Section name
//   @param key - Key name
//   @param value - Value to write
//   @param config_file - INI file base name (default "Engine")
//   @returns { applied, section, key, value, readback_value }
TSharedPtr<FJsonObject> HandleSetEngineIniValue(const TSharedPtr<FJsonObject>& Params)
{
    FString Section, Key, Value, ConfigFile = TEXT("Engine");
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("section"), Section)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("key"), Key)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("value"), Value, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    Params->TryGetStringField(TEXT("config_file"), ConfigFile);

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

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("applied"), bApplied);
    Data->SetStringField(TEXT("section"), Section);
    Data->SetStringField(TEXT("key"), Key);
    Data->SetStringField(TEXT("value"), Value);
    Data->SetStringField(TEXT("readback_value"), ReadBackValue);
    return mcp_common::MakeOkReply(Data);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("get_cvar"), &HandleGetCVar },
    { TEXT("get_engine_ini_value"), &HandleGetEngineIniValue },
    { TEXT("get_scalability_levels"), &HandleGetScalabilityLevels },
    { TEXT("list_cvars"), &HandleListCVars },
    { TEXT("list_cvars_with_prefix"), &HandleListCVarsWithPrefix },
    { TEXT("list_engine_ini_sections"), &HandleListEngineIniSections },
    { TEXT("search_cvars"), &HandleSearchCVars },
    { TEXT("set_cvar"), &HandleSetCVar },
    { TEXT("set_cvars_from_json"), &HandleSetCVarsFromJson },
    { TEXT("set_engine_ini_value"), &HandleSetEngineIniValue },
    { TEXT("set_scalability_quality"), &HandleSetScalabilityQuality },
};
} // namespace UEAgentCVarCommands

#undef LOCTEXT_NAMESPACE
