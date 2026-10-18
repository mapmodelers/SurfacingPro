// Copyright MapModelers. All Rights Reserved.

#include "SurfSidePanel.h"
#include "SurfMaterialsView.h"
#include "SurfVerticalLabel.h"
#include "SurfTheme.h"
#include "SurfEdMode.h"
#include "SurfSettings.h"
#include "SurfSession.h"
#include "SurfPaintView.h"
#include "SurfFlagsView.h"
#include "SurfAssetsView.h"
#include "SurfPreviewMaterial.h"
#include "Materials/MaterialInterface.h"
#include "SurfMaterialInfo.h"
#include "SurfInspector.h"
#include "SurfGroup.h"
#include "SurfIcons.h"
#include "SurfLevelBake.h"

#include "CoreGlobals.h"
#include "Editor.h"
#include "ScopedTransaction.h"
#include "EditorModeManager.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Fonts/FontMeasure.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Rendering/DrawElements.h"
#include "Rendering/SlateRenderer.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/StyleColors.h"
#include "Styling/StyleDefaults.h"
#include "Widgets/Colors/SColorBlock.h"
#include "Widgets/Colors/SColorPicker.h"
#include "Widgets/Images/SImage.h"
#include "Brushes/SlateImageBrush.h"
#include "Engine/Texture2D.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Input/SNumericEntryBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SLeafWidget.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "SurfacingProSidePanel"

namespace
{
	/** The strip of tabs, the grip: what the panel takes beside its own width. */
	constexpr float TabStripWidth = 20.0f;
	constexpr float GripWidth = 5.0f;

	USurfSettings* SidePanelSettings()
	{
		return GetMutableDefault<USurfSettings>();
	}

	USurfEdMode* SidePanelMode()
	{
		return Cast<USurfEdMode>(GLevelEditorModeTools().GetActiveScriptableMode(USurfEdMode::EM_SurfacingPro));
	}

	/** "x, y, z", "x y z" or UE's own "(X=.. Y=.. Z=..)" / "(Pitch=.. Yaw=.. Roll=..)". */
	bool ParseThree(const FString& Text, FVector& Out)
	{
		FString Clean = Text.TrimStartAndEnd();
		if (Clean.Contains(TEXT("X=")))
		{
			return Out.InitFromString(Clean);
		}
		if (Clean.Contains(TEXT("Pitch=")))
		{
			FRotator Rot;
			if (Rot.InitFromString(Clean))
			{
				Out = FVector(Rot.Roll, Rot.Pitch, Rot.Yaw);
				return true;
			}
			return false;
		}
		Clean.ReplaceInline(TEXT(","), TEXT(" "));
		Clean.ReplaceInline(TEXT("("), TEXT(" "));
		Clean.ReplaceInline(TEXT(")"), TEXT(" "));
		TArray<FString> Parts;
		Clean.ParseIntoArrayWS(Parts);
		if (Parts.Num() != 3)
		{
			return false;
		}
		Out = FVector(FCString::Atod(*Parts[0]), FCString::Atod(*Parts[1]), FCString::Atod(*Parts[2]));
		return true;
	}

	/** A colour chip that opens the picker; Set is called live as the colour moves. */
	TSharedRef<SWidget> Swatch(const FText& Tip, TFunction<FLinearColor()> Get, TFunction<void(const FLinearColor&)> Set)
	{
		return SNew(SBox).WidthOverride(30.0f).HeightOverride(16.0f)
			[
				SNew(SColorBlock)
				.Color_Lambda([Get]() { return Get(); })
				.ShowBackgroundForAlpha(false)
				.AlphaDisplayMode(EColorBlockAlphaDisplayMode::Ignore)
				.ToolTipText(Tip)
				.OnMouseButtonDown_Lambda([Get, Set](const FGeometry&, const FPointerEvent&) -> FReply
				{
					FColorPickerArgs Args;
					Args.bUseAlpha = false;
					Args.bOnlyRefreshOnOk = false;
					Args.InitialColor = Get();
					Args.OnColorCommitted = FOnLinearColorValueChanged::CreateLambda([Set](FLinearColor Picked) { Set(Picked); });
					OpenColorPicker(Args);
					return FReply::Handled();
				})
			];
	}

	void ViewColoursChanged()
	{
		SurfPreviewMaterial::RefreshViewColours();
		if (USurfEdMode* Mode = SidePanelMode())
		{
			Mode->RefreshPaintView();
		}
		if (GEditor)
		{
			GEditor->RedrawLevelEditingViewports();
		}
	}

	/**
	 * The active patch's stack level, and whether it wears a see-through
	 * material (S197a): "item could also show the layer (only have layer if
	 * Alpha, not Opaque)". A patch already lifted shows it too, so it can
	 * come back to 0.
	 */
	bool ActivePatchLayer(int32& OutLevel)
	{
		OutLevel = 0;
		USurfEdMode* Mode = SidePanelMode();
		if (!Mode)
		{
			return false;
		}
		FSurfPatchRef Ref = Mode->GetSelectedPatch();
		ASurfGroup* Group = Ref.Group.Get();
		const FSurfPatch* Patch = Group ? Group->FindPatch(Ref.PatchId) : nullptr;
		if (!Patch)
		{
			const TArray<FSurfPatchRef> Held = Mode->PatchesHeldBySelection();
			if (Held.Num() > 0)
			{
				Group = Held[0].Group.Get();
				Patch = Group ? Group->FindPatch(Held[0].PatchId) : nullptr;
			}
		}
		if (!Patch)
		{
			return false;
		}
		OutLevel = Patch->StackLevel;
		bool bAlpha = false;
		if (UMaterialInterface* Material = Patch->Material.Get())
		{
			if (const USurfMaterialInfo* Info = Cast<USurfMaterialInfo>(Material->GetAssetUserDataOfClass(USurfMaterialInfo::StaticClass())))
			{
				bAlpha = Info->Use == ESurfMaterialUse::Layer;
			}
			bAlpha = bAlpha || Material->GetBlendMode() != BLEND_Opaque;
		}
		return bAlpha || OutLevel > 0;
	}

	/** Five small toggles, 0 to 4, the active patch's level lit. */
	TSharedRef<SWidget> LayerSteps()
	{
		TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox);
		for (int32 Level = 0; Level <= 4; ++Level)
		{
			Row->AddSlot().AutoWidth().Padding(0.0f, 0.0f, 2.0f, 0.0f)
			[
				SNew(SCheckBox)
				.Style(FAppStyle::Get(), "ToggleButtonCheckbox")
				.Padding(FMargin(6.0f, 1.0f))
				.ToolTipText(Level == 0 ? LOCTEXT("ItemLayer0Tip", "Layer 0: the opaque surface.") : FText::Format(LOCTEXT("ItemLayerNTip", "Layer {0}: drawn over layer {1}."), FText::AsNumber(Level), FText::AsNumber(Level - 1)))
				.IsChecked_Lambda([Level]() { int32 Now; ActivePatchLayer(Now); return Now == Level ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
				.OnCheckStateChanged_Lambda([Level](ECheckBoxState)
				{
					if (USurfEdMode* Mode = SidePanelMode())
					{
						if (Mode->Inspector)
						{
							Mode->Inspector->SetLayer(Level);
						}
					}
				})
				[
					SNew(STextBlock).Text(FText::AsNumber(Level)).Font(SurfTheme::Small())
				]
			];
		}
		return Row;
	}

	/** The matcap balls (S188a), made once. */
	const FSlateBrush* MatcapBall(int32 Look)
	{
		static TArray<TSharedPtr<FSlateImageBrush>> Brushes;
		Brushes.SetNum(static_cast<int32>(ESurfMatcapLook::Count));
		if (!Brushes.IsValidIndex(Look))
		{
			return FAppStyle::GetBrush("WhiteBrush");
		}
		if (!Brushes[Look].IsValid())
		{
			if (UTexture2D* Ball = SurfPreviewMaterial::GetMatcapLookIcon(Look))
			{
				Brushes[Look] = MakeShared<FSlateImageBrush>(Ball, FVector2D(26.0, 26.0));
			}
		}
		return Brushes[Look].IsValid() ? Brushes[Look].Get() : FAppStyle::GetBrush("WhiteBrush");
	}

	FText MatcapLookName(int32 Look)
	{
		const UEnum* Names = StaticEnum<ESurfMatcapLook>();
		return Names ? Names->GetDisplayNameTextByValue(Look) : FText::AsNumber(Look);
	}

	void PickMatcapLook(int32 Look)
	{
		if (USurfSettings* W = SidePanelSettings())
		{
			W->MatcapLook = static_cast<ESurfMatcapLook>(Look);
			W->SaveConfig();
		}
		ViewColoursChanged();
		if (USurfEdMode* Mode = SidePanelMode())
		{
			if (Mode->GetViewKind() != 2)
			{
				Mode->SetViewKind(2);
			}
		}
		SurfSession::Verb(TEXT("matcap_look"), MatcapLookName(Look).ToString());
	}

	/**
	 * The clay in a pull-down beside its chip (S249a: "have the clay
	 * parameters available from a dropdown, right next to the Matcap icon
	 * ... instead of its own line"): each look's ball with its name, the one
	 * in use ticked, and the clay's colour. A pick also turns the view on.
	 */
	TSharedRef<SWidget> MatcapMenu()
	{
		FMenuBuilder Menu(/*bShouldCloseWindowAfterMenuSelection*/ true, nullptr, nullptr, false, &SurfTheme::MenuStyle(), /*bSearchable*/ false);
		Menu.BeginSection(NAME_None, LOCTEXT("MatcapLooks", "Look"));
		const int32 Count = static_cast<int32>(ESurfMatcapLook::Count);
		for (int32 Look = 0; Look < Count; ++Look)
		{
			Menu.AddMenuEntry(
				FUIAction(
					FExecuteAction::CreateLambda([Look]() { PickMatcapLook(Look); }),
					FCanExecuteAction(),
					FIsActionChecked::CreateLambda([Look]() { const USurfSettings* S = GetDefault<USurfSettings>(); return S && static_cast<int32>(S->MatcapLook) == Look; })),
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 1.0f, 8.0f, 1.0f)
				[
					SNew(SBox).WidthOverride(22.0f).HeightOverride(22.0f)
					[
						SNew(SImage).Image(MatcapBall(Look))
					]
				]
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				[
					SNew(STextBlock).Text(MatcapLookName(Look)).Font(SurfTheme::Font(9))
				],
				NAME_None, TAttribute<FText>(), EUserInterfaceActionType::RadioButton);
		}
		Menu.EndSection();
		Menu.BeginSection(NAME_None, LOCTEXT("MatcapColourHeading", "Colour"));
		const TSharedRef<SWidget> Colour = SNew(SBox).Padding(FMargin(4.0f, 2.0f))
			[
				Swatch(LOCTEXT("MatcapColourTip", "The clay's colour. Click to pick."),
					[]() { const USurfSettings* S = GetDefault<USurfSettings>(); return S ? S->MatcapColour : FLinearColor(0.6f, 0.6f, 0.6f); },
					[](const FLinearColor& C) { if (USurfSettings* W = SidePanelSettings()) { W->MatcapColour = C; W->SaveConfig(); } ViewColoursChanged(); })
			];
		Menu.AddWidget(Colour, FText::GetEmpty());
		Menu.EndSection();
		return Menu.MakeWidget();
	}
}

