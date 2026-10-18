// Copyright MapModelers. All Rights Reserved.
//
// The right-click menu.
//
// A verb's options belong next to the verb. "Weld, but keep the UVs" is a
// decision made at the moment of welding, by someone looking at the seam -
// not a setting found later in a window two clicks away. So the menu lists
// the verbs that apply to what is selected, with their options beside them.

#include "SurfEdMode.h"
#include "SurfInspector.h"
#include "SurfSettings.h"
#include "SurfSession.h"

#include "EditorViewportClient.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "SEditorViewport.h"

#define LOCTEXT_NAMESPACE "SurfacingProMenu"

void USurfEdMode::OpenContextMenu(FEditorViewportClient* ViewportClient)
{
	if (!ViewportClient || !FSlateApplication::IsInitialized())
	{
		return;
	}

	TSharedPtr<SEditorViewport> Parent = ViewportClient->GetEditorViewportWidget();
	if (!Parent.IsValid())
	{
		return;
	}

	TWeakObjectPtr<USurfEdMode> Self = this;

	const int32 Points = PointSelection.Num();
	const int32 Faces = QuadSelection.Num();
	const bool bHasPatch = SelectedPatch.IsValid();

	FMenuBuilder Menu(/*bShouldCloseWindowAfterMenuSelection*/ true, nullptr);

	// --- Weld, with its options ---------------------------------------------
	Menu.BeginSection("Weld", LOCTEXT("WeldSection", "Weld"));
	{
		Menu.AddMenuEntry(
			LOCTEXT("WeldCentre", "Weld at centre"),
			LOCTEXT("WeldCentreTip", "Collapse the selected points onto their centre. M."),
			FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateLambda([Self]() { if (Self.IsValid()) { Self->WeldSelectedPoints(true); } }),
				FCanExecuteAction::CreateLambda([Points]() { return Points >= 2; })));

		Menu.AddMenuEntry(
			LOCTEXT("WeldFirst", "Weld onto the first picked"),
			LOCTEXT("WeldFirstTip", "Collapse the selected points onto the one picked first."),
			FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateLambda([Self]() { if (Self.IsValid()) { Self->WeldSelectedPoints(false); } }),
				FCanExecuteAction::CreateLambda([Points]() { return Points >= 2; })));

		Menu.AddMenuEntry(
			LOCTEXT("WeldDistance", "Weld by distance"),
			LOCTEXT("WeldDistanceTip", "Collapse anything already closer than the tolerance in the panel. Nothing picked means the whole patch."),
			FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateLambda([Self]()
				{
					if (Self.IsValid())
					{
						const double Tolerance = Self->Inspector ? Self->Inspector->WeldToleranceCm : 1.0;
						Self->WeldSelectionByDistance(Tolerance);
					}
				}),
				FCanExecuteAction::CreateLambda([bHasPatch]() { return bHasPatch; })));

		// The option, beside the verbs it changes. A check mark, because it
		// is a state and not an action.
		Menu.AddMenuEntry(
			LOCTEXT("WeldKeepUVs", "Weld keeps the UVs"),
			LOCTEXT("WeldKeepUVsTip", "Leave the texture where it was across the pinch. The patch's mapping is frozen to do it."),
			FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateLambda([]()
				{
					if (USurfSettings* Settings = GetMutableDefault<USurfSettings>())
					{
						Settings->bWeldKeepsUVs = !Settings->bWeldKeepsUVs;
						Settings->SaveConfig();
					}
				}),
				FCanExecuteAction(),
				FIsActionChecked::CreateLambda([]()
				{
					const USurfSettings* Settings = GetDefault<USurfSettings>();
					return Settings && Settings->bWeldKeepsUVs;
				})),
			NAME_None,
			EUserInterfaceActionType::ToggleButton);
	}
	Menu.EndSection();

	// --- The rest of the verbs that apply -------------------------------------
	Menu.BeginSection("Edit", LOCTEXT("EditSection", "Edit"));
	{
		Menu.AddMenuEntry(
			LOCTEXT("Extrude", "Extrude"),
			LOCTEXT("ExtrudeTip", "Grow past the selected edge, then pull it out. E."),
			FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateLambda([Self]()
				{
					if (Self.IsValid() && Self->ExtrudeSelectedEdge())
					{
						Self->Inspector->RefreshSelectionCount();
					}
				}),
				FCanExecuteAction::CreateLambda([Points]() { return Points >= 2; })));

		Menu.AddMenuEntry(
			LOCTEXT("Separate", "Separate"),
			LOCTEXT("SeparateTip", "Cut the picked block of faces out as its own patch; the rest comes apart into patches too."),
			FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateLambda([Self]() { if (Self.IsValid()) { Self->SeparateSelectedFaces(); } }),
				FCanExecuteAction::CreateLambda([Faces]() { return Faces > 0; })));

		Menu.AddMenuEntry(
			LOCTEXT("Join", "Join with the patch sharing an edge"),
			LOCTEXT("JoinTip", "Fold whichever patch shares an edge with this one into it."),
			FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateLambda([Self]()
				{
					if (Self.IsValid())
					{
						const double Tolerance = Self->Inspector ? Self->Inspector->WeldToleranceCm : 1.0;
						Self->JoinSelectedPatches(Tolerance);
					}
				}),
				FCanExecuteAction::CreateLambda([bHasPatch]() { return bHasPatch; })));

		Menu.AddMenuEntry(
			LOCTEXT("Delete", "Delete"),
			LOCTEXT("DeleteTip", "The patch, or the selected rows and columns."),
			FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateLambda([Self]() { if (Self.IsValid()) { Self->DeleteSelection(); } }),
				FCanExecuteAction::CreateLambda([bHasPatch]() { return bHasPatch; })));
	}
	Menu.EndSection();

	// --- Sub-mode, because the menu is where the hand already is --------------
	Menu.BeginSection("SubMode", LOCTEXT("SubModeSection", "Select"));
	{
		auto AddSubMode = [&Menu, Self](const FText& Label, ESurfSubMode Which)
		{
			Menu.AddMenuEntry(Label, FText::GetEmpty(), FSlateIcon(),
				FUIAction(
					FExecuteAction::CreateLambda([Self, Which]() { if (Self.IsValid()) { Self->SetSubMode(Which); } }),
					FCanExecuteAction(),
					FIsActionChecked::CreateLambda([Self, Which]() { return Self.IsValid() && Self->GetSubMode() == Which; })),
				NAME_None,
				EUserInterfaceActionType::RadioButton);
		};

		AddSubMode(LOCTEXT("SubVert", "Vertices (1)"), ESurfSubMode::Vertex);
		AddSubMode(LOCTEXT("SubEdge", "Edges (2)"), ESurfSubMode::Edge);
		AddSubMode(LOCTEXT("SubFace", "Faces (3)"), ESurfSubMode::Face);
		AddSubMode(LOCTEXT("SubObj", "Patches (4)"), ESurfSubMode::Object);
	}
	Menu.EndSection();

	FSlateApplication::Get().PushMenu(
		Parent.ToSharedRef(),
		FWidgetPath(),
		Menu.MakeWidget(),
		FSlateApplication::Get().GetCursorPos(),
		FPopupTransitionEffect(FPopupTransitionEffect::ContextMenu));

	SurfSession::Gesture(TEXT("context_menu"));
}

#undef LOCTEXT_NAMESPACE
