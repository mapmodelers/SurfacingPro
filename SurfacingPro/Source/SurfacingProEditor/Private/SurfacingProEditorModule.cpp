// Copyright MapModelers. All Rights Reserved.

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "SurfacingProModule.h"
#include "SurfPreviewMaterial.h"
#include "SurfEdMode.h"
#include "SurfSettings.h"
#include "Editor.h"
#include "SurfOutliner.h"
#include "SurfMenu.h"
#include "SurfTheme.h"
#include "SurfIcons.h"
#include "SurfToolPalette.h"
#include "SurfPrefabThumbnail.h"
#include "SurfPrefab.h"
#include "ThumbnailRendering/ThumbnailManager.h"
#include "SurfUVView.h"
#include "SurfMaterialsView.h"
#include "SurfTexturesView.h"
#include "SurfMaterialManager.h"
#include "SurfMaterialThumbnails.h"
#include "SurfPaintView.h"
#include "SurfFlagsView.h"
#include "SurfParamsView.h"
#include "SurfAutoBake.h"
#include "SurfNotesView.h"
#include "SurfPrefabFactory.h"

#include "EditorModeManager.h"
#include "Framework/Application/IInputProcessor.h"
#include "Framework/Application/SlateApplication.h"
#include "Slate/SceneViewport.h"
#include "SurfSession.h"
#include "LevelEditorViewport.h"
#include "SEditorViewport.h"
#include "SurfGroup.h"
#include "SurfPrefabInstance.h"
#include "Misc/CoreDelegates.h"
#include "EngineUtils.h"
#include "UnrealClient.h"
#include "Styling/AppStyle.h"
#include "Styling/SlateIconFinder.h"
#include "ToolMenus.h"
#include "Styling/SlateStyle.h"
#include "Styling/SlateStyleRegistry.h"
#include "Brushes/SlateColorBrush.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Styling/StyleColors.h"
#include "Brushes/SlateImageBrush.h"
#include "Engine/Texture2D.h"

#define LOCTEXT_NAMESPACE "SurfacingProEditorModule"

namespace
{
	bool IsModeActive()
	{
		return GLevelEditorModeTools().IsModeActive(USurfEdMode::EM_SurfacingPro);
	}

	void ToggleMode()
	{
		FEditorModeTools& Tools = GLevelEditorModeTools();

		if (Tools.IsModeActive(USurfEdMode::EM_SurfacingPro))
		{
			Tools.DeactivateMode(USurfEdMode::EM_SurfacingPro);
		}
		else
		{
			Tools.ActivateMode(USurfEdMode::EM_SurfacingPro);
		}
	}

	/**
	 * A button on the level editor toolbar, because the mode lives behind a
	 * dropdown otherwise and entering it was three clicks from a cold start.
	 * Registered through the startup callback so it survives the tool menus
	 * being rebuilt, which happens whenever the editor layout is reset.
	 */
	void RegisterToolbarButton()
	{
		FToolMenuOwnerScoped Owner(TEXT("SurfacingPro"));

		UToolMenu* Toolbar = UToolMenus::Get()->ExtendMenu("LevelEditor.LevelEditorToolBar.User");
		if (!Toolbar)
		{
			return;
		}

		FToolMenuSection& Section = Toolbar->FindOrAddSection("SurfacingPro");

		FToolUIAction Action;
		Action.ExecuteAction = FToolMenuExecuteAction::CreateLambda(
			[](const FToolMenuContext&) { ToggleMode(); });
		Action.GetActionCheckState = FToolMenuGetActionCheckState::CreateLambda(
			[](const FToolMenuContext&)
			{
				return IsModeActive() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
			});

		FToolMenuEntry Entry = FToolMenuEntry::InitToolBarButton(
			"SurfacingProMode",
			Action,
			LOCTEXT("SurfacingProModeLabel", "Surfacing"),
			LOCTEXT("SurfacingProModeTip", "Enter or leave the surfacing mode."),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.MeshPaintMode"),
			EUserInterfaceActionType::ToggleButton);

		Section.AddEntry(Entry);
	}

	/**
	 * Our look, among the editor's own (S182a): "you haven't done the menu
	 * update (view mode, flags)". Each entry lands in the editor's list
	 * beside its neighbours, with an icon like theirs - no section of our
	 * own. The same entries fill the SurfacingPro menu on the menu bar,
	 * where they are simply listed.
	 */
	TSharedPtr<FSlateStyleSet> SurfStyle;
	TSharedPtr<FSlateStyleSet> SurfMenuStyle;