namespace SurfSidePanel
{
	namespace
	{
		int32 Bit(ESurfSideTab Tab)
		{
			return 1 << static_cast<int32>(Tab);
		}

		int32 ShownMask()
		{
			const USurfSettings* S = GetDefault<USurfSettings>();
			return S ? (S->SidePanelShown & ((1 << static_cast<int32>(ESurfSideTab::Count)) - 1)) : 1;
		}

		void SetShownMask(int32 Mask)
		{
			if (USurfSettings* S = SidePanelSettings())
			{
				S->SidePanelShown = Mask;
				S->SaveConfig();
			}
		}
	}

	void Toggle()
	{
		if (USurfSettings* S = SidePanelSettings())
		{
			S->bSidePanelOpen = !S->bSidePanelOpen;
			S->SaveConfig();
			SurfSession::Verb(TEXT("side_panel"), S->bSidePanelOpen ? TEXT("open") : TEXT("closed"));
		}
	}

	void Open()
	{
		FEditorModeTools& Tools = GLevelEditorModeTools();
		if (!Tools.IsModeActive(USurfEdMode::EM_SurfacingPro))
		{
			Tools.ActivateMode(USurfEdMode::EM_SurfacingPro);
		}
		if (USurfSettings* S = SidePanelSettings())
		{
			S->bSidePanelOpen = true;
			if (ShownMask() == 0)
			{
				S->SidePanelShown = Bit(Order()[0]);
			}
			S->SaveConfig();
		}
	}

	void Show(ESurfSideTab Tab)
	{
		FEditorModeTools& Tools = GLevelEditorModeTools();
		if (!Tools.IsModeActive(USurfEdMode::EM_SurfacingPro))
		{
			Tools.ActivateMode(USurfEdMode::EM_SurfacingPro);
		}
		const int32 Mask = ShownMask();
		// Several pinned: it joins them. One or none: it is the one.
		const bool bPinned = FMath::CountBits(static_cast<uint64>(Mask)) > 1;
		if (USurfSettings* S = SidePanelSettings())
		{
			S->bSidePanelOpen = true;
			S->SidePanelShown = bPinned ? (Mask | Bit(Tab)) : Bit(Tab);
			S->SaveConfig();
		}
		SurfSession::Verb(TEXT("side_panel"), TabName(Tab).ToString());
	}

	void ClickTab(ESurfSideTab Tab, bool bCtrl)
	{
		const int32 Mask = ShownMask();
		int32 Next;
		if (bCtrl)
		{
			// Pinned beside the others, or taken out of them.
			Next = Mask ^ Bit(Tab);
		}
		else
		{
			// The one shown alone; clicked again, the panel folds to its tabs.
			Next = Mask == Bit(Tab) ? 0 : Bit(Tab);
		}
		SetShownMask(Next);
		SurfSession::Verb(TEXT("side_panel_tab"), FString::Printf(TEXT("%s%s shown=%d"), bCtrl ? TEXT("ctrl ") : TEXT(""), *TabName(Tab).ToString(), Next));
	}

	bool IsOpen()
	{
		const USurfSettings* S = GetDefault<USurfSettings>();
		return S && S->bSidePanelOpen;
	}

	bool IsShown(ESurfSideTab Tab)
	{
		return (ShownMask() & Bit(Tab)) != 0;
	}

	bool IsFolded()
	{
		return ShownMask() == 0;
	}

	TArray<ESurfSideTab> Order()
	{
		const int32 Count = static_cast<int32>(ESurfSideTab::Count);
		TArray<ESurfSideTab> Out;
		if (const USurfSettings* S = GetDefault<USurfSettings>())
		{
			TArray<FString> Parts;
			S->SidePanelOrder.ParseIntoArray(Parts, TEXT(","));
			for (const FString& Part : Parts)
			{
				const int32 Index = FCString::Atoi(*Part);
				if (Index >= 0 && Index < Count && !Out.Contains(static_cast<ESurfSideTab>(Index)))
				{
					Out.Add(static_cast<ESurfSideTab>(Index));
				}
			}
		}
		// Any tab the saved order does not name goes at the end.
		for (int32 I = 0; I < Count; ++I)
		{
			Out.AddUnique(static_cast<ESurfSideTab>(I));
		}
		return Out;
	}

	void SetOrder(const TArray<ESurfSideTab>& NewOrder)
	{
		if (USurfSettings* S = SidePanelSettings())
		{
			TArray<FString> Parts;
			for (ESurfSideTab Tab : NewOrder)
			{
				Parts.Add(FString::FromInt(static_cast<int32>(Tab)));
			}
			S->SidePanelOrder = FString::Join(Parts, TEXT(","));
		}
	}

	FText TabName(ESurfSideTab Tab)
	{
		switch (Tab)
		{
		case ESurfSideTab::Item:  return LOCTEXT("TabItem", "Item");
		case ESurfSideTab::Paint: return LOCTEXT("TabPaint", "Paint");
		case ESurfSideTab::Flags: return LOCTEXT("TabFlags", "Flags");
		case ESurfSideTab::View:  return LOCTEXT("TabView", "View");
		case ESurfSideTab::Assets: return LOCTEXT("TabAssets", "Assets");
		case ESurfSideTab::Materials: return LOCTEXT("TabMaterials", "Materials");
		default:                  return FText::GetEmpty();
		}
	}

	float Width()
	{
		const USurfSettings* S = GetDefault<USurfSettings>();
		if (!S || !S->bSidePanelOpen)
		{
			return 0.0f;
		}
		if (IsFolded())
		{
			return TabStripWidth;
		}
		return static_cast<float>(FMath::Clamp(S->SidePanelWidthPx, 180, 520)) + TabStripWidth + GripWidth;
	}
}

