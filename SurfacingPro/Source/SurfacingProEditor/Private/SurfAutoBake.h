// Copyright MapModelers. All Rights Reserved.
//
// The bake that happens by itself (S153a): "Automatic bake sound good. with
// toggle on off option? Default is off?" Before Play and before a save,
// when the level's own tick is on (S153b, kept on the level), whatever is
// not already baked and standing is baked - every group and prefab instance
// the world holds - and what the bake made is saved with the level. The
// editor never draws a bake; Game View shows it.

#pragma once

#include "CoreMinimal.h"

class ULevel;
class UWorld;

namespace SurfAutoBake
{
	void Register();
	void Unregister();

	/**
	 * A scene about to be kept (S153c): "i guess it should, right?" - its
	 * groups baked, if the level it is edited in has the tick, before the
	 * edit is committed and the scene written.
	 */
	void BeforeSceneKept(UWorld* EditorWorld, const ULevel* SceneLevel);

	/** Whatever is not standing, baked now if the level bakes by itself (S154d: before Game View shows the level). Answers how many meshes were made. */
	int32 BeforeView(UWorld* EditorWorld);

	/** The editor shows the bake side, or the previews, on every group and instance (S154e). */
	void ShowBake(UWorld* EditorWorld, bool bShow);
}
