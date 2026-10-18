// Copyright MapModelers. All Rights Reserved.
//
// Compile (S66a, D75). What the mode edits is a preview: a dynamic mesh
// rebuilt on every drag, drawn as many components as there are groups. What
// ships is static meshes - Nanite, with collision from the surface as it is
// drawn - one per group and cell, sections per material, and one per prefab
// asset that every instance of it shares. Bake is explicit, never runs on
// save (rule 3), and is undone by Unbake or by the next edit.

#pragma once

#include "CoreMinimal.h"

class ASurfGroup;
class ASurfPrefabInstance;
class UWorld;

namespace SurfBake
{
	struct FReport
	{
		int32 Groups = 0;
		int32 Instances = 0;
		int32 Meshes = 0;
		int32 Actors = 0;
		int32 Triangles = 0;
		double Seconds = 0.0;

		/**
		 * What would not bake, and why, one line each (S106a): "perhaps see
		 * the progress(es) or issue(s) somewhere still?". A bake that quietly
		 * skips a group is a bake you find out about in the game.
		 */
		TArray<FString> Problems;

		/** Meshes of earlier bakes of the same groups that nothing stands on any more, deleted (S153c). */
		int32 Tidied = 0;
	};

	/**
	 * A bake in progress (S106a).
	 *
	 * "bake can we have some background bake option on?" - a static mesh can
	 * only be built on the game thread, so there is no other thread to put
	 * this on. What there is is the gap between two frames: the work is cut
	 * into slices, a slice runs each tick until its budget is spent, and the
	 * editor draws in between. That is what background means from the chair.
	 *
	 * The actors are weak: a group deleted or edited while its turn is still
	 * coming is skipped rather than crashed into.
	 */
	struct FJob
	{
		TArray<TWeakObjectPtr<ASurfGroup>> Groups;
		TArray<TWeakObjectPtr<ASurfPrefabInstance>> Instances;

		/** How far through Groups then Instances, as one list. */
		int32 Next = 0;

		FReport Report;
		double Started = 0.0;
		bool bStopped = false;

		int32 Total() const { return Groups.Num() + Instances.Num(); }
		int32 Done() const { return FMath::Clamp(Next, 0, Total()); }
		bool IsFinished() const { return bStopped || Next >= Total(); }
	};

	/** Everything a level bake will touch, in the order it will be done. With bSkipStanding, what is already baked and standing is left alone (S153a); with OnlyLevel, only that level's (S153c: a scene about to be kept). */
	TSharedRef<FJob> PlanLevelBake(UWorld* World, bool bSkipStanding = false, const ULevel* OnlyLevel = nullptr);

	/**
	 * One slice. Works until the budget is spent or the job is done, and
	 * answers true while there is more to do. A budget of zero or less runs
	 * the whole thing, which is what the foreground bake is.
	 */
	bool StepBake(FJob& Job, double BudgetSeconds);

	/** Bake one group: its preview split by cell, a static mesh per cell, an actor per mesh. */
	bool BakeGroup(ASurfGroup& Group, FReport& Report);

	/** Bake one instance: its asset's mesh (shared with every other instance of the asset), one actor. */
	bool BakeInstance(ASurfPrefabInstance& Instance, FReport& Report);

	/** Every group and every instance in the level; with bSkipStanding, only what is not already baked; with OnlyLevel, only that level's. */
	FReport BakeLevel(UWorld* World, bool bSkipStanding = false, const ULevel* OnlyLevel = nullptr);

	/** Every baked group and instance back to its preview. The assets stay. */
	int32 UnbakeLevel(UWorld* World);
}
