// Copyright (c) 2026 CarlxsMG.

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
 * Setters verify by reading the value back (set_cvars_from_json per name, set_scalability_quality per
 * group); a missing cvar produces an honest error.
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

    /**
     * The groups of Wanted that Got does not hold, as "Group: wanted W, engine has G". bAll compares every group (and
     * ResolutionQuality); otherwise only the one named Category (as set_scalability_quality reports it).
     */
    TArray<FString> ScalabilityMismatches(const Scalability::FQualityLevels& Wanted, const Scalability::FQualityLevels& Got,
        bool bAll, const FString& Category)
    {
        TArray<FString> Out;
        const auto Check = [&](const TCHAR* Name, const TCHAR* Group, int32 W, int32 G)
        {
            if ((bAll || Category == Group) && W != G)
            {
                Out.Add(FString::Printf(TEXT("%s: wanted %d, engine has %d"), Name, W, G));
            }
        };
        if (bAll && !FMath::IsNearlyEqual(Wanted.ResolutionQuality, Got.ResolutionQuality, 0.01f))
        {
            Out.Add(FString::Printf(TEXT("ResolutionQuality: wanted %g, engine has %g"), Wanted.ResolutionQuality, Got.ResolutionQuality));
        }
        Check(TEXT("ViewDistanceQuality"), TEXT("ViewDistance"), Wanted.ViewDistanceQuality, Got.ViewDistanceQuality);
        Check(TEXT("AntiAliasingQuality"), TEXT("AntiAliasing"), Wanted.AntiAliasingQuality, Got.AntiAliasingQuality);
        Check(TEXT("ShadowQuality"), TEXT("Shadows"), Wanted.ShadowQuality, Got.ShadowQuality);
        Check(TEXT("GlobalIlluminationQuality"), TEXT("GlobalIllumination"), Wanted.GlobalIlluminationQuality, Got.GlobalIlluminationQuality);
        Check(TEXT("ReflectionQuality"), TEXT("Reflection"), Wanted.ReflectionQuality, Got.ReflectionQuality);
        Check(TEXT("PostProcessQuality"), TEXT("PostProcess"), Wanted.PostProcessQuality, Got.PostProcessQuality);
        Check(TEXT("TextureQuality"), TEXT("Texture"), Wanted.TextureQuality, Got.TextureQuality);
        Check(TEXT("EffectsQuality"), TEXT("Effects"), Wanted.EffectsQuality, Got.EffectsQuality);
        Check(TEXT("FoliageQuality"), TEXT("Foliage"), Wanted.FoliageQuality, Got.FoliageQuality);
        Check(TEXT("ShadingQuality"), TEXT("Shading"), Wanted.ShadingQuality, Got.ShadingQuality);
        Check(TEXT("LandscapeQuality"), TEXT("Landscape"), Wanted.LandscapeQuality, Got.LandscapeQuality);
        return Out;
    }

    /** Text that reads whole as a number: an optional sign, digits with at most one '.', and an optional exponent. */
    bool CVar_IsNumberText(const FString& Text)
    {
        int32 ExponentAt = INDEX_NONE;
        if (!Text.FindChar(TEXT('e'), ExponentAt))
        {
            Text.FindChar(TEXT('E'), ExponentAt);
        }
        const FString Mantissa = ExponentAt == INDEX_NONE ? Text : Text.Left(ExponentAt);
        bool bHasDigit = false;
        for (const TCHAR Char : Mantissa)
        {
            bHasDigit |= FChar::IsDigit(Char);
        }
        if (!bHasDigit || !FCString::IsNumeric(*Mantissa))
        {
            return false;
        }
        if (ExponentAt == INDEX_NONE)
        {
            return true;
        }
        FString Exponent = Text.Mid(ExponentAt + 1);
        if (Exponent.StartsWith(TEXT("+")) || Exponent.StartsWith(TEXT("-")))
        {
            Exponent.RightChopInline(1);
        }
        if (Exponent.IsEmpty())
        {
            return false;
        }
        for (const TCHAR Char : Exponent)
        {
            if (!FChar::IsDigit(Char))
            {
                return false;
            }
        }
        return true;
    }

    /**
     * The text IConsoleVariable::Set gets for one set_cvars_from_json entry, or false with OutError when the cvar would
     * read it as something else. A JSON bool becomes 1/0 and a number its text; a string is passed as it is, except
     * that a numeric or bool cvar takes only a number or one of the words true/false/yes/no/on/off (case ignored,
     * turned into 1/0 as the engine does for .ini values): any other text would be read as 0 without a word.
     */
    bool CVar_ValueText(IConsoleVariable* CVar, const TSharedPtr<FJsonValue>& Value, FString& Out, FString& OutError)
    {
        switch (Value.IsValid() ? Value->Type : EJson::None)
        {
        case EJson::Boolean:
            Out = Value->AsBool() ? TEXT("1") : TEXT("0");
            return true;
        case EJson::Number:
        {
            const double Number = Value->AsNumber();
            const bool bIntegral = FMath::IsFinite(Number) && FMath::Abs(Number) < 9.0e15 && FMath::FloorToDouble(Number) == Number;
            Out = bIntegral ? FString::Printf(TEXT("%lld"), static_cast<int64>(Number)) : FString::SanitizeFloat(Number);
            return true;
        }
        case EJson::String:
            Out = Value->AsString();
            break;
        default:
            OutError = TEXT("the value must be a string, number or bool (true/false); this one is null, an array or an object");
            return false;
        }

        if (CVar->IsVariableString())
        {
            return true;
        }
        const FString Trimmed = Out.TrimStartAndEnd();
        if (Trimmed.Equals(TEXT("true"), ESearchCase::IgnoreCase) || Trimmed.Equals(TEXT("yes"), ESearchCase::IgnoreCase)
            || Trimmed.Equals(TEXT("on"), ESearchCase::IgnoreCase))
        {
            Out = TEXT("1");
            return true;
        }
        if (Trimmed.Equals(TEXT("false"), ESearchCase::IgnoreCase) || Trimmed.Equals(TEXT("no"), ESearchCase::IgnoreCase)
            || Trimmed.Equals(TEXT("off"), ESearchCase::IgnoreCase))
        {
            Out = TEXT("0");
            return true;
        }
        if (CVar->IsVariableInt() || CVar->IsVariableFloat() || CVar->IsVariableBool())
        {
            if (!CVar_IsNumberText(Trimmed))
            {
                OutError = FString::Printf(TEXT("'%s' is not a number, and this %s console variable would read it as 0; give a number, or true/false/yes/no/on/off"),
                    *Out, CVar->IsVariableFloat() ? TEXT("float") : (CVar->IsVariableBool() ? TEXT("bool") : TEXT("integer")));
                return false;
            }
            Out = Trimmed;
        }
        return true;
    }

    /** Whether CVar now holds the value Text sets, compared in the cvar's own type (so "75" matches a float's "75.0"). */
    bool CVar_Holds(IConsoleVariable* CVar, const FString& Text)
    {
        if (CVar->IsVariableString())
        {
            return CVar->GetString() == Text;
        }
        if (CVar->IsVariableBool())
        {
            return CVar->GetBool() == FCString::ToBool(*Text);
        }
        if (CVar->IsVariableInt())
        {
            return CVar->GetInt() == FCString::Atoi(*Text);
        }
        if (CVar->IsVariableFloat() || CVar_IsNumberText(Text))
        {
            const float Wanted = FCString::Atof(*Text);
            return FMath::IsNearlyEqual(CVar->GetFloat(), Wanted, FMath::Max(1.e-6f, FMath::Abs(Wanted) * 1.e-6f));
        }
        return CVar->GetString().Equals(Text, ESearchCase::IgnoreCase);
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
// MCP:get_cvar - Read the live value of one console variable of the running editor through IConsoleManager, without changing it: the value as a string and parsed as int, float and bool, its help text, its raw flags and set_by (which layer last set it, such as Constructor, Scalability, ProjectSetting or Console). Use it to read one variable by exact name; to find names by substring or prefix use list_cvars; to change a value use set_cvar (or set_cvars_from_json for many); scalability groups as a whole: get_scalability_levels. A name that is not a registered console variable (including a console command, which has no value) is an error.
//   @param name - Exact console variable name, e.g. "r.ScreenPercentage" or "t.MaxFPS", as list_cvars returns it (case-insensitive).
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
// MCP:list_cvars - List the console variables registered in the running editor whose name contains filter and/or starts with prefix, each with its current value as text and its help, without changing anything. Console commands are left out unless include_commands is true (they come with help but no value); with neither filter nor prefix every variable is walked. The list stops at max_results, and truncated plus total_matches tell how many more matched. Use it to discover names; then read one in full (int, float and bool values, flags, set_by) with get_cvar, change it with set_cvar or several at once with set_cvars_from_json, and read the scalability groups with get_scalability_levels. Matching nothing is a success with an empty cvars list.
//   @param filter - Text the name must contain, case ignored, e.g. "Shadow" (default empty = no filter). Without prefix, space-separated words must all appear and a single character matches as a name prefix; with prefix, a name must start with prefix and contain the whole filter.
//   @param prefix - Text the name must start with, case ignored, e.g. "r.Shadow." or "t." (default empty = any name).
//   @param include_commands - true to also list console commands (name and help, no value); default false lists variables only.
//   @param max_results - Most entries to return, a whole number of at least 1 (default 200); total_matches still counts every match.
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
// MCP:set_scalability_quality - Set the running editor's scalability quality, every sg.* group at once (category all, the default) or one group, through Scalability::SetQualityLevels + ApplyCachedQualityLevels, then read all the levels back. It starts from the current levels, so a single-group change leaves the other groups as they were. The change lives in this editor session only: it is not undoable and is not written to GameUserSettings, an INI or a device profile. Check the levels before with get_scalability_levels; for one sg.* or r.* variable use set_cvar instead. An unknown quality word or category is an error and nothing changes; a number outside 0-4 is clamped. The levels are read back after the change: set is true only when every group asked for now holds its level, and otherwise success is false, with mismatches naming each group the engine left elsewhere. Repeating the call sets the same levels again (idempotent).
//   @param quality - Level as a number 0-4 (0 Low, 1 Medium, 2 High, 3 Epic, 4 Cinematic; others clamped) or one of "Low", "Medium", "High", "Epic", "Cinematic", case ignored. Required.
//   @param category - One group, case ignored: "ViewDistance", "AntiAliasing", "Shadows", "GlobalIllumination" (or "GI"), "Reflection", "PostProcess", "Texture", "Effects", "Foliage", "Shading", "Landscape"; or "all" (the default) for every group.
//   @returns { set: bool, requested_level: int, category: string, ResolutionQuality: int, ViewDistanceQuality: int, AntiAliasingQuality: int, ShadowQuality: int, GlobalIlluminationQuality: int, ReflectionQuality: int, PostProcessQuality: int, TextureQuality: int, EffectsQuality: int, FoliageQuality: int, ShadingQuality: int, LandscapeQuality: int, mismatches?: [string] }

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

    // Read back the live state and compare it with what was asked: the engine ignores the sg.* write of a group
    // last set at a higher priority (by the console, set_cvar or a device profile), and then the group keeps its level.
    const Scalability::FQualityLevels Verified = Scalability::GetQualityLevels();
    const TArray<FString> Mismatches = ScalabilityMismatches(Quality, Verified, AppliedCategory == TEXT("all"), AppliedCategory);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("set"), Mismatches.Num() == 0);
    Data->SetNumberField(TEXT("requested_level"), Level);
    Data->SetStringField(TEXT("category"), AppliedCategory);
    FillScalabilityLevels(Data, Verified);
    if (Mismatches.Num() > 0)
    {
        Data->SetArrayField(TEXT("mismatches"), mcp_common::StringsToJson(Mismatches));
        return mcp_common::MakeOutcomeReply(Data, false, FString::Printf(
            TEXT("set_scalability_quality: the engine did not take the level for %s. A group last set at a higher priority than ")
            TEXT("scalability (with set_cvar or the console on its sg.* variable, or by a device profile) keeps its value; get_cvar ")
            TEXT("on that sg.* variable shows set_by."), *FString::Join(Mismatches, TEXT("; "))));
    }
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// get_scalability_levels
// ---------------------------------------------------------------------------
// MCP:get_scalability_levels - Read the engine's live scalability quality levels (Scalability::GetQualityLevels, the sg.* groups of the editor session) without changing anything: one level per group, 0 Low, 1 Medium, 2 High, 3 Epic, 4 Cinematic, except ResolutionQuality, which is a screen percentage; single_quality_level is the shared level when every group agrees and -1 when they differ (custom). Use it before and after set_scalability_quality, which changes one group or all; for a single sg.* or r.* console variable use get_cvar; for frame rate under those settings use get_fps_info. These are the editor's current settings, not a per-platform device profile or a saved user setting. Takes no parameters and does not fail.
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

