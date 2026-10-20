#include "McpEditorUI.h"

#include "UEAgentBridge.h"
#include "Mcp/McpActivityPanel.h"
#include "Mcp/McpAgentClients.h"
#include "Mcp/McpClaudeCode.h"
#include "Mcp/McpSkills.h"
#include "Mcp/McpPreconditions.h"
#include "Mcp/McpToolCatalog.h"
#include "UEAgentSettings.h"

#include "Brushes/SlateRoundedBoxBrush.h"
#include "Containers/Ticker.h"
#include "Editor.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Framework/Notifications/NotificationManager.h"
#include "HAL/PlatformApplicationMisc.h"
#include "ISettingsModule.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/App.h"
#include "Misc/Paths.h"
#include "Misc/ScopedSlowTask.h"
#include "Modules/ModuleManager.h"
#include "Styling/AppStyle.h"
#include "Styling/StyleColors.h"
#include "ToolMenus.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "UEAgentEditorUI"

// Namespace name unique per file: the module builds as unity (see Build.cs).
namespace McpEditorUIPrivate
{
	using McpClaudeCode::EState;

	static const FName OwnerName(TEXT("UEAgentEditorUI"));
	static FDelegateHandle ToolMenusStartupHandle;
	static FDelegateHandle EditorInitializedHandle;
	static FTSTicker::FDelegateHandle HintTickerHandle;
	static TWeakPtr<SNotificationItem> HintItem;

	static UUEAgentBridge* Bridge()
	{
		return GEditor ? GEditor->GetEditorSubsystem<UUEAgentBridge>() : nullptr;
	}

	enum class EServerState : uint8
	{
		Serving,
		ServingNoCatalog, // listening, but tools/list comes back empty
		Failed,           // could not bind the port (another editor, another program)
		Stopped,
		Disabled,
	};

	static EServerState ServerState()
	{
		const UUEAgentBridge* B = Bridge();
		if (B && B->IsHttpRunning())
		{
			return FMcpToolCatalog::GetCurrent().IsValid() ? EServerState::Serving : EServerState::ServingNoCatalog;
		}
		if (!GetDefault<UUEAgentSettings>()->bEnableHttpServer)
		{
			return EServerState::Disabled;
		}
		return B && !B->GetLastHttpError().IsEmpty() ? EServerState::Failed : EServerState::Stopped;
	}

	// The dot color is evaluated on every status-bar paint and the .mcp.json lives on disk: it is
	// re-read at most every 2 s (or when the menu opens, which asks for fresh data).
	static const McpClaudeCode::FStatus& ProjectStatus(bool bRefresh = false)
	{
		static McpClaudeCode::FStatus Cached;
		static double LastRead = -1.0;
		const double Now = FPlatformTime::Seconds();
		if (bRefresh || LastRead < 0.0 || Now - LastRead > 2.0)
		{
			Cached = McpClaudeCode::InspectProject();
			LastRead = Now;
		}
		return Cached;
	}

	// The other clients' files, re-read like the .mcp.json: at most every 2 s, or when the menu opens.
	static const TArray<McpAgentClients::FStatus>& OtherClients(bool bRefresh = false)
	{
		static TArray<McpAgentClients::FStatus> Cached;
		static double LastRead = -1.0;
		const double Now = FPlatformTime::Seconds();
		if (bRefresh || LastRead < 0.0 || Now - LastRead > 2.0)
		{
			Cached.Reset();
			for (const McpAgentClients::EClient Client : McpAgentClients::All())
			{
				Cached.Add(McpAgentClients::InspectProject(Client));
			}
			LastRead = Now;
		}
		return Cached;
	}

	static TArray<FString> ConnectedOtherClients(bool bRefresh = false)
	{
		TArray<FString> Names;
		for (const McpAgentClients::FStatus& Status : OtherClients(bRefresh))
		{
			if (Status.IsUsable())
			{
				Names.Add(McpAgentClients::DisplayName(Status.Client));
			}
		}
		return Names;
	}

