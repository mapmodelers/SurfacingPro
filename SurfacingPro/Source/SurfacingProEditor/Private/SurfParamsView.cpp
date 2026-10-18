// Copyright MapModelers. All Rights Reserved.

#include "SurfParamsView.h"
#include "SurfTheme.h"
#include "SurfEdMode.h"
#include "SurfSession.h"
#include "SurfToolPalette.h"
#include "SurfWindows.h"

#include "Editor.h"
#include "EditorModeManager.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/StyleColors.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"

#define LOCTEXT_NAMESPACE "SurfacingProParameters"

namespace
{
	USurfEdMode* ParamsMode()
	{
		return Cast<USurfEdMode>(GLevelEditorModeTools().GetActiveScriptableMode(USurfEdMode::EM_SurfacingPro));
	}

	/** The verb the window is on, by its id; the serial says when it changed. */
	FString FocusedId;
	uint32 FocusSerial = 0;
	/** Opened by a verb pressed rather than by hand: a viewport click puts it away (S157h). */
	bool bPassing = false;

	class SSurfParamsPanel : public SCompoundWidget
	{
	public:
		SLATE_BEGIN_ARGS(SSurfParamsPanel) {}
		SLATE_END_ARGS()

		void Construct(const FArguments&)
		{
			ChildSlot
			[
				SNew(SScrollBox)
				+ SScrollBox::Slot()
				[
					SAssignNew(Body, SBox).Padding(FMargin(8.0f, 6.0f))
				]
			];
			Rebuild();
		}

		virtual void Tick(const FGeometry&, const double, const float) override
		{
			// Rebuilt when the verb changes, and when the mode comes or goes
			// - never on a timer, which would pull a spin box out from under
			// a drag.
			const bool bModeUp = ParamsMode() != nullptr;
			if (BuiltSerial != FocusSerial || bBuiltWithMode != bModeUp || BuiltLayout != SurfVerbLayout::Serial())
			{
				Rebuild();
			}
		}

	private:
		TSharedPtr<SBox> Body;
		uint32 BuiltSerial = ~0u;
		int32 BuiltLayout = -1;
		bool bBuiltWithMode = false;

		void Rebuild()
		{
			BuiltSerial = FocusSerial;
			BuiltLayout = SurfVerbLayout::Serial();
			USurfEdMode* Mode = ParamsMode();
			bBuiltWithMode = Mode != nullptr;

			if (!Mode)
			{
				Body->SetContent(Note(LOCTEXT("ParamsModeOff", "Enter the SurfacingPro mode to set a verb's parameters.")));
				return;
			}

			const TArray<FSurfVerb> Pool = SurfVerbs::BuildPool(Mode, nullptr);
			const FSurfVerb* Verb = Pool.FindByPredicate([](const FSurfVerb& Each) { return SurfVerbLayout::IdOf(Each) == FocusedId; });
			if (!Verb)
			{
				Body->SetContent(Note(LOCTEXT("ParamsNothing", "Right-click a verb - on the bar, in the Actions window or in a menu - and what it has to set is here, with where its button lives.")));
				return;
			}

			// The options and the arrangement as one menu widget, standing
			// in the tab rather than popping up: the same builders the
			// right-click used to open, so nothing is written twice.
			FMenuBuilder Menu(/*bShouldCloseWindowAfterMenuSelection*/ false, nullptr, nullptr, false, &FCoreStyle::Get(), /*bSearchable*/ false);
			if (Verb->Options)
			{
				Menu.BeginSection("Options", LOCTEXT("ParamsOptions", "Options"));
				Verb->Options(Menu);
				Menu.EndSection();
			}
			else
			{
				Menu.BeginSection("Options", LOCTEXT("ParamsOptions", "Options"));
				Menu.AddMenuEntry(LOCTEXT("ParamsNone", "Nothing to set"), FText::GetEmpty(), FSlateIcon(), FUIAction());
				Menu.EndSection();
			}
			// The name, the options, and where the button lives as icons -
			// no description (S154a: "dont show description of a button
			// again in its right clicked menu"); the tip is on the name.
			Body->SetContent(
				SNew(SVerticalBox)
				// A category header (S193d).
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 2.0f)
				[
					SNew(SBorder)
					.BorderImage(FAppStyle::GetBrush("Brushes.Header"))
					.Padding(FMargin(8.0f, 0.0f))
					[
						SNew(SBox).HeightOverride(24.0f).VAlign(VAlign_Center)
						[
							SNew(STextBlock)
							.Text(Verb->Label)
							.ToolTipText(SurfVerbs::VerbTip(*Verb))
							.Font(SurfTheme::Bold(9))
							.ColorAndOpacity(SurfTheme::Header())
						]
					]
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					Menu.MakeWidget()
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(4.0f, 6.0f, 4.0f, 2.0f)
				[
					SurfVerbLayout::MakeArrangeRow(*Verb, SurfVerbLayout::PoolIds(Pool))
				]);
		}