// MCP:set_cvars_from_json - Set several console variables of the running editor in one call, from a JSON object of name/value pairs, each through IConsoleManager at SetByCode priority, then read each one back. Not atomic: names are applied one by one, and the others still apply when one fails. applied counts only the variables that hold the new value afterwards; results gives each one's read-back value and set_by. A variable last set at a higher priority (by the console or by set_cvar) ignores this write and goes to errors, as do an unknown name and a value the variable would misread; then success is false (use set_cvar, console priority, for those). Find names with list_cvars. The values live in this editor session only: nothing is written to an INI (set_engine_ini_value does that) and nothing is undoable. Text that is not a JSON object is an error and nothing changes. Repeating the call sets the same values again.
//   @param cvars - A STRING holding a JSON object of exact cvar names to values, e.g. '{"r.ScreenPercentage":"75","t.MaxFPS":60}'; names as list_cvars returns them. A value is text, a number (passed as its text) or a bool (passed as 1/0); a numeric or bool cvar takes only a number or one of the words true/false/yes/no/on/off (as 1/0), and other text, null, an array or an object is refused for that name.
//   @returns { applied, requested, results: [{name, requested_value, value, set_by, applied}], errors: [{name, error}] }
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
    TArray<TSharedPtr<FJsonValue>> ResultArr;
    IConsoleManager& CM = IConsoleManager::Get();
    const auto AddError = [&ErrArr](const FString& Name, const FString& Error)
    {
        TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
        E->SetStringField(TEXT("name"), Name);
        E->SetStringField(TEXT("error"), Error);
        ErrArr.Add(MakeShared<FJsonValueObject>(E));
    };
    for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : JsonObj->Values)
    {
        IConsoleVariable* V = CM.FindConsoleVariable(*Pair.Key);
        if (!V)
        {
            AddError(Pair.Key, TEXT("cvar not found"));
            continue;
        }
        // A value the cvar would misread (JSON null/array/object, or words in a numeric cvar) is refused before
        // anything is set for that name.
        FString ValueStr, ValueError;
        if (!CVar_ValueText(V, Pair.Value, ValueStr, ValueError))
        {
            AddError(Pair.Key, ValueError + TEXT("; nothing was set for it"));
            continue;
        }
        V->Set(*ValueStr, ECVF_SetByCode);

        // Read back: the engine ignores a SetByCode write to a cvar last set at a higher priority (the console or
        // set_cvar), and a cvar can clamp or reject a value, so applied counts only the values the cvar now holds.
        const bool bHolds = CVar_Holds(V, ValueStr);
        const EConsoleVariableFlags SetBy = static_cast<EConsoleVariableFlags>(V->GetFlags() & ECVF_SetByMask);
        const TCHAR* SetByName = GetConsoleVariableSetByName(SetBy);
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("name"), Pair.Key);
        Entry->SetStringField(TEXT("requested_value"), ValueStr);
        Entry->SetStringField(TEXT("value"), V->GetString());
        Entry->SetStringField(TEXT("set_by"), SetByName ? FString(SetByName) : FString());
        Entry->SetBoolField(TEXT("applied"), bHolds);
        ResultArr.Add(MakeShared<FJsonValueObject>(Entry));
        if (bHolds)
        {
            ++Applied;
            continue;
        }
        AddError(Pair.Key, FString::Printf(
            TEXT("not applied: the cvar still reads '%s' (set_by %s). A cvar last set by the console or set_cvar ignores this lower-priority write; use set_cvar for it"),
            *V->GetString(), SetByName ? SetByName : TEXT("unknown")));
    }
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetNumberField(TEXT("applied"), Applied);
    Data->SetNumberField(TEXT("requested"), JsonObj->Values.Num());
    Data->SetArrayField(TEXT("results"), ResultArr);
    Data->SetArrayField(TEXT("errors"), ErrArr);
    if (ErrArr.Num() > 0)
    {
        return mcp_common::MakeOutcomeReply(Data, false, FString::Printf(
            TEXT("set_cvars_from_json: %d of %d console variables were applied; errors says why each other one was not (the applied ones keep their new values)."),
            Applied, JsonObj->Values.Num()));
    }
    return mcp_common::MakeOkReply(Data);
}

