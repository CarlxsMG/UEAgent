#include "Mcp/McpActivityPanel.h"

#include "Mcp/McpActivity.h"

#include "Algo/Reverse.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "Styling/AppStyle.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SHeaderRow.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "UEAgentActivity"

// Namespace name unique per file: the module builds as unity (see Build.cs).
namespace McpActivityPanelPrivate
{
	using McpActivity::EOutcome;
	using FEntryPtr = TSharedPtr<const McpActivity::FEntry>;

	static const FName TabName(TEXT("UEAgentActivity"));
	static const FName TimeColumn(TEXT("Time"));
	static const FName ToolColumn(TEXT("Tool"));
	static const FName DurationColumn(TEXT("Duration"));
	static const FName ResultColumn(TEXT("Result"));
	static const FName DetailColumn(TEXT("Detail"));

	static FSlateColor OutcomeColor(EOutcome Outcome)
	{
		switch (Outcome)
		{
		case EOutcome::Warning: return FLinearColor(1.0f, 0.72f, 0.2f);
		case EOutcome::Failed:  return FLinearColor(0.95f, 0.33f, 0.33f);
		case EOutcome::Refused: return FLinearColor(0.75f, 0.55f, 1.0f);
		default:                return FLinearColor(0.35f, 0.8f, 0.45f);
		}
	}

	class SActivityRow : public SMultiColumnTableRow<FEntryPtr>
	{
	public:
		SLATE_BEGIN_ARGS(SActivityRow) {}
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& InOwnerTable, FEntryPtr InEntry)
		{
			Entry = InEntry;
			FSuperRowType::Construct(FTableRowArgs(), InOwnerTable);
		}

		virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& Column) override
		{
			FText Text;
			FSlateColor Color = FSlateColor::UseForeground();
			if (Column == TimeColumn)
			{
				Text = FText::FromString(Entry->Started.ToString(TEXT("%H:%M:%S")));
			}
			else if (Column == ToolColumn)
			{
				Text = FText::FromString(Entry->Tool);
			}
			else if (Column == DurationColumn)
			{
				Text = FText::FromString(FString::Printf(TEXT("%.0f ms"), Entry->Milliseconds));
			}
			else if (Column == ResultColumn)
			{
				Text = FText::FromString(McpActivity::OutcomeName(Entry->Outcome));
				Color = OutcomeColor(Entry->Outcome);
			}
			else if (Column == DetailColumn)
			{
				Text = FText::FromString(Entry->Detail);
			}
			return SNew(SBox)
				.Padding(FMargin(6.0f, 3.0f))
				[
					SNew(STextBlock)
					.Text(Text)
					.ColorAndOpacity(Color)
					.ToolTipText(Column == DetailColumn ? Text : FText::GetEmpty())
				];
		}

	private:
		FEntryPtr Entry;
	};

	class SActivityPanel : public SCompoundWidget
	{
	public:
		SLATE_BEGIN_ARGS(SActivityPanel) {}
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs)
		{
			Refresh();
			ChangedHandle = McpActivity::OnChanged().AddSP(this, &SActivityPanel::OnActivityChanged);

			ChildSlot
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(8.0f, 6.0f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock).Text(this, &SActivityPanel::SummaryText)
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					[
						SNew(SButton)
						.Text(LOCTEXT("Clear", "Clear"))
						.ToolTipText(LOCTEXT("ClearTip", "Empties this list. It does not undo anything."))
						.OnClicked_Lambda([]()
						{
							McpActivity::Clear();
							return FReply::Handled();
						})
					]
				]
				+ SVerticalBox::Slot()
				.FillHeight(1.0f)
				[
					SAssignNew(List, SListView<FEntryPtr>)
					.ListItemsSource(&Rows)
					.OnGenerateRow(this, &SActivityPanel::MakeRow)
					.HeaderRow
					(
						SNew(SHeaderRow)
						+ SHeaderRow::Column(TimeColumn).DefaultLabel(LOCTEXT("Time", "Time")).FixedWidth(76.0f)
						+ SHeaderRow::Column(ToolColumn).DefaultLabel(LOCTEXT("Tool", "Tool")).FillWidth(0.3f)
						+ SHeaderRow::Column(DurationColumn).DefaultLabel(LOCTEXT("Duration", "Duration")).FixedWidth(80.0f)
						+ SHeaderRow::Column(ResultColumn).DefaultLabel(LOCTEXT("Result", "Result")).FixedWidth(80.0f)
						+ SHeaderRow::Column(DetailColumn).DefaultLabel(LOCTEXT("Detail", "Detail")).FillWidth(0.7f)
					)
				]
			];
		}

		virtual ~SActivityPanel() override
		{
			McpActivity::OnChanged().Remove(ChangedHandle);
		}

	private:
		void Refresh()
		{
			Rows = McpActivity::GetEntries();
			Algo::Reverse(Rows); // newest first
		}

		void OnActivityChanged()
		{
			Refresh();
			if (List.IsValid())
			{
				List->RequestListRefresh();
			}
		}

		TSharedRef<ITableRow> MakeRow(FEntryPtr Entry, const TSharedRef<STableViewBase>& OwnerTable)
		{
			return SNew(SActivityRow, OwnerTable, Entry);
		}

		FText SummaryText() const
		{
			int32 Warnings = 0;
			int32 Failures = 0;
			int32 Refusals = 0;
			for (const FEntryPtr& Entry : Rows)
			{
				Warnings += Entry->Outcome == EOutcome::Warning ? 1 : 0;
				Failures += Entry->Outcome == EOutcome::Failed ? 1 : 0;
				Refusals += Entry->Outcome == EOutcome::Refused ? 1 : 0;
			}
			return FText::Format(LOCTEXT("Summary", "{0} calls, newest first (the last {1} are kept): {2} warnings, {3} failed, {4} refused"),
				Rows.Num(), McpActivity::MaxEntries, Warnings, Failures, Refusals);
		}

		TArray<FEntryPtr> Rows;
		TSharedPtr<SListView<FEntryPtr>> List;
		FDelegateHandle ChangedHandle;
	};
}

void McpActivityPanel::Register()
{
	using namespace McpActivityPanelPrivate;
	if (!FSlateApplication::IsInitialized())
	{
		return;
	}
	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(TabName, FOnSpawnTab::CreateLambda([](const FSpawnTabArgs&)
	{
		return SNew(SDockTab)
			.TabRole(ETabRole::NomadTab)
			[
				SNew(SActivityPanel)
			];
	}))
	.SetDisplayName(LOCTEXT("TabTitle", "UEAgent Activity"))
	.SetTooltipText(LOCTEXT("TabTip", "The last tool calls the agents made in this editor, with their duration and outcome."))
	.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Recent"))
	.SetMenuType(ETabSpawnerMenuType::Hidden);
}

void McpActivityPanel::Unregister()
{
	if (FSlateApplication::IsInitialized())
	{
		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(McpActivityPanelPrivate::TabName);
	}
}

void McpActivityPanel::Open()
{
	FGlobalTabmanager::Get()->TryInvokeTab(FTabId(McpActivityPanelPrivate::TabName));
}

#undef LOCTEXT_NAMESPACE
