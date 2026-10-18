// Copyright MapModelers. All Rights Reserved.

#include "SurfFlagsView.h"
#include "SurfTheme.h"
#include "SurfEdMode.h"
#include "SurfPrefabInstance.h"
#include "SurfSession.h"
#include "SurfTypes.h"
#include "SurfWindows.h"
#include "SurfSidePanel.h"

#include "Editor.h"
#include "EditorModeManager.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/StyleColors.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"

#define LOCTEXT_NAMESPACE "SurfacingProFlags"

namespace
{
	USurfEdMode* ActiveMode()
	{
		return Cast<USurfEdMode>(GLevelEditorModeTools().GetActiveScriptableMode(USurfEdMode::EM_SurfacingPro));
	}

	void Picked(TArray<FSurfPatchRef>& P, TArray<FSurfFaceRef>& B)
	{
		if (const USurfEdMode* Mode = ActiveMode())
		{
			Mode->PiecesPicked(P, B);
		}
	}

	/** Placed prefabs picked and no piece (S201a): the ticks are the placements' override. */
	int32 PlacementsOnly()
	{
		TArray<FSurfPatchRef> P; TArray<FSurfFaceRef> B; Picked(P, B);
		const USurfEdMode* Mode = ActiveMode();
		return Mode && P.Num() + B.Num() == 0 ? Mode->PlacementsPicked().Num() : 0;
	}

	/**
	 * The window reads the mode each frame rather than holding it: the mode
	 * comes and goes with the level and the tab comes back with the layout,
	 * and a tab that held a dead mode would be a tab of four grey ticks.
	 */
	class SSurfFlagsPanel : public SCompoundWidget
	{
	public:
		SLATE_BEGIN_ARGS(SSurfFlagsPanel) {}
		SLATE_END_ARGS()

		void Construct(const FArguments&)
		{
			TSharedRef<SVerticalBox> Rows = SNew(SVerticalBox);

			// What is picked, first: the ticks below are about these.
			Rows->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				SNew(STextBlock)
				.Font(SurfTheme::Bold(8))
				.ColorAndOpacity(SurfTheme::Header())
				.Text_Lambda([]()
				{
					if (!ActiveMode())
					{
						return LOCTEXT("FlagsModeOff", "Enter the SurfacingPro mode to set flags");
					}
					TArray<FSurfPatchRef> P; TArray<FSurfFaceRef> B; Picked(P, B);
					if (const int32 Placed = PlacementsOnly())
					{
						return FText::Format(LOCTEXT("FlagsPlaced", "{0} placed prefab{1}"), FText::AsNumber(Placed), Placed == 1 ? FText::GetEmpty() : FText::FromString(TEXT("s")));
					}
					if (P.Num() + B.Num() == 0)
					{
						return LOCTEXT("FlagsNothing", "Pick a patch, a primitive or a placed prefab");
					}
					FString Who;
					if (P.Num() > 0) { Who += FString::Printf(TEXT("%d patch%s"), P.Num(), P.Num() == 1 ? TEXT("") : TEXT("es")); }
					if (B.Num() > 0) { Who += FString::Printf(TEXT("%s%d primitive%s"), Who.IsEmpty() ? TEXT("") : TEXT(", "), B.Num(), B.Num() == 1 ? TEXT("") : TEXT("s")); }
					return FText::FromString(Who);
				})
			];