		static TSharedRef<SWidget> Note(const FText& Text)
		{
			return SNew(STextBlock)
				.Text(Text)
				.Font(SurfTheme::Font(8))
				.ColorAndOpacity(FStyleColors::Foreground)
				.AutoWrapText(true);
		}
	};
}

namespace SurfParamsTab
{
	const FName TabName("SurfacingProParameters");

	static TSharedRef<SDockTab> Spawn(const FSpawnTabArgs&)
	{
		TSharedRef<SDockTab> Tab = SNew(SDockTab)
			.TabRole(ETabRole::NomadTab)
			.Label(LOCTEXT("ParamsTabLabel", "Parameters"))
			[
				SNew(SSurfParamsPanel)
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
			.SetDisplayName(LOCTEXT("ParamsTabTitle", "SurfacingPro Parameters"))
			.SetTooltipText(LOCTEXT("ParamsTabTip", "What one verb has to set, and where its button lives. Right-click any verb to bring it here."))
			.SetGroup(WorkspaceMenu::GetMenuStructure().GetLevelEditorCategory())
			.SetIcon(FSlateIcon(TEXT("SurfacingProStyle"), "SurfacingPro.Verb.adjustments-horizontal"));
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
		bPassing = false;
		Register();
		if (TSharedPtr<SDockTab> Docked = FGlobalTabmanager::Get()->FindExistingLiveTab(FTabId(TabName)))
		{
			if (SurfWindows::TabIsDockedInEditor(Docked))
			{
				// Docked by him: the tab is the window.
				FGlobalTabmanager::Get()->TryInvokeTab(FTabId(TabName));
				return;
			}
			// A floating tab the layout restored (S157d): the frame replaces it.
			Docked->RequestCloseTab();
		}
		SurfWindows::OpenFrame(TabName, LOCTEXT("ParamsFrameTitle", "Parameters"), []() -> TSharedRef<SWidget> { return SNew(SSurfParamsPanel); }, FVector2D(250.0, 460.0));
		SurfSession::Verb(TEXT("params_window"), TEXT("open"));
	}

	void Follow(const FString& Id)
	{
		if (!FSlateApplication::IsInitialized())
		{
			return;
		}
		if (Id != FocusedId)
		{
			FocusedId = Id;
			++FocusSerial;
		}
		if (USurfEdMode* Mode = ParamsMode())
		{
			Mode->TurnOptionsTo(Id);
		}
		// It turns, and opens nothing (S158a, back to S154a): "so for the
		// popup param we're keeping the window from the left [the adjust
		// frame]. forget last instructions." A window opening on Ctrl+R
		// also took the focus and cancelled the loop preview - "ctrl r IS
		// ALL broken".
	}

	void Dismiss()
	{
		if (bPassing && SurfWindows::IsFrameOpen(TabName))
		{
			SurfWindows::CloseFrame(TabName);
		}
		bPassing = false;
	}

	void Show(const FSurfVerb& Verb)
	{
		FocusedId = SurfVerbLayout::IdOf(Verb);
		++FocusSerial;
		bPassing = false;
		// One parameter surface (S160a): the right-click opens the frame in
		// the viewport on this verb; the docked tab only follows if it is up.
		if (USurfEdMode* Mode = ParamsMode())
		{
			Mode->ShowVerbOptions(FocusedId, /*bAtCursor*/ true);
			return;
		}
		Register();
		if (TSharedPtr<SDockTab> Docked = FGlobalTabmanager::Get()->FindExistingLiveTab(FTabId(TabName)))
		{
			if (SurfWindows::TabIsDockedInEditor(Docked))
			{
				// Docked by him: the tab is the window.
				FGlobalTabmanager::Get()->TryInvokeTab(FTabId(TabName));
				return;
			}
			// A floating tab the layout restored (S157d): the frame replaces it.
			Docked->RequestCloseTab();
		}
		SurfWindows::OpenFrame(TabName, LOCTEXT("ParamsFrameTitle", "Parameters"), []() -> TSharedRef<SWidget> { return SNew(SSurfParamsPanel); }, FVector2D(250.0, 460.0));
		SurfSession::Verb(TEXT("params_window"), FocusedId);
	}
}

#undef LOCTEXT_NAMESPACE
