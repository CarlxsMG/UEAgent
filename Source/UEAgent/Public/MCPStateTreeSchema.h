// Copyright (c) 2026 CarlxsMG.

#pragma once

#include "CoreMinimal.h"
#include "StateTreeSchema.h"
#include "MCPStateTreeSchema.generated.h"

/**
 * Minimal concrete UStateTreeSchema shipped by the plugin.
 *
 * UStateTreeSchema is UCLASS(Abstract) (it declares no pure-virtuals -- only permissive defaults),
 * so assigning the abstract base as a StateTree's schema trips the abstract-class ensure and the
 * schema is nulled out on save (the saved asset loses its schema). This concrete subclass inherits
 * every default unchanged and gives create_state_tree a valid, instantiable, persistable schema.
 * Projects can assign their own concrete UStateTreeSchema subclass instead.
 */
UCLASS()
class UEAGENT_API UMCPStateTreeSchema : public UStateTreeSchema
{
	GENERATED_BODY()
};