void SSurfSidePanel::Construct(const FArguments& InArgs)
{
	Mode = InArgs._Mode;
	bTabsOnRight = InArgs._TabsOnRight;

	for (int32 I = 0; I < static_cast<int32>(ESurfSideTab::Count); ++I)
	{
		Tabs[I] = BuildTab(static_cast<ESurfSideTab>(I));
		Sections[I] = BuildSection(static_cast<ESurfSideTab>(I));
	}
	// The panel's own boxes let the hand through to the viewport (S224a):
	// its rectangle is the tabs' length by the body's width, and the empty
	// part under a short section was dead ground for the viewport.
	SetVisibility(EVisibility::SelfHitTestInvisible);
	SAssignNew(TabBox, SVerticalBox).Visibility(EVisibility::SelfHitTestInvisible);
	SAssignNew(SectionBox, SVerticalBox);
	Rearrange();

	// An order dragged in the other edge's panel shows here too.
	RegisterActiveTimer(0.3f, FWidgetActiveTimerDelegate::CreateLambda([this](double, float)
	{
		const USurfSettings* S = GetDefault<USurfSettings>();
		if (S && S->SidePanelOrder != OrderSeen && PressedTab < 0)
		{
			Rearrange();
		}
		return EActiveTimerReturnType::Continue;
	}));

	// The edge that sizes it: five pixels on the inner side, where the hand
	// already is; it shows itself only while it is dragged.
	TSharedRef<SWidget> Grip = SNew(SBox)
		.WidthOverride(GripWidth)
		.Visibility_Lambda([]() { return SurfSidePanel::IsFolded() ? EVisibility::Collapsed : EVisibility::Visible; })
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
			.BorderBackgroundColor_Lambda([this]() { return bResizing ? FStyleColors::White25 : FSlateColor(FLinearColor::Transparent); })
			.Cursor(EMouseCursor::ResizeLeftRight)
			.ToolTipText(LOCTEXT("GripTip", "Drag to make the panel wider or narrower."))
			.OnMouseButtonDown_Lambda([this](const FGeometry&, const FPointerEvent& Event) -> FReply
			{
				if (Event.GetEffectingButton() != EKeys::LeftMouseButton)
				{
					return FReply::Unhandled();
				}
				const USurfSettings* S = GetDefault<USurfSettings>();
				DragStartWidth = S ? S->SidePanelWidthPx : 240;
				DragStartX = static_cast<float>(Event.GetScreenSpacePosition().X);
				bResizing = true;
				return FReply::Handled().CaptureMouse(SharedThis(this));
			})
		];

	TSharedRef<SWidget> Body = SNew(SBox)
		.Visibility_Lambda([]() { return SurfSidePanel::IsFolded() ? EVisibility::Collapsed : EVisibility::SelfHitTestInvisible; })
		.WidthOverride_Lambda([]()
		{
			const USurfSettings* S = GetDefault<USurfSettings>();
			return FOptionalSize(static_cast<float>(S ? FMath::Clamp(S->SidePanelWidthPx, 180, 520) : 240));
		})
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("Brushes.Panel"))
			.Padding(0.0f)
			[
				SNew(SBox)
				.MaxDesiredHeight(InArgs._MaxHeight)
				[
					SNew(SScrollBox)
					.ScrollBarThickness(FVector2D(2.0f, 2.0f))
					// The wheel over the panel is the panel's, never the camera's.
					.ConsumeMouseWheel(EConsumeMouseWheel::Always)
					+ SScrollBox::Slot()
					[
						SectionBox.ToSharedRef()
					]
				]
			]
		];

	// The body and its grip stand as tall as the content, no taller (S217a:
	// "its open full length even when content is short. frame it to the
	// content") - the tabs run their own length beside.
	TSharedRef<SHorizontalBox> Sheet = SNew(SHorizontalBox).Visibility(EVisibility::SelfHitTestInvisible);
	if (bTabsOnRight)
	{
		Sheet->AddSlot().AutoWidth()[ Grip ];
		Sheet->AddSlot().AutoWidth()[ Body ];
	}
	else
	{
		Sheet->AddSlot().AutoWidth()[ Body ];
		Sheet->AddSlot().AutoWidth()[ Grip ];
	}
	TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox).Visibility(EVisibility::SelfHitTestInvisible);
	if (bTabsOnRight)
	{
		Row->AddSlot().AutoWidth().VAlign(VAlign_Top)[ Sheet ];
		Row->AddSlot().AutoWidth().VAlign(VAlign_Top)[ TabBox.ToSharedRef() ];
	}
	else
	{
		Row->AddSlot().AutoWidth().VAlign(VAlign_Top)[ TabBox.ToSharedRef() ];
		Row->AddSlot().AutoWidth().VAlign(VAlign_Top)[ Sheet ];
	}

	ChildSlot
	[
		Row
	];
}

void SSurfSidePanel::Rearrange()
{
	const USurfSettings* S = GetDefault<USurfSettings>();
	OrderSeen = S ? S->SidePanelOrder : FString();
	TabBox->ClearChildren();
	SectionBox->ClearChildren();
	for (ESurfSideTab Tab : SurfSidePanel::Order())
	{
		const int32 I = static_cast<int32>(Tab);
		TabBox->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 1.0f)[ Tabs[I].ToSharedRef() ];
		SectionBox->AddSlot().AutoHeight()[ Sections[I].ToSharedRef() ];
	}
}

FReply SSurfSidePanel::OnMouseMove(const FGeometry& Geometry, const FPointerEvent& Event)
{
	if (bResizing && HasMouseCapture())
	{
		const float Scale = FMath::Max(0.1f, Geometry.Scale);
		const float Moved = (static_cast<float>(Event.GetScreenSpacePosition().X) - DragStartX) / Scale;
		// On the right edge the grip is on the left: dragging left widens it.
		const int32 Width = FMath::Clamp(FMath::RoundToInt(DragStartWidth + (bTabsOnRight ? -Moved : Moved)), 180, 520);
		if (USurfSettings* S = SidePanelSettings())
		{
			S->SidePanelWidthPx = Width;
		}
		return FReply::Handled();
	}

	if (PressedTab >= 0 && HasMouseCapture())
	{
		const float Y = static_cast<float>(Event.GetScreenSpacePosition().Y);
		if (!bTabDragging && FMath::Abs(Y - PressY) > 5.0f * FMath::Max(0.1f, Geometry.Scale))
		{
			bTabDragging = true;
		}
		if (bTabDragging)
		{
			// Over another tab's half: the two change places, live.
			TArray<ESurfSideTab> Now = SurfSidePanel::Order();
			const int32 From = Now.IndexOfByKey(static_cast<ESurfSideTab>(PressedTab));
			for (int32 To = 0; To < Now.Num(); ++To)
			{
				if (To == From)
				{
					continue;
				}
				const FGeometry& Other = Tabs[static_cast<int32>(Now[To])]->GetCachedGeometry();
				const float Top = static_cast<float>(Other.GetAbsolutePosition().Y);
				const float Height = static_cast<float>(Other.GetAbsoluteSize().Y);
				if (Height < 1.0f)
				{
					continue;
				}
				const bool bPast = To < From ? Y < Top + Height * 0.5f : Y > Top + Height * 0.5f;
				const bool bOver = Y >= Top && Y <= Top + Height;
				if (bOver && bPast)
				{
					const ESurfSideTab Moving = Now[From];
					Now.RemoveAt(From);
					Now.Insert(Moving, To);
					SurfSidePanel::SetOrder(Now);
					Rearrange();
					break;
				}
			}
		}
		return FReply::Handled();
	}
	return FReply::Unhandled();
}

FReply SSurfSidePanel::OnMouseButtonUp(const FGeometry& Geometry, const FPointerEvent& Event)
{
	if (bResizing)
	{
		bResizing = false;
		if (USurfSettings* S = SidePanelSettings())
		{
			S->SaveConfig();
		}
		return FReply::Handled().ReleaseMouseCapture();
	}
	if (PressedTab >= 0)
	{
		const ESurfSideTab Tab = static_cast<ESurfSideTab>(PressedTab);
		if (bTabDragging)
		{
			if (USurfSettings* S = SidePanelSettings())
			{
				S->SaveConfig();
			}
			SurfSession::Verb(TEXT("side_panel_order"), GetDefault<USurfSettings>()->SidePanelOrder);
		}
		else
		{
			SurfSidePanel::ClickTab(Tab, bPressedCtrl);
		}
		PressedTab = -1;
		bTabDragging = false;
		return FReply::Handled().ReleaseMouseCapture();
	}
	return FReply::Unhandled();
}

TSharedRef<SWidget> SSurfSidePanel::BuildTab(ESurfSideTab Tab)
{
	TSharedPtr<SBorder> Holder;
	SAssignNew(Holder, SBorder)
		.Padding(FMargin(3.0f, 9.0f))
		.Cursor(EMouseCursor::Hand)
		.ToolTipText(FText::Format(LOCTEXT("TabTip", "{0}. Click: this one alone - again to fold the panel to its tabs. Ctrl+click: pin it beside the others. Drag: up or down."), SurfSidePanel::TabName(Tab)))
		.OnMouseButtonDown_Lambda([this, Tab](const FGeometry&, const FPointerEvent& Event) -> FReply
		{
			if (Event.GetEffectingButton() != EKeys::LeftMouseButton)
			{
				return FReply::Unhandled();
			}
			PressedTab = static_cast<int32>(Tab);
			bPressedCtrl = Event.IsControlDown();
			bTabDragging = false;
			PressY = static_cast<float>(Event.GetScreenSpacePosition().Y);
			return FReply::Handled().CaptureMouse(SharedThis(this));
		})
		[
			SNew(SSurfVerticalLabel)
			.Text(SurfSidePanel::TabName(Tab))
			.Colour_Lambda([Tab]() { return SurfSidePanel::IsShown(Tab) ? FSlateColor(FStyleColors::White) : FSlateColor(FStyleColors::Foreground); })
		];

	// The ones shown are the panel's own grey, the others sit back darker;
	// the one under the cursor or in the hand lifts, the way UE's tabs do.
	TWeakPtr<SBorder> Weak = Holder;
	Holder->SetBorderImage(TAttribute<const FSlateBrush*>::CreateLambda([this, Weak, Tab]() -> const FSlateBrush*
	{
		if (bTabDragging && PressedTab == static_cast<int32>(Tab))
		{
			return FAppStyle::GetBrush("Brushes.Hover");
		}
		if (SurfSidePanel::IsShown(Tab))
		{
			return FAppStyle::GetBrush("Brushes.Panel");
		}
		TSharedPtr<SBorder> Live = Weak.Pin();
		return Live.IsValid() && Live->IsHovered() ? FAppStyle::GetBrush("Brushes.Header") : FAppStyle::GetBrush("Brushes.Recessed");
	}));

	// The open one carries a line of the editor's blue on its outer edge
	// (S192c), as an active editor tab does.
	return SNew(SOverlay)
		+ SOverlay::Slot()
		[
			Holder.ToSharedRef()
		]
		+ SOverlay::Slot().HAlign(bTabsOnRight ? HAlign_Right : HAlign_Left)
		[
			SNew(SBox)
			.WidthOverride(2.0f)
			.Visibility_Lambda([Tab]() { return SurfSidePanel::IsShown(Tab) ? EVisibility::HitTestInvisible : EVisibility::Collapsed; })
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
				.BorderBackgroundColor(FStyleColors::Primary)
				.Padding(0.0f)
			]
		];
}

