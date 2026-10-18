// Copyright MapModelers. All Rights Reserved.

#include "SurfPaintView.h"
#include "SurfTheme.h"
#include "SurfEdMode.h"
#include "SurfInspector.h"
#include "SurfSettings.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Widgets/Input/SComboButton.h"
#include "SurfSession.h"
#include "SurfWindows.h"
#include "SurfSidePanel.h"

#include "Editor.h"
#include "EditorModeManager.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/StyleDefaults.h"
#include "Widgets/Colors/SColorBlock.h"
#include "Widgets/Colors/SColorPicker.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SSpinBox.h"
#include "SurfNumberField.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"

#define LOCTEXT_NAMESPACE "SurfacingProPaint"

namespace
{
	USurfEdMode* ActiveMode()
	{
		return Cast<USurfEdMode>(GLevelEditorModeTools().GetActiveScriptableMode(USurfEdMode::EM_SurfacingPro));
	}

	USurfSettings* Settings()
	{
		return GetMutableDefault<USurfSettings>();
	}

	void Saved()
	{
		if (USurfSettings* S = Settings())
		{
			S->SaveConfig();
		}
	}
}

class SSurfPaintPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SSurfPaintPanel)
		: _Bare(false)
		{}
		/** In the SidePanel (S177a): no plate of its own, the panel is the plate. */
		SLATE_ARGUMENT(bool, Bare)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		// Compact since S229a ("a lot of text at the moment and big buttons
		// ... not too much text on screen at the same time"): small type,
		// tight buttons, the words in the tooltips.
		const FCheckBoxStyle& Toggle = FAppStyle::Get().GetWidgetStyle<FCheckBoxStyle>("ToggleButtonCheckbox");

		// R G B A (S248a: "instead of 'Col' just have R G B all three
		// enabled"): a click takes the one, Shift-click adds or takes it; R,
		// G and B together are the colour - the swatches beside them.
		auto Channel = [&Toggle](const FText& Label, const FText& Tip, int32 Bit)
		{
			return SNew(SBox).Padding(1.0f, 0.0f)
				[
					SNew(SCheckBox)
					.Style(&Toggle)
					.ToolTipText(Tip)
					.IsChecked_Lambda([Bit]()
					{
						const USurfSettings* S = Settings();
						return S && (S->PaintSet().Bits & Bit) != 0 ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
					})
					.OnCheckStateChanged_Lambda([Bit](ECheckBoxState)
					{
						USurfSettings* S = Settings();
						if (!S) { return; }
						if (FSlateApplication::Get().GetModifierKeys().IsShiftDown())
						{
							const int32 Bits = S->PaintSet().Bits ^ Bit;
							S->SetPaintSet(Bits != 0 ? Bits : Bit);
						}
						else
						{
							S->SetPaintSet(Bit);
						}
						S->SaveConfig();
						if (USurfEdMode* Mode = ActiveMode()) { Mode->RefreshPaintView(); }
					})
					.Padding(FMargin(6.0f, 2.0f))
					[
						SNew(STextBlock).Text(Label).Font(SurfTheme::Small())
					]
				];
		};

		auto Swatch = [](bool bFront) -> TSharedRef<SWidget>
		{
			return SNew(SBox).WidthOverride(bFront ? 30.0f : 20.0f).HeightOverride(bFront ? 18.0f : 13.0f)
				[
					SNew(SColorBlock)
					// The colour as it is, whatever channels are on (S247g: "why
					// when switching from Col to R G or B, the color changes
					// too?" - it dimmed to say it was not in use, and read as a
					// faded red). The chips say which channels are on.
					.Color_Lambda([bFront]() -> FLinearColor
					{
						const USurfSettings* S = Settings();
						return S ? (bFront ? S->PaintColour : S->PaintColourBack) : FLinearColor::Red;
					})
					.ShowBackgroundForAlpha(false)
					.AlphaDisplayMode(EColorBlockAlphaDisplayMode::Ignore)
					.ToolTipText(bFront
						? LOCTEXT("ColourTip", "The colour a drag paints with R, G and B on. Click to pick it; R, G and B come on.")
						: LOCTEXT("ColourBackTip", "The colour a Ctrl-drag paints and Clear fills with, R, G and B on. Click to pick."))
					.OnMouseButtonDown_Lambda([bFront](const FGeometry&, const FPointerEvent&) -> FReply
					{
						// A colour picked is a colour wanted (S238a: "pressing FILL
						// while im in Green only with a red color = fills with
						// green, it should not"): R, G and B come on (S248a), A
						// as it was.
						if (USurfSettings* S = Settings())
						{
							if (!S->PaintSet().IsColour())
							{
								S->SetPaintSet(7 | (S->PaintSet().Bits & 8));
								S->SaveConfig();
								if (USurfEdMode* Mode = ActiveMode()) { Mode->RefreshPaintView(); }
							}
						}
						FColorPickerArgs Args;
						Args.bUseAlpha = false;
						Args.bOnlyRefreshOnOk = false;
						Args.InitialColor = Settings() ? (bFront ? Settings()->PaintColour : Settings()->PaintColourBack) : FLinearColor::Red;
						Args.OnColorCommitted = FOnLinearColorValueChanged::CreateLambda([bFront](FLinearColor Picked)
						{
							if (USurfSettings* S = Settings()) { (bFront ? S->PaintColour : S->PaintColourBack) = Picked; S->SaveConfig(); }
						});
						OpenColorPicker(Args);
						return FReply::Handled();
					})
				];
		};

		auto Row = [](const FText& Label)
		{
			return SNew(STextBlock)
				.Text(Label)
				.Font(SurfTheme::Bold(8))
				.ColorAndOpacity(SurfTheme::Header());
		};
		auto Word = [](const FText& Text, const FText& Tip)
		{
			return SNew(SBox).VAlign(VAlign_Center).Padding(0.0f, 0.0f, 4.0f, 0.0f)
				[
					SNew(STextBlock).Text(Text).ToolTipText(Tip).Font(SurfTheme::Small()).ColorAndOpacity(SurfTheme::Text())
				];
		};
		auto Number = [](TFunction<double()> Get, TFunction<void(double)> Set, double Min, double Max, double Step, int32 Digits, const FText& Tip)
		{
			return SNew(SSurfNumberField)
				.Width(44.0f)
				.Step(Step)
				.Digits(Digits)
				.ToolTipText(Tip)
				.Value_Lambda([Get]() { return TOptional<double>(Get()); })
				.OnSet([Set, Min, Max](double V) { Set(FMath::Clamp(V, Min, Max)); });
		};
		auto Press = [](const FText& Label, const FText& Tip, TFunction<void()> Run)
		{
			return SNew(SBox).Padding(0.0f, 1.0f, 3.0f, 1.0f)
				[
					SNew(SButton)
					.ButtonStyle(&SurfTheme::ToolButton())
					.ToolTipText(Tip)
					.ContentPadding(FMargin(6.0f, 1.0f))
					.OnClicked_Lambda([Run]() { Run(); return FReply::Handled(); })
					[
						SNew(STextBlock).Text(Label).Font(SurfTheme::Small())
					]
				];
		};
		auto Adjust = [Press](const FText& Label, const FText& Tip, const TCHAR* Id, int32 Kind, bool bOptions)
		{
			const FString Name(Id);
			return Press(Label, Tip, [Name, Kind, bOptions]()
			{
				USurfEdMode* M = ActiveMode();
				if (!M) { return; }
				M->NoteVerb(Name, [M, Kind]() { M->AdjustPaint(Kind); });
				M->AdjustPaint(Kind);
				M->AfterVerbRan(Name, bOptions);
			});
		};
		ChildSlot
		[
			SNew(SBorder)
			.BorderImage(InArgs._Bare ? FStyleDefaults::GetNoBrush() : FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.Padding(FMargin(6.0f, 4.0f))
			[
				SNew(SVerticalBox)

				// The brush, the channels, the two colours - one line.
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 4.0f)
				[
					SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(0.0f, 2.0f))
					+ SWrapBox::Slot().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)
					[
						SNew(SCheckBox)
						.Style(&Toggle)
						.ToolTipText(LOCTEXT("BrushTip", "The paint brush, Shift+B in the viewport: drag paints up, Ctrl-drag paints down, Ctrl+wheel or [ ] for the size, Escape puts it away. Paint View shows the channel on every surface."))
						.IsChecked_Lambda([]() { const USurfEdMode* Mode = ActiveMode(); return Mode && Mode->IsPainting() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
						.OnCheckStateChanged_Lambda([](ECheckBoxState) { if (USurfEdMode* Mode = ActiveMode()) { Mode->TogglePaint(); } })
						.Padding(FMargin(8.0f, 2.0f))
						[
							SNew(STextBlock).Font(SurfTheme::Bold(8)).Text(LOCTEXT("Brush", "Brush"))
						]
					]
					// The view kind beside the brush (S247f: "add shortcut toggle
					// button to paint viewmode under Paint NPanel section").
					+ SWrapBox::Slot().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)
					[
						SNew(SCheckBox)
						.Style(&Toggle)
						.ToolTipText(LOCTEXT("PaintViewChipTip", "Paint View: the channel on every surface, brush or not - dark where empty, lit where full. A view kind beside Lit, Unlit and Matcap."))
						.IsChecked_Lambda([]() { const USurfEdMode* Mode = ActiveMode(); return Mode && Mode->GetViewKind() == 3 ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
						.OnCheckStateChanged_Lambda([](ECheckBoxState) { if (USurfEdMode* Mode = ActiveMode()) { Mode->SetViewKind(Mode->GetViewKind() == 3 ? 1 : 3); } })
						.Padding(FMargin(8.0f, 2.0f))
						[
							SNew(STextBlock).Font(SurfTheme::Bold(8)).Text(LOCTEXT("PaintViewChip", "View"))
						]
					]
					+ SWrapBox::Slot()[ Channel(LOCTEXT("ChR", "R"), LOCTEXT("ChRTip", "The red channel - what a material's R reads. Shift-click adds one; R, G and B together paint the colour beside."), 1) ]
					+ SWrapBox::Slot()[ Channel(LOCTEXT("ChG", "G"), LOCTEXT("ChGTip", "The green channel. Shift-click adds one; R, G and B together paint the colour beside."), 2) ]
					+ SWrapBox::Slot()[ Channel(LOCTEXT("ChB", "B"), LOCTEXT("ChBTip", "The blue channel. Shift-click adds one; R, G and B together paint the colour beside."), 4) ]
					+ SWrapBox::Slot()[ Channel(LOCTEXT("ChA", "A"), LOCTEXT("ChATip", "The vertex alpha. Shift-click adds it to the others."), 8) ]
					+ SWrapBox::Slot().VAlign(VAlign_Center).Padding(5.0f, 0.0f, 0.0f, 0.0f)[ Swatch(true) ]
					+ SWrapBox::Slot().VAlign(VAlign_Center)
					[
						SNew(SButton)
						.ButtonStyle(FAppStyle::Get(), "SimpleButton")
						.ContentPadding(FMargin(1.0f, 0.0f))
						.ToolTipText(LOCTEXT("SwapTip", "Swap the two colours (Shift+X in the viewport while the brush is out)."))
						.OnClicked_Lambda([]() { if (USurfEdMode* Mode = ActiveMode()) { Mode->SwapPaintColours(); } return FReply::Handled(); })
						[
							SNew(STextBlock).Text(LOCTEXT("SwapWord", "⇄")).Font(SurfTheme::Font(9)).ColorAndOpacity(SurfTheme::Dim())
						]
					]
					+ SWrapBox::Slot().VAlign(VAlign_Center)[ Swatch(false) ]
				]

				// Strength, size, Fill, Clear - one line.
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
				[
					SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(0.0f, 2.0f))
					+ SWrapBox::Slot().VAlign(VAlign_Center)[ Word(LOCTEXT("Strength", "Strength"), LOCTEXT("StrengthTip", "How much one pass sets at the brush's centre, falling off to nothing at its ring.")) ]
					+ SWrapBox::Slot().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
					[
						Number([]() { const USurfSettings* S = Settings(); return S ? S->PaintStrength : 1.0; },
							[](double V) { if (USurfSettings* S = Settings()) { S->PaintStrength = V; S->SaveConfig(); } }, 0.05, 1.0, 0.05, 2,
							LOCTEXT("StrengthTip", "How much one pass sets at the brush's centre, falling off to nothing at its ring."))
					]
					+ SWrapBox::Slot().VAlign(VAlign_Center)[ Word(LOCTEXT("SizePx", "Size"), LOCTEXT("SizeTip", "The brush's radius on screen, in pixels. Ctrl+wheel or [ ] in the viewport.")) ]
					+ SWrapBox::Slot().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
					[
						Number([]() { const USurfSettings* S = Settings(); return S ? S->BrushSelectRadiusPx : 40.0; },
							[](double V) { if (USurfSettings* S = Settings()) { S->BrushSelectRadiusPx = V; S->SaveConfig(); } if (GEditor) { GEditor->RedrawLevelEditingViewports(); } }, 4.0, 400.0, 2.0, 0,
							LOCTEXT("SizeTip", "The brush's radius on screen, in pixels. Ctrl+wheel or [ ] in the viewport."))
					]
					+ SWrapBox::Slot().VAlign(VAlign_Center)[ Press(LOCTEXT("Fill", "Fill"), LOCTEXT("FillTip", "The ticked channels set to full on the picked points - every point of the selected pieces when none is picked; R, G and B together take the colour."), []() { if (USurfEdMode* Mode = ActiveMode()) { Mode->FillSelectedPaint(true); } }) ]
					+ SWrapBox::Slot().VAlign(VAlign_Center)[ Press(LOCTEXT("Clear", "Clear"), LOCTEXT("ClearTip", "The ticked channels set to nothing on the picked points - every point of the selected pieces when none is picked; R, G and B together take the second colour."), []() { if (USurfEdMode* Mode = ActiveMode()) { Mode->FillSelectedPaint(false); } }) ]
				]

				// The adjustments (S238a): five verbs on the tab's channels, each
				// with a frame whose knobs run it again live.
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 2.0f)[ Row(LOCTEXT("AdjustRow", "Adjust")) ]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
				[
					SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(0.0f, 2.0f))
					+ SWrapBox::Slot()[ Adjust(LOCTEXT("FromCavity", "From Cavity"), LOCTEXT("FromCavityTip", "Dirt into the channels: none on the flat, full in every crevice; patches that meet read as one. Frame: blend, amount, smooth, level, invert (ridges)."), TEXT("From Cavity"), USurfSettings::PaintAdjustCavity, true) ]
					+ SWrapBox::Slot()[ Adjust(LOCTEXT("SmoothPaint", "Smooth"), LOCTEXT("SmoothPaintTip", "The channels eased towards their neighbours. Its frame: amount and passes."), TEXT("Smooth Paint"), USurfSettings::PaintAdjustSmooth, true) ]
					+ SWrapBox::Slot()[ Adjust(LOCTEXT("ContrastPaint", "Contrast"), LOCTEXT("ContrastPaintTip", "The channels pushed from the middle or pulled to it. Its frame: the amount."), TEXT("Contrast"), USurfSettings::PaintAdjustContrast, true) ]
					+ SWrapBox::Slot()[ Adjust(LOCTEXT("LevelsPaint", "Levels"), LOCTEXT("LevelsPaintTip", "The channels remapped: in black and white, gamma, out black and white."), TEXT("Levels"), USurfSettings::PaintAdjustLevels, true) ]
					+ SWrapBox::Slot()[ Adjust(LOCTEXT("InvertPaint", "Invert"), LOCTEXT("InvertPaintTip", "The channels turned over."), TEXT("Invert Paint"), USurfSettings::PaintAdjustInvert, false) ]
				]

				// The line.
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock)
					.Font(SurfTheme::Small())
					.ColorAndOpacity(SurfTheme::Dim())
					.AutoWrapText(true)
					.Text_Lambda([]() -> FText
					{
						const USurfEdMode* Mode = ActiveMode();
						if (!Mode) { return LOCTEXT("NoMode", "Enter the SurfacingPro mode to paint."); }
						if (!Mode->IsPainting())
						{
							return Mode->GetSelectedPatch().IsValid()
								? LOCTEXT("Ready", "Brush (Shift+B) paints the selection.")
								: LOCTEXT("SelectFirst", "Select a piece, then Brush (Shift+B).");
						}
						return LOCTEXT("Painting", "Drag paints up, Ctrl-drag down; Shift+X swaps the colours.");
					})
				]
			]
		];
	}
};

