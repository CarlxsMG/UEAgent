#pragma once

#include "CoreMinimal.h"
#include "GameFramework/SaveGame.h"
#include "MCPSaveGame.generated.h"

/**
 * Minimal concrete USaveGame subclass shipped by the plugin.
 *
 * USaveGame itself is UCLASS(abstract) and cannot be instantiated (NewObject on it yields a
 * degenerate object the linker nulls out on save). The SaveGame commands therefore default to
 * this concrete class when no `save_game_class` is supplied -- giving a usable, non-degenerate
 * generic save out of the box. Projects with their own USaveGame subclass pass it by name instead.
 */
UCLASS()
class UEAGENT_API UMCPSaveGame : public USaveGame
{
	GENERATED_BODY()

public:
	/** Free-form payload for a generic save (a project's own USaveGame subclass can replace this). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "MCP")
	FString Payload;
};