TSharedRef<SWidget> SSurfSidePanel::BuildSection(ESurfSideTab Tab)
{
	TSharedRef<SWidget> Content = SNullWidget::NullWidget;
	switch (Tab)
	{
	case ESurfSideTab::Item:  Content = BuildItem(); break;
	case ESurfSideTab::Paint: Content = SNew(SBox).Padding(2.0f, 0.0f)[ SurfPaintTab::MakePanel() ]; break;
	case ESurfSideTab::Flags: Content = SurfFlagsTab::MakePanel(); break;
	case ESurfSideTab::View:  Content = BuildView(); break;
	case ESurfSideTab::Assets: Content = SurfAssetsTab::MakePanel(); break;
	case ESurfSideTab::Materials: Content = SurfMaterialsTab::MakePanel(); break;
	default: break;
	}

	// Each shown tab under a strip of its own, the frame's: the name small
	// and grey, a cross that puts this one away.
	return SNew(SBox)
		.Visibility_Lambda([Tab]() { return SurfSidePanel::IsShown(Tab) ? EVisibility::Visible : EVisibility::Collapsed; })
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("Brushes.Header"))
				.Padding(FMargin(8.0f, 0.0f, 4.0f, 0.0f))
				[
					// The editor's category header (S192c): 24 pixels, the
					// name in its small bold header type.
					SNew(SBox).HeightOverride(24.0f)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
						[
							SNew(STextBlock)
							.Text(SurfSidePanel::TabName(Tab))
							.Font(SurfTheme::Bold(8))
							.ColorAndOpacity(SurfTheme::Header())
						]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
						[
							SNew(SButton)
							.ButtonStyle(FAppStyle::Get(), "SimpleButton")
							.ContentPadding(FMargin(3.0f, 0.0f))
							.ToolTipText(LOCTEXT("CloseTip", "Put this one away; its tab stays."))
							.OnClicked_Lambda([Tab]() { SurfSidePanel::ClickTab(Tab, /*bCtrl*/ true); return FReply::Handled(); })
							[
								SNew(SImage)
								.Image(FAppStyle::GetBrush("Icons.X"))
								.DesiredSizeOverride(FVector2D(10.0, 10.0))
								.ColorAndOpacity(FSlateColor::UseSubduedForeground())
							]
						]
					]
				]
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				Content
			]
		];
}

