// Copyright MapModelers. All Rights Reserved.

#include "SurfWindows.h"
#include "SurfTheme.h"

#include "Editor.h"
#include "EditorViewportClient.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "LevelEditorViewport.h"
#include "SEditorViewport.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/SWindow.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Images/SImage.h"
#include "Styling/AppStyle.h"
#include "Styling/StyleColors.h"
#include "Containers/Ticker.h"
#include "GenericPlatform/GenericWindow.h"
#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace SurfWindows
{
	namespace
	{
		/** The floating windows this placed, so the next one goes above them. */
		TArray<TWeakPtr<SWindow>> Placed;

		/** The level viewport's rectangle on the screen, or the main window's. */
		FSlateRect ViewportRect()
		{
			if (GCurrentLevelEditingViewportClient)
			{
				if (TSharedPtr<SWidget> Widget = GCurrentLevelEditingViewportClient->GetEditorViewportWidget())
				{
					const FGeometry& Geometry = Widget->GetCachedGeometry();
					const FVector2D Position = Geometry.GetAbsolutePosition();
					const FVector2D Size = Geometry.GetAbsoluteSize();
					if (Size.X > 100.0 && Size.Y > 100.0)
					{
						return FSlateRect(Position.X, Position.Y, Position.X + Size.X, Position.Y + Size.Y);
					}
				}
			}
			if (TSharedPtr<SWindow> Root = FGlobalTabmanager::Get()->GetRootWindow())
			{
				return Root->GetRectInScreen();
			}
			return FSlateRect(0.0, 0.0, 1600.0, 900.0);
		}
	}

	namespace
	{
		/**
		 * Off the taskbar (S154g): "perhaps they eat up a windows bar slot.
		 * it shouldnt even show in that bar." A nomad tab's window is made
		 * as an app window; it becomes a tool window of the editor's, which
		 * Windows keeps off the bar. The style only takes on a re-show.
		 */
		void OffTaskbar(const TSharedPtr<SWindow>& Window)
		{
#if PLATFORM_WINDOWS
			TSharedPtr<SWindow> Root = FGlobalTabmanager::Get()->GetRootWindow();
			if (!Window.IsValid() || Window == Root)
			{
				return;
			}
			if (TSharedPtr<FGenericWindow> Native = Window->GetNativeWindow())
			{
				if (HWND Handle = static_cast<HWND>(Native->GetOSWindowHandle()))
				{
					const LONG_PTR Style = GetWindowLongPtr(Handle, GWL_EXSTYLE);
					if (Style & WS_EX_TOOLWINDOW)
					{
						return;
					}
					::ShowWindow(Handle, SW_HIDE);
					SetWindowLongPtr(Handle, GWL_EXSTYLE, (Style & ~WS_EX_APPWINDOW) | WS_EX_TOOLWINDOW);
					::ShowWindow(Handle, SW_SHOW);
				}
			}
#endif
		}
	}

	namespace
	{
		/** The window set at the bottom-right of the viewport, above the ones already standing there. */
		void Place(const TSharedPtr<SWindow>& Window, const FVector2D& Pixels, float Scale)
		{
			const FSlateRect View = ViewportRect();
			const double Gap = 10.0 * Scale;

			// Bottom-right of the viewport, then up: the corner nothing is
			// modelled in, and the windows read as one stack.
			double Right = View.Right - Gap;
			double Bottom = View.Bottom - Gap;
			Placed.RemoveAll([](const TWeakPtr<SWindow>& Each) { return !Each.IsValid(); });
			for (const TWeakPtr<SWindow>& Each : Placed)
			{
				TSharedPtr<SWindow> Standing = Each.Pin();
				if (!Standing.IsValid() || Standing == Window || !Standing->IsVisible())
				{
					continue;
				}
				const FSlateRect Rect = Standing->GetRectInScreen();
				// Only the ones still in the stack's column count.
				if (FMath::Abs(Rect.Right - Right) < Gap * 3.0)
				{
					Bottom = FMath::Min(Bottom, Rect.Top - Gap);
				}
			}
			if (Bottom - Pixels.Y < View.Top + Gap)
			{
				// No room above: a new column to the left, from the bottom again.
				Bottom = View.Bottom - Gap;
				Right -= Pixels.X + Gap;
			}
			const FVector2D Position(FMath::Max(View.Left, Right - Pixels.X), FMath::Max(View.Top, Bottom - Pixels.Y));

			// Resize takes screen pixels.
			Window->Resize(Pixels);
			Window->MoveWindowTo(Position);
		}
	}

	namespace
	{
		/** The strip is the title bar: the OS drags the window by it. */
		class SSurfFrameStrip : public SBorder
		{
		public:
			virtual EWindowZone::Type GetWindowZoneOverride() const override { return EWindowZone::TitleBar; }
		};

		TMap<FName, TWeakPtr<SWindow>> Frames;
	}

	bool TabIsDockedInEditor(const TSharedPtr<SDockTab>& Tab)
	{
		if (!Tab.IsValid())
		{
			return false;
		}
		TSharedPtr<SWindow> Window = Tab->GetParentWindow();
		TSharedPtr<SWindow> Root = FGlobalTabmanager::Get()->GetRootWindow();
		return Window.IsValid() && Window == Root;
	}

	bool IsFrameOpen(FName Id)
	{
		const TWeakPtr<SWindow>* Found = Frames.Find(Id);
		return Found && Found->IsValid();
	}

	void CloseFrame(FName Id)
	{
		if (TWeakPtr<SWindow>* Found = Frames.Find(Id))
		{
			if (TSharedPtr<SWindow> Window = Found->Pin())
			{
				Window->RequestDestroyWindow();
			}
			Frames.Remove(Id);
		}
	}

	void CloseAllFrames()
	{
		TArray<FName> Ids;
		Frames.GetKeys(Ids);
		for (const FName& Id : Ids)
		{
			CloseFrame(Id);
		}
	}

	TSharedPtr<SWindow> OpenFrame(FName Id, const FText& Title, TFunction<TSharedRef<SWidget>()> MakeContent, FVector2D Size)
	{
		if (!FSlateApplication::IsInitialized())
		{
			return nullptr;
		}
		if (TWeakPtr<SWindow>* Found = Frames.Find(Id))
		{
			if (TSharedPtr<SWindow> Open = Found->Pin())
			{
				Open->BringToFront();
				return Open;
			}
		}

		TSharedRef<SWidget> Content = MakeContent();
		TSharedPtr<SWindow> Window = SNew(SWindow)
			.Type(EWindowType::Normal)
			.CreateTitleBar(false)
			.HasCloseButton(false)
			.SupportsMaximize(false)
			.SupportsMinimize(false)
			.SizingRule(ESizingRule::UserSized)
			.FocusWhenFirstShown(false)
			.ClientSize(Size)
			.UserResizeBorder(FMargin(4.0f));

		TWeakPtr<SWindow> WeakWindow = Window;
		Window->SetContent(
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("Brushes.Panel"))
			.Padding(0.0f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SSurfFrameStrip)
					.BorderImage(FAppStyle::GetBrush("Brushes.Header"))
					.Padding(FMargin(8.0f, 0.0f, 4.0f, 0.0f))
					[
						// The frames' title strip (S193d), as the adjust frame's:
						// 24 pixels, the name in the header type.
						SNew(SBox).HeightOverride(24.0f)
						[
							SNew(SHorizontalBox)
							+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
							[
								SNew(STextBlock)
								.Text(Title)
								.Font(SurfTheme::Bold(9))
								.ColorAndOpacity(SurfTheme::Header())
							]
							+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
							[
								SNew(SButton)
								.ButtonStyle(FAppStyle::Get(), "SimpleButton")
								.ContentPadding(FMargin(3.0f, 0.0f))
								.OnClicked_Lambda([Id]() { CloseFrame(Id); return FReply::Handled(); })
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
				+ SVerticalBox::Slot().FillHeight(1.0f)
				[
					Content
				]
			]);

		Window->SetOnWindowClosed(FOnWindowClosed::CreateLambda([Id](const TSharedRef<SWindow>&) { Frames.Remove(Id); }));

		TSharedPtr<SWindow> Root = FGlobalTabmanager::Get()->GetRootWindow();
		if (Root.IsValid())
		{
			FSlateApplication::Get().AddWindowAsNativeChild(Window.ToSharedRef(), Root.ToSharedRef(), /*bShowImmediately*/ true);
		}
		else
		{
			FSlateApplication::Get().AddWindow(Window.ToSharedRef(), true);
		}
		Frames.Add(Id, Window);
		Placed.Add(Window);
		OffTaskbar(Window);

		// Sized for what it holds, a moment later, like the tabs were.
		TWeakPtr<SWidget> WeakContent = Content;
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([WeakWindow, WeakContent, Size](float) -> bool
		{
			TSharedPtr<SWindow> Live = WeakWindow.Pin();
			TSharedPtr<SWidget> Held = WeakContent.Pin();
			if (!Live.IsValid() || !Held.IsValid())
			{
				return false;
			}
			const float Scale = Live->GetDPIScaleFactor();
			const FVector2D Wanted = Held->GetDesiredSize();
			// The strip, the borders and a line of air under the content.
			const double Chrome = 30.0;
			const double Height = Wanted.Y > 1.0 ? FMath::Clamp(Wanted.Y + Chrome, 120.0, Size.Y) : Size.Y;
			Place(Live, FVector2D(Size.X, Height) * Scale, Scale);
			return false;
		}), 0.05f);
		return Window;
	}

	void KeepOffTaskbar(TSharedPtr<SDockTab> Tab)
	{
		TWeakPtr<SDockTab> Weak = Tab;
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Weak](float) -> bool
		{
			if (TSharedPtr<SDockTab> Live = Weak.Pin())
			{
				OffTaskbar(Live->GetParentWindow());
			}
			return false;
		}), 0.2f);
	}

	TSharedPtr<SDockTab> OpenPlaced(FName TabName, FVector2D Size)
	{
		if (!FSlateApplication::IsInitialized())
		{
			return nullptr;
		}
		const bool bWasOpen = FGlobalTabmanager::Get()->FindExistingLiveTab(FTabId(TabName)).IsValid();
		TSharedPtr<SDockTab> Tab = FGlobalTabmanager::Get()->TryInvokeTab(FTabId(TabName));
		if (!Tab.IsValid() || bWasOpen)
		{
			return Tab;
		}

		TSharedPtr<SWindow> Window = Tab->GetParentWindow();
		TSharedPtr<SWindow> Root = FGlobalTabmanager::Get()->GetRootWindow();
		if (!Window.IsValid() || Window == Root)
		{
			// Docked: it is where he docked it.
			return Tab;
		}

		// Sized for what it holds (S154h): "always open up at the right
		// height when possible so user can read all at once". The content
		// has no size until Slate has laid it out once, so the placing is
		// done a moment later, from the measured height; Size.Y is the most
		// it may take.
		Placed.Add(Window);
		OffTaskbar(Window);
		TWeakPtr<SWindow> WeakWindow = Window;
		TWeakPtr<SDockTab> WeakTab = Tab;
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([WeakWindow, WeakTab, Size](float) -> bool
		{
			TSharedPtr<SWindow> Window = WeakWindow.Pin();
			TSharedPtr<SDockTab> Tab = WeakTab.Pin();
			if (!Window.IsValid() || !Tab.IsValid())
			{
				return false;
			}
			const float Scale = Window->GetDPIScaleFactor();
			const FVector2D Wanted = Tab->GetContent()->GetDesiredSize();
			// The tab's own strip and the window's border, on top of the content.
			const double Chrome = 46.0;
			const double Height = Wanted.Y > 1.0 ? FMath::Clamp(Wanted.Y + Chrome, 200.0, Size.Y) : Size.Y;
			const FVector2D Pixels = FVector2D(Size.X, Height) * Scale;
			Place(Window, Pixels, Scale);
			return false;
		}), 0.05f);
		return Tab;
	}


	TSharedRef<SWidget> MenuBarPullDown(const FText& Label, const FText& Tip, TFunction<TSharedRef<SWidget>()> Content)
	{
		return SNew(SComboButton)
			.ButtonStyle(FAppStyle::Get(), "SimpleButton")
			.HasDownArrow(false)
			.ContentPadding(FMargin(7.0f, 2.0f, 6.0f, 2.0f))
			.ToolTipText(Tip)
			.OnGetMenuContent_Lambda([Content]() { return Content(); })
			.ButtonContent()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(STextBlock).Text(Label).Font(SurfTheme::Font(9))
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4.0f, 1.0f, 0.0f, 0.0f)
				[
					SNew(SBox).WidthOverride(8.0f).HeightOverride(8.0f)
					[
						SNew(SImage)
						.Image(FAppStyle::GetBrush("Icons.ChevronDown"))
						.ColorAndOpacity(SurfTheme::Dim())
					]
				]
			];
	}
}
