// Copyright (c) 2026 CarlxsMG.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "UEAgentProjectSettings.generated.h"

/**
 * Project-wide MCP settings (Project Settings > Plugins > UEAgent).
 *
 * Saved to Config/DefaultEditor.ini, so the whole team shares them through source control --
 * unlike UUEAgentSettings (port, token), which is per user. Everything here describes the
 * PROJECT's content layout; none of it has a project-specific default.
 */
UCLASS(config = Editor, defaultconfig, meta = (DisplayName = "UEAgent"))
class UEAGENT_API UUEAgentProjectSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	/**
	 * Content roots of vendored / third-party libraries (e.g. /Game/SomeMarketplacePack).
	 * MCP tools refuse to save, rename, move or delete anything under them, and the lineage
	 * audits treat the classes there as library classes. Empty: nothing is protected.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Content roots")
	TArray<FString> ReadOnlyContentRoots;

	/**
	 * The project's own content tree: the default scope of audit_class_purity. Classes under a
	 * read-only root are never audited, even when this root contains them.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Content roots")
	FString ProjectContentRoot = TEXT("/Game");

	/**
	 * Asset-registry metadata tag a thin fork carries to declare the library class it forks:
	 * fork_class_chain writes it, audit_class_purity checks it.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Forks")
	FString ForkOriginTag = TEXT("mcp.forkOrigin");

	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }

	/** ReadOnlyContentRoots without empty entries or trailing slashes. */
	TArray<FString> GetReadOnlyContentRoots() const;
	/** ProjectContentRoot without a trailing slash ("/Game" when empty). */
	FString GetProjectContentRoot() const;
	/** ForkOriginTag as an FName ("mcp.forkOrigin" when empty). */
	FName GetForkOriginTag() const;
};