TSharedRef<SWidget> SSurfSidePanel::BuildItem()
{
	TWeakObjectPtr<USurfEdMode> M = Mode;

	// What is picked, read once a frame however many fields ask.
	struct FRead
	{
		uint64 Frame = MAX_uint64;
		bool bValid = false;
		bool bAbsolute = false;
		FVector Position = FVector::ZeroVector;
		FRotator Rotation = FRotator::ZeroRotator;
		FVector Scale = FVector::OneVector;
	};
	TSharedRef<FRead> Cache = MakeShared<FRead>();
	auto Fresh = [M, Cache]() -> const FRead&
	{
		if (Cache->Frame != GFrameCounter)
		{
			Cache->Frame = GFrameCounter;
			Cache->bValid = M.IsValid() && M->GetStripTransform(Cache->Position, Cache->Rotation, Cache->Scale, Cache->bAbsolute);
		}
		return *Cache;
	};

	enum class ERow : uint8 { Position, Rotation, Scale, Pivot };

	// The turn as X = roll, Y = pitch, Z = yaw: the axes it turns about.
	auto Read = [M, Fresh](ERow Row) -> TOptional<FVector>
	{
		const FRead& Now = Fresh();
		if (Row == ERow::Pivot)
		{
			if (M.IsValid() && M->HasPivot())
			{
				return M->GetPivot();
			}
			return Now.bValid ? TOptional<FVector>(Now.Position) : TOptional<FVector>();
		}
		if (!Now.bValid)
		{
			return TOptional<FVector>();
		}
		switch (Row)
		{
		case ERow::Position: return Now.Position;
		case ERow::Rotation: return FVector(Now.Rotation.Roll, Now.Rotation.Pitch, Now.Rotation.Yaw);
		default:             return Now.Scale;
		}
	};
	auto Write = [M, Cache](ERow Row, const FVector& Value)
	{
		if (!M.IsValid())
		{
			return;
		}
		Cache->Frame = MAX_uint64;
		switch (Row)
		{
		case ERow::Position: M->ApplyStripTransform(&Value, nullptr, nullptr); break;
		case ERow::Rotation: { const FRotator Rot(Value.Y, Value.Z, Value.X); M->ApplyStripTransform(nullptr, &Rot, nullptr); break; }
		case ERow::Scale:    M->ApplyStripTransform(nullptr, nullptr, &Value); break;
		case ERow::Pivot:    M->SetPivot(Value); break;
		}
	};

	/**
	 * One number (S184b): "could the TRS slider under Item update while
	 * scrolling". A drag applies as it goes, all of it one undo step. On
	 * our geometry a turn or a size is applied and reads 0 or 1 again, so
	 * the drag applies what moved since the last step, not the whole value
	 * each time.
	 */
	struct FDrag
	{
		bool bActive = false;
		double Last = 0.0;
		TSharedPtr<FScopedTransaction> Transaction;
	};
	auto Field = [Read, Write, Fresh](ERow Row, int32 Axis) -> TSharedRef<SWidget>
	{
		TSharedRef<FDrag> Drag = MakeShared<FDrag>();
		auto Step = [Read, Write, Fresh, Row, Axis, Drag](double Value)
		{
			const TOptional<FVector> Now = Read(Row);
			if (!Now.IsSet())
			{
				return;
			}
			const bool bRelative = !Fresh().bAbsolute && (Row == ERow::Rotation || Row == ERow::Scale);
			FVector V = Now.GetValue();
			if (bRelative && Row == ERow::Rotation)
			{
				V = FVector::ZeroVector;
				V[Axis] = Value - Drag->Last;
			}
			else if (bRelative)
			{
				V = FVector::OneVector;
				V[Axis] = FMath::Abs(Drag->Last) > 1.e-4 ? Value / Drag->Last : 1.0;
			}
			else
			{
				V[Axis] = Value;
			}
			Drag->Last = Value;
			if (bRelative ? !V.Equals(Row == ERow::Rotation ? FVector::ZeroVector : FVector::OneVector, 1.e-6) : !V.Equals(Now.GetValue(), 1.e-6))
			{
				Write(Row, V);
			}
		};
		// The editor's vector field (S192c): its numeric box, the axis colour
		// as the narrow mark inside its left edge, as Details draws one.
		return SNew(SNumericEntryBox<double>)
				.Font(SurfTheme::Font(8))
				.AllowSpin(true)
				.MinSliderValue(TOptional<double>())
				.MaxSliderValue(TOptional<double>())
				.LabelPadding(FMargin(0.0f))
				.LabelLocation(SNumericEntryBox<double>::ELabelLocation::Inside)
				.Label()
				[
					SNumericEntryBox<double>::BuildNarrowColorLabel(SurfTheme::Axis(Axis))
				]
				.Delta(Row == ERow::Scale ? 0.01 : 1.0)
				.MinFractionalDigits(Row == ERow::Scale ? 2 : 0)
				.MaxFractionalDigits(Row == ERow::Scale ? 2 : 1)
				.Value_Lambda([Read, Row, Axis]() -> TOptional<double> { const TOptional<FVector> V = Read(Row); return V.IsSet() ? TOptional<double>(V.GetValue()[Axis]) : TOptional<double>(); })
				.IsEnabled_Lambda([Read, Row]() { return Read(Row).IsSet(); })
				.OnBeginSliderMovement_Lambda([Read, Row, Axis, Drag]()
				{
					const TOptional<FVector> V = Read(Row);
					Drag->bActive = true;
					Drag->Last = V.IsSet() ? V.GetValue()[Axis] : 0.0;
					Drag->Transaction = MakeShared<FScopedTransaction>(LOCTEXT("ItemDrag", "Transform"));
				})
				.OnValueChanged_Lambda([Step, Drag](double Value)
				{
					if (Drag->bActive)
					{
						Step(Value);
					}
				})
				.OnValueCommitted_Lambda([Read, Step, Drag, Row, Axis](double Value, ETextCommit::Type)
				{
					if (Drag->bActive)
					{
						// The release of a drag: the last bit of it.
						Step(Value);
						return;
					}
					const TOptional<FVector> V = Read(Row);
					Drag->Last = V.IsSet() ? V.GetValue()[Axis] : 0.0;
					if (!FMath::IsNearlyEqual(Drag->Last, Value, 1.e-4))
					{
						Step(Value);
					}
				})
				.OnEndSliderMovement_Lambda([Drag](double)
				{
					Drag->bActive = false;
					Drag->Transaction.Reset();
				});
	};

	/**
	 * A faint cross to put a number back (S189a): "add a subtle discreet
	 * cross to the right of each field so user may reset it. add it per line
	 * so user may reset whole line at once. and add it too for all". Back to
	 * 0, or 1 for a size; the origin row back to the centre. On our geometry
	 * a turn or a size is applied as it is typed, so there is nothing to put
	 * back and the cross does not show.
	 */
	auto Default = [](ERow Row) { return Row == ERow::Scale ? FVector::OneVector : FVector::ZeroVector; };
	auto CanReset = [M, Read, Fresh, Default](ERow Row, int32 Axis) -> bool
	{
		if (Row == ERow::Pivot)
		{
			return M.IsValid() && M->HasPivot();
		}
		const TOptional<FVector> V = Read(Row);
		if (!V.IsSet() || (!Fresh().bAbsolute && Row != ERow::Position))
		{
			return false;
		}
		const FVector D = Default(Row);
		return Axis < 0 ? !V->Equals(D, 1.e-4) : !FMath::IsNearlyEqual((*V)[Axis], D[Axis], 1.e-4);
	};
	auto Reset = [M, Read, Write, Default](ERow Row, int32 Axis)
	{
		if (Row == ERow::Pivot)
		{
			if (M.IsValid())
			{
				M->ClearPivot();
			}
			return;
		}
		const TOptional<FVector> V = Read(Row);
		if (!V.IsSet())
		{
			return;
		}
		FVector Want = Axis < 0 ? Default(Row) : V.GetValue();
		if (Axis >= 0)
		{
			Want[Axis] = Default(Row)[Axis];
		}
		const FScopedTransaction Transaction(LOCTEXT("ItemReset", "Reset Transform"));
		Write(Row, Want);
		SurfSession::Verb(TEXT("item_reset"), FString::Printf(TEXT("row=%d axis=%d"), static_cast<int32>(Row), Axis));
	};
	auto Cross = [](const FText& Tip, TFunction<bool()> Enabled, TFunction<void()> Run) -> TSharedRef<SWidget>
	{
		// The editor's reset-to-default arrow (S192c), shown only where a
		// value differs, as in Details.
		return SNew(SBox).WidthOverride(12.0f)
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.ContentPadding(FMargin(0.0f))
				.HAlign(HAlign_Center)
				.VAlign(VAlign_Center)
				.ToolTipText(Tip)
				.Visibility_Lambda([Enabled]() { return Enabled() ? EVisibility::Visible : EVisibility::Hidden; })
				.OnClicked_Lambda([Run]() { Run(); return FReply::Handled(); })
				[
					SNew(SImage)
					.Image(FAppStyle::GetBrush("PropertyWindow.DiffersFromDefault"))
					.DesiredSizeOverride(FVector2D(10.0, 10.0))
				]
			];
	};
	auto FieldWithReset = [Field, Cross, CanReset, Reset](ERow Row, int32 Axis) -> TSharedRef<SWidget>
	{
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.0f)[ Field(Row, Axis) ]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				Cross(LOCTEXT("ResetOneTip", "Put this number back: 0, or 1 for a size."),
					[CanReset, Row, Axis]() { return CanReset(Row, Axis); }, [Reset, Row, Axis]() { Reset(Row, Axis); })
			];
	};

	// A row: a short name, then three numbers filling the width; right-click
	// copies or pastes the three (from another row or UE's Details too).
	auto RowOf = [this, FieldWithReset, Cross, CanReset, Reset, Read, Write, Fresh](ERow Row, const FText& Name) -> TSharedRef<SWidget>
	{
		// Our geometry has no rotation or scale of its own - its points are
		// where they are (S208a: "rotation remains 0 0 0, scale remain 1").
		// The row then says Turn / Size, dimmed: a number there turns or
		// sizes it by that much and reads 0 or 1 again. An actor's rows keep
		// their names and their numbers.
		const bool bDelta = Row == ERow::Rotation || Row == ERow::Scale;
		auto Relative = [Fresh]() { const FRead& Now = Fresh(); return Now.bValid && !Now.bAbsolute; };
		TAttribute<FText> Shown = bDelta
			? TAttribute<FText>::CreateLambda([Relative, Row, Name]() { return Relative() ? (Row == ERow::Rotation ? LOCTEXT("TurnBy", "Turn") : LOCTEXT("SizeBy", "Size")) : Name; })
			: TAttribute<FText>(Name);
		TAttribute<FSlateColor> Shade = bDelta
			? TAttribute<FSlateColor>::CreateLambda([Relative]() { return Relative() ? SurfTheme::Dim() : SurfTheme::Text(); })
			: TAttribute<FSlateColor>(SurfTheme::Text());
		TAttribute<FText> Tip = bDelta
			? TAttribute<FText>::CreateLambda([Relative, Row]()
			{
				if (!Relative())
				{
					return Row == ERow::Rotation ? LOCTEXT("RotTip", "The actor's rotation.") : LOCTEXT("ScaleTip", "The actor's scale.");
				}
				return Row == ERow::Rotation
					? LOCTEXT("TurnTip", "A patch or primitive has no rotation of its own - its points are where they are. A number here turns it by that much, about the pivot, and reads 0 again.")
					: LOCTEXT("SizeTip", "A patch or primitive has no scale of its own - its points are where they are. A number here sizes it by that much, about the pivot, and reads 1 again.");
			})
			: TAttribute<FText>(FText::GetEmpty());
		// The fields go quieter with the label (S209a: "the fields should be
		// bit more discreet too... useable but not fully supported").
		auto Quieter = [bDelta, Relative](TSharedRef<SWidget> Field) -> TSharedRef<SWidget>
		{
			if (!bDelta)
			{
				return Field;
			}
			return SNew(SBorder)
				.BorderImage(FStyleDefaults::GetNoBrush())
				.Padding(0.0f)
				.ColorAndOpacity_Lambda([Relative]() { return Relative() ? FLinearColor(1.0f, 1.0f, 1.0f, 0.55f) : FLinearColor::White; })
				[
					Field
				];
		};
		return SNew(SBorder)
			.BorderImage(FStyleDefaults::GetNoBrush())
			.Padding(0.0f)
			.OnMouseButtonUp_Lambda([this, Read, Write, Row](const FGeometry&, const FPointerEvent& Event) -> FReply
			{
				if (Event.GetEffectingButton() != EKeys::RightMouseButton)
				{
					return FReply::Unhandled();
				}
				FMenuBuilder Menu(true, nullptr, nullptr, false, &FCoreStyle::Get(), false);
				Menu.AddMenuEntry(LOCTEXT("Copy", "Copy"), LOCTEXT("CopyTip", "The three numbers, as x, y, z."), FSlateIcon(),
					FUIAction(FExecuteAction::CreateLambda([Read, Row]()
					{
						const TOptional<FVector> V = Read(Row);
						if (V.IsSet())
						{
							FPlatformApplicationMisc::ClipboardCopy(*FString::Printf(TEXT("%g, %g, %g"), V->X, V->Y, V->Z));
						}
					})));
				Menu.AddMenuEntry(LOCTEXT("Paste", "Paste"), LOCTEXT("PasteTip", "Three numbers: x, y, z - or what UE's Details copies."), FSlateIcon(),
					FUIAction(FExecuteAction::CreateLambda([Write, Row]()
					{
						FString Text;
						FPlatformApplicationMisc::ClipboardPaste(Text);
						FVector V;
						if (ParseThree(Text, V))
						{
							Write(Row, V);
						}
					})));
				FSlateApplication::Get().PushMenu(SharedThis(this), FWidgetPath(), Menu.MakeWidget(),
					FSlateApplication::Get().GetCursorPos(), FPopupTransitionEffect(FPopupTransitionEffect::ContextMenu));
				return FReply::Handled();
			})
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(SBox).WidthOverride(44.0f)
					[
						SNew(STextBlock).Text(Shown).ToolTipText(Tip).Font(SurfTheme::Font(8)).ColorAndOpacity(Shade)
					]
				]
				+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(0.0f, 0.0f, 1.0f, 0.0f)[ Quieter(FieldWithReset(Row, 0)) ]
				+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(0.0f, 0.0f, 1.0f, 0.0f)[ Quieter(FieldWithReset(Row, 1)) ]
				+ SHorizontalBox::Slot().FillWidth(1.0f)[ Quieter(FieldWithReset(Row, 2)) ]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(2.0f, 0.0f, 0.0f, 0.0f)
				[
					Cross(Row == ERow::Pivot ? LOCTEXT("ResetOriginTip", "The origin back to the centre.") : LOCTEXT("ResetRowTip", "Put the row back: 0, or 1 for a size."),
						[CanReset, Row]() { return CanReset(Row, -1); }, [Reset, Row]() { Reset(Row, -1); })
				]
			];
	};

	auto Small = [](const FText& Label, const FText& Tip, TFunction<bool()> Enabled, TFunction<void()> Run) -> TSharedRef<SWidget>
	{
		return SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), "SimpleButton")
			.ContentPadding(FMargin(4.0f, 0.0f))
			.ToolTipText(Tip)
			.IsEnabled_Lambda([Enabled]() { return Enabled(); })
			.OnClicked_Lambda([Run]() { Run(); return FReply::Handled(); })
			[
				SNew(STextBlock).Text(Label).Font(SurfTheme::Font(8)).ColorAndOpacity(FSlateColor::UseSubduedForeground())
			];
	};

	// Minimal (S184b): "all fields in Item are way too consuming screen
	// space - minimize". One line a row, no headings, no sentences.
	return SNew(SBox)
		.Padding(FMargin(6.0f, 4.0f))
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 1.0f)[ RowOf(ERow::Position, LOCTEXT("Location", "Location")) ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 1.0f)[ RowOf(ERow::Rotation, LOCTEXT("Rotation", "Rotation")) ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 1.0f)[ RowOf(ERow::Scale, LOCTEXT("Scale", "Scale")) ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 1.0f)[ RowOf(ERow::Pivot, LOCTEXT("Pivot", "Origin")) ]
			// The stack level, for a see-through patch only (S197a).
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 1.0f)
			[
				SNew(SHorizontalBox)
				.Visibility_Lambda([]() { int32 Level; return ActivePatchLayer(Level) ? EVisibility::Visible : EVisibility::Collapsed; })
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(SBox).WidthOverride(44.0f)
					[
						SNew(STextBlock).Text(LOCTEXT("ItemLayer", "Layer")).Font(SurfTheme::Small()).ColorAndOpacity(SurfTheme::Text())
						.ToolTipText(LOCTEXT("ItemLayerTip", "The stack level: 0 the opaque surface, 1 to 4 drawn over it in turn, lifted by the layer offset at build."))
					]
				]
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				[
					LayerSteps()
				]
			]
			// The group the pick is (S249b): how many, and Ungroup beside it.
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 1.0f)
			[
				SNew(SHorizontalBox)
				.Visibility_Lambda([M]() { return M.IsValid() && !M->DescribePickedGroups().IsEmpty() ? EVisibility::Visible : EVisibility::Collapsed; })
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				[
					SNew(STextBlock).Font(SurfTheme::Font(8)).ColorAndOpacity(SurfTheme::Text())
					.Text_Lambda([M]() { return M.IsValid() ? FText::FromString(M->DescribePickedGroups()) : FText::GetEmpty(); })
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					Small(LOCTEXT("ItemUngroup", "Ungroup"), LOCTEXT("ItemUngroupTip", "The group undone, one level; a piece picked alone leaves its group. Ctrl+Alt+G."),
						[M]() { return M.IsValid(); }, [M]() { if (M.IsValid()) { M->UngroupSelection(); } })
				]
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SHorizontalBox)
				// The texture follows a move, or stays (S185b).
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				[
					SNew(SCheckBox)
					.ToolTipText(LOCTEXT("UVsFollowTip", "On: moving points re-measures the UVs, so the texture follows the new shape. Off: the UVs stay where they were."))
					.IsChecked_Lambda([]() { const USurfSettings* S = GetDefault<USurfSettings>(); return !S || S->bUVsFollowEdits ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
					.OnCheckStateChanged_Lambda([](ECheckBoxState State) { if (USurfSettings* W = SidePanelSettings()) { W->bUVsFollowEdits = State == ECheckBoxState::Checked; W->SaveConfig(); } })
					[
						SNew(STextBlock).Text(LOCTEXT("UVsFollow", "UVs follow")).Font(SurfTheme::Font(8)).ColorAndOpacity(SurfTheme::Text())
					]
				]
			]
			// The buttons on a line of their own (S192d): beside the tick
			// they ran into it at the panel's usual width.
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f, 0.0f, 0.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f)[ SNullWidget::NullWidget ]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					Small(LOCTEXT("PivotFromSelection", "To selection"), LOCTEXT("PivotFromSelectionTip", "Set the origin to the selection: one vertex exactly, faces or several vertices at their centre."),
						[M]() { return M.IsValid() && M->CanSetPivot(); }, [M]() { if (M.IsValid()) { M->SetPivotFromSelection(); } })
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					Small(LOCTEXT("PivotCentre", "Centre"), LOCTEXT("PivotCentreTip", "The origin back to the centre of the selection."),
						[M]() { return M.IsValid() && M->HasPivot(); }, [M]() { if (M.IsValid()) { M->ClearPivot(); } })
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					Small(LOCTEXT("ResetAll", "Reset all"),
						LOCTEXT("ResetAllTip", "Put everything back: location 0, rotation 0, scale 1, the origin to the centre."),
						[CanReset]() { return CanReset(ERow::Position, -1) || CanReset(ERow::Rotation, -1) || CanReset(ERow::Scale, -1) || CanReset(ERow::Pivot, -1); },
						[CanReset, Reset]()
						{
							const FScopedTransaction Transaction(LOCTEXT("ItemResetAll", "Reset All"));
							for (const ERow Row : { ERow::Position, ERow::Rotation, ERow::Scale, ERow::Pivot })
							{
								if (CanReset(Row, -1))
								{
									Reset(Row, -1);
								}
							}
						})
				]
			]
			// Nothing picked: the level itself (S249a: "level bakes itself =
			// thats a parameter for the level ... do we have a place for
			// level params?"). Its name and its own tick.
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 0.0f)
			[
				SNew(SHorizontalBox)
				.Visibility_Lambda([Fresh]() { return Fresh().bValid ? EVisibility::Collapsed : EVisibility::Visible; })
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(SBox).WidthOverride(44.0f)
					[
						SNew(STextBlock).Text(LOCTEXT("ItemLevel", "Level")).Font(SurfTheme::Font(8)).ColorAndOpacity(SurfTheme::Text())
					]
				]
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				[
					SNew(STextBlock).Font(SurfTheme::Font(8)).ColorAndOpacity(SurfTheme::Dim())
					.Text_Lambda([]()
					{
						const UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
						return World ? FText::FromString(World->GetMapName()) : FText::GetEmpty();
					})
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(SCheckBox)
					.ToolTipText(LOCTEXT("ItemLevelBakesTip", "The level bakes by itself before Play and before a save: its geometry and every placed prefab not already baked."))
					.IsChecked_Lambda([]() { return USurfLevelBake::IsOn(GEditor ? GEditor->GetEditorWorldContext().World() : nullptr) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
					.OnCheckStateChanged_Lambda([](ECheckBoxState New) { if (GEditor) { USurfLevelBake::SetOn(GEditor->GetEditorWorldContext().World(), New == ECheckBoxState::Checked); } })
					[
						SNew(STextBlock).Text(LOCTEXT("ItemLevelBakes", "Bakes itself")).Font(SurfTheme::Font(8)).ColorAndOpacity(SurfTheme::Text())
					]
				]
			]
		];
}

