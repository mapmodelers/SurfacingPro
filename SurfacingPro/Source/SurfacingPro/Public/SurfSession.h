// Copyright MapModelers. All Rights Reserved.
//
// A recording of what actually happened in a session.
//
// The author does not read logs and should not have to describe a workflow
// problem in prose - "trying to rotate the camera while one is selected moves
// it" took a paragraph to say and is one line here. So the mode writes every
// key, verb, selection change and gesture to a flat file with timings, and
// that file is read on the other side.
//
// One line per event, tab separated, in a stable order: elapsed seconds, a
// kind, a name, then free-form detail. Grep-friendly on purpose.
//
// This is diagnostics, not telemetry: it is local, it is off unless the
// setting is on, and nothing leaves the machine.

#pragma once

#include "CoreMinimal.h"

namespace SurfSession
{
	/** Opens the file for this run. Called when the mode is entered. */
	SURFACINGPRO_API void Begin();

	/** Flushes and closes. Called when the mode is exited. */
	SURFACINGPRO_API void End();

	SURFACINGPRO_API bool IsRecording();

	/** A bound action fired, with the chord that fired it. */
	SURFACINGPRO_API void Key(const FString& ActionName, const FString& Chord);

	/** A panel button ran. */
	SURFACINGPRO_API void Verb(const FString& Name, const FString& Detail = FString());

	/** What is selected changed, and to what. */
	SURFACINGPRO_API void Selection(const FString& Detail);

	/** A drag or gesture started and ended, with how long it took. */
	SURFACINGPRO_API void Gesture(const FString& Name, const FString& Detail = FString());

	/** Something the user probably did not intend. The interesting lines. */
	SURFACINGPRO_API void Friction(const FString& Name, const FString& Detail = FString());

	/**
	 * A note typed by the user: their own words on what they see, in context.
	 *
	 * A note is followed by two lines it writes for itself (S117a):
	 *
	 *   note_before  the last dozen things that happened, newest last
	 *   note_state   where the tool was standing when he wrote it
	 *
	 * "improve logging of notes if needed. so you know more about the context
	 * etc. read what i was doing before/after etc." **A note that says
	 * "selecting objects in prefab edit mode doesnt work" is a sentence; the
	 * same note with the twelve lines before it is a reproduction.** What
	 * happened after is already in the file, immediately below.
	 */
	SURFACINGPRO_API void Note(const FString& Text);

	/**
	 * Where the tool is standing, in one line, for the note to carry.
	 *
	 * The recorder is in the runtime module and knows nothing about modes,
	 * sub-modes or selections. The editor binds this when it comes up and
	 * clears it when it goes; unbound, a note simply carries no state line.
	 */
	SURFACINGPRO_API extern TDelegate<FString()> NoteStateProvider;

	/**
	 * A burst: a knob being dragged replays an operation many times a
	 * second, and every replay wrote three lines - the record was mostly
	 * that. Between BeginBurst and EndBurst, verb and selection lines are
	 * counted rather than written; EndBurst writes one line with the count.
	 * Keys, gestures, frictions and notes are written as ever, and a key or
	 * gesture ends a burst on its own, so nothing is lost if the end is
	 * never reached. Nested begins are one burst.
	 */
	SURFACINGPRO_API void BeginBurst(const FString& Name);
	SURFACINGPRO_API void EndBurst();

	/** Where the file is, for the digest to pick up. */
	SURFACINGPRO_API FString CurrentPath();
}
