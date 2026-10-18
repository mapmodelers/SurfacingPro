// Copyright MapModelers. All Rights Reserved.
//
// The Notes window (S153f): every line the tool has said this session -
// what each verb did, or why it would not - kept in order, newest at the
// bottom, in a tab that docks and floats like the rest. The corner shows
// one line for a few seconds; this is where a line that went by is read.

#pragma once

#include "CoreMinimal.h"

namespace SurfNotesTab
{
	extern const FName TabName;

	void Register();
	void Unregister();

	/** Open it, or bring it forward. */
	void Open();
}