namespace SurfPaintTab
{
	const FName TabName("SurfacingProPaint");

	static TSharedRef<SDockTab> Spawn(const FSpawnTabArgs&)
	{
		TSharedRef<SDockTab> Tab = SNew(SDockTab)
			.TabRole(ETabRole::NomadTab)
			.Label(LOCTEXT("PaintTabLabel", "Paint"))
			[
				SNew(SSurfPaintPanel)
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
			.SetDisplayName(LOCTEXT("PaintTabTitle", "SurfacingPro Paint"))
			.SetTooltipText(LOCTEXT("PaintTabTip", "The paint brush: channels, colours, strength, size, Fill and Clear, and the adjustments - From Cavity, Smooth, Contrast, Levels, Invert."))
			.SetGroup(WorkspaceMenu::GetMenuStructure().GetLevelEditorCategory())
			.SetIcon(FSlateIcon(TEXT("SurfacingProStyle"), "SurfacingPro.Verb.brush"));
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
		// The frame retired (S177a): Paint is a tab of the SidePanel.
		SurfSidePanel::Show(ESurfSideTab::Paint);
	}

	TSharedRef<SWidget> MakePanel()
	{
		return SNew(SSurfPaintPanel).Bare(true);
	}
}

#undef LOCTEXT_NAMESPACE
