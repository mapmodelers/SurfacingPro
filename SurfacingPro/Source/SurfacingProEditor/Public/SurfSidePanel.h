// Copyright MapModelers. All Rights Reserved.
//
// The SidePanel (S177a): a panel down the viewport's edge, on the side the
// bar is not, opened and closed with N. Tabs down its outer edge: Item (the
// numbers of what is picked, and the pivot), Paint (the brush), Flags (what
// the bake does with the picked pieces), View (the grid, the backdrop, the
// clay). "Add to it panel you judge worth it... the brushpaint window frame
// could be moved there, the flags too. Theme wise - the flag in-viewport
// frame looks good, that could be the reference."
//
// S178a: a click on the open tab folds the panel to its tabs; Ctrl+click
// pins a tab beside the ones shown, so several stand stacked; a tab drags
// up or down to a new place.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Layout/Visibility.h"

class USurfEdMode;
class SVerticalBox;

enum class ESurfSideTab : uint8
{
	Item,
	Paint,
	Flags,
	View,
	Assets,
	Materials,
	Count
};

namespace SurfSidePanel
{
	/** Open or close it (N). */
	SURFACINGPROEDITOR_API void Toggle();

	/**
	 * Open it on a tab - the mode comes up first when it is not. With
	 * several tabs pinned the tab joins them; otherwise it replaces the one
	 * shown.
	 */
	SURFACINGPROEDITOR_API void Show(ESurfSideTab Tab);

	/** Open it as it was left - or on its first tab when everything was folded. */
	SURFACINGPROEDITOR_API void Open();

	SURFACINGPROEDITOR_API bool IsOpen();
	SURFACINGPROEDITOR_API bool IsShown(ESurfSideTab Tab);
	/** True when the panel is open with no tab shown: only the tabs stand. */
	SURFACINGPROEDITOR_API bool IsFolded();

	/** A click on a tab: the one shown alone, or folded when it already was; with Ctrl, pinned or unpinned. */
	SURFACINGPROEDITOR_API void ClickTab(ESurfSideTab Tab, bool bCtrl);

	/** The tabs top to bottom, as he arranged them. */
	SURFACINGPROEDITOR_API TArray<ESurfSideTab> Order();
	SURFACINGPROEDITOR_API void SetOrder(const TArray<ESurfSideTab>& NewOrder);

	SURFACINGPROEDITOR_API FText TabName(ESurfSideTab Tab);

	/** How wide it stands, tabs included, in slate units - 0 when closed. */
	SURFACINGPROEDITOR_API float Width();
}

class SURFACINGPROEDITOR_API SSurfSidePanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SSurfSidePanel)
		: _TabsOnRight(true)
		{}
		SLATE_ARGUMENT(TWeakObjectPtr<USurfEdMode>, Mode)
		/** True on the viewport's right edge: the tabs on the outside, the grip on the inside. */
		SLATE_ARGUMENT(bool, TabsOnRight)
		SLATE_ATTRIBUTE(FOptionalSize, MaxHeight)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** The grip and the tabs capture the mouse; these finish the drag or the click. */
	virtual FReply OnMouseMove(const FGeometry& Geometry, const FPointerEvent& Event) override;
	virtual FReply OnMouseButtonUp(const FGeometry& Geometry, const FPointerEvent& Event) override;

private:
	/** The tabs and the sections put back in the order the settings hold. */
	void Rearrange();
	TSharedRef<SWidget> BuildTab(ESurfSideTab Tab);
	TSharedRef<SWidget> BuildSection(ESurfSideTab Tab);
	TSharedRef<SWidget> BuildItem();
	TSharedRef<SWidget> BuildView();

	TWeakObjectPtr<USurfEdMode> Mode;
	bool bTabsOnRight = true;

	bool bResizing = false;
	float DragStartX = 0.0f;
	int32 DragStartWidth = 240;

	/** A tab pressed: a click on release, or a drag once it has moved a few pixels. */
	int32 PressedTab = -1;
	bool bPressedCtrl = false;
	bool bTabDragging = false;
	float PressY = 0.0f;

	TSharedPtr<SVerticalBox> TabBox;
	TSharedPtr<SVerticalBox> SectionBox;
	TSharedPtr<SWidget> Tabs[static_cast<int32>(ESurfSideTab::Count)];
	TSharedPtr<SWidget> Sections[static_cast<int32>(ESurfSideTab::Count)];
	FString OrderSeen;
};
