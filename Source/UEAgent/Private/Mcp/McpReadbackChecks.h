#pragma once

#include "CoreMinimal.h"
#include "Mcp/McpReadback.h"

/**
 * The READBACK check families, each in its own file under Mcp/Readback/.
 * They are registered once, when the tables are built (McpReadback::Tables()).
 */
namespace McpReadbackChecks
{
	void RegisterAll(McpReadback::FTables& Tables);

	void RegisterInResponseA(McpReadback::FTables& Tables);
	void RegisterInResponseB(McpReadback::FTables& Tables);
	void RegisterInResponseC(McpReadback::FTables& Tables);
	void RegisterPython(McpReadback::FTables& Tables);
	void RegisterFollowUp(McpReadback::FTables& Tables);
}