	void RegisterStyle()
	{
		if (!SurfMenuStyle.IsValid())
		{
			// The top strip's menus (S193c): recessed, a dark edge, square rows.
			SurfMenuStyle = MakeShared<FSlateStyleSet>(TEXT("SurfacingProMenuStyle"));
			SurfMenuStyle->SetParentStyleName(FAppStyle::GetAppStyleSetName());
			SurfMenuStyle->Set("Menu.Background", new FSlateColorBrush(FStyleColors::Recessed));
			SurfMenuStyle->Set("Menu.Outline", new FSlateRoundedBoxBrush(FStyleColors::Transparent, 0.0f, FStyleColors::WindowBorder, 1.0f));
			SurfMenuStyle->Set("Menu.Button", FButtonStyle(FAppStyle::Get().GetWidgetStyle<FButtonStyle>("Menu.Button"))
				.SetHovered(FSlateColorBrush(FStyleColors::Hover))
				.SetPressed(FSlateColorBrush(FStyleColors::Header)));
			FSlateStyleRegistry::RegisterSlateStyle(*SurfMenuStyle);
		}
		if (SurfStyle.IsValid())
		{
			return;
		}
		SurfStyle = MakeShared<FSlateStyleSet>(TEXT("SurfacingProStyle"));
		if (UTexture2D* Sphere = SurfPreviewMaterial::GetMatcapIcon())
		{
			SurfStyle->Set(TEXT("SurfacingPro.Matcap"), new FSlateImageBrush(Sphere, FVector2D(16.0, 16.0)));
		}
		// The five balls (S189a), for the looks in the menus.
		for (int32 Look = 0; Look < static_cast<int32>(ESurfMatcapLook::Count); ++Look)
		{
			if (UTexture2D* Ball = SurfPreviewMaterial::GetMatcapLookIcon(Look))
			{
				SurfStyle->Set(FName(*FString::Printf(TEXT("SurfacingPro.MatcapLook%d"), Look)), new FSlateImageBrush(Ball, FVector2D(16.0, 16.0)));
			}
		}
		// The verbs' icons (S195a).
		SurfIcons::Register(*SurfStyle);
		FSlateStyleRegistry::RegisterSlateStyle(*SurfStyle);
	}

	void UnregisterStyle()
	{
		if (SurfMenuStyle.IsValid())
		{
			FSlateStyleRegistry::UnRegisterSlateStyle(*SurfMenuStyle);
			SurfMenuStyle.Reset();
		}
		if (SurfStyle.IsValid())
		{
			FSlateStyleRegistry::UnRegisterSlateStyle(*SurfStyle);
			SurfStyle.Reset();
		}
	}

	FSlateIcon AppIcon(const TCHAR* Name)
	{
		return FSlateIcon(FAppStyle::GetAppStyleSetName(), FName(Name));
	}

	USurfEdMode* LiveMode()
	{
		return Cast<USurfEdMode>(GLevelEditorModeTools().GetActiveScriptableMode(USurfEdMode::EM_SurfacingPro));
	}

	FToolUIAction SettingTick(bool USurfSettings::* Field)
	{
		FToolUIAction Action;
		Action.ExecuteAction = FToolMenuExecuteAction::CreateLambda([Field](const FToolMenuContext&)
		{
			if (USurfSettings* Settings = GetMutableDefault<USurfSettings>())
			{
				Settings->*Field = !(Settings->*Field);
				Settings->SaveConfig();
				SurfPreviewMaterial::RefreshViewColours();
				if (GEditor)
				{
					GEditor->RedrawLevelEditingViewports();
				}
			}
		});
		Action.GetActionCheckState = FToolMenuGetActionCheckState::CreateLambda([Field](const FToolMenuContext&)
		{
			const USurfSettings* Settings = GetDefault<USurfSettings>();
			return Settings && Settings->*Field ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
		});
		return Action;
	}