	// Some client of the project reaches this server.
	static bool AnyClientConnected(bool bRefresh = false)
	{
		return ProjectStatus(bRefresh).IsUsable() || ConnectedOtherClients(bRefresh).Num() > 0;
	}

	static FString ServerLine()
	{
		const UUEAgentBridge* B = Bridge();
		const uint16 Port = McpClaudeCode::CurrentPort();
		switch (ServerState())
		{
		case EServerState::Serving:
			return FString::Printf(TEXT("Serving on %s"), *McpClaudeCode::ServerUrl(Port));
		case EServerState::ServingNoCatalog:
			return FString::Printf(TEXT("Serving on %s, but with NO tool catalog: Claude Code will see no tools (the reason is in the log)."), *McpClaudeCode::ServerUrl(Port));
		case EServerState::Failed:
			return FString::Printf(TEXT("Did NOT start on port %d: %s"), Port, B ? *B->GetLastHttpError() : TEXT(""));
		case EServerState::Disabled:
			return TEXT("Disabled in the settings (Editor Preferences > Plugins > UEAgent).");
		default:
			return TEXT("Stopped.");
		}
	}

	// --- Notifications -----------------------------------------------------------------------------

	enum class ENotice : uint8 { Success, Warning, Failure };

	static void Notify(const FText& Title, const FString& Detail, ENotice Kind)
	{
		FNotificationInfo Info(Title);
		if (!Detail.IsEmpty())
		{
			Info.SubText = FText::FromString(Detail);
		}
		Info.ExpireDuration = Kind == ENotice::Success ? 6.0f : 15.0f;
		Info.WidthOverride = 440.0f;
		if (Kind == ENotice::Warning)
		{
			Info.bUseSuccessFailIcons = false;
			Info.Image = FAppStyle::GetBrush(TEXT("Icons.WarningWithColor"));
		}
		if (TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info))
		{
			Item->SetCompletionState(Kind == ENotice::Success ? SNotificationItem::CS_Success
				: Kind == ENotice::Failure ? SNotificationItem::CS_Fail : SNotificationItem::CS_None);
		}
	}

	// --- Actions -----------------------------------------------------------------------------------

	static void ConnectProject()
	{
		FString Message;
		const bool bOk = McpClaudeCode::Connect(McpClaudeCode::ProjectConfigPath(), McpClaudeCode::CurrentPort(), McpClaudeCode::CurrentRequiresToken(), Message);
		const McpClaudeCode::FStatus& After = ProjectStatus(/*bRefresh*/ true);
		if (!bOk)
		{
			UE_LOG(LogTemp, Error, TEXT("[MCP] Connect Claude Code: %s"), *Message);
			Notify(LOCTEXT("ConnectFailed", "Could not connect Claude Code"), Message, ENotice::Failure);
			return;
		}
		UE_LOG(LogTemp, Display, TEXT("[MCP] Connect Claude Code: %s"), *Message);
		if (!After.IsUsable())
		{
			Notify(LOCTEXT("ConnectBlocked", "The .mcp.json points here, but Claude Code will not use it"), Message, ENotice::Warning);
			return;
		}
		Notify(LOCTEXT("ConnectOk", "Claude Code connected to this project"),
			Message + TEXT(" If Claude Code was already open in that folder, restart it so it picks up the change."), ENotice::Success);
	}

	static void CopyAddCommand()
	{
		const bool bToken = McpClaudeCode::CurrentRequiresToken();
		const FString Command = McpClaudeCode::AddCommand(McpClaudeCode::CurrentPort(), bToken);
		FPlatformApplicationMisc::ClipboardCopy(*Command);
		FString Detail = Command + TEXT("\nPaste it into a terminal opened in the project folder: it connects only for you, without touching the repo's .mcp.json.");
		if (bToken)
		{
			Detail += FString::Printf(TEXT(" Set %s to the token before opening Claude Code."), McpClaudeCode::TokenEnvVar);
		}
		Notify(LOCTEXT("Copied", "Command copied to the clipboard"), Detail, ENotice::Success);
	}

	static void ConnectOtherClient(McpAgentClients::EClient Client)
	{
		FString Message;
		const bool bOk = McpAgentClients::Connect(Client, McpAgentClients::ProjectConfigPath(Client), McpClaudeCode::CurrentPort(), McpClaudeCode::CurrentRequiresToken(), Message);
		OtherClients(/*bRefresh*/ true);
		const FString Name = McpAgentClients::DisplayName(Client);
		if (!bOk)
		{
			UE_LOG(LogTemp, Error, TEXT("[MCP] Connect %s: %s"), *Name, *Message);
			Notify(FText::FromString(FString::Printf(TEXT("Could not connect %s"), *Name)), Message, ENotice::Failure);
			return;
		}
		UE_LOG(LogTemp, Display, TEXT("[MCP] Connect %s: %s"), *Name, *Message);
		Notify(FText::FromString(FString::Printf(TEXT("%s connected to this project"), *Name)),
			Message + TEXT(" If it was already open in this folder, reload its MCP servers or restart it."), ENotice::Success);
	}

	static void CopyCodexCommand()
	{
		const bool bToken = McpClaudeCode::CurrentRequiresToken();
		const FString Command = McpAgentClients::CodexAddCommand(McpClaudeCode::CurrentPort(), bToken);
		FPlatformApplicationMisc::ClipboardCopy(*Command);
		FString Detail = Command + TEXT("\nPaste it into a terminal: Codex keeps its servers in ~/.codex/config.toml, for every project.");
		if (bToken)
		{
			Detail += FString::Printf(TEXT(" Set %s to the token before opening Codex."), McpClaudeCode::TokenEnvVar);
		}
		Notify(LOCTEXT("CopiedCodex", "Command copied to the clipboard"), Detail, ENotice::Success);
	}

	static void InstallSkills(McpSkills::ETarget Target)
	{
		FString Message;
		const bool bOk = McpSkills::Install(McpSkills::ShippedDir(), McpSkills::ProjectTargetDir(Target), McpSkills::PluginVersion(), Message);
		const FString For = McpSkills::TargetClients(Target);
		if (!bOk)
		{
			UE_LOG(LogTemp, Error, TEXT("[MCP] Install skills (%s): %s"), McpSkills::TargetId(Target), *Message);
			Notify(FText::FromString(FString::Printf(TEXT("Could not install the skills for %s"), *For)), Message, ENotice::Failure);
			return;
		}
		UE_LOG(LogTemp, Display, TEXT("[MCP] Install skills (%s): %s"), McpSkills::TargetId(Target), *Message);
		Notify(FText::FromString(FString::Printf(TEXT("Skills ready for %s"), *For)),
			Message + TEXT(" A client that is already open may need a restart to see them."), ENotice::Success);
	}

	static void RemoveSkills()
	{
		TArray<FString> Messages;
		bool bOk = true;
		for (const McpSkills::ETarget Target : McpSkills::AllTargets())
		{
			FString Message;
			bOk &= McpSkills::Remove(McpSkills::ProjectTargetDir(Target), Message);
			Messages.Add(Message);
		}
		const FString Detail = FString::Join(Messages, TEXT("\n"));
		UE_LOG(LogTemp, Display, TEXT("[MCP] Remove skills: %s"), *Detail);
		Notify(bOk ? LOCTEXT("SkillsRemoved", "UEAgent's skills removed from the project") : LOCTEXT("SkillsRemoveFailed", "Some skills could not be removed"),
			Detail, bOk ? ENotice::Success : ENotice::Failure);
	}

	static void RestartServer()
	{
		UUEAgentBridge* B = Bridge();
		if (!B)
		{
			return;
		}
		B->StopHttpServer();
		if (B->StartHttpServer())
		{
			Notify(LOCTEXT("Restarted", "MCP server restarted"), McpClaudeCode::ServerUrl(B->GetHttpPort()), ENotice::Success);
		}
		else if (!GetDefault<UUEAgentSettings>()->bEnableHttpServer)
		{
			Notify(LOCTEXT("RestartDisabled", "The MCP server is disabled in the settings"), FString(), ENotice::Warning);
		}
		// If the bind failed, the server itself already reports why: do not duplicate it.
		ProjectStatus(/*bRefresh*/ true); // the port may have changed
	}

	static void RunContractSelfTest()
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("UEAgent"));
		if (!Plugin.IsValid())
		{
			Notify(LOCTEXT("SelfTestNoPlugin", "The UEAgent plugin cannot be found"), FString(), ENotice::Failure);
			return;
		}
		const FString Dir = FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir() / TEXT("Resources/ContractTests"));
		// Only the two suites that run on fake probes. registry_vectors.json queries the project's
		// real Asset Registry, so it is left to the console command mcp.contract.selftest.
		const TCHAR* const Files[] = { TEXT("refuse_vectors.json"), TEXT("readback_vectors.json") };

		FScopedSlowTask Task(UE_ARRAY_COUNT(Files), LOCTEXT("SelfTestTask", "MCP contract self-test..."));
		Task.MakeDialogDelayed(0.5f);
		int32 Passed = 0;
		int32 Total = 0;
		TArray<FString> Problems;
		for (const TCHAR* File : Files)
		{
			Task.EnterProgressFrame(1.0f, FText::FromString(File));
			FString Report;
			const FString Summary = McpPreconditions::RunSelfTest(Dir / File, Report);
			UE_LOG(LogTemp, Display, TEXT("[MCP] Contract self-test, %s: %s (report: %s)"), File, *Summary, *Report);
			FString PassedText, TotalText;
			if (Summary.Split(TEXT("/"), &PassedText, &TotalText) && PassedText.IsNumeric() && TotalText.IsNumeric())
			{
				Passed += FCString::Atoi(*PassedText);
				Total += FCString::Atoi(*TotalText);
				if (PassedText != TotalText)
				{
					Problems.Add(FString::Printf(TEXT("%s: %s (report at %s)"), File, *Summary, *Report));
				}
			}
			else
			{
				Problems.Add(FString::Printf(TEXT("%s: %s"), File, *Summary));
			}
		}
		if (Problems.Num() == 0)
		{
			Notify(FText::FromString(FString::Printf(TEXT("Contract passes: %d/%d checks"), Passed, Total)),
				TEXT("Refusals and READBACK, on fake probes: no asset was touched."), ENotice::Success);
		}
		else
		{
			Notify(LOCTEXT("SelfTestFailed", "The contract self-test has failures"), FString::Join(Problems, TEXT("\n")), ENotice::Failure);
		}
	}

	static void OpenSettings()
	{
		FModuleManager::LoadModuleChecked<ISettingsModule>("Settings").ShowViewer("Editor", "Plugins", "UEAgentSettings");
	}

	// --- Status bar --------------------------------------------------------------------------------

	static FSlateColor DotColor()
	{
		switch (ServerState())
		{
		case EServerState::Serving:
			return AnyClientConnected() ? FStyleColors::Success : FStyleColors::Warning;
		case EServerState::ServingNoCatalog:
			return FStyleColors::Warning;
		case EServerState::Failed:
			return FStyleColors::Error;
		default:
			return FStyleColors::AccentGray;
		}
	}

	static FText LabelText()
	{
		// Queried on every paint: a new FText each time would force the text to be rebuilt every time.
		static uint16 CachedPort = 0;
		static FText Cached;
		const uint16 Port = McpClaudeCode::CurrentPort();
		if (Port != CachedPort || Cached.IsEmpty())
		{
			CachedPort = Port;
			Cached = FText::FromString(FString::Printf(TEXT("MCP \u00B7 %d"), Port));
		}
		return Cached;
	}

	static FText TooltipText()
	{
		const TArray<FString> Others = ConnectedOtherClients();
		return FText::FromString(FString::Printf(TEXT("MCP server: %s\nClaude Code: %s\nOther clients: %s\n\nClick to see the options."),
			*ServerLine(), *ProjectStatus().Detail, Others.Num() > 0 ? *FString::Printf(TEXT("%s connected."), *FString::Join(Others, TEXT(", "))) : TEXT("none connected.")));
	}

	static TSharedRef<SWidget> InfoBlock(const TArray<FString>& Lines)
	{
		TSharedRef<SVerticalBox> Box = SNew(SVerticalBox);
		for (const FString& Line : Lines)
		{
			Box->AddSlot()
			.AutoHeight()
			.Padding(0.0f, 1.0f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Line))
				.AutoWrapText(true)
			];
		}
		return SNew(SBox)
			.MaxDesiredWidth(440.0f)
			.Padding(FMargin(12.0f, 4.0f, 12.0f, 6.0f))
			[
				Box
			];
	}

	static TSharedRef<SWidget> MakeMenu()
	{
		const McpClaudeCode::FStatus& Project = ProjectStatus(/*bRefresh*/ true);
		FMenuBuilder Menu(/*bShouldCloseWindowAfterMenuSelection*/ true, nullptr);

		Menu.BeginSection("UEAgentServer", LOCTEXT("ServerSection", "MCP Server"));
		{
			TArray<FString> Lines;
			Lines.Add(ServerLine());
			const UUEAgentBridge* B = Bridge();
			if (B && B->IsHttpRunning())
			{
				int32 Connections = 0;
				int64 Requests = 0;
				int64 ToolCalls = 0;
				B->GetHttpCounters(Connections, Requests, ToolCalls);
				Lines.Add(FString::Printf(TEXT("%d open connections \u00B7 %lld requests \u00B7 %lld tools/call since it started"), Connections, Requests, ToolCalls));
			}
			if (const TSharedPtr<const FMcpToolCatalog> Catalog = FMcpToolCatalog::GetCurrent())
			{
				Lines.Add(B && B->IsHttpCompactTools()
					? FString::Printf(TEXT("%d tools in the catalog, listed as 3 meta-tools (UEAGENT_TOOLS=compact)"), Catalog->NumTools())
					: FString::Printf(TEXT("%d tools in the catalog"), Catalog->NumTools()));
			}
			Menu.AddWidget(InfoBlock(Lines), FText::GetEmpty(), /*bNoIndent*/ true);
			Menu.AddMenuEntry(
				LOCTEXT("Activity", "Activity..."),
				LOCTEXT("ActivityTip", "The last tool calls the agents made in this editor: time, duration, outcome and detail."),
				FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Recent"),
				FUIAction(FExecuteAction::CreateStatic(&McpActivityPanel::Open)));
			Menu.AddMenuEntry(
				LOCTEXT("Restart", "Restart the server"),
				LOCTEXT("RestartTip", "Closes the open connections, reloads the tool catalog and listens again with the current settings. This is what applies a port or token change."),
				FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Refresh"),
				FUIAction(FExecuteAction::CreateStatic(&RestartServer)));
			Menu.AddMenuEntry(
				LOCTEXT("SelfTest", "Contract self-test"),
				LOCTEXT("SelfTestTip", "Runs the Resources/ContractTests vectors (refusals and READBACK) against the C++ with fake probes: it touches no asset and not the world."),
				FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Check"),
				FUIAction(FExecuteAction::CreateStatic(&RunContractSelfTest)));
		}
		Menu.EndSection();

		Menu.BeginSection("UEAgentClients", LOCTEXT("ClientsSection", "AI clients"));
		{
			const TArray<McpAgentClients::FStatus>& Others = OtherClients(/*bRefresh*/ true);
			const TArray<FString> OthersConnected = ConnectedOtherClients();
			Menu.AddWidget(InfoBlock({
				FString::Printf(TEXT("Claude Code: %s"), *Project.Detail),
				FString::Printf(TEXT("Other clients: %s"), OthersConnected.Num() > 0 ? *FString::Printf(TEXT("%s connected."), *FString::Join(OthersConnected, TEXT(", "))) : TEXT("none connected.")) }),
				FText::GetEmpty(), /*bNoIndent*/ true);
			if (Project.State != EState::Connected && Project.State != EState::Unreadable)
			{
				Menu.AddMenuEntry(
					LOCTEXT("Connect", "Connect Claude Code to this project"),
					FText::FromString(FString::Printf(TEXT("Writes the \"%s\" entry into %s without touching the rest of the file."), McpClaudeCode::ServerName, *Project.Path)),
					FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Link"),
					FUIAction(FExecuteAction::CreateStatic(&ConnectProject)));
			}
			Menu.AddSubMenu(
				LOCTEXT("ConnectOther", "Connect another client"),
				LOCTEXT("ConnectOtherTip", "Cursor, VS Code and Antigravity CLI read their servers from a file in the project; Codex from your user folder."),
				FNewMenuDelegate::CreateLambda([Others](FMenuBuilder& Sub)
				{
					for (const McpAgentClients::FStatus& Status : Others)
					{
						const McpAgentClients::EClient Client = Status.Client;
						const bool bCanConnect = Status.State != McpAgentClients::EState::Connected
							&& Status.State != McpAgentClients::EState::Unreadable
							&& Status.State != McpAgentClients::EState::AuthUnsupported;
						Sub.AddMenuEntry(
							FText::FromString(Status.IsUsable()
								? FString::Printf(TEXT("%s (connected)"), McpAgentClients::DisplayName(Client))
								: FString::Printf(TEXT("Connect %s"), McpAgentClients::DisplayName(Client))),
							FText::FromString(bCanConnect
								? FString::Printf(TEXT("Writes the \"%s\" entry into %s without touching the rest of the file. %s"), McpClaudeCode::ServerName, *Status.Path, *Status.Detail)
								: Status.Detail),
							FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Link"),
							FUIAction(
								FExecuteAction::CreateLambda([Client]() { ConnectOtherClient(Client); }),
								FCanExecuteAction::CreateLambda([bCanConnect]() { return bCanConnect; })));
					}
					Sub.AddMenuSeparator();
					Sub.AddMenuEntry(
						LOCTEXT("CopyCodex", "Copy the \"codex mcp add\" command"),
						LOCTEXT("CopyCodexTip", "Codex keeps its servers in ~/.codex/config.toml: paste the command into a terminal once, for every project."),
						FSlateIcon(FAppStyle::GetAppStyleSetName(), "GenericCommands.Copy"),
						FUIAction(FExecuteAction::CreateStatic(&CopyCodexCommand)));
				}),
				/*bInOpenSubMenuOnClick*/ false,
				FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Link"));
			Menu.AddMenuEntry(
				LOCTEXT("CopyCommand", "Copy the \"claude mcp add\" command"),
				LOCTEXT("CopyCommandTip", "To connect it only for you, without touching the repo's .mcp.json: paste it into a terminal opened in the project folder."),
				FSlateIcon(FAppStyle::GetAppStyleSetName(), "GenericCommands.Copy"),
				FUIAction(FExecuteAction::CreateStatic(&CopyAddCommand)));
		}
		Menu.EndSection();

		Menu.BeginSection("UEAgentSkills", LOCTEXT("SkillsSection", "Skills"));
		{
			TArray<FString> Lines;
			bool bAnyInstalled = false;
			TArray<TPair<McpSkills::ETarget, McpSkills::FStatus>> Targets;
			for (const McpSkills::ETarget Target : McpSkills::AllTargets())
			{
				const McpSkills::FStatus Status = McpSkills::InspectProject(Target);
				bAnyInstalled |= Status.AnyInstalled();
				Lines.Add(FString::Printf(TEXT("%s (%s): %s"), McpSkills::TargetClients(Target), McpSkills::TargetRelativeDir(Target), *Status.Summary()));
				Targets.Add({ Target, Status });
			}
			Menu.AddWidget(InfoBlock(Lines), FText::GetEmpty(), /*bNoIndent*/ true);
			for (const auto& Pair : Targets)
			{
				const McpSkills::ETarget Target = Pair.Key;
				const McpSkills::FStatus& Status = Pair.Value;
				const bool bCurrent = Status.AnyInstalled() && !Status.NeedsUpdate();
				const FString Verb = Status.NeedsUpdate() ? TEXT("Update") : TEXT("Install");
				Menu.AddMenuEntry(
					FText::FromString(bCurrent
						? FString::Printf(TEXT("Skills for %s are current"), McpSkills::TargetClients(Target))
						: FString::Printf(TEXT("%s the skills for %s"), *Verb, McpSkills::TargetClients(Target))),
					FText::FromString(FString::Printf(TEXT("Copies UEAgent's skills (how to use its tools, and how to report an issue or send a fix) into %s, stamped with version %s. Only folders UEAgent installed are ever replaced or deleted."),
						*Status.Dir, *Status.Version)),
					FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Download"),
					FUIAction(
						FExecuteAction::CreateLambda([Target]() { InstallSkills(Target); }),
						FCanExecuteAction::CreateLambda([bCurrent]() { return !bCurrent; })));
			}
			if (bAnyInstalled)
			{
				Menu.AddMenuEntry(
					LOCTEXT("RemoveSkills", "Remove UEAgent's skills from the project"),
					LOCTEXT("RemoveSkillsTip", "Deletes the skill folders UEAgent installed in .claude/skills and .agents/skills, and nothing else."),
					FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Delete"),
					FUIAction(FExecuteAction::CreateStatic(&RemoveSkills)));
			}
		}
		Menu.EndSection();

		Menu.BeginSection("UEAgentSettings", FText::GetEmpty());
		{
			Menu.AddMenuEntry(
				LOCTEXT("Settings", "Settings..."),
				LOCTEXT("SettingsTip", "Editor Preferences > Plugins > UEAgent: port, token and hints."),
				FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Settings"),
				FUIAction(FExecuteAction::CreateStatic(&OpenSettings)));
		}
		Menu.EndSection();

		return Menu.MakeWidget();
	}

	static TSharedRef<SWidget> MakeStatusWidget()
	{
		// A circle: an 8x8 rounded box with radius 4, tinted by the state.
		static const FSlateRoundedBoxBrush DotBrush(FLinearColor::White, 4.0f, FVector2f(8.0f, 8.0f));
		return SNew(SComboButton)
			.ContentPadding(FMargin(6.0f, 0.0f))
			.MenuPlacement(MenuPlacement_AboveAnchor)
			.ComboButtonStyle(&FAppStyle::Get().GetWidgetStyle<FComboButtonStyle>("SimpleComboButton"))
			.ToolTipText_Static(&TooltipText)
			.OnGetMenuContent(FOnGetContent::CreateStatic(&MakeMenu))
			.ButtonContent()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(SImage)
					.Image(&DotBrush)
					.ColorAndOpacity_Static(&DotColor)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(FMargin(5.0f, 0.0f, 0.0f, 0.0f))
				[
					SNew(STextBlock)
					.TextStyle(&FAppStyle::Get().GetWidgetStyle<FTextBlockStyle>("NormalText"))
					.Text_Static(&LabelText)
				]
			];
	}

	static void RegisterStatusBar()
	{
		FToolMenuOwnerScoped OwnerScoped(OwnerName);
		UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("LevelEditor.StatusBar.ToolBar");
		FToolMenuSection& Section = Menu->AddSection("UEAgent", FText::GetEmpty(), FToolMenuInsert(NAME_None, EToolMenuInsertType::First));
		// Dynamic: a new widget every time the bar is generated, never one shared between two parents.
		Section.AddDynamicEntry("UEAgentStatus", FNewToolMenuSectionDelegate::CreateLambda([](FToolMenuSection& InSection)
		{
			InSection.AddEntry(FToolMenuEntry::InitWidget("UEAgentStatusWidget", MakeStatusWidget(), FText::GetEmpty(), /*bNoIndent*/ true, /*bSearchable*/ false));
		}));
	}

	// --- Startup hint ------------------------------------------------------------------------------

	static void CloseHint(SNotificationItem::ECompletionState State)
	{
		if (const TSharedPtr<SNotificationItem> Item = HintItem.Pin())
		{
			Item->SetCompletionState(State);
			Item->ExpireAndFadeout();
		}
		HintItem.Reset();
	}

	static void ShowConnectHint()
	{
		const UUEAgentSettings* Settings = GetDefault<UUEAgentSettings>();
		if (!Settings->bShowConnectHint || !Settings->bEnableHttpServer || HintItem.IsValid())
		{
			return;
		}
		const McpClaudeCode::FStatus& Project = ProjectStatus(/*bRefresh*/ true);
		// Only what "Connect" can repair: not an unreadable file, nor a rejection by Claude Code. And
		// not when the project already uses another client.
		if (Project.State == EState::Connected || Project.State == EState::Unreadable || ConnectedOtherClients(/*bRefresh*/ true).Num() > 0)
		{
			return;
		}

		FNotificationInfo Info(LOCTEXT("HintTitle", "No AI client is connected to this editor"));
		Info.SubText = FText::FromString(Project.Detail + TEXT(" Cursor, VS Code, Antigravity CLI and Codex: the \"MCP\" menu in the status bar."));
		Info.bFireAndForget = false;
		Info.WidthOverride = 440.0f;
		Info.ButtonDetails.Add(FNotificationButtonInfo(
			LOCTEXT("HintConnect", "Connect Claude Code"),
			FText::FromString(FString::Printf(TEXT("Writes the \"%s\" entry into %s without touching the rest of the file."), McpClaudeCode::ServerName, *Project.Path)),
			FSimpleDelegate::CreateLambda([]()
			{
				CloseHint(SNotificationItem::CS_Success);
				ConnectProject();
			}),
			SNotificationItem::CS_Pending));
		Info.ButtonDetails.Add(FNotificationButtonInfo(
			LOCTEXT("HintLater", "Not now"),
			LOCTEXT("HintLaterTip", "Still available from the \"MCP\" menu in the status bar."),
			FSimpleDelegate::CreateLambda([]() { CloseHint(SNotificationItem::CS_None); }),
			SNotificationItem::CS_Pending));
		Info.ButtonDetails.Add(FNotificationButtonInfo(
			LOCTEXT("HintNever", "Don't show again"),
			LOCTEXT("HintNeverTip", "Turn it back on in Editor Preferences > Plugins > UEAgent."),
			FSimpleDelegate::CreateLambda([]()
			{
				UUEAgentSettings* Mutable = GetMutableDefault<UUEAgentSettings>();
				Mutable->bShowConnectHint = false;
				Mutable->SaveConfig();
				CloseHint(SNotificationItem::CS_None);
			}),
			SNotificationItem::CS_Pending));

		HintItem = FSlateNotificationManager::Get().AddNotification(Info);
		if (const TSharedPtr<SNotificationItem> Item = HintItem.Pin())
		{
			Item->SetCompletionState(SNotificationItem::CS_Pending);
		}
	}

	static void OnEditorInitialized(double /*Duration*/)
	{
		if (IsRunningCommandlet() || FApp::IsUnattended() || GIsAutomationTesting)
		{
			return;
		}
		// A few seconds later: on startup the editor chains its own notifications and this one
		// would get lost.
		HintTickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float)
		{
			HintTickerHandle.Reset();
			ShowConnectHint();
			return false;
		}), 4.0f);
	}
}

void McpEditorUI::Startup()
{
	using namespace McpEditorUIPrivate;
	ToolMenusStartupHandle = UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateStatic(&RegisterStatusBar));
	EditorInitializedHandle = FEditorDelegates::OnEditorInitialized.AddStatic(&OnEditorInitialized);
	McpActivityPanel::Register();
}

void McpEditorUI::Shutdown()
{
	using namespace McpEditorUIPrivate;
	if (HintTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(HintTickerHandle);
		HintTickerHandle.Reset();
	}
	if (EditorInitializedHandle.IsValid())
	{
		FEditorDelegates::OnEditorInitialized.Remove(EditorInitializedHandle);
		EditorInitializedHandle.Reset();
	}
	if (ToolMenusStartupHandle.IsValid())
	{
		UToolMenus::UnRegisterStartupCallback(ToolMenusStartupHandle);
		ToolMenusStartupHandle.Reset();
	}
	UToolMenus::UnregisterOwner(OwnerName);
	McpActivityPanel::Unregister();
}

#undef LOCTEXT_NAMESPACE
