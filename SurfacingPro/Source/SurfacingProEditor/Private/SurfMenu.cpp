// Copyright MapModelers. All Rights Reserved.
//
// Every verb as a menu (S79). See SurfMenu.h for why.

#include "SurfMenu.h"
#include "SurfSession.h"
#include "SurfAssetsView.h"
#include "SurfTheme.h"
#include "SurfIcons.h"
#include "Widgets/Images/SImage.h"
#include "SurfSidePanel.h"

#include "SurfEdMode.h"
#include "SurfMaterialsView.h"
#include "SurfPaintView.h"
#include "SurfFlagsView.h"
#include "SurfParamsView.h"
#include "SurfNotesView.h"
#include "SurfSettings.h"
#include "SurfTexturesView.h"
#include "SurfMaterialManager.h"
#include "SurfToolPalette.h"
#include "SurfKeymap.h"
#include "SurfUVView.h"

#include "EditorModeManager.h"
#include "EditorViewportClient.h"
#include "LevelEditorViewport.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Framework/Docking/TabManager.h"
#include "ToolMenus.h"
#include "Widgets/SWidget.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/STextBlock.h"
#include "Styling/AppStyle.h"
#include "Styling/StyleColors.h"
#include "Styling/StyleDefaults.h"

#define LOCTEXT_NAMESPACE "SurfacingPro"

namespace SurfMenu
{
	namespace
	{
		/** The mode, if it is up. A menu entry that outlives it does nothing. */
		USurfEdMode* ActiveMenuMode()
		{
			return Cast<USurfEdMode>(GLevelEditorModeTools().GetActiveScriptableMode(USurfEdMode::EM_SurfacingPro));
		}

		const FText& GroupTitle(ESurfVerbGroup Group)
		{
			static const FText Create = LOCTEXT("MenuCreate", "Create");
			static const FText Geometry = LOCTEXT("MenuGeometry", "Geometry");
			static const FText Shape = LOCTEXT("MenuShape", "Shape");
			static const FText Surface = LOCTEXT("MenuSurface", "Surface");
			static const FText UVName = LOCTEXT("MenuUV", "UV");
			static const FText Select = LOCTEXT("MenuSelect", "Select");
			static const FText View = LOCTEXT("MenuView", "View");
			switch (Group)
			{
			case ESurfVerbGroup::View:     return View;
			case ESurfVerbGroup::Create:   return Create;
			case ESurfVerbGroup::Geometry: return Geometry;
			case ESurfVerbGroup::Shape:    return Shape;
			case ESurfVerbGroup::Surface:  return Surface;
			case ESurfVerbGroup::UV:       return UVName;
			default:                       return Select;
			}
		}

		/**
		 * One verb as a row (S152e): the left button runs it, the right
		 * button opens its parameters - the same right-click as on the bar,
		 * "too bad we cant right click from top menu?" - and the menu lists
		 * verbs and nothing else. A menu entry cannot be right-clicked; a
		 * widget in a menu can.
		 */
		void AddVerb(FMenuBuilder& Menu, const FSurfVerb& Verb)
		{
			Menu.AddWidget(MakeVerbRow(Verb), FText::GetEmpty(), /*bNoIndent*/ true);
		}

		/** The ids that name a whole menu in the Q list, in English, whatever the words on screen. */
		const TCHAR* FamilyKey(ESurfVerbGroup Family)
		{
			switch (Family)
			{
			case ESurfVerbGroup::View:     return TEXT("View");
			case ESurfVerbGroup::Create:   return TEXT("Create");
			case ESurfVerbGroup::Geometry: return TEXT("Geometry");
			case ESurfVerbGroup::Shape:    return TEXT("Shape");
			case ESurfVerbGroup::Surface:  return TEXT("Surface");
			case ESurfVerbGroup::UV:       return TEXT("UV");
			default:                       return TEXT("Select");
			}
		}

		const ESurfVerbGroup AllFamilies[] = { ESurfVerbGroup::Create, ESurfVerbGroup::Geometry,
			ESurfVerbGroup::Shape, ESurfVerbGroup::Surface, ESurfVerbGroup::UV, ESurfVerbGroup::Select, ESurfVerbGroup::View };

		bool FamilyOfFavId(const FString& Id, ESurfVerbGroup& Out)
		{
			for (ESurfVerbGroup Family : AllFamilies)
			{
				if (Id.Equals(FamilyFavId(Family), ESearchCase::IgnoreCase))
				{
					Out = Family;
					return true;
				}
			}
			return false;
		}

		TSharedPtr<SWidget> RootParent()
		{
			if (TSharedPtr<SWindow> Root = FGlobalTabmanager::Get()->GetRootWindow())
			{
				return Root->GetContent();
			}
			return nullptr;
		}