	/**
	 * Our draw switches (S164a, S174a) - in the viewport's eye menu among
	 * the editor's common flags, alphabetically, each with an icon
	 * (S182a). With bAmongEditorFlags false they are simply listed.
	 */
	void AddViewTicks(FToolMenuSection& Section, bool bAmongEditorFlags = false)
	{
		auto Tick = [&Section, bAmongEditorFlags](const FName Name, const FText& Label, const FText& Tip, bool USurfSettings::* Field,
			const TCHAR* Icon, const TCHAR* Beside, EToolMenuInsertType Where)
		{
			FToolMenuEntry Entry = FToolMenuEntry::InitMenuEntry(Name, Label, Tip, AppIcon(Icon), SettingTick(Field), EUserInterfaceActionType::ToggleButton);
			if (bAmongEditorFlags)
			{
				Entry.InsertPosition = FToolMenuInsert(FName(Beside), Where);
			}
			Section.AddEntry(Entry);
		};
		Tick("SurfBackdrop", LOCTEXT("ShowBackdrop", "Backdrop Gradient"), LOCTEXT("ShowBackdropTip", "A gradient behind everything in the unlit view, instead of black."),
			&USurfSettings::bBackdropGradient, TEXT("ShowFlagsMenu.Atmosphere"), TEXT("Common_Collision"), EToolMenuInsertType::Before);
		// Where he looked for it (S186b): "looking like some colour id to me".
		Tick("SurfColourIds", LOCTEXT("ShowColourIds", "Colour IDs"), LOCTEXT("ShowColourIdsTip", "Every patch's cage in a colour of its own, from its id. Off: one cage colour."),
			&USurfSettings::bColourIdWires, TEXT("ShowFlagsMenu.Collision"), TEXT("Common_Collision"), EToolMenuInsertType::After);
		Tick("SurfGizmo", LOCTEXT("ShowGizmo", "Gizmo"), LOCTEXT("ShowGizmoTip", "The transform gizmo. Off, G R S are the whole interface."),
			&USurfSettings::bShowGizmo, TEXT("EditorViewport.TranslateRotateMode"), TEXT("Common_Fog"), EToolMenuInsertType::After);
		Tick("SurfHints", LOCTEXT("ShowHints", "Hints"), LOCTEXT("ShowHintsTip", "The foot line: the keys that mean something right now."),
			&USurfSettings::bShowTips, TEXT("Icons.Info"), TEXT("Common_Hair"), EToolMenuInsertType::After);
		Tick("SurfNames", LOCTEXT("ShowNames", "Names"), LOCTEXT("ShowNamesTip", "What things are called, on a plate in the viewport."),
			&USurfSettings::bShowNames, TEXT("ClassIcon.TextRenderComponent"), TEXT("Common_MediaPlanes"), EToolMenuInsertType::After);
		// Apart since S199a: the origin and the box.
		Tick("SurfMarks", LOCTEXT("ShowMarks", "Prefab Origins"), LOCTEXT("ShowMarksTip", "The origin cross of every placed prefab."),
			&USurfSettings::bShowPrefabMarks, TEXT("Icons.Transform"), TEXT("Common_Particles"), EToolMenuInsertType::After);
		Tick("SurfBounds", LOCTEXT("ShowBounds", "Prefab Bounds"), LOCTEXT("ShowBoundsTip", "The extent box of every placed prefab, faint."),
			&USurfSettings::bShowPrefabBounds, TEXT("ClassIcon.BoxComponent"), TEXT("SurfMarks"), EToolMenuInsertType::After);
	}

	/** Matcap as a view mode (S179a): the unlit view with the clay on; picked again, plain unlit. */
	FToolUIAction MatcapAction()
	{
		FToolUIAction Action;
		Action.ExecuteAction = FToolMenuExecuteAction::CreateLambda([](const FToolMenuContext&)
		{
			if (USurfEdMode* Mode = LiveMode())
			{
				Mode->SetViewKind(Mode->GetViewKind() == 2 ? 1 : 2);
			}
			else if (USurfSettings* Mutable = GetMutableDefault<USurfSettings>())
			{
				Mutable->bMatcap = !Mutable->bMatcap;
				Mutable->SaveConfig();
			}
		});
		Action.GetActionCheckState = FToolMenuGetActionCheckState::CreateLambda([](const FToolMenuContext&)
		{
			const USurfEdMode* Mode = LiveMode();
			return Mode && Mode->GetViewKind() == 2 ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
		});
		return Action;
	}

	/** Paint as a view mode (S229a): the channel on every surface; picked again, plain unlit. */
	FToolUIAction PaintViewAction()
	{
		FToolUIAction Action;
		Action.ExecuteAction = FToolMenuExecuteAction::CreateLambda([](const FToolMenuContext&)
		{
			if (USurfEdMode* Mode = LiveMode())
			{
				Mode->SetViewKind(Mode->GetViewKind() == 3 ? 1 : 3);
			}
		});
		Action.GetActionCheckState = FToolMenuGetActionCheckState::CreateLambda([](const FToolMenuContext&)
		{
			const USurfEdMode* Mode = LiveMode();
			return Mode && Mode->GetViewKind() == 3 ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
		});
		return Action;
	}

