// Copyright MapModelers. All Rights Reserved.
//
// Snapping as the hand has it right now (S198a): the Snap switch, flipped
// while Ctrl is held - "Flips snapping". Every snap of ours asks here first;
// the editor's own move, rotation and scale steps flip with Ctrl too.

#pragma once

#include "CoreMinimal.h"

namespace SurfSnapState
{
	/** Ctrl is held, and not because the wheel is sizing the snap reach. */
	bool CtrlFlip();

	/** Our snapping (the targets in the snap menu) is on: the switch, flipped by Ctrl. */
	bool On();

	/** One of the editor's own toggles (grid, rotation grid, scale grid), flipped by Ctrl. */
	bool Flipped(bool bEditorToggle);

	/** Ctrl+wheel just sized the reach: Ctrl does not flip snapping for a moment after. */
	void NoteReachWheel();

	/**
	 * What moves, as the snap measures it (S205a): every point for Closest,
	 * else the one point Snap with names - the box's middle, the average,
	 * or the last point (the one picked last, when the list is in picking
	 * order).
	 */
	void ReduceForSnapWith(TArray<FVector>& Points);

	/** The foot of the perpendicular from From to the segment A-B (S200a); false when it falls off the segment or the segment has no length. */
	bool PerpendicularFoot(const FVector& From, const FVector& A, const FVector& B, FVector& OutFoot);
}
