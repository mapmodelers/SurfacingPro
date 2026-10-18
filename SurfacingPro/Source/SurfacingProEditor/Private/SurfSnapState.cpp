// Copyright MapModelers. All Rights Reserved.

#include "SurfSnapState.h"
#include "SurfSettings.h"

#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformTime.h"

namespace SurfSnapState
{
	namespace
	{
		double GReachWheelAt = -100.0;
	}

	bool CtrlFlip()
	{
		// The tests read no hand on the keyboard (S202a).
		if (!FSlateApplication::IsInitialized() || GIsAutomationTesting)
		{
			return false;
		}
		if (!FSlateApplication::Get().GetModifierKeys().IsControlDown())
		{
			return false;
		}
		// Holding Ctrl to wheel the reach is not asking for the snap to flip.
		return FPlatformTime::Seconds() - GReachWheelAt > 0.8;
	}

	bool On()
	{
		const USurfSettings* Settings = GetDefault<USurfSettings>();
		const bool bSwitch = !Settings || Settings->bSnapEnabled;
		return bSwitch != CtrlFlip();
	}

	bool Flipped(bool bEditorToggle)
	{
		return bEditorToggle != CtrlFlip();
	}

	void NoteReachWheel()
	{
		GReachWheelAt = FPlatformTime::Seconds();
	}

	void ReduceForSnapWith(TArray<FVector>& Points)
	{
		const USurfSettings* Settings = GetDefault<USurfSettings>();
		if (!Settings || Points.Num() <= 1 || Settings->SnapWith == ESurfSnapWith::Closest)
		{
			return;
		}
		FVector One = Points.Last();
		if (Settings->SnapWith == ESurfSnapWith::Centre)
		{
			One = FBox(Points).GetCenter();
		}
		else if (Settings->SnapWith == ESurfSnapWith::Median)
		{
			FVector Sum = FVector::ZeroVector;
			for (const FVector& P : Points)
			{
				Sum += P;
			}
			One = Sum / static_cast<double>(Points.Num());
		}
		Points.Reset();
		Points.Add(One);
	}

	bool PerpendicularFoot(const FVector& From, const FVector& A, const FVector& B, FVector& OutFoot)
	{
		const FVector Run = B - A;
		const double Length2 = Run.SizeSquared();
		if (Length2 < 1.0e-6)
		{
			return false;
		}
		const double T = FVector::DotProduct(From - A, Run) / Length2;
		if (T < 0.0 || T > 1.0)
		{
			return false;
		}
		OutFoot = A + Run * T;
		return true;
	}
}