			auto Row = [&Rows](ESurfPrimitiveFlags Which, const FText& Label, const FText& Tip)
			{
				Rows->AddSlot().AutoHeight().Padding(0.0f, 2.0f)
				[
					SNew(SCheckBox)
					.ToolTipText(Tip)
					.IsEnabled_Lambda([]()
					{
						TArray<FSurfPatchRef> P; TArray<FSurfFaceRef> B; Picked(P, B);
						const USurfEdMode* Mode = ActiveMode();
						return P.Num() + B.Num() > 0 || (PlacementsOnly() > 0 && Mode && Mode->PlacementOverrideState() != 0);
					})
					.IsChecked_Lambda([Which]()
					{
						const USurfEdMode* Mode = ActiveMode();
						const int32 State = !Mode ? 0 : PlacementsOnly() > 0 ? Mode->PlacementFlagState(Which) : Mode->PieceFlagState(Which);
						return State == 1 ? ECheckBoxState::Checked : State == -1 ? ECheckBoxState::Undetermined : ECheckBoxState::Unchecked;
					})
					.OnCheckStateChanged_Lambda([Which](ECheckBoxState New)
					{
						if (USurfEdMode* Mode = ActiveMode())
						{
							if (PlacementsOnly() > 0) { Mode->SetPlacementFlag(Which, New == ECheckBoxState::Checked); }
							else { Mode->SetPieceFlag(Which, New == ECheckBoxState::Checked); }
						}
					})
					[
						SNew(STextBlock).Text(Label).Font(SurfTheme::Font(9))
					]
				];
			};
			Row(ESurfPrimitiveFlags::Detail, LOCTEXT("FlagDetail", "Detail"), LOCTEXT("FlagDetailTip", "Trim, not structure: baked apart and never in the HLOD, so the far half of a map is not built from it."));
			Row(ESurfPrimitiveFlags::NonColliding, LOCTEXT("FlagNoCol", "No collision"), LOCTEXT("FlagNoColTip", "Baked with no collision at all."));
			Row(ESurfPrimitiveFlags::NoShadow, LOCTEXT("FlagNoShadow", "No shadow"), LOCTEXT("FlagNoShadowTip", "Baked casting no shadow."));
			Row(ESurfPrimitiveFlags::NoDraw, LOCTEXT("FlagNoDraw", "No draw"), LOCTEXT("FlagNoDrawTip", "Hidden in the game, kept for collision: a blocker the player walks into and never sees. Drawn in the editor; Game view and the game hide it."));

