// Copyright MapModelers. All Rights Reserved.

#include "SurfNotesView.h"
#include "SurfTheme.h"
#include "SurfEdMode.h"
#include "SurfInspector.h"
#include "SurfSession.h"
#include "SurfWindows.h"

#include "Editor.h"
#include "EditorModeManager.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/StyleColors.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"

#define LOCTEXT_NAMESPACE "SurfacingProNotes"

namespace
{
	USurfEdMode* NotesMode()
	{
		return Cast<USurfEdMode>(GLevelEditorModeTools().GetActiveScriptableMode(USurfEdMode::EM_SurfacingPro));
	}

	class SSurfNotesPanel : public SCompoundWidget
	{
	public:
		SLATE_BEGIN_ARGS(SSurfNotesPanel) {}
		SLATE_END_ARGS()

		void Construct(const FArguments&)
		{
			ChildSlot
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().FillHeight(1.0f)
				[
					SAssignNew(Scroll, SScrollBox)
					+ SScrollBox::Slot()
					[
						SAssignNew(Lines, SVerticalBox)
					]
				]
				+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right).Padding(4.0f, 3.0f)
				[
					SNew(SButton)
					.ButtonStyle(&SurfTheme::ToolButton())
					.Text(LOCTEXT("NotesClear", "Clear"))
					.ToolTipText(LOCTEXT("NotesClearTip", "Forget the lines so far. The session record keeps them."))
					.OnClicked_Lambda([]()
					{
						if (USurfEdMode* Mode = NotesMode())
						{
							if (Mode->Inspector) { Mode->Inspector->ResultHistory.Reset(); ++Mode->Inspector->ResultSerial; }
						}
						return FReply::Handled();
					})
				]
			];
			Rebuild();
		}

		virtual void Tick(const FGeometry&, const double, const float) override
		{
			const USurfEdMode* Mode = NotesMode();
			const uint32 Serial = Mode && Mode->Inspector ? Mode->Inspector->ResultSerial : 0;
			const bool bUp = Mode != nullptr;
			if (Serial != BuiltSerial || bUp != bBuiltUp)
			{
				Rebuild();
			}
		}

	private:
		TSharedPtr<SScrollBox> Scroll;
		TSharedPtr<SVerticalBox> Lines;
		uint32 BuiltSerial = ~0u;
		bool bBuiltUp = false;

		void Rebuild()
		{
			Lines->ClearChildren();
			const USurfEdMode* Mode = NotesMode();
			bBuiltUp = Mode != nullptr;
			BuiltSerial = Mode && Mode->Inspector ? Mode->Inspector->ResultSerial : 0;
			if (!Mode || !Mode->Inspector)
			{
				Lines->AddSlot().AutoHeight().Padding(8.0f, 6.0f)
				[
					SNew(STextBlock).Text(LOCTEXT("NotesModeOff", "Enter the SurfacingPro mode to see what it says.")).ColorAndOpacity(FStyleColors::Foreground)
				];
				return;
			}
			if (Mode->Inspector->ResultHistory.Num() == 0)
			{
				Lines->AddSlot().AutoHeight().Padding(8.0f, 6.0f)
				[
					SNew(STextBlock).Text(LOCTEXT("NotesEmpty", "What each verb did, or why it would not, is kept here as it happens.")).ColorAndOpacity(FStyleColors::Foreground).AutoWrapText(true)
				];
				return;
			}
			for (const FString& Line : Mode->Inspector->ResultHistory)
			{
				// A refusal reads amber, as it does in the corner.
				const bool bRefusal = USurfInspector::IsRefusal(Line);
				Lines->AddSlot().AutoHeight().Padding(8.0f, 1.0f)
				[
					SNew(STextBlock)
					.Text(FText::FromString(Line))
					.Font(SurfTheme::Font(8))
					.ColorAndOpacity(bRefusal ? SurfTheme::Warn() : FSlateColor(FStyleColors::White))
					.AutoWrapText(true)
				];
			}
			if (Scroll.IsValid())
			{
				Scroll->ScrollToEnd();
			}
		}
	};
}

namespace SurfNotesTab
{
	const FName TabName("SurfacingProNotes");

	static TSharedRef<SDockTab> Spawn(const FSpawnTabArgs&)
	{
		TSharedRef<SDockTab> Tab = SNew(SDockTab)
			.TabRole(ETabRole::NomadTab)
			.Label(LOCTEXT("NotesTabLabel", "Notes"))
			[
				SNew(SSurfNotesPanel)
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
			.SetDisplayName(LOCTEXT("NotesTabTitle", "SurfacingPro Notes"))
			.SetTooltipText(LOCTEXT("NotesTabTip", "Every line the tool has said this session, in order."))
			.SetGroup(WorkspaceMenu::GetMenuStructure().GetLevelEditorCategory())
			.SetIcon(FSlateIcon(TEXT("SurfacingProStyle"), "SurfacingPro.Verb.notes"));
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
		SurfWindows::OpenFrame(TabName, LOCTEXT("NotesFrameTitle", "Notes"), []() -> TSharedRef<SWidget> { return SNew(SSurfNotesPanel); }, FVector2D(320.0, 220.0));
		SurfSession::Verb(TEXT("notes_window"), TEXT("open"));
	}
}

#undef LOCTEXT_NAMESPACE