		/**
		 * At the cursor. Anchored to a row inside a menu, the new one is that
		 * menu's child and the menu stays open under it; otherwise every
		 * menu goes first.
		 */
		void PushAtCursor(FMenuBuilder& Menu, TSharedPtr<SWidget> Anchor)
		{
			if (Anchor.IsValid())
			{
				FSlateApplication::Get().PushMenu(Anchor.ToSharedRef(), FWidgetPath(), Menu.MakeWidget(),
					FSlateApplication::Get().GetCursorPos(), FPopupTransitionEffect(FPopupTransitionEffect::ContextMenu));
				return;
			}
			FSlateApplication::Get().DismissAllMenus();
			TSharedPtr<SWidget> Parent = RootParent();
			if (!Parent.IsValid())
			{
				return;
			}
			FSlateApplication::Get().PushMenu(Parent.ToSharedRef(), FWidgetPath(), Menu.MakeWidget(),
				FSlateApplication::Get().GetCursorPos(), FPopupTransitionEffect(FPopupTransitionEffect::ContextMenu));
		}

		void AddFamilyLine(FMenuBuilder& Menu, ESurfVerbGroup Family)
		{
			const FString Id = FamilyFavId(Family);
			const bool bIn = SurfVerbLayout::IsFavorite(Id);
			Menu.AddMenuEntry(
				FText::Format(bIn ? LOCTEXT("CtxFamilyOut", "Remove the {0} menu from the Q menu") : LOCTEXT("CtxFamilyIn", "Add the {0} menu to the Q menu"), GroupTitle(Family)),
				LOCTEXT("CtxFamilyTip", "The whole menu, as one entry of the Q menu."), FSlateIcon(),
				FUIAction(FExecuteAction::CreateLambda([Id]() { SurfVerbLayout::ToggleFavorite(Id); FSlateApplication::Get().DismissAllMenus(); })));
		}