	/**
	 * The cage's view as one cycling entry (S184c): "we don't need actions
	 * for what the Z key does - if you make a button make a cycling button,
	 * not one for each". Its label says where it stands; a click steps on,
	 * as Z does. The surface wireframe is a tick beside it.
	 */
	void AddWireEntries(FToolMenuSection& Section)
	{
		FToolUIAction Cycle;
		Cycle.ExecuteAction = FToolMenuExecuteAction::CreateLambda([](const FToolMenuContext&)
		{
			if (USurfEdMode* Mode = LiveMode())
			{
				Mode->CycleWireView();
			}
			else if (USurfSettings* Settings = GetMutableDefault<USurfSettings>())
			{
				Settings->WireView = Settings->WireView == ESurfWireView::Selected ? ESurfWireView::All
					: Settings->WireView == ESurfWireView::All ? ESurfWireView::XRay : ESurfWireView::Selected;
				Settings->SaveConfig();
			}
		});
		const TAttribute<FText> Label = TAttribute<FText>::CreateLambda([]()
		{
			const USurfSettings* Settings = GetDefault<USurfSettings>();
			switch (Settings ? Settings->WireView : ESurfWireView::Selected)
			{
			case ESurfWireView::All:  return LOCTEXT("CageAll", "Cage: every patch");
			case ESurfWireView::XRay: return LOCTEXT("CageXRay", "Cage: every patch, x-ray");
			default:                  return LOCTEXT("CageEdited", "Cage: the edited patches");
			}
		});
		Section.AddEntry(FToolMenuEntry::InitMenuEntry("SurfCage", Label,
			LOCTEXT("CageTip", "Which cages show: the edited patches, every patch, every patch x-ray (a click reaches through). Click, or Z, for the next."),
			AppIcon(TEXT("EditorViewport.WireframeMode")), Cycle, EUserInterfaceActionType::Button));
		Section.AddEntry(FToolMenuEntry::InitMenuEntry("SurfSurfaceWire", LOCTEXT("SurfaceWire", "Surface Wireframe"),
			LOCTEXT("SurfaceWireTip", "The tessellated surface as thin lines under the cage, so a curve's real shape shows."), AppIcon(TEXT("EditorViewport.LitWireframeMode")),
			SettingTick(&USurfSettings::bShowSurfaceWireframe), EUserInterfaceActionType::ToggleButton));
	}

	/** The Paint view's own ball (S249a), green to red to black. */
	FSlateIcon PaintViewIcon()
	{
		return SurfStyle.IsValid() ? FSlateIcon(SurfStyle->GetStyleSetName(), "SurfacingPro.Verb.paint-sphere") : AppIcon(TEXT("LevelEditor.MeshPaintMode"));
	}

	FSlateIcon MatcapIcon()
	{
		return SurfStyle.IsValid() ? FSlateIcon(SurfStyle->GetStyleSetName(), "SurfacingPro.Matcap") : AppIcon(TEXT("EditorViewport.LitMode"));
	}

	/**
	 * The matcap's looks in the view-mode menus (S189a): "matcap icon only
	 * show in settings" - the balls were only in the SidePanel. A pick turns
	 * the matcap view on.
	 */
	void SetMatcapLook(int32 Look)
	{
		if (USurfSettings* Mutable = GetMutableDefault<USurfSettings>())
		{
			Mutable->MatcapLook = static_cast<ESurfMatcapLook>(Look);
			Mutable->SaveConfig();
		}
		SurfPreviewMaterial::RefreshViewColours();
		if (USurfEdMode* Mode = LiveMode())
		{
			if (Mode->GetViewKind() != 2)
			{
				Mode->SetViewKind(2);
			}
		}
		if (GEditor)
		{
			GEditor->RedrawLevelEditingViewports();
		}
		const UEnum* Names = StaticEnum<ESurfMatcapLook>();
		SurfSession::Verb(TEXT("matcap_look"), Names ? Names->GetDisplayNameTextByValue(Look).ToString() : FString::FromInt(Look));
	}

	void FillMatcapLooks(UToolMenu* Sub)
	{
		if (!Sub)
		{
			return;
		}
		FToolMenuSection& Section = Sub->AddSection("SurfMatcapLooks");
		const UEnum* Names = StaticEnum<ESurfMatcapLook>();
		for (int32 Look = 0; Look < static_cast<int32>(ESurfMatcapLook::Count); ++Look)
		{
			FToolUIAction Action;
			Action.ExecuteAction = FToolMenuExecuteAction::CreateLambda([Look](const FToolMenuContext&) { SetMatcapLook(Look); });
			Action.GetActionCheckState = FToolMenuGetActionCheckState::CreateLambda([Look](const FToolMenuContext&)
			{
				const USurfSettings* S = GetDefault<USurfSettings>();
				return S && static_cast<int32>(S->MatcapLook) == Look ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
			});
			const FSlateIcon Icon = SurfStyle.IsValid()
				? FSlateIcon(SurfStyle->GetStyleSetName(), FName(*FString::Printf(TEXT("SurfacingPro.MatcapLook%d"), Look)))
				: MatcapIcon();
			Section.AddEntry(FToolMenuEntry::InitMenuEntry(FName(*FString::Printf(TEXT("SurfMatcapLook%d"), Look)),
				Names ? Names->GetDisplayNameTextByValue(Look) : FText::AsNumber(Look), FText::GetEmpty(), Icon, Action, EUserInterfaceActionType::RadioButton));
		}
	}

	void AddMatcapLooksEntry(FToolMenuSection& Section)
	{
		Section.AddSubMenu("SurfMatcapLook", LOCTEXT("ViewMatcapLook", "Matcap Look"), LOCTEXT("ViewMatcapLookTip", "The ball the clay is read from: clay, soft grey, terracotta, studio, red wax."),
			FNewToolMenuDelegate::CreateStatic(&FillMatcapLooks), false, MatcapIcon());
	}

