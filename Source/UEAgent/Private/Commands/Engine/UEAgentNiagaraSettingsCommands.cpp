// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// One tool per Niagara thing that has settings -- an emitter of a system, a component's user parameter -- instead of
// one per setting or per value type. Each setting is applied by the tool that owns it (its validation, undo step and
// save), through the bridge's dispatch.
namespace UEAgentNiagaraSettingsCommands
{

// MCP-DOMAIN: Engine

namespace
{
    TSharedPtr<FJsonObject> EmitterStepParams(const FString& SystemPath, const FString& EmitterName)
    {
        TSharedPtr<FJsonObject> StepParams = MakeShared<FJsonObject>();
        StepParams->SetStringField(TEXT("system_path"), SystemPath);
        StepParams->SetStringField(TEXT("emitter_name"), EmitterName);
        return StepParams;
    }

    void CopyField(const TSharedPtr<FJsonObject>& From, const TSharedPtr<FJsonObject>& To, const TCHAR* Key)
    {
        if (const TSharedPtr<FJsonValue> Value = From->TryGetField(Key))
        {
            To->SetField(Key, Value);
        }
    }
}

// MCP:set_niagara_emitter_properties - Change any settings of ONE emitter of a Niagara System asset, found by handle name, in one call: enabled, simulation target (CPU or GPU), local space, determinism, bounds mode and fixed box, particle allocation mode and pre-allocation count, GPU particle cap and persistent IDs. Pass only the settings to change; each is validated and applied by its own step, in the order listed here, the system recompiled where the setting needs it and saved, and the call stops at the first setting refused, with applied listing those already changed. System-level settings (warmup, effect type, determinism of the system) are properties of the system asset: set_properties with save=true. get_niagara_emitter_info reads an emitter back.
//   @param system_path - Niagara System asset path, e.g. "/Game/FX/NS_Fire"
//   @param emitter_name - Emitter handle name as shown in the system (case-insensitive)
//   @param enabled [bool] - Optional: enable or disable the emitter handle
//   @param sim_target - Optional: "CPUSim" or "GPUComputeSim"
//   @param local_space [bool] - Optional: simulate particles in the emitter's local space (true) or world space (false)
//   @param deterministic [bool] - Optional: the emitter's determinism flag
//   @param bounds_mode - Optional: "Dynamic", "Fixed" or "Programmable"
//   @param bounds_min - Optional fixed-bounds minimum corner as an FVector string, e.g. "X=-50 Y=-50 Z=0"
//   @param bounds_max - Optional fixed-bounds maximum corner as an FVector string, e.g. "X=50 Y=50 Z=100"
//   @param allocation_mode - Optional: "AutomaticEstimate", "ManualEstimate" or "FixedCount"
//   @param pre_allocation_count [integer] - Optional particle count to allocate up front; needs allocation_mode
//   @param max_gpu_particles [integer] - Optional cap on GPU particles (0 = no cap)
//   @param requires_persistent_ids [bool] - Optional: whether particles carry persistent IDs
//   @returns { system_path: string, emitter_name: string, applied: { setting: that setting's reply }, failed?: { label, command, error } (when a setting is refused) }
TSharedPtr<FJsonObject> HandleSetNiagaraEmitterProperties(const TSharedPtr<FJsonObject>& Params)
{
    FString SystemPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("system_path"), SystemPath)) { return Missing; }
    FString EmitterName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("emitter_name"), EmitterName)) { return Missing; }

    TArray<mcp_common::FSubCommand> Steps;
    bool bFlag = false;
    double Number = 0.0;
    FString Text;

    if (Params->TryGetBoolField(TEXT("enabled"), bFlag))
    {
        TSharedPtr<FJsonObject> Enabled = EmitterStepParams(SystemPath, EmitterName);
        Enabled->SetBoolField(TEXT("enabled"), bFlag);
        Steps.Add({ TEXT("enabled"), TEXT("set_niagara_emitter_enabled"), Enabled });
    }
    if (Params->HasField(TEXT("sim_target")) || Params->HasField(TEXT("local_space")) || Params->HasField(TEXT("deterministic")))
    {
        TSharedPtr<FJsonObject> Sim = EmitterStepParams(SystemPath, EmitterName);
        if (Params->TryGetStringField(TEXT("sim_target"), Text))
        {
            Sim->SetStringField(TEXT("sim_target"), Text);
        }
        if (Params->TryGetBoolField(TEXT("local_space"), bFlag))
        {
            Sim->SetBoolField(TEXT("local_space"), bFlag);
        }
        if (Params->TryGetBoolField(TEXT("deterministic"), bFlag))
        {
            Sim->SetBoolField(TEXT("deterministic"), bFlag);
        }
        Steps.Add({ TEXT("simulation"), TEXT("set_niagara_emitter_sim_target"), Sim });
    }
    if (Params->HasField(TEXT("bounds_mode")) || Params->HasField(TEXT("bounds_min")) || Params->HasField(TEXT("bounds_max")))
    {
        TSharedPtr<FJsonObject> Bounds = EmitterStepParams(SystemPath, EmitterName);
        if (Params->TryGetStringField(TEXT("bounds_mode"), Text))
        {
            Bounds->SetStringField(TEXT("bounds_mode"), Text);
        }
        if (Params->TryGetStringField(TEXT("bounds_min"), Text))
        {
            Bounds->SetStringField(TEXT("bounds_min"), Text);
        }
        if (Params->TryGetStringField(TEXT("bounds_max"), Text))
        {
            Bounds->SetStringField(TEXT("bounds_max"), Text);
        }
        Steps.Add({ TEXT("bounds"), TEXT("set_niagara_emitter_bounds"), Bounds });
    }
    if (Params->TryGetStringField(TEXT("allocation_mode"), Text))
    {
        TSharedPtr<FJsonObject> Allocation = EmitterStepParams(SystemPath, EmitterName);
        Allocation->SetStringField(TEXT("allocation_mode"), Text);
        if (Params->TryGetNumberField(TEXT("pre_allocation_count"), Number))
        {
            Allocation->SetNumberField(TEXT("pre_allocation_count"), Number);
        }
        Steps.Add({ TEXT("allocation"), TEXT("set_niagara_emitter_allocation_mode"), Allocation });
    }
    else if (Params->HasField(TEXT("pre_allocation_count")))
    {
        return mcp_common::MakeErrorReply(TEXT("pre_allocation_count needs allocation_mode; nothing was changed"));
    }
    if (Params->TryGetNumberField(TEXT("max_gpu_particles"), Number))
    {
        TSharedPtr<FJsonObject> Gpu = EmitterStepParams(SystemPath, EmitterName);
        Gpu->SetNumberField(TEXT("max_gpu_particles"), Number);
        Steps.Add({ TEXT("max_gpu_particles"), TEXT("set_niagara_emitter_max_gpu_particles"), Gpu });
    }
    if (Params->TryGetBoolField(TEXT("requires_persistent_ids"), bFlag))
    {
        TSharedPtr<FJsonObject> Ids = EmitterStepParams(SystemPath, EmitterName);
        Ids->SetBoolField(TEXT("requires_persistent_ids"), bFlag);
        Steps.Add({ TEXT("requires_persistent_ids"), TEXT("set_niagara_emitter_requires_persistent_ids"), Ids });
    }
    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetStringField(TEXT("system_path"), SystemPath);
    Reply->SetStringField(TEXT("emitter_name"), EmitterName);
    return mcp_common::RunSubCommands(TEXT("set_niagara_emitter_properties"), Steps, Reply);
}