// MCP:list_engine_ini_sections - List the section names of one INI (Engine, Game, Input, Editor, Scalability...) without changing anything: by default as the running editor holds it in GConfig, the in-memory merge of every config layer (the engine's Base ini included); with include_base=false only the sections the project itself defines, read from disk in its Config/Default<name>.ini and its saved Saved/Config/<Platform>/<name>.ini (files_read lists the ones that exist). An INI GConfig does not know (a misspelt name, say) is a success with found=false. Use it to discover a section before reading one key with get_engine_ini_value or writing one with set_engine_ini_value; for UDeveloperSettings classes use get_project_settings_value. file_path is the saved-config path Saved/Config/<Platform>/<name>.ini built from the name.
//   @param config_file - INI base name, e.g. "Engine" (default), "Game", "Input", "Editor", "Scalability"; a trailing ".ini" is dropped.
//   @param include_base - true (default) lists the whole in-memory merge, engine Base ini layers included; false lists only the sections in the project's Config/Default<name>.ini and Saved/Config/<Platform>/<name>.ini, as they are on disk.
//   @returns { count, sections: [string], found: bool, include_base: bool, config_file, file_path, files_read?: [string] }
TSharedPtr<FJsonObject> HandleListEngineIniSections(const TSharedPtr<FJsonObject>& Params)
{
    FString ConfigFile = TEXT("Engine");
    Params->TryGetStringField(TEXT("config_file"), ConfigFile);
    // The default is what the tool always listed: the whole merge.
    bool bIncludeBase = true;
    const bool bIncludeBaseRead = Params->TryGetBoolField(TEXT("include_base"), bIncludeBase);
    if (!bIncludeBaseRead && Params->HasField(TEXT("include_base")))
    {
        return mcp_common::MakeErrorReply(TEXT("include_base must be true or false"));
    }

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
    if (!bIncludeBase)
    {
        // Only the project's own layers, read from the files on disk (FConfigFile::Read is one file, no merge):
        // what Config/Default<name>.ini and the saved Saved/Config/<Platform>/<name>.ini define.
        const FString ProjectIni = FPaths::ConvertRelativePathToFull(
            FPaths::SourceConfigDir() / FString::Printf(TEXT("Default%s.ini"), *ConfigFile));
        const FString SavedIni = FPaths::ConvertRelativePathToFull(
            FConfigCacheIni::GetDestIniFilename(*ConfigFile, nullptr, *FPaths::GeneratedConfigDir()));
        TArray<FString> FilesRead;
        SectionNames.Reset();
        for (const FString& IniPath : { ProjectIni, SavedIni })
        {
            if (!FPaths::FileExists(IniPath) || FilesRead.Contains(IniPath))
            {
                continue;
            }
            FConfigFile Layer;
            Layer.Read(IniPath);
            TArray<FString> LayerSections;
            Layer.GetKeys(LayerSections);
            for (const FString& Section : LayerSections)
            {
                SectionNames.AddUnique(Section);
            }
            FilesRead.Add(IniPath);
        }
        Data->SetArrayField(TEXT("files_read"), mcp_common::StringsToJson(FilesRead));
    }

    Data->SetNumberField(TEXT("count"), SectionNames.Num());
    Data->SetBoolField(TEXT("found"), bFound);
    Data->SetBoolField(TEXT("include_base"), bIncludeBase);
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