	/** Unlit as the editor's own, but not ticked while Matcap is on (S183a). */
	FToolUIAction UnlitAction()
	{
		FToolUIAction Action;
		Action.ExecuteAction = FToolMenuExecuteAction::CreateLambda([](const FToolMenuContext&)
		{
			if (USurfEdMode* Mode = LiveMode())
			{
				Mode->SetViewKind(1);
			}
			else if (GCurrentLevelEditingViewportClient)
			{
				GCurrentLevelEditingViewportClient->SetViewMode(VMI_Unlit);
				GCurrentLevelEditingViewportClient->Invalidate();
			}
		});
		Action.GetActionCheckState = FToolMenuGetActionCheckState::CreateLambda([](const FToolMenuContext&)
		{
			if (const USurfEdMode* Mode = LiveMode())
			{
				return Mode->GetViewKind() == 1 ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
			}
			return GCurrentLevelEditingViewportClient && GCurrentLevelEditingViewportClient->GetViewMode() == VMI_Unlit
				? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
		});
		return Action;
	}

	const TCHAR* ViewModesMenus[] = { TEXT("LevelEditor.ViewportToolbar.ViewModes"), TEXT("LevelEditor.LevelViewportToolbar.View") };

	/**
	 * The view-mode menu (S178a, S182a, S183a): a SurfacingPro section on
	 * top, as Landscape has its own - "have surfacingpro part on top
	 * always, like Landscape has its own section" - holding **Matcap**
	 * (the sphere its icon) and **Cage** (the wire views and the surface
	 * wireframe). "Instead of ticking both unlit and matcap, a single
	 * button": the editor's Unlit entry is hidden and one of ours stands
	 * in its place, ticked only when the view is unlit without the clay.
	 */
	void RegisterViewModesMenu()
	{
		RegisterStyle();
		FToolMenuOwnerScoped Owner(TEXT("SurfacingPro"));
		for (const TCHAR* MenuName : ViewModesMenus)
		{
			UToolMenu* Menu = UToolMenus::Get()->ExtendMenu(FName(MenuName));
			if (!Menu)
			{
				continue;
			}
			FToolMenuSection& Ours = Menu->AddSection("SurfacingPro", LOCTEXT("ViewModesSurfacingPro", "SurfacingPro"),
				FToolMenuInsert(NAME_None, EToolMenuInsertType::First));
			Ours.AddEntry(FToolMenuEntry::InitMenuEntry("SurfMatcap", LOCTEXT("ViewMatcap", "Matcap"),
				LOCTEXT("ViewMatcapTip", "Unlit, with clay on everything - the shape alone - and the backdrop behind. Lit or any other view mode turns it off."),
				MatcapIcon(), MatcapAction(), EUserInterfaceActionType::RadioButton));
			AddMatcapLooksEntry(Ours);
			Ours.AddEntry(FToolMenuEntry::InitMenuEntry("SurfPaintView", LOCTEXT("ViewPaint", "Paint"),
				LOCTEXT("ViewPaintTip", "The paint channel on every surface, brush or not. Lit or any other view mode turns it off."),
				PaintViewIcon(), PaintViewAction(), EUserInterfaceActionType::RadioButton));
			AddWireEntries(Ours);

			FToolMenuSection& Modes = Menu->FindOrAddSection("ViewMode");
			FToolMenuEntry Unlit = FToolMenuEntry::InitMenuEntry("SurfUnlit", LOCTEXT("ViewUnlit", "Unlit"),
				LOCTEXT("ViewUnlitTip", "Renders the scene with no lights."), AppIcon(TEXT("EditorViewport.UnlitMode")), UnlitAction(), EUserInterfaceActionType::RadioButton);
			Unlit.InsertPosition = FToolMenuInsert("LitMode", EToolMenuInsertType::After);
			Modes.AddEntry(Unlit);

			if (FCustomizedToolMenu* Custom = UToolMenus::Get()->AddMenuCustomization(FName(MenuName)))
			{
				Custom->AddEntry("UnlitMode")->Visibility = ECustomizedToolMenuVisibility::Hidden;
			}
		}
	}

	void UnregisterViewModesMenu()
	{
		if (!UObjectInitialized())
		{
			return;
		}
		if (UToolMenus* Menus = UToolMenus::TryGet())
		{
			for (const TCHAR* MenuName : ViewModesMenus)
			{
				if (FCustomizedToolMenu* Custom = Menus->AddMenuCustomization(FName(MenuName)))
				{
					Custom->AddEntry("UnlitMode")->Visibility = ECustomizedToolMenuVisibility::None;
				}
			}
		}
	}

