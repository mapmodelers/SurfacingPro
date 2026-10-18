// Copyright MapModelers. All Rights Reserved.

#include "SurfKeymap.h"
#include "SurfSettings.h"

namespace SurfKeymap
{
namespace
{
	FSurfKeyBinding Bind(ESurfAction Action, const FKey& Key, bool bCtrl = false, bool bShift = false, bool bAlt = false)
	{
		FSurfKeyBinding Binding;
		Binding.Action = Action;
		Binding.Key = Key;
		Binding.bCtrl = bCtrl;
		Binding.bShift = bShift;
		Binding.bAlt = bAlt;
		return Binding;
	}
}

FSurfKeyBinding Default(ESurfKeyScheme Scheme, ESurfAction Action)
{
	// Shared by both schemes. Sub-modes count 1 2 3 4 in Blender's order -
	// vertex, edge, face - with object on the end, because that is the order
	// the hand already knows. Focus is F because that is what Unreal has
	// trained every user of this editor to press.
	switch (Action)
	{
	case ESurfAction::SubModeVertex:   return Bind(Action, EKeys::One);
	case ESurfAction::SubModeEdge:     return Bind(Action, EKeys::Two);
	case ESurfAction::SubModeFace:     return Bind(Action, EKeys::Three);
	case ESurfAction::SubModeObject:   return Bind(Action, EKeys::Four);
	case ESurfAction::FocusSelection:  return Bind(Action, EKeys::F);

	// Slide had the second tap of G until that became the axis cycle.
	case ESurfAction::SlideEdge:       return Bind(Action, EKeys::G, false, /*Shift*/ true);
	// Blender's Shift+N, and N is free in the level viewport.
	case ESurfAction::FaceOutward:     return Bind(Action, EKeys::N, false, /*Shift*/ true);
	case ESurfAction::Delete:          return Bind(Action, EKeys::Delete);

	// M is merge in Blender and free in the level viewport.
	case ESurfAction::Weld:            return Bind(Action, EKeys::M);

	// Bracket-like keys next to each other, and free in both schemes: which
	// patch a verb is about to hit is worth being able to change quickly.
	case ESurfAction::PreviousPatch:   return Bind(Action, EKeys::Comma);
	case ESurfAction::NextPatch:       return Bind(Action, EKeys::Period);

	// Space puts the camera where you are pointing. It is the one navigation
	// key worth taking: the editor's own use of it (cycling the gizmo) is
	// covered by W, E and R.
	case ESurfAction::TeleportToCursor: return Bind(Action, EKeys::SpaceBar);

	// Ctrl+B is bevel in Blender. Unreal's Ctrl+B is "find in content browser",
	// which nobody reaches for with a control point selected.
	case ESurfAction::Bevel:           return Bind(Action, EKeys::B, /*Ctrl*/ true);

	// Z is the shading pie in Blender; here it walks the wire views. Free in
	// the level viewport.
	case ESurfAction::CycleWireView:   return Bind(Action, EKeys::Z);
	case ESurfAction::InsertLoop:      return Bind(Action, EKeys::R, /*Ctrl*/ true);
	case ESurfAction::ConvertToSheet:  return Bind(Action, EKeys::Enter, /*Ctrl*/ true, /*Shift*/ true);

	// O is proportional editing in Blender, and free in the level viewport, so
	// both schemes can have it. The radius keys sit next to each other under
	// the same hand; the wheel is the viewport's and is not taken.
	case ESurfAction::ToggleSoftSelection: return Bind(Action, EKeys::O);
	case ESurfAction::SoftRadiusDown:      return Bind(Action, EKeys::LeftBracket);
	case ESurfAction::SoftRadiusUp:        return Bind(Action, EKeys::RightBracket);

	// The snap reach under the same two keys with Shift: the other radius
	// the hand sets while dragging, beside the first.
	case ESurfAction::SnapReachDown:       return Bind(Action, EKeys::LeftBracket, false, /*Shift*/ true);
	case ESurfAction::SnapReachUp:         return Bind(Action, EKeys::RightBracket, false, /*Shift*/ true);

	// Shift+D is duplicate in Blender and free in the level viewport; the
	// editor's own Ctrl+D and Ctrl+W reach the same verb through the toolkit.
	case ESurfAction::Duplicate:           return Bind(Action, EKeys::D, false, /*Shift*/ true);

	// Shift+Z is Blender's wireframe toggle; here it is the modelling view
	// (unlit) against the shaded one. Shift+O cycles the proportional
	// editing falloff, as in Blender. X is delete in Blender, but a search
	// at the cursor is what the hand wants there more than a second Delete.
	case ESurfAction::ToggleShadedView:    return Bind(Action, EKeys::Z, false, /*Shift*/ true);
	case ESurfAction::CycleSoftFalloff:    return Bind(Action, EKeys::O, false, /*Shift*/ true);
	case ESurfAction::Search:              return Bind(Action, EKeys::X);

	// Blender's Ctrl+Space maximises the area; the editor's Ctrl+Space is
	// the content drawer, so Shift+Space, which is free.
	case ESurfAction::ToggleMaximizeViewport: return Bind(Action, EKeys::SpaceBar, false, /*Shift*/ true);
	case ESurfAction::FocusAllViewports:      return Bind(Action, EKeys::F, false, /*Shift*/ true);
	case ESurfAction::ToggleFourViews:        return Bind(Action, EKeys::Q, /*Ctrl*/ true, false, /*Alt*/ true);

	// Hiding is Blender's in both schemes; nothing else wanted H.
	case ESurfAction::HideSelected:           return Bind(Action, EKeys::H);
	case ESurfAction::HideUnselected:         return Bind(Action, EKeys::H, false, /*Shift*/ true);
	case ESurfAction::UnhideAll:              return Bind(Action, EKeys::H, false, false, /*Alt*/ true);

	// Blender's local view is the numpad's slash, in both schemes.
	case ESurfAction::IsolateSelection:       return Bind(Action, EKeys::Divide);

	// Blender's repeat-last, and R is free with Shift in both schemes.
	case ESurfAction::RepeatLastVerb:         return Bind(Action, EKeys::R, false, /*Shift*/ true);

	// Blender's Quick Favorites key; Q alone is free in both schemes.
	case ESurfAction::QuickFavorites:         return Bind(Action, EKeys::Q);

	// Grow and shrink the selection along the grid, on Blender's keys.
	case ESurfAction::GrowSelection:          return Bind(Action, EKeys::Add, /*Ctrl*/ true);
	case ESurfAction::ShrinkSelection:        return Bind(Action, EKeys::Subtract, /*Ctrl*/ true);

	// Select linked, Blender's key, free in both schemes.
	case ESurfAction::SelectLinked:           return Bind(Action, EKeys::L, /*Ctrl*/ true);

	// Object / Edit, Blender's Tab, in both schemes.
	case ESurfAction::ToggleObjectEdit:       return Bind(Action, EKeys::Tab);

	// The SidePanel, Blender's N; Shift+N stays Face outward.
	case ESurfAction::ToggleSidePanel:        return Bind(Action, EKeys::N);

	// Snapping on and off (S198a), Shift+Tab in both schemes; Tab is Object / Edit.
	case ESurfAction::ToggleSnap:             return Bind(Action, EKeys::Tab, false, /*Shift*/ true);

	// The overlay (S199a), Alt+Z beside Z (wires) and Shift+Z (shaded), both schemes.
	case ESurfAction::ToggleOverlay:          return Bind(Action, EKeys::Z, false, false, /*Alt*/ true);

	// Groups (S249b): Ctrl+G as the editor groups actors; Ctrl+Alt+G takes
	// one level off, Shift+G being Slide. Both schemes.
	case ESurfAction::GroupPieces:            return Bind(Action, EKeys::G, /*Ctrl*/ true);
	case ESurfAction::UngroupPieces:          return Bind(Action, EKeys::G, /*Ctrl*/ true, false, /*Alt*/ true);

	// B alone is box select, Blender's own key for it; the brush that paints
	// a layer in takes B with Shift, which is free in both schemes.
	case ESurfAction::PaintWeight:            return Bind(Action, EKeys::B, false, /*Shift*/ true);

	// Down into a prefab and up out of it, one step of the nesting each,
	// under the two keys that already mean down and up; Alt keeps them
	// from the editor's own. Free in both schemes.
	case ESurfAction::PrefabIn:               return Bind(Action, EKeys::PageDown, false, false, /*Alt*/ true);
	case ESurfAction::PrefabOut:              return Bind(Action, EKeys::PageUp, false, false, /*Alt*/ true);
	default: break;
	}

	if (Scheme == ESurfKeyScheme::Blender)
	{
		switch (Action)
		{
		case ESurfAction::SelectAll:       return Bind(Action, EKeys::A);
		case ESurfAction::SelectNone:      return Bind(Action, EKeys::A, false, false, /*Alt*/ true);
		case ESurfAction::InvertSelection: return Bind(Action, EKeys::I, /*Ctrl*/ true);
		case ESurfAction::BoxSelect:       return Bind(Action, EKeys::B);
		case ESurfAction::LassoSelect:     return Bind(Action, EKeys::L);
		// No key for the brush on its own (S158d): "don't need C for brush
		// select since W already cycles through it."
		case ESurfAction::BrushSelect:     return FSurfKeyBinding();
		case ESurfAction::CycleSelectTool: return Bind(Action, EKeys::W);

		// Blender's own key for it, and free here.
		case ESurfAction::Separate:        return Bind(Action, EKeys::P);
		case ESurfAction::Translate:       return Bind(Action, EKeys::G);
		case ESurfAction::Rotate:          return Bind(Action, EKeys::R);
		case ESurfAction::Scale:           return Bind(Action, EKeys::S);

		// C is circle select here, so the knife key takes the slice.
		case ESurfAction::Slice:           return Bind(Action, EKeys::K);
		case ESurfAction::Extrude:         return Bind(Action, EKeys::E);
		default: break;
		}

		return FSurfKeyBinding();
	}

	switch (Action)
	{
	// Not Ctrl+A: that is the level editor's Select All, registered as a
	// global command, and a global command is handled before a viewport mode
	// ever sees the key. Pressing it selected every actor in the outliner.
	case ESurfAction::SelectAll:       return Bind(Action, EKeys::A);
	case ESurfAction::SelectNone:      return Bind(Action, EKeys::D, /*Ctrl*/ true);
	case ESurfAction::InvertSelection: return Bind(Action, EKeys::I, /*Ctrl*/ true);
	case ESurfAction::BoxSelect:       return Bind(Action, EKeys::B);
	case ESurfAction::LassoSelect:     return Bind(Action, EKeys::L);
	case ESurfAction::BrushSelect:     return Bind(Action, EKeys::P);
	case ESurfAction::CycleSelectTool: return Bind(Action, EKeys::W, false, /*Shift*/ true);

	// P is the brush in this scheme, so separate takes the shifted key.
	case ESurfAction::Separate:        return Bind(Action, EKeys::P, false, /*Shift*/ true);

	// Unreal's own W/E/R, so the house scheme does not fight the editor.
	case ESurfAction::Translate:       return Bind(Action, EKeys::W);
	case ESurfAction::Rotate:          return Bind(Action, EKeys::E);
	case ESurfAction::Scale:           return Bind(Action, EKeys::R);

	case ESurfAction::Slice:           return Bind(Action, EKeys::C);

	// E is the rotate gizmo in this scheme, so extrude takes the shifted key.
	case ESurfAction::Extrude:         return Bind(Action, EKeys::E, false, /*Shift*/ true);
	default: break;
	}

	return FSurfKeyBinding();
}

FSurfKeyBinding Resolve(ESurfAction Action)
{
	const USurfSettings* Settings = GetDefault<USurfSettings>();
	if (!Settings)
	{
		return Default(ESurfKeyScheme::SurfacingPro, Action);
	}

	// A user override beats the scheme, so one awkward key can be moved
	// without abandoning the layout it came from.
	for (const FSurfKeyBinding& Override : Settings->KeyOverrides)
	{
		if (Override.Action == Action && Override.Key.IsValid())
		{
			return Override;
		}
	}

	return Default(Settings->KeyScheme, Action);
}

ESurfAction Match(const FKey& Key, bool bCtrl, bool bShift, bool bAlt)
{
	if (!Key.IsValid())
	{
		return ESurfAction::None;
	}

	// Most specific first: a chord with modifiers must win over the bare key,
	// or Ctrl+R would fire whatever R alone is bound to.
	ESurfAction Best = ESurfAction::None;
	int32 BestModifiers = -1;

	for (int32 I = 1; I < static_cast<int32>(ESurfAction::Count); ++I)
	{
		const ESurfAction Action = static_cast<ESurfAction>(I);
		const FSurfKeyBinding Binding = Resolve(Action);

		if (!Binding.Key.IsValid() || Binding.Key != Key)
		{
			continue;
		}

		if (Binding.bCtrl != bCtrl || Binding.bShift != bShift || Binding.bAlt != bAlt)
		{
			continue;
		}

		const int32 Modifiers = (Binding.bCtrl ? 1 : 0) + (Binding.bShift ? 1 : 0) + (Binding.bAlt ? 1 : 0);
		if (Modifiers > BestModifiers)
		{
			BestModifiers = Modifiers;
			Best = Action;
		}
	}

	return Best;
}

FString Describe(ESurfAction Action)
{
	const FSurfKeyBinding Binding = Resolve(Action);
	if (!Binding.Key.IsValid())
	{
		return TEXT("unbound");
	}

	FString Text;
	if (Binding.bCtrl)  { Text += TEXT("Ctrl+"); }
	if (Binding.bShift) { Text += TEXT("Shift+"); }
	if (Binding.bAlt)   { Text += TEXT("Alt+"); }

	Text += Binding.Key.GetDisplayName(false).ToString();
	return Text;
}

FString ActionName(ESurfAction Action)
{
	const UEnum* Enum = StaticEnum<ESurfAction>();
	return Enum ? Enum->GetNameStringByValue(static_cast<int64>(Action)) : TEXT("None");
}

} // namespace SurfKeymap