// MCP:set_niagara_component_variable - Set a user parameter (variable override) on the Niagara component of an editor-world actor, found by internal name or label, the first UNiagaraComponent on it used: type says what value holds -- float, int, bool, vec2, vector, vec4, position, color, quat, matrix or object -- and the component's own SetVariable<Type> runs, then the value is read back. The override is added locally when the component has none for that name. One undo step; nothing is saved (save_level for a placed actor). The fixed bounds of the component are set_niagara_component_fixed_bounds; a parameter's default in the system asset is set_niagara_user_parameter.
//   @param actor_name - The actor carrying the Niagara component (internal name or editor label)
//   @param variable_name - The user parameter, e.g. "User.SpawnRate" or "SpawnRate"
//   @param type - One of float, int, bool, vec2, vector, vec4, position, color, quat, matrix, object
//   @param value [any] - The value: a number (float, int), a bool, an array of 2/3/4 numbers (vec2, vector and position, vec4, color as [R, G, B] or [R, G, B, A]), [x, y, z, w] (quat), 16 numbers in row-major order (matrix), or an asset path (object)
//   @returns { the setter's reply: actor_name, variable_name, the value read back }
TSharedPtr<FJsonObject> HandleSetNiagaraComponentVariable(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), ActorName)) { return Missing; }
    FString VariableName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("variable_name"), VariableName)) { return Missing; }
    FString Type;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("type"), Type)) { return Missing; }
    const TSharedPtr<FJsonValue> Value = Params->TryGetField(TEXT("value"));
    if (!Value.IsValid() || Value->IsNull())
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter 'value'; nothing was changed"));
    }
    Type = Type.ToLower();

    TSharedPtr<FJsonObject> StepParams = MakeShared<FJsonObject>();
    StepParams->SetStringField(TEXT("actor_name"), ActorName);
    StepParams->SetStringField(TEXT("variable_name"), VariableName);
    const TArray<TSharedPtr<FJsonValue>>* Numbers = nullptr;
    const bool bArray = Value->TryGetArray(Numbers) && Numbers;
    auto Components = [&](const TCHAR* const* Keys, int32 Count) -> TSharedPtr<FJsonObject>
    {
        if (!bArray || Numbers->Num() != Count)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("value for type %s must be an array of %d numbers; nothing was changed"), *Type, Count));
        }
        for (int32 Index = 0; Index < Count; ++Index)
        {
            double Component = 0.0;
            if (!(*Numbers)[Index].IsValid() || !(*Numbers)[Index]->TryGetNumber(Component))
            {
                return mcp_common::MakeErrorReply(FString::Printf(TEXT("value for type %s must hold numbers; nothing was changed"), *Type));
            }
            StepParams->SetNumberField(Keys[Index], Component);
        }
        return nullptr;
    };

    FString Command;
    if (Type == TEXT("float") || Type == TEXT("int") || Type == TEXT("bool"))
    {
        Command = FString::Printf(TEXT("set_niagara_component_%s"), *Type);
        StepParams->SetField(TEXT("value"), Value);
    }
    else if (Type == TEXT("vector") || Type == TEXT("position") || Type == TEXT("color") || Type == TEXT("matrix"))
    {
        Command = FString::Printf(TEXT("set_niagara_component_%s"), *Type);
        StepParams->SetField(*Type, Value);
    }
    else if (Type == TEXT("object"))
    {
        Command = TEXT("set_niagara_component_object");
        StepParams->SetField(TEXT("object_path"), Value);
    }
    else if (Type == TEXT("vec2"))
    {
        const TCHAR* const Keys[] = { TEXT("x"), TEXT("y") };
        if (TSharedPtr<FJsonObject> Error = Components(Keys, 2)) { return Error; }
        Command = TEXT("set_niagara_component_vec2");
    }
    else if (Type == TEXT("vec4") || Type == TEXT("quat"))
    {
        const TCHAR* const Keys[] = { TEXT("x"), TEXT("y"), TEXT("z"), TEXT("w") };
        if (TSharedPtr<FJsonObject> Error = Components(Keys, 4)) { return Error; }
        Command = FString::Printf(TEXT("set_niagara_component_%s"), *Type);
    }
    else
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Unknown type '%s': one of float, int, bool, vec2, vector, vec4, position, color, quat, matrix, object. Nothing was changed"), *Type));
    }

    // One step: the reply is the setter's own, with its read-back value.
    TArray<mcp_common::FSubCommand> Steps;
    Steps.Add({ Type, Command, StepParams });
    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetStringField(TEXT("actor_name"), ActorName);
    Reply->SetStringField(TEXT("variable_name"), VariableName);
    Reply->SetStringField(TEXT("type"), Type);
    return mcp_common::RunSubCommands(TEXT("set_niagara_component_variable"), Steps, Reply);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("set_niagara_component_variable"), &HandleSetNiagaraComponentVariable },
    { TEXT("set_niagara_emitter_properties"), &HandleSetNiagaraEmitterProperties },
};
} // namespace UEAgentNiagaraSettingsCommands