	/** The SurfacingPro menu's View section: the same entries, listed. */
	void AddShadingEntries(FToolMenuSection& Section)
	{
		Section.AddEntry(FToolMenuEntry::InitMenuEntry("SurfMatcap", LOCTEXT("ViewMatcap", "Matcap"),
			LOCTEXT("ViewMatcapTipShort", "Unlit, with clay on everything."), MatcapIcon(), MatcapAction(), EUserInterfaceActionType::RadioButton));
		AddMatcapLooksEntry(Section);
		AddWireEntries(Section);
	}
}

namespace
{
	/** The SurfacingPro menu's entries (S174a/S175a), for the editor's menu bar and the viewport's toolbar alike. */
	void FillSurfacingProMenu(UToolMenu* Menu, bool bModeSection = true)
	{
		if (!Menu)
		{
			return;
		}
		if (bModeSection)
		{
			FToolMenuSection& Mode = Menu->AddSection("SurfacingProMode", LOCTEXT("MainMenuModeSection", "Mode"));
			FToolUIAction Action;
			Action.ExecuteAction = FToolMenuExecuteAction::CreateLambda([](const FToolMenuContext&)
			{
				FEditorModeTools& Tools = GLevelEditorModeTools();
				if (Tools.IsModeActive(USurfEdMode::EM_SurfacingPro))
				{
					Tools.DeactivateMode(USurfEdMode::EM_SurfacingPro);
				}
				else
				{
					Tools.ActivateMode(USurfEdMode::EM_SurfacingPro);
				}
			});
			Action.GetActionCheckState = FToolMenuGetActionCheckState::CreateLambda([](const FToolMenuContext&)
			{
				return GLevelEditorModeTools().IsModeActive(USurfEdMode::EM_SurfacingPro) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
			});
			Mode.AddEntry(FToolMenuEntry::InitMenuEntry("SurfModeOn", LOCTEXT("MainMenuModeOn", "SurfacingPro mode"),
				LOCTEXT("MainMenuModeOnTip", "Enter or leave the mode."), FSlateIcon(), Action, EUserInterfaceActionType::ToggleButton));
		}
		{
			// Submenus, not rows (S247f: "perhaps use a menu under view so all
			// screenspace isn't consumed by the toggles").
			FToolMenuSection& More = Menu->AddSection("SurfacingProMore");
			More.AddSubMenu("SurfShow", LOCTEXT("MainMenuShow", "Show"), LOCTEXT("MainMenuShowTip", "Names, prefab origins and bounds, the gizmo, hints, colour ids, the backdrop - the View tab's ticks."),
				FNewToolMenuChoice(FNewToolMenuDelegate::CreateLambda([](UToolMenu* Sub)
				{
					FToolMenuSection& Section = Sub->AddSection("SurfShowSection");
					AddViewTicks(Section);
				})));
			More.AddSubMenu("SurfShading", LOCTEXT("MainMenuShading", "Shading"), LOCTEXT("MainMenuShadingTip", "Matcap and its looks, the cage's wires."),
				FNewToolMenuChoice(FNewToolMenuDelegate::CreateLambda([](UToolMenu* Sub)
				{
					FToolMenuSection& Section = Sub->AddSection("SurfShadingSection");
					AddShadingEntries(Section);
				})));
			More.AddSubMenu("SurfWindows", LOCTEXT("MainMenuWindows", "Windows"), LOCTEXT("MainMenuWindowsTip", "Actions, Assets, Material Library, Material Manager, Textures, UVs, the SidePanel."),
				FNewToolMenuChoice(FNewMenuDelegate::CreateLambda([](FMenuBuilder& Builder) { SurfMenu::BuildWindows(Builder); })));
			More.AddSubMenu("SurfInterface", LOCTEXT("MainMenuInterface", "Interface"), LOCTEXT("MainMenuInterfaceTip", "The bar, Edit UI, the menu line, saved arrangements."),
				FNewToolMenuChoice(FNewMenuDelegate::CreateLambda([](FMenuBuilder& Builder)
				{
					USurfEdMode* Mode = Cast<USurfEdMode>(GLevelEditorModeTools().GetActiveScriptableMode(USurfEdMode::EM_SurfacingPro));
					SurfVerbLayout::AppendInterfaceMenu(Builder, Mode);
				})));
			// No Settings entry here (S247c): the View family's own row is the one above.
		}
	}

}

const ISlateStyle& SurfTheme::MenuStyle()
{
	return SurfMenuStyle.IsValid() ? static_cast<const ISlateStyle&>(*SurfMenuStyle) : FAppStyle::Get();
}

/**
 * The top strip's View menu (S193c): "perhaps view and surfacingpro menus
 * are the same kind ... move it so everything same place". The View
 * family's verbs, then what the viewport toolbar's SurfacingPro menu held:
 * the view switches, the shading, the windows, the interface, settings.
 */