			// A placed prefab (S201a): its pieces' flags, or its own over all of
			// them - one button to take the override, one to give it back.
			Rows->AddSlot().AutoHeight().Padding(0.0f, 6.0f, 0.0f, 0.0f)
			[
				SNew(SHorizontalBox)
				.Visibility_Lambda([]() { return PlacementsOnly() > 0 ? EVisibility::Visible : EVisibility::Collapsed; })
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SButton)
					.ButtonStyle(&SurfTheme::ToolButton())
					.ContentPadding(FMargin(8.0f, 1.0f))
					.ToolTipText_Lambda([]()
					{
						const USurfEdMode* Mode = ActiveMode();
						return Mode && Mode->PlacementOverrideState() == 1
							? LOCTEXT("FlagsCancelOverrideTip", "Back to its pieces' own flags.")
							: LOCTEXT("FlagsOverrideTip", "Flags for the whole placement, over every piece in it. To differ piece by piece: save it as a new prefab, or unpack it.");
					})
					.OnClicked_Lambda([]()
					{
						if (USurfEdMode* Mode = ActiveMode())
						{
							Mode->SetPlacementOverride(Mode->PlacementOverrideState() != 1);
						}
						return FReply::Handled();
					})
					[
						SNew(STextBlock)
						.Font(SurfTheme::Font(9))
						.Text_Lambda([]()
						{
							const USurfEdMode* Mode = ActiveMode();
							// The state in the word itself (S249a: "override button under
							// flags takes two lines ... perhaps remove the comment").
							const int32 State = Mode ? Mode->PlacementOverrideState() : 0;
							return State == 1 ? LOCTEXT("FlagsCancelOverride", "Cancel override")
								: State == -1 ? LOCTEXT("FlagsOverrideAll", "Override all")
								: LOCTEXT("FlagsOverride", "Override");
						})
					]
				]
				// Its own paint (S251j).
				+ SHorizontalBox::Slot().AutoWidth().Padding(4.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SButton)
					.ButtonStyle(&SurfTheme::ToolButton())
					.ContentPadding(FMargin(8.0f, 1.0f))
					.ToolTipText_Lambda([]()
					{
						const USurfEdMode* Mode = ActiveMode();
						return Mode && Mode->PlacementPaintOverrideState() == 1
							? LOCTEXT("FlagsCancelPaintTip", "Back to the prefab's paint; this one's own paint is dropped.")
							: LOCTEXT("FlagsPaintTip", "Paint this placed prefab where it stands, starting from the prefab's paint; the asset and the other instances keep theirs.");
					})
					.OnClicked_Lambda([]()
					{
						if (USurfEdMode* Mode = ActiveMode())
						{
							Mode->SetPlacementPaintOverride(Mode->PlacementPaintOverrideState() != 1);
						}
						return FReply::Handled();
					})
					[
						SNew(STextBlock)
						.Font(SurfTheme::Font(9))
						.Text_Lambda([]()
						{
							const USurfEdMode* Mode = ActiveMode();
							const int32 State = Mode ? Mode->PlacementPaintOverrideState() : 0;
							return State == 1 ? LOCTEXT("FlagsCancelPaint", "Cancel paint override")
								: State == -1 ? LOCTEXT("FlagsPaintAll", "Paint override all")
								: LOCTEXT("FlagsPaint", "Paint override");
						})
					]
				]
			];

			// Each picked piece and what it wears, one line each (S154c):
			// the window says exactly what a tick will land on.
			Rows->AddSlot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Font(SurfTheme::Font(8))
				.ColorAndOpacity(FStyleColors::Foreground)
				.AutoWrapText(true)
				.Text_Lambda([]()
				{
					const USurfEdMode* Mode = ActiveMode();
					const TArray<FString> Lines = Mode ? Mode->PieceFlagLines() : TArray<FString>();
					return Lines.Num() > 0 ? FText::FromString(FString::Join(Lines, TEXT("\n"))) : FText::GetEmpty();
				})
			];
			ChildSlot
			.Padding(10.0f, 8.0f)
			[
				SNew(SBox).MaxDesiredWidth(260.0f)
				[
					Rows
				]
			];
		}
	};
}

namespace SurfFlagsTab
{
	const FName TabName("SurfacingProFlags");

	static TSharedRef<SDockTab> Spawn(const FSpawnTabArgs&)
	{
		TSharedRef<SDockTab> Tab = SNew(SDockTab)
			.TabRole(ETabRole::NomadTab)
			.Label(LOCTEXT("FlagsTabLabel", "Flags"))
			[
				SNew(SSurfFlagsPanel)
			];
		SurfWindows::KeepOffTaskbar(Tab);
		return Tab;
	}

	void Register()
	{
		if (!FSlateApplication::IsInitialized() || FGlobalTabmanager::Get()->HasTabSpawner(TabName))
		{
			return;
		}
		FGlobalTabmanager::Get()->RegisterNomadTabSpawner(TabName, FOnSpawnTab::CreateStatic(&Spawn))
			.SetDisplayName(LOCTEXT("FlagsTabTitle", "SurfacingPro Flags"))
			.SetTooltipText(LOCTEXT("FlagsTabTip", "What the bake does with the picked pieces: Detail, No collision, No shadow, No draw."))
			.SetGroup(WorkspaceMenu::GetMenuStructure().GetLevelEditorCategory())
			.SetIcon(FSlateIcon(TEXT("SurfacingProStyle"), "SurfacingPro.Verb.flag"));
	}

	void Unregister()
	{
		if (FSlateApplication::IsInitialized())
		{
			FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(TabName);
		}
	}

	void Open()
	{
		// The frame retired (S177a): Flags is a tab of the SidePanel.
		SurfSidePanel::Show(ESurfSideTab::Flags);
	}

	TSharedRef<SWidget> MakePanel()
	{
		return SNew(SSurfFlagsPanel);
	}
}

#undef LOCTEXT_NAMESPACE
