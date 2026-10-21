// Copyright (c) 2026 CarlxsMG.

#include "../McpReadbackChecks.h"

// The family index. Each family lives in its own file; the core (the generic FOLLOW_UP and the
// pre-command snapshots) is registered by McpReadback::RegisterCore.
namespace McpReadbackChecks
{
	void RegisterAll(McpReadback::FTables& Tables)
	{
		RegisterInResponseA(Tables);
		RegisterInResponseB(Tables);
		RegisterInResponseC(Tables);
		RegisterPython(Tables);
		RegisterFollowUp(Tables);
	}
}