TSharedRef<SWidget> SSurfSidePanel::BuildView()
{
	auto S = []() { return GetDefault<USurfSettings>(); };

	auto Spin = [](TFunction<double()> Get, TFunction<void(double)> Set, double Min, double Max, const FText& Tip) -> TSharedRef<SWidget>
	{
		return SNew(SBox).WidthOverride(70.0f)
			[
				SNew(SSpinBox<double>)
				.Font(SurfTheme::Font(8))
				.MinValue(Min).MaxValue(Max)
				.MinSliderValue(Min).MaxSliderValue(Max)
				.Delta(0.01)
				.MinFractionalDigits(2)
				.MaxFractionalDigits(2)
				.Value_Lambda([Get]() { return Get(); })
				.OnValueChanged_Lambda([Set](double V) { Set(V); })
				.OnValueCommitted_Lambda([Set](double V, ETextCommit::Type) { Set(V); })
				.ToolTipText(Tip)
			];
	};

	// Compact (S248a: "can we have some icons instead of full names for Lit
	// Unlit Paint etc? takes screen space ... review this section, maybe add
	// some missing options"): one line a subject, chips on it, the words in
	// the tooltips; the views wear the editor's own view-mode icons.
	const FCheckBoxStyle& Toggle = FAppStyle::Get().GetWidgetStyle<FCheckBoxStyle>("ToggleButtonCheckbox");
	auto Line = [](const FText& Label, TSharedRef<SWidget> Content) -> TSharedRef<SWidget>
	{
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)
			[
				SNew(SBox).WidthOverride(50.0f)
				[
					SNew(STextBlock).Text(Label).Font(SurfTheme::Bold(8)).ColorAndOpacity(SurfTheme::Header())
				]
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
			[
				Content
			];
	};
	auto Chip = [&Toggle](TSharedRef<SWidget> Face, const FText& Tip, TFunction<bool()> Get, TFunction<void()> Press) -> TSharedRef<SWidget>
	{
		return SNew(SBox).Padding(1.0f, 1.0f)
			[
				SNew(SCheckBox)
				.Style(&Toggle)
				.ToolTipText(Tip)
				.IsChecked_Lambda([Get]() { return Get() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
				.OnCheckStateChanged_Lambda([Press](ECheckBoxState) { Press(); })
				.Padding(FMargin(6.0f, 1.0f))
				[
					Face
				]
			];
	};
	auto Word = [](const FText& Text) -> TSharedRef<SWidget>
	{
		return SNew(STextBlock).Text(Text).Font(SurfTheme::Small());
	};
	auto Icon = [](const FSlateBrush* Brush) -> TSharedRef<SWidget>
	{
		return SNew(SBox).WidthOverride(16.0f).HeightOverride(16.0f)
			[
				SNew(SImage).Image(Brush).ColorAndOpacity(FSlateColor::UseForeground())
			];
	};
	// The view kinds square (S250a: "view icons are not square, theyre too vertical").
	auto Kind = [&Toggle](int32 Which, TSharedRef<SWidget> Face, const FText& Tip) -> TSharedRef<SWidget>
	{
		return SNew(SBox).Padding(1.0f).WidthOverride(26.0f).HeightOverride(26.0f)
			[
				SNew(SCheckBox)
				.Style(&Toggle)
				.ToolTipText(Tip)
				.HAlign(HAlign_Center)
				.Padding(FMargin(4.0f))
				.IsChecked_Lambda([Which]() { const USurfEdMode* Mode = SidePanelMode(); return Mode && Mode->GetViewKind() == Which ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
				.OnCheckStateChanged_Lambda([Which](ECheckBoxState) { if (USurfEdMode* Mode = SidePanelMode()) { Mode->SetViewKind(Which); } })
				[
					Face
				]
			];
	};
	auto Setting = [&Chip, &Word](const FText& Label, const FText& Tip, bool USurfSettings::* Field) -> TSharedRef<SWidget>
	{
		return Chip(Word(Label), Tip,
			[Field]() { const USurfSettings* W = GetDefault<USurfSettings>(); return W && W->*Field; },
			[Field]()
			{
				if (USurfSettings* W = SidePanelSettings())
				{
					W->*Field = !(W->*Field);
					W->SaveConfig();
				}
				ViewColoursChanged();
			});
	};
	auto Wires = [&Chip, &Word](ESurfWireView Which, const FText& Label, const FText& Tip) -> TSharedRef<SWidget>
	{
		return Chip(Word(Label), Tip,
			[Which]() { const USurfSettings* W = GetDefault<USurfSettings>(); return W && W->WireView == Which; },
			[Which]()
			{
				if (USurfSettings* W = SidePanelSettings())
				{
					W->WireView = Which;
					W->SaveConfig();
				}
				if (const USurfEdMode* Mode = SidePanelMode())
				{
					SurfSession::Verb(TEXT("wire_view"), Mode->DescribeWireView());
				}
				ViewColoursChanged();
			});
	};
	TSharedRef<SWidget> Clay = SNew(SBox).WidthOverride(16.0f).HeightOverride(16.0f)
		[
			SNew(SImage).Image_Lambda([]() { const USurfSettings* W = GetDefault<USurfSettings>(); return MatcapBall(W ? static_cast<int32>(W->MatcapLook) : 0); })
		];

	// A tick in a pull-down, for a setting (S250a).
	auto Tick = [](FMenuBuilder& Menu, const FText& Label, const FText& Tip, bool USurfSettings::* Field)
	{
		Menu.AddMenuEntry(Label, Tip, FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateLambda([Field]()
				{
					if (USurfSettings* W = SidePanelSettings())
					{
						W->*Field = !(W->*Field);
						W->SaveConfig();
					}
					ViewColoursChanged();
				}),
				FCanExecuteAction(),
				FIsActionChecked::CreateLambda([Field]() { const USurfSettings* W = GetDefault<USurfSettings>(); return W && W->*Field; })),
			NAME_None, EUserInterfaceActionType::ToggleButton);
	};
	const FText GhostLabel = LOCTEXT("ViewContextGhost", "Ghost");
	const FText GhostTip = LOCTEXT("ViewContextGhostTip", "Round a prefab open where it stands, the rest of the level ghosted - there to fit against, out of reach.");
	const FText WireLabel = LOCTEXT("ViewContextWire", "Wire");
	const FText WireTip = LOCTEXT("ViewContextWireTip", "Round a prefab open where it stands, the rest of the level as wires only, out of reach.");
	const FText RegularLabel = LOCTEXT("ViewContextRegular", "Regular");
	const FText RegularTip = LOCTEXT("ViewContextRegularTip", "Round a prefab open where it stands, the rest of the level as it is, in the view in use - out of reach all the same.");

	// A small arrow beside a chip, its settings in a pull-down (S249a).
	auto Drop = [](const FText& Tip, TFunction<TSharedRef<SWidget>()> Content) -> TSharedRef<SWidget>
	{
		return SNew(SBox).Padding(FMargin(0.0f, 1.0f, 3.0f, 1.0f)).VAlign(VAlign_Center)
			[
				SNew(SComboButton)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.HasDownArrow(true)
				.ContentPadding(FMargin(1.0f, 0.0f))
				.ToolTipText(Tip)
				.OnGetMenuContent_Lambda([Content]() { return Content(); })
				.ButtonContent()
				[
					SNullWidget::NullWidget
				]
			];
	};
	// The wires' colours and strength (S249a: "possible to have wireframe
	// color changed" / "also expose wire strength?").
	auto WiresMenu = [Spin, S, Tick]() -> TSharedRef<SWidget>
	{
		FMenuBuilder Menu(/*bShouldCloseWindowAfterMenuSelection*/ false, nullptr, nullptr, false, &SurfTheme::MenuStyle(), false);
		Menu.BeginSection(NAME_None, LOCTEXT("WiresDrawn", "Drawn"));
		Tick(Menu, LOCTEXT("ViewThrough", "Through"), LOCTEXT("ViewThroughTip", "The wires drawn through the surfaces, the hidden ones too."), &USurfSettings::bSeeThroughWires);
		Tick(Menu, LOCTEXT("ViewIds", "IDs"), LOCTEXT("ViewIdsTip", "Every patch's cage in a colour of its own, from its id."), &USurfSettings::bColourIdWires);
		Menu.EndSection();
		Menu.BeginSection(NAME_None, LOCTEXT("WiresColours", "Colours"));
		Menu.AddWidget(Swatch(LOCTEXT("WiresEditedTip", "The cage of what is being edited. Click to pick."),
			[S]() { return S() ? S()->ControlGridColour : FLinearColor::White; },
			[](const FLinearColor& C) { if (USurfSettings* W = SidePanelSettings()) { W->ControlGridColour = C; W->SaveConfig(); } ViewColoursChanged(); }),
			LOCTEXT("WiresEdited", "Edited"));
		Menu.AddWidget(Swatch(LOCTEXT("WiresOthersTip", "Every other patch's cage, and placed prefabs', when all are shown. Click to pick."),
			[S]() { return S() ? S()->InactiveGridColour : FLinearColor::Gray; },
			[](const FLinearColor& C) { if (USurfSettings* W = SidePanelSettings()) { W->InactiveGridColour = C; W->SaveConfig(); } ViewColoursChanged(); }),
			LOCTEXT("WiresOthers", "Others"));
		Menu.AddWidget(Swatch(LOCTEXT("WiresPrimitivesTip", "The edges of primitives. Click to pick."),
			[S]() { return S() ? S()->BrushWireColour : FLinearColor::Red; },
			[](const FLinearColor& C) { if (USurfSettings* W = SidePanelSettings()) { W->BrushWireColour = C; W->SaveConfig(); } ViewColoursChanged(); }),
			LOCTEXT("WiresPrimitives", "Primitives"));
		Menu.EndSection();
		Menu.BeginSection(NAME_None, LOCTEXT("WiresStrengthHeading", "Strength"));
		Menu.AddWidget(Spin([S]() { return S() ? static_cast<double>(S()->WireStrength) : 1.0; },
			[](double V) { if (USurfSettings* W = SidePanelSettings()) { W->WireStrength = static_cast<float>(FMath::Clamp(V, 0.05, 1.0)); W->SaveConfig(); } if (GEditor) { GEditor->RedrawLevelEditingViewports(); } },
			0.05, 1.0, LOCTEXT("WiresStrengthTip", "How strong the wires are, 1 full. What is picked keeps its full colour.")),
			LOCTEXT("WiresStrength", "Strength"));
		Menu.EndSection();
		return Menu.MakeWidget();
	};
	// The rest of the level round a prefab edited where it stands (S249a).
	auto Context = [&Chip, &Word](ESurfContextLook Which, const FText& Label, const FText& Tip) -> TSharedRef<SWidget>
	{
		return Chip(Word(Label), Tip,
			[Which]() { const USurfSettings* W = GetDefault<USurfSettings>(); return W && W->ContextLook == Which; },
			[Which]()
			{
				if (USurfSettings* W = SidePanelSettings())
				{
					W->ContextLook = Which;
					W->SaveConfig();
				}
				if (USurfEdMode* Mode = SidePanelMode())
				{
					Mode->ApplyContextLook();
				}
			});
	};
	// The Paint view's own ball (S249a: "a sphere like the unlit one. with a
	// gradient green/red/black"), in its colours, not the text's.
	const FSlateBrush* PaintBrush = SurfIcons::Named(TEXT("paint-sphere"));
	TSharedRef<SWidget> PaintBall = SNew(SBox).WidthOverride(16.0f).HeightOverride(16.0f)
		[
			SNew(SImage).Image(PaintBrush ? PaintBrush : FAppStyle::GetBrush("LevelEditor.MeshPaintMode"))
		];

	// What else is drawn over the view, and the context's look (S250a).
	auto ShowMenu = [Spin, S, Tick, GhostLabel, GhostTip, WireLabel, WireTip, RegularLabel, RegularTip]() -> TSharedRef<SWidget>
	{
		FMenuBuilder Menu(/*bShouldCloseWindowAfterMenuSelection*/ false, nullptr, nullptr, false, &SurfTheme::MenuStyle(), false);
		Menu.BeginSection(NAME_None, LOCTEXT("ShowMarks", "Marks"));
		Tick(Menu, LOCTEXT("ViewOrigins", "Origins"), LOCTEXT("ViewOriginsTip", "The origin cross of every placed prefab."), &USurfSettings::bShowPrefabMarks);
		Tick(Menu, LOCTEXT("ViewBounds", "Prefab bounds"), LOCTEXT("ViewBoundsTip", "The extent box of every placed prefab, faint."), &USurfSettings::bShowPrefabBounds);
		Tick(Menu, LOCTEXT("ViewGroupBounds", "Group bounds"), LOCTEXT("ViewGroupBoundsTip", "Every group's corners, faint; a picked group shows its own whatever this says."), &USurfSettings::bShowGroupBounds);
		Tick(Menu, LOCTEXT("ViewHints", "Hints"), LOCTEXT("ViewHintsTip", "The foot line: the keys that mean something right now."), &USurfSettings::bShowTips);
		Menu.EndSection();
		Menu.BeginSection(NAME_None, LOCTEXT("ShowGrid", "Grid"));
		Tick(Menu, LOCTEXT("ViewAxes", "Axes"), LOCTEXT("ViewAxesTip", "X red and Y green through the origin, a short Z blue."), &USurfSettings::bShowGridAxes);
		Menu.AddWidget(Spin([S]() { return S() ? static_cast<double>(S()->GridOpacity) : 0.22; },
			[](double V) { if (USurfSettings* W = SidePanelSettings()) { W->GridOpacity = static_cast<float>(FMath::Clamp(V, 0.02, 1.0)); W->SaveConfig(); } if (GEditor) { GEditor->RedrawLevelEditingViewports(); } },
			0.02, 1.0, LOCTEXT("ViewGridStrengthTip", "How strong the lines are; the major lines and the axes stronger.")),
			LOCTEXT("ViewGridStrength", "Strength"));
		Menu.EndSection();
		Menu.BeginSection(NAME_None, LOCTEXT("ShowBackdrop", "Backdrop"));
		Tick(Menu, LOCTEXT("ViewBackdrop", "Gradient"), LOCTEXT("ViewBackdropTip", "A gradient behind everything in the unlit views, instead of black."), &USurfSettings::bBackdropGradient);
		Menu.AddWidget(Swatch(LOCTEXT("ViewTopTip", "The colour at the top. Click to pick."),
			[S]() { return S() ? S()->BackdropTop : FLinearColor::Black; },
			[](const FLinearColor& C) { if (USurfSettings* W = SidePanelSettings()) { W->BackdropTop = C; W->SaveConfig(); } ViewColoursChanged(); }),
			LOCTEXT("ViewTop", "Top"));
		Menu.AddWidget(Swatch(LOCTEXT("ViewBottomTip", "The colour at the bottom. Click to pick."),
			[S]() { return S() ? S()->BackdropBottom : FLinearColor::Black; },
			[](const FLinearColor& C) { if (USurfSettings* W = SidePanelSettings()) { W->BackdropBottom = C; W->SaveConfig(); } ViewColoursChanged(); }),
			LOCTEXT("ViewBottom", "Bottom"));
		Menu.EndSection();
		Menu.BeginSection(NAME_None, LOCTEXT("ShowContext", "Round an open prefab"));
		auto Look = [&Menu](ESurfContextLook Which, const FText& Label, const FText& Tip)
		{
			Menu.AddMenuEntry(Label, Tip, FSlateIcon(),
				FUIAction(
					FExecuteAction::CreateLambda([Which]()
					{
						if (USurfSettings* W = SidePanelSettings())
						{
							W->ContextLook = Which;
							W->SaveConfig();
						}
						if (USurfEdMode* Mode = SidePanelMode())
						{
							Mode->ApplyContextLook();
						}
					}),
					FCanExecuteAction(),
					FIsActionChecked::CreateLambda([Which]() { const USurfSettings* W = GetDefault<USurfSettings>(); return W && W->ContextLook == Which; })),
				NAME_None, EUserInterfaceActionType::RadioButton);
		};
		Look(ESurfContextLook::Ghost, GhostLabel, GhostTip);
		Look(ESurfContextLook::Wire, WireLabel, WireTip);
		Look(ESurfContextLook::Regular, RegularLabel, RegularTip);
		Menu.EndSection();
		return Menu.MakeWidget();
	};

	// Three lines (S250a: "review View UI. it feels overcrowded. not so
	// practical"): what is reached for often on its chip, the rest under the
	// arrow at the end of its line; the context line only while a prefab is
	// open where it stands.
	return SNew(SBox)
		.Padding(FMargin(10.0f, 8.0f))
		[
			SNew(SVerticalBox)

			// The view: Lit, Unlit, Matcap (its ball; the clay under the arrow), Paint.
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 1.0f)
			[
				Line(LOCTEXT("ViewLine", "View"),
					SNew(SWrapBox).UseAllottedSize(true)
					+ SWrapBox::Slot().VAlign(VAlign_Center)[ Kind(0, Icon(FAppStyle::GetBrush("EditorViewport.LitMode")), LOCTEXT("ViewLitTip", "Lit: the level's lighting. Shift+Z switches with Unlit.")) ]
					+ SWrapBox::Slot().VAlign(VAlign_Center)[ Kind(1, Icon(FAppStyle::GetBrush("EditorViewport.UnlitMode")), LOCTEXT("ViewUnlitTip", "Unlit: the modelling view, nothing of the level's lighting paid for.")) ]
					+ SWrapBox::Slot().VAlign(VAlign_Center)[ Kind(2, Clay, LOCTEXT("ViewMatcapTip", "Matcap: unlit, with clay on everything - the shape alone. The look and its colour under the arrow beside it.")) ]
					+ SWrapBox::Slot().VAlign(VAlign_Center)[ Drop(LOCTEXT("ViewClayDropTip", "The clay: its look and its colour."), []() { return MatcapMenu(); }) ]
					+ SWrapBox::Slot().VAlign(VAlign_Center)[ Kind(3, PaintBall, LOCTEXT("ViewPaintTip", "Paint: the ticked channels on every surface, brush or not, as painted.")) ])
			]
			// The wires: Z's three; Through, IDs, colours and strength under the arrow.
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 1.0f)
			[
				Line(LOCTEXT("ViewWiresLine", "Wires"),
					SNew(SWrapBox).UseAllottedSize(true)
					+ SWrapBox::Slot().VAlign(VAlign_Center)[ Wires(ESurfWireView::Selected, LOCTEXT("ViewWiresEdited", "Edited"), LOCTEXT("ViewWiresEditedTip", "The cages of the patches being edited (Z walks the three).")) ]
					+ SWrapBox::Slot().VAlign(VAlign_Center)[ Wires(ESurfWireView::All, LOCTEXT("ViewWiresAll", "All"), LOCTEXT("ViewWiresAllTip", "Every patch's cage and every placed prefab's, the others quiet.")) ]
					+ SWrapBox::Slot().VAlign(VAlign_Center)[ Wires(ESurfWireView::XRay, LOCTEXT("ViewWiresXRay", "X-ray"), LOCTEXT("ViewWiresXRayTip", "Every cage, and a click picks through the surfaces.")) ]
					+ SWrapBox::Slot().VAlign(VAlign_Center)[ Drop(LOCTEXT("ViewWiresDropTip", "Through, IDs, the wires' colours and their strength."), WiresMenu) ])
			]
			// What is drawn over the view; marks, the grid's axes and strength, the backdrop and the context under the arrow.
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 1.0f)
			[
				Line(LOCTEXT("ViewShowLine", "Show"),
					SNew(SWrapBox).UseAllottedSize(true)
					+ SWrapBox::Slot().VAlign(VAlign_Center)
					[
						Chip(Word(LOCTEXT("ViewOverlay", "Overlay")), LOCTEXT("ViewOverlayTip", "Our wires, points, marks and names at all (Alt+Z); off, the surfaces alone."),
							[]() { const USurfEdMode* Mode = SidePanelMode(); return Mode && !Mode->IsOverlayHidden(); },
							[]() { if (USurfEdMode* Mode = SidePanelMode()) { Mode->ToggleOverlay(); } })
					]
					+ SWrapBox::Slot().VAlign(VAlign_Center)
					[
						Chip(Word(LOCTEXT("ViewGrid", "Grid")), LOCTEXT("ViewGridTip", "The floor grid at zero, spaced by how high the eye is."),
							[]() { const USurfEdMode* Mode = SidePanelMode(); return Mode && Mode->IsGridShown(); },
							[]() { if (USurfEdMode* Mode = SidePanelMode()) { Mode->SetGridShown(!Mode->IsGridShown()); } })
					]
					+ SWrapBox::Slot().VAlign(VAlign_Center)[ Setting(LOCTEXT("ViewNames", "Names"), LOCTEXT("ViewNamesTip", "What things are called, on a plate in the viewport."), &USurfSettings::bShowNames) ]
					+ SWrapBox::Slot().VAlign(VAlign_Center)[ Setting(LOCTEXT("ViewGizmo", "Gizmo"), LOCTEXT("ViewGizmoTip", "The transform gizmo; off, G R S are the whole interface."), &USurfSettings::bShowGizmo) ]
					+ SWrapBox::Slot().VAlign(VAlign_Center)[ Drop(LOCTEXT("ViewShowDropTip", "Origins, bounds, hints, the grid's axes and strength, the backdrop, the look round an open prefab."), ShowMenu) ])
			]
			// Round a prefab open where it stands - only then.
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 1.0f)
			[
				SNew(SBox)
				.Visibility_Lambda([]() { const USurfEdMode* Mode = SidePanelMode(); return Mode && Mode->IsInsidePrefab() && Mode->IsRoomInPlace() ? EVisibility::Visible : EVisibility::Collapsed; })
				[
					Line(LOCTEXT("ViewContextLine", "Context"),
						SNew(SWrapBox).UseAllottedSize(true)
						+ SWrapBox::Slot().VAlign(VAlign_Center)[ Context(ESurfContextLook::Ghost, GhostLabel, GhostTip) ]
						+ SWrapBox::Slot().VAlign(VAlign_Center)[ Context(ESurfContextLook::Wire, WireLabel, WireTip) ]
						+ SWrapBox::Slot().VAlign(VAlign_Center)[ Context(ESurfContextLook::Regular, RegularLabel, RegularTip) ])
				]
			]
		];
}

#undef LOCTEXT_NAMESPACE