		/**
		 * A family as a submenu that takes a right-click (S196a): "right
		 * quick add to quick menu ... submenu etc. so it adds the whole menu
		 * to the Q panel". The entry is our widget, so the right button
		 * reaches it; the left one opens it as any submenu.
		 */
		void AddFamilySubMenu(FMenuBuilder& Menu, ESurfVerbGroup Family, const TArray<FSurfVerb>& Mine)
		{
			TSharedRef<TWeakPtr<SWidget>> Self = MakeShared<TWeakPtr<SWidget>>();
			TSharedRef<SWidget> Entry = SNew(SBorder)
				.BorderImage(FStyleDefaults::GetNoBrush())
				.Padding(0.0f)
				.ToolTipText(LOCTEXT("FamilyEntryTip", "Right-click: add the whole menu to the Q menu."))
				.OnMouseButtonDown_Lambda([Family, Self](const FGeometry&, const FPointerEvent& Event) -> FReply
				{
					if (Event.GetEffectingButton() == EKeys::RightMouseButton)
					{
						OpenFamilyContext(Family, Self->Pin());
						return FReply::Handled();
					}
					return FReply::Unhandled();
				})
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(0.0f, 2.0f)
					[
						SNew(STextBlock)
						.TextStyle(&FAppStyle::Get().GetWidgetStyle<FTextBlockStyle>("Menu.Label"))
						.Text(GroupTitle(Family))
					]
					// No arrow of ours (S200a): the menu draws its own - two showed.
				];
			*Self = Entry;
			Menu.AddSubMenu(Entry, FNewMenuDelegate::CreateLambda([Mine](FMenuBuilder& Sub)
			{
				for (const FSurfVerb& Verb : Mine)
				{
					AddVerb(Sub, Verb);
				}
			}));
		}
	}

	FString FamilyFavId(ESurfVerbGroup Family)
	{
		return FString(TEXT("menu:")) + FamilyKey(Family);
	}

	void OpenFamilyContext(ESurfVerbGroup Family, TSharedPtr<SWidget> Anchor)
	{
		if (!FSlateApplication::IsInitialized())
		{
			return;
		}
		FMenuBuilder Menu(/*bShouldCloseWindowAfterMenuSelection*/ true, nullptr, nullptr, false, &SurfTheme::MenuStyle(), /*bSearchable*/ false);
		AddFamilyLine(Menu, Family);
		PushAtCursor(Menu, Anchor);
	}

	void AddVerbLines(FMenuBuilder& Menu, const FSurfVerb& Verb)
	{
		auto Then = [](TFunction<void()> Do)
		{
			return FUIAction(FExecuteAction::CreateLambda([Do]() { Do(); FSlateApplication::Get().DismissAllMenus(); }));
		};
		const FString Id = SurfVerbLayout::IdOf(Verb);
		USurfEdMode* Mode = ActiveMenuMode();
		const TArray<FString> PoolIds = Mode ? SurfVerbLayout::PoolIds(SurfVerbs::BuildPool(Mode, nullptr)) : TArray<FString>();
		const bool bFav = SurfVerbLayout::IsFavorite(Id);
		Menu.AddMenuEntry(
			bFav ? LOCTEXT("CtxQOut", "Remove from the Q menu") : LOCTEXT("CtxQIn", "Add to the Q menu"),
			LOCTEXT("CtxQTip", "Q opens your picks at the cursor."), FSlateIcon(),
			Then([Id]() { SurfVerbLayout::ToggleFavorite(Id); }));
		AddFamilyLine(Menu, Verb.Group);
		if (PoolIds.Num() > 0)
		{
			Menu.AddSeparator();
			const bool bOnBar = SurfVerbLayout::OnBar(Id, PoolIds);
			const bool bOnSide = bOnBar || SurfVerbLayout::OnShelf(Id, PoolIds);
			Menu.AddMenuEntry(
				bOnSide ? LOCTEXT("CtxSideOut", "Remove from sidebar") : LOCTEXT("CtxSideIn", "Add to sidebar"),
				LOCTEXT("CtxSideTip", "The column beside the viewport, a tab per family."), FSlateIcon(),
				Then([Id, PoolIds, bOnSide]()
				{
					if (bOnSide) { SurfVerbLayout::RemoveFromSurfaces(Id, PoolIds); }
					else { SurfVerbLayout::SendTo(Id, /*bBar*/ false, PoolIds); }
				}));
			Menu.AddMenuEntry(
				bOnBar ? LOCTEXT("CtxUnpin", "Unpin") : LOCTEXT("CtxPin", "Pin to the top of the sidebar"),
				LOCTEXT("CtxPinTip", "Pinned buttons stand above the tabs, always shown."), FSlateIcon(),
				Then([Id, PoolIds, bOnBar]() { SurfVerbLayout::SendTo(Id, /*bBar*/ !bOnBar, PoolIds); }));
		}
	}

	bool AddVerbNamed(FMenuBuilder& Menu, const FString& Label, const FText& Shown)
	{
		USurfEdMode* Mode = ActiveMenuMode();
		if (!Mode)
		{
			return false;
		}
		for (const FSurfVerb& Verb : SurfVerbs::BuildPool(Mode, nullptr))
		{
			if (Verb.Label.ToString() == Label)
			{
				FSurfVerb Named = Verb;
				if (!Shown.IsEmpty())
				{
					// The row says Shown; the verb, its Q entry and its sidebar place keep the name.
					Named.LiveLabel = [Shown]() { return Shown; };
				}
				AddVerb(Menu, Named);
				return true;
			}
		}
		return false;
	}

	void OpenVerbContext(const FSurfVerb& Verb, TSharedPtr<SWidget> Anchor, int32 ShowAsSurface)
	{
		if (!FSlateApplication::IsInitialized())
		{
			return;
		}
		// A choice closes the menu underneath too.
		auto Then = [](TFunction<void()> Do)
		{
			return FUIAction(FExecuteAction::CreateLambda([Do]() { Do(); FSlateApplication::Get().DismissAllMenus(); }));
		};
		const FSurfVerb Copy = Verb;
		FMenuBuilder Menu(/*bShouldCloseWindowAfterMenuSelection*/ true, nullptr, nullptr, false, &SurfTheme::MenuStyle(), /*bSearchable*/ false);
		// Arranging (S206a): the section's words / icons first.
		if (ShowAsSurface >= 0 && SurfVerbLayout::IsEditing())
		{
			SurfVerbLayout::AddShowAsLines(Menu, static_cast<ESurfIconSurface>(ShowAsSurface));
			Menu.AddSeparator();
		}
		if (Copy.Options)
		{
			Menu.AddMenuEntry(LOCTEXT("CtxOptions", "Options..."), LOCTEXT("CtxOptionsTip", "Its parameters, in the frame at the cursor."), FSlateIcon(),
				Then([Copy]() { SurfParamsTab::Show(Copy); }));
			Menu.AddSeparator();
		}
		AddVerbLines(Menu, Copy);
		PushAtCursor(Menu, Anchor);
	}

	TSharedRef<SWidget> MakeVerbRow(const FSurfVerb& Verb)
	{
		{
			const FSurfVerb Copy = Verb;
			// Words, icons or both, as the menus' setting says (S195a).
			const ESurfIconMode IconMode = SurfIcons::ModeFor(ESurfIconSurface::Menus);
			const FSlateBrush* Icon = IconMode != ESurfIconMode::Words ? SurfIcons::ForVerb(Verb.Label.ToString()) : nullptr;
			const bool bShowWord = !Icon || IconMode == ESurfIconMode::IconAndWord;
			TFunction<bool()> Enabled = Verb.IsEnabled;
			const bool bHasOptions = static_cast<bool>(Verb.Options);
			TSharedRef<TWeakPtr<SWidget>> Self = MakeShared<TWeakPtr<SWidget>>();

			TSharedRef<SWidget> Row = SNew(SBorder)
				.BorderImage(FStyleDefaults::GetNoBrush())
				.Padding(0.0f)
				.OnMouseButtonUp_Lambda([Copy, Self](const FGeometry&, const FPointerEvent& Event) -> FReply
				{
					if (Event.GetEffectingButton() == EKeys::RightMouseButton)
					{
						// The same right-click everywhere (S196a), the menu
						// kept open under it (S197a).
						OpenVerbContext(Copy, Self->Pin(), static_cast<int32>(ESurfIconSurface::Menus));
						return FReply::Handled();
					}
					return FReply::Unhandled();
				})
				[
					SNew(SButton)
					.ButtonStyle(&SurfTheme::MenuStyle().GetWidgetStyle<FButtonStyle>("Menu.Button"))
					.ContentPadding(FMargin(14.0f, 3.0f, 10.0f, 3.0f))
					.HAlign(HAlign_Fill)
					.ToolTipText(FText::Format(LOCTEXT("MenuVerbTip", "{0}\nRight-click: options, the Q menu, the sidebar."), SurfVerbs::VerbTip(Copy)))
					.IsEnabled_Lambda([Enabled]() { return Enabled ? Enabled() : true; })
					.OnClicked_Lambda([Copy]()
					{
						FSlateApplication::Get().DismissAllMenus();
						if (Copy.Run)
						{
							// Remembered before it runs (S170a), as the bar's
							// chips do: the frame's replay undoes what the last
							// verb wrote, from where it started - a verb run
							// from a menu left that mark stale, and the replay
							// undid his deletes too - "showed two other boxes
							// from nowhere".
							if (USurfEdMode* Mode = Cast<USurfEdMode>(GLevelEditorModeTools().GetActiveScriptableMode(USurfEdMode::EM_SurfacingPro)))
							{
								Mode->NoteVerb(SurfVerbLayout::IdOf(Copy), Copy.Run);
							}
							Copy.Run();
						}
						return FReply::Handled();
					})
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
						[
							SNew(SBox)
							.WidthOverride(16.0f)
							.HeightOverride(16.0f)
							.Visibility(IconMode != ESurfIconMode::Words ? EVisibility::Visible : EVisibility::Collapsed)
							[
								// Quieter than the words (S196a): "in menus, icons
								// can be more subtle ... too white bright".
								SNew(SImage)
								.Image(Icon)
								.ColorAndOpacity(SurfTheme::Dim())
								.Visibility(Icon ? EVisibility::Visible : EVisibility::Hidden)
							]
						]
						+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
						[
							SNew(STextBlock)
							.Visibility(bShowWord ? EVisibility::Visible : EVisibility::Collapsed)
							.TextStyle(&FAppStyle::Get().GetWidgetStyle<FTextBlockStyle>("Menu.Label"))
							.Text_Lambda([Copy]() { return Copy.LiveLabel ? Copy.LiveLabel() : Copy.Label; })
						]
						// The key at the right, in grey, the editor's own way (S156c);
						// the ellipsis only for a verb with parameters and no key.
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(12.0f, 0.0f, 0.0f, 0.0f)
						[
							// The editor's key column type (S192e).
							SNew(STextBlock)
							.TextStyle(&FAppStyle::Get().GetWidgetStyle<FTextBlockStyle>("Menu.Keybinding"))
							.Text_Lambda([Copy, bHasOptions]()
							{
								const FString Key = SurfKeymap::Describe(Copy.Shortcut);
								if (!Key.IsEmpty() && !Key.Equals(TEXT("unbound")))
								{
									return FText::FromString(Key);
								}
								// No options mark (S193c): it read as a dropdown.
								return FText::GetEmpty();
							})
							.ColorAndOpacity(FSlateColor::UseSubduedForeground())
						]
					]
				];
			*Self = Row;
			return Row;
		}
	}

	namespace
	{

		void AddWindows(FMenuBuilder& Menu)
		{
			// The windows as their verbs' rows (S204a): each can go on the
			// sidebar or in the Q menu from here. Without the mode, plain entries.
			USurfEdMode* Mode = ActiveMenuMode();
			const TArray<FSurfVerb> Pool = Mode ? SurfVerbs::BuildPool(Mode, nullptr) : TArray<FSurfVerb>();
			auto Window = [&Menu, &Pool](const TCHAR* VerbLabel, const FText& Shown, const FText& Tip, const FName& Icon, TFunction<void()> Open)
			{
				if (const FSurfVerb* Verb = Pool.FindByPredicate([VerbLabel](const FSurfVerb& Each) { return Each.Label.ToString() == VerbLabel; }))
				{
					FSurfVerb Named = *Verb;
					Named.LiveLabel = [Shown]() { return Shown; };
					AddVerb(Menu, Named);
					return;
				}
				Menu.AddMenuEntry(Shown, Tip, FSlateIcon(TEXT("SurfacingProStyle"), FName(*(FString(TEXT("SurfacingPro.Verb.")) + Icon.ToString()))), FUIAction(FExecuteAction::CreateLambda([Open]() { Open(); })));
			};
			Window(TEXT("Actions"), LOCTEXT("MenuActionsWindow", "Actions"), LOCTEXT("MenuActionsWindowTip", "Every action by family, in a window that docks: run, star, add to the sidebar."), "layout-grid", []() { SurfActionsTab::Open(); });
			Window(TEXT("Assets"), LOCTEXT("MenuAssets", "Assets"), LOCTEXT("MenuAssetsTip", "The SidePanel's Assets tab: prefabs, meshes and levels as tiles, to place."), "folders", []() { SurfSidePanel::Show(ESurfSideTab::Assets); });
			Window(TEXT("Material Library"), LOCTEXT("MenuMaterialsWindow", "Material Library"), LOCTEXT("MenuMaterialsWindowTip", "The project's materials, with a search; click one to put it on the selection."), "sphere", []() { SurfMaterialsTab::Open(); });
			Window(TEXT("Material Manager"), LOCTEXT("MenuManagerWindow", "Material Manager"), LOCTEXT("MenuManagerWindowTip", "The picked material on a preview cube, its parameters beside it to edit and save."), "tool", []() { SurfMaterialManagerTab::Open(); });
			Window(TEXT("Textures"), LOCTEXT("MenuTexturesWindow", "Textures"), LOCTEXT("MenuTexturesWindowTip", "Textures brought in as ours, from any folder on disk."), "texture", []() { SurfTexturesTab::Open(); });
			Window(TEXT("UVs"), LOCTEXT("MenuUVWindow", "UVs"), LOCTEXT("MenuUVWindowTip", "The selected patches' nets over the tile grid, live."), "grid-dots", []() { SurfUVTab::Open(); });
			// No Paint or Flags entries (S197a): "remove standalone flags window now we
			// have side panel? same for paint" - they are the SidePanel's tabs.
			Window(TEXT("Side Panel"), LOCTEXT("MenuSidePanel", "SidePanel"), LOCTEXT("MenuSidePanelTip", "Down the viewport's edge, N: Item, Paint, Flags, View, Assets, Materials."), "layout-sidebar-right", []() { SurfSidePanel::Open(); });
			// No Parameters entry since S162f: the frame that pops on use is the
			// one parameter surface (S160a) - "two param windows redundant".
			// Docked › Parameters keeps the tab for whoever docks it.
			if (const USurfSettings* Recording = GetDefault<USurfSettings>(); Recording && Recording->bRecordSessions)
			{
				Window(TEXT("Notes"), LOCTEXT("MenuNotesWindow", "Notes"), LOCTEXT("MenuNotesWindowTip", "Every line the tool has said this session - what each verb did, or why it would not - in order."), "notes", []() { SurfNotesTab::Open(); });
			}
			// The small ones are frames of their own (S156e); docked, they are
			// the editor's tabs, for whoever wants them in the layout.
			Menu.AddSubMenu(LOCTEXT("MenuDocked", "Docked"), LOCTEXT("MenuDockedTip", "The same windows as editor tabs, to dock into the layout."),
				FNewMenuDelegate::CreateLambda([](FMenuBuilder& Sub)
				{
					auto Tab = [&Sub](const FText& Label, const TCHAR* Name)
					{
						const FName TabName(Name);
						Sub.AddMenuEntry(Label, FText::GetEmpty(), FSlateIcon(),
							FUIAction(FExecuteAction::CreateLambda([TabName]() { FGlobalTabmanager::Get()->TryInvokeTab(FTabId(TabName)); })));
					};
					Tab(LOCTEXT("DockParams", "Parameters"), TEXT("SurfacingProParameters"));
					if (const USurfSettings* Recording = GetDefault<USurfSettings>(); Recording && Recording->bRecordSessions)
					{
						Tab(LOCTEXT("DockNotes", "Notes"), TEXT("SurfacingProNotes"));
					}
				}));
			// Settings is a page of the editor's preferences, not a window (S247a): its row is the View family's.
		}
	}

	const FText& FamilyTitle(ESurfVerbGroup Family)
	{
		return GroupTitle(Family);
	}

	void BuildWindows(FMenuBuilder& Menu)
	{
		AddWindows(Menu);
	}

	void BuildFamily(FMenuBuilder& Menu, USurfEdMode* Mode, ESurfVerbGroup Family)
	{
		if (!Mode)
		{
			Menu.AddMenuEntry(LOCTEXT("MenuModeOffEntry", "Enter the SurfacingPro mode to use these"),
				LOCTEXT("MenuModeOffTip", "The verbs work inside the SurfacingPro mode (the editor's Modes)."), FSlateIcon(), FUIAction());
			return;
		}
		// Copies, for the same reason as Build's (S87b): the submenus of the
		// verbs with options open after this has returned.
		const TArray<FSurfVerb> Pool = SurfVerbs::BuildPool(Mode, nullptr);
		int32 Added = 0;
		for (const FSurfVerb& Verb : Pool)
		{
			// A window's verb is under Windows, not among View's rows (S247f).
			if (Verb.Group == Family && !Verb.bRare && !Verb.bWindow)
			{
				AddVerb(Menu, Verb);
				++Added;
			}
		}
		if (Added == 0)
		{
			Menu.AddMenuEntry(LOCTEXT("MenuFamilyEmpty", "Nothing here"), FText::GetEmpty(), FSlateIcon(), FUIAction());
		}
	}

	void Build(FMenuBuilder& Menu, USurfEdMode* Mode)
	{
		if (!Mode)
		{
			Menu.BeginSection("Off", LOCTEXT("MenuModeOff", "SurfacingPro"));
			Menu.AddMenuEntry(LOCTEXT("MenuModeOffEntry", "Enter the SurfacingPro mode to use these"),
				LOCTEXT("MenuModeOffTip", "The verbs work inside the SurfacingPro mode (the editor's Modes)."), FSlateIcon(), FUIAction());
			Menu.EndSection();
			return;
		}

		const TArray<FSurfVerb> Pool = SurfVerbs::BuildPool(Mode, nullptr);

		// The pivot first (S177a): "right click set selection as pivot".
		for (const FSurfVerb& Verb : Pool)
		{
			if (Verb.Label.ToString() == TEXT("Origin"))
			{
				// Named for what it does, in the right-click (S183a).
				// The row says it; the verb keeps its name for Q and the sidebar (S204a).
				FSurfVerb Named = Verb;
				Named.LiveLabel = []() { return LOCTEXT("MenuSetOrigin", "Set Origin to Selection"); };
				AddVerb(Menu, Named);
				break;
			}
		}
		if (Mode->HasPivot())
		{
			TWeakObjectPtr<USurfEdMode> Held = Mode;
			Menu.AddMenuEntry(LOCTEXT("MenuPivotCentre", "Origin back to the centre"), LOCTEXT("MenuPivotCentreTip", "Rotation and scale go about the selection's centre again."), FSlateIcon(),
				FUIAction(FExecuteAction::CreateLambda([Held]() { if (Held.IsValid()) { Held->ClearPivot(); } })));
		}
		Menu.AddSeparator();

		// The picked placed things - a prefab, a mesh, a level - and what is
		// done to them (S228a): Replace with... (a picker), Details, Materials.
		if (SurfAssetsTab::HasPickedActors())
		{
			SurfAssetsTab::AddActorLines(Menu);
			Menu.AddSeparator();
		}

		// What the picked wears, in the Material Manager (S251a): "right
		// click: add option to open material in mat manager? (if multiple
		// we can select which one.)"
		{
			TArray<UMaterialInterface*> Worn;
			SurfMaterialsLib::SelectionMaterials(Mode, Worn);
			// Not the checker a piece wears with no material (S251i): it is
			// ours, made on the fly, and not something to edit.
			Worn.RemoveAll([](const UMaterialInterface* Each) { return !Each || Each->GetOutermost() == GetTransientPackage(); });
			if (Worn.Num() == 1)
			{
				const TWeakObjectPtr<UMaterialInterface> One = Worn[0];
				Menu.AddMenuEntry(FText::Format(LOCTEXT("MenuInManagerOne", "Open {0} in the Material Manager"), FText::FromString(Worn[0]->GetName())),
					LOCTEXT("MenuInManagerOneTip", "The material the picked wears, with its parameters to edit."),
					FSlateIcon(FAppStyle::GetAppStyleSetName(), "ClassIcon.MaterialInstanceConstant"),
					FUIAction(FExecuteAction::CreateLambda([One]() { if (One.IsValid()) { SurfMaterialManagerTab::OpenWith(One.Get()); } })));
				Menu.AddSeparator();
			}
			else if (Worn.Num() > 1)
			{
				TArray<TWeakObjectPtr<UMaterialInterface>> Choices;
				for (UMaterialInterface* Each : Worn)
				{
					Choices.Add(Each);
				}
				Menu.AddSubMenu(FText::Format(LOCTEXT("MenuInManagerMany", "Open in the Material Manager ({0})"), FText::AsNumber(Worn.Num())),
					LOCTEXT("MenuInManagerManyTip", "The materials the picked wears; choose one to see it with its parameters."),
					FNewMenuDelegate::CreateLambda([Choices](FMenuBuilder& Sub)
					{
						for (const TWeakObjectPtr<UMaterialInterface>& Choice : Choices)
						{
							if (!Choice.IsValid())
							{
								continue;
							}
							Sub.AddMenuEntry(FText::FromString(Choice->GetName()), FText::GetEmpty(),
								FSlateIcon(FAppStyle::GetAppStyleSetName(), "ClassIcon.MaterialInstanceConstant"),
								FUIAction(FExecuteAction::CreateLambda([Choice]() { if (Choice.IsValid()) { SurfMaterialManagerTab::OpenWith(Choice.Get()); } })));
						}
					}));
				Menu.AddSeparator();
			}
		}

		// A section per family, in the order the families are worked in, each
		// one holding the verbs of that family from the same pool the bar and
		// the window draw from.
		const ESurfVerbGroup Families[] = { ESurfVerbGroup::Create, ESurfVerbGroup::Geometry,
			ESurfVerbGroup::Shape, ESurfVerbGroup::Surface, ESurfVerbGroup::UV, ESurfVerbGroup::Select, ESurfVerbGroup::View };

		for (ESurfVerbGroup Family : Families)
		{
			/**
			 * The verbs themselves, copied, not pointers into Pool (S87b).
			 *
			 * A submenu's delegate runs when the submenu is opened, which is
			 * long after Build has returned - and Pool is Build's own local.
			 * Every pointer in it was dangling by the time the hand reached
			 * the submenu, so opening one took the editor down with an
			 * access violation inside FSurfVerb's copy constructor.
			 *
			 * The rule this is the second instance of: **a delegate that
			 * outlives the call that made it may capture values and weak
			 * handles, never addresses of locals.**
			 */
			TArray<FSurfVerb> Mine;
			for (const FSurfVerb& Verb : Pool)
			{
				if (Verb.Group == Family && !Verb.bRare && !Verb.bWindow)
				{
					Mine.Add(Verb);
				}
			}
			if (Mine.Num() == 0)
			{
				continue;
			}

			// The family is a submenu, not a long section: a right-click
			// menu with sixty entries is a wall, and the hand knows which
			// family it wants before it knows which verb.
			AddFamilySubMenu(Menu, Family, Mine);
		}

		Menu.AddSubMenu(LOCTEXT("MenuWindows", "Windows"), FText::GetEmpty(),
			FNewMenuDelegate::CreateLambda([](FMenuBuilder& Sub) { AddWindows(Sub); }));

		// "right click > edit UI - thats an overall mode can edit all uis
		// then. save/reset/load configs available from setting." (S82a)
		TWeakObjectPtr<USurfEdMode> Weak = Mode;
		Menu.AddSubMenu(LOCTEXT("MenuInterface", "Interface"), FText::GetEmpty(),
			FNewMenuDelegate::CreateLambda([Weak](FMenuBuilder& Sub)
			{
				SurfVerbLayout::AppendInterfaceMenu(Sub, Weak.Get());
			}));
	}

	namespace
	{
		TSharedPtr<SWidget> MenuParent(FEditorViewportClient* ViewportClient)
		{
			TSharedPtr<SWidget> Parent;
			if (ViewportClient)
			{
				Parent = ViewportClient->GetEditorViewportWidget();
			}
			if (!Parent.IsValid())
			{
				if (TSharedPtr<SWindow> Root = FGlobalTabmanager::Get()->GetRootWindow())
				{
					Parent = Root->GetContent();
				}
			}
			return Parent;
		}
	}

	void OpenAtCursor(USurfEdMode* Mode, FEditorViewportClient* ViewportClient)
	{
		if (!FSlateApplication::IsInitialized())
		{
			return;
		}

		FMenuBuilder Menu(/*bShouldCloseWindowAfterMenuSelection*/ true, nullptr, nullptr, false, &FCoreStyle::Get(), /*bSearchable*/ false);
		Menu.BeginSection("SurfacingPro", LOCTEXT("MenuTitle", "SurfacingPro"));
		Build(Menu, Mode);
		Menu.EndSection();

		TSharedPtr<SWidget> Parent = MenuParent(ViewportClient);
		if (!Parent.IsValid())
		{
			return;
		}

		FSlateApplication::Get().PushMenu(Parent.ToSharedRef(), FWidgetPath(), Menu.MakeWidget(),
			FSlateApplication::Get().GetCursorPos(), FPopupTransitionEffect(FPopupTransitionEffect::ContextMenu));
	}

	/**
	 * Quick Favorites (S156b): "you can assign to it any action you like. it
	 * opens like a right click menu at cursor position when pressing the
	 * key." The star in the Parameters window's foot assigns; Q opens. The
	 * rows are the same rows as the menus' - left runs, right opens the
	 * parameters.
	 */
	bool OpenQuickFavorites(USurfEdMode* Mode, FEditorViewportClient* ViewportClient)
	{
		if (!FSlateApplication::IsInitialized())
		{
			return false;
		}
		const USurfSettings* Settings = GetDefault<USurfSettings>();
		if (!Settings || Settings->QuickFavorites.Num() == 0)
		{
			return false;
		}
		const TArray<FSurfVerb> Pool = SurfVerbs::BuildPool(Mode, nullptr);
		// No search box (S166a): "pressing q shouldnt search for Q
		// automatically" - the Q that opened it landed in the box.
		FMenuBuilder Menu(/*bShouldCloseWindowAfterMenuSelection*/ true, nullptr, nullptr, false, &FCoreStyle::Get(), /*bSearchable*/ false);
		int32 Listed = 0;
		for (const FString& Id : Settings->QuickFavorites)
		{
			// A whole menu (S196a), as its submenu.
			ESurfVerbGroup Family;
			if (FamilyOfFavId(Id, Family))
			{
				TArray<FSurfVerb> Mine;
				for (const FSurfVerb& Each : Pool)
				{
					if (Each.Group == Family)
					{
						Mine.Add(Each);
					}
				}
				if (Mine.Num() > 0)
				{
					AddFamilySubMenu(Menu, Family, Mine);
					++Listed;
				}
				continue;
			}
			const FSurfVerb* Verb = Pool.FindByPredicate([&Id](const FSurfVerb& Each) { return SurfVerbLayout::IdOf(Each).Equals(Id, ESearchCase::IgnoreCase); });
			if (Verb)
			{
				AddVerb(Menu, *Verb);
				++Listed;
			}
		}
		if (Listed == 0)
		{
			return false;
		}
		TSharedPtr<SWidget> Parent = MenuParent(ViewportClient);
		if (!Parent.IsValid())
		{
			return false;
		}
		FSlateApplication::Get().PushMenu(Parent.ToSharedRef(), FWidgetPath(), Menu.MakeWidget(),
			FSlateApplication::Get().GetCursorPos(), FPopupTransitionEffect(FPopupTransitionEffect::ContextMenu));
		SurfSession::Gesture(TEXT("quick_favorites"), FString::FromInt(Listed));
		return true;
	}

	void RegisterMainMenu()
	{
		if (!UToolMenus::IsToolMenuUIEnabled())
		{
			return;
		}

		UToolMenu* Bar = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu");
		if (!Bar)
		{
			return;
		}

		// The same tree the right button opens, in the editor's own menu bar:
		// one place that lists everything, for the verb used once a week.
		FToolMenuSection& Section = Bar->FindOrAddSection(NAME_None);
		Section.AddSubMenu("SurfacingPro",
			LOCTEXT("MainMenuTitle", "SurfacingPro"),
			LOCTEXT("MainMenuTip", "Everything the tool can do, by family - the same verbs as the bar and the SurfacingPro window."),
			FNewToolMenuChoice(FNewMenuDelegate::CreateLambda([](FMenuBuilder& Menu)
			{
				Build(Menu, ActiveMenuMode());
			})));
	}

	void UnregisterMainMenu()
	{
		if (UObjectInitialized() && UToolMenus::IsToolMenuUIEnabled())
		{
			UToolMenus::Get()->RemoveEntry("LevelEditor.MainMenu", NAME_None, "SurfacingPro");
		}
	}
}

#undef LOCTEXT_NAMESPACE