TSharedRef<SWidget> SurfMenu::MakeViewMenu()
{
	static const FName Name(TEXT("SurfacingPro.StripView"));
	UToolMenus* Menus = UToolMenus::Get();
	if (!Menus->IsMenuRegistered(Name))
	{
		UToolMenu* Menu = Menus->RegisterMenu(Name, NAME_None, EMultiBoxType::Menu, false);
		Menu->SetStyleSet(&SurfTheme::MenuStyle());
		// No search box (S252a), as the six family menus have none.
		Menu->bSearchable = false;
		Menu->AddDynamicSection("SurfViewVerbs", FNewToolMenuDelegate::CreateLambda([](UToolMenu* Dynamic)
		{
			USurfEdMode* Live = Cast<USurfEdMode>(GLevelEditorModeTools().GetActiveScriptableMode(USurfEdMode::EM_SurfacingPro));
			if (!Live)
			{
				return;
			}
			FToolMenuSection& Verbs = Dynamic->AddSection("SurfViewVerbsSection");
			for (const FSurfVerb& Verb : SurfVerbs::BuildPool(Live, nullptr))
			{
				// The windows' verbs are the Windows submenu's (S247f).
				if (Verb.Group == ESurfVerbGroup::View && !Verb.bWindow && !Verb.bRare)
				{
					Verbs.AddEntry(FToolMenuEntry::InitWidget(FName(*SurfVerbLayout::IdOf(Verb)), SurfMenu::MakeVerbRow(Verb), FText::GetEmpty(), /*bNoIndent*/ true));
				}
			}
		}));
		FillSurfacingProMenu(Menu, /*bModeSection*/ false);
	}
	return Menus->GenerateWidget(Name, FToolMenuContext());
}

/**
 * A double-click on one of our actors enters the mode (S158c): "Double
 * clicking surfacingpro asset enters the surfacingpro mode?" - yes. In the
 * editor's own selection mode, a double-click over a group or a prefab
 * instance in the level viewport activates SurfacingPro; the mode's own
 * double-clicks are untouched because this only listens while it is off.
 */
class FSurfEntryByDoubleClick : public IInputProcessor
{
public:
	virtual void Tick(const float DeltaTime, FSlateApplication& SlateApp, TSharedRef<ICursor> Cursor) override {}

	virtual bool HandleMouseButtonDoubleClickEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent) override
	{
		if (MouseEvent.GetEffectingButton() != EKeys::LeftMouseButton || !GCurrentLevelEditingViewportClient)
		{
			return false;
		}
		if (GLevelEditorModeTools().IsModeActive(USurfEdMode::EM_SurfacingPro))
		{
			return false;
		}
		FLevelEditorViewportClient* Client = GCurrentLevelEditingViewportClient;
		TSharedPtr<SEditorViewport> Widget = Client->GetEditorViewportWidget();
		FViewport* Viewport = Client->Viewport;
		if (!Widget.IsValid() || !Viewport)
		{
			return false;
		}
		/**
		 * The picture's own widget (S188a): "couldnt doubleclick-open from far
		 * away still" - the editor viewport's geometry holds more than the
		 * picture, so the pixel read was off by the difference and a small,
		 * far piece was missed. And a few pixels of reach, nearest first.
		 */
		FGeometry Geometry = Widget->GetCachedGeometry();
		if (TSharedPtr<FSceneViewport> Scene = Widget->GetSceneViewport())
		{
			Geometry = Scene->GetCachedGeometry();
		}
		if (!Geometry.IsUnderLocation(MouseEvent.GetScreenSpacePosition()))
		{
			return false;
		}
		const FVector2D Local = Geometry.AbsoluteToLocal(MouseEvent.GetScreenSpacePosition()) * Geometry.Scale;
		const FIntPoint Size = Viewport->GetSizeXY();
		auto Ours = [](AActor* Hit) -> bool
		{
			for (AActor* Each = Hit; Each; Each = Each->GetAttachParentActor())
			{
				if (Each->IsA<ASurfGroup>() || Each->IsA<ASurfPrefabInstance>() || Each->Tags.Contains(TEXT("SurfacingProBaked")))
				{
					return true;
				}
			}
			return false;
		};
		const HHitProxy* First = nullptr;
		AActor* FirstActor = nullptr;
		const int32 CX = static_cast<int32>(Local.X);
		const int32 CY = static_cast<int32>(Local.Y);
		for (int32 Ring = 0; Ring <= 8; Ring += 2)
		{
			for (int32 DY = -Ring; DY <= Ring; DY += 2)
			{
				for (int32 DX = -Ring; DX <= Ring; DX += 2)
				{
					if (FMath::Max(FMath::Abs(DX), FMath::Abs(DY)) != Ring)
					{
						continue;
					}
					const int32 PX = CX + DX;
					const int32 PY = CY + DY;
					if (PX < 0 || PY < 0 || PX >= Size.X || PY >= Size.Y)
					{
						continue;
					}
					const HHitProxy* Proxy = Viewport->GetHitProxy(PX, PY);
					AActor* Actor = (Proxy && Proxy->IsA(HActor::StaticGetType())) ? static_cast<const HActor*>(Proxy)->Actor : nullptr;
					if (Ring == 0)
					{
						First = Proxy;
						FirstActor = Actor;
					}
					if (Ours(Actor))
					{
						GLevelEditorModeTools().ActivateMode(USurfEdMode::EM_SurfacingPro);
						return true;
					}
				}
			}
		}
		// What the double-click landed on, when it was not ours (S187b).
		SurfSession::Begin();
		SurfSession::Verb(TEXT("dblclick_missed"), FString::Printf(TEXT("proxy=%s actor=%s class=%s at=%.0f,%.0f size=%d,%d"),
			First ? First->GetType()->GetName() : TEXT("none"),
			FirstActor ? *FirstActor->GetActorLabel() : TEXT("-"),
			FirstActor ? *FirstActor->GetClass()->GetName() : TEXT("-"),
			Local.X, Local.Y, Size.X, Size.Y));
		return false;
	}
};

class FSurfacingProEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		// Only the binding happens here. The checker itself is built the
		// first time a group asks for it, so an editor that never opens the
		// mode never compiles a material it will not show.
		SurfPreviewMaterial::RegisterAsGroupDefault();

		SurfOutliner::RegisterMapOpenHook();

		// The UV tab is a nomad tab: registered once so the Window menu can
		// offer it and a saved layout can restore it, whether or not the mode
		// has ever been entered.
		SurfUVTab::Register();
	// No SurfacingPro menu on the editor's menu bar (S247a, redundancy V1):
	// the toolbar button enters the mode, the editor's Window menu lists
	// the tabs, the top strip holds the rest while the mode is up. No
	// Settings tab either (W3): the settings are the editor's own
	// preferences page.

	// A picture for every prefab in the Content Browser (S81a).
	UThumbnailManager::Get().RegisterCustomRenderer(USurfPrefab::StaticClass(), USurfPrefabThumbnailRenderer::StaticClass());
		SurfMaterialsTab::Register();
		SurfTexturesTab::Register();
		SurfMaterialManagerTab::Register();
		// Paint and Flags are the SidePanel's tabs only (S197a).
		SurfParamsTab::Register();
		SurfNotesTab::Register();
		SurfAutoBake::Register();
		// No Prefabs window (S247a, redundancy W1): the Assets tab is the library.
		SurfActionsTab::Register();

		// Dragging a prefab asset into a level places an instance of it.
		SurfPrefabFactory::Register();

		// A double-click on our actors enters the mode (S158c).
		FCoreDelegates::GetOnPostEngineInit().AddLambda([this]()
		{
			if (FSlateApplication::IsInitialized())
			{
				EntryByDoubleClick = MakeShared<FSurfEntryByDoubleClick>();
				FSlateApplication::Get().RegisterInputPreProcessor(EntryByDoubleClick);
			}
		});

		UToolMenus::RegisterStartupCallback(
			FSimpleMulticastDelegate::FDelegate::CreateStatic(&RegisterToolbarButton));
		// Our ticks left the viewport's Show menu (S247a, redundancy X3):
		// the View tab is their one place. The view kinds stay in the
		// view-mode menu, where the editor keeps its own.
		UToolMenus::RegisterStartupCallback(
			FSimpleMulticastDelegate::FDelegate::CreateStatic(&RegisterViewModesMenu));

		UE_LOG(LogSurfacingPro, Log, TEXT("SurfacingPro editor module started."));
	}

	TSharedPtr<FSurfEntryByDoubleClick> EntryByDoubleClick;

	virtual void ShutdownModule() override
	{
		if (EntryByDoubleClick.IsValid() && FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().UnregisterInputPreProcessor(EntryByDoubleClick);
		}
		EntryByDoubleClick.Reset();
		SurfUVTab::Unregister();
		SurfActionsTab::Unregister();
	if (UObjectInitialized())
	{
		UThumbnailManager::Get().UnregisterCustomRenderer(USurfPrefab::StaticClass());
	}
		SurfMaterialsTab::Unregister();
		SurfTexturesTab::Unregister();
		SurfMaterialManagerTab::Unregister();
		SurfMaterialThumbnails::Forget();
		SurfParamsTab::Unregister();
		SurfNotesTab::Unregister();
		SurfAutoBake::Unregister();
		SurfOutliner::UnregisterMapOpenHook();

		UToolMenus::UnRegisterStartupCallback(this);
		UToolMenus::UnregisterOwner(TEXT("SurfacingPro"));

		SurfPreviewMaterial::UnregisterAsGroupDefault();
		UnregisterViewModesMenu();
		UnregisterStyle();
	}
};

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FSurfacingProEditorModule, SurfacingProEditor)
