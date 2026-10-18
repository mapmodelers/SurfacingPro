// Copyright MapModelers. All Rights Reserved.

#include "SurfSettings.h"

USurfSettings::USurfSettings()
{
	GridLadderCm = { 1.0, 5.0, 10.0, 25.0, 50.0, 100.0, 200.0, 500.0 };
	DefaultGridIndex = 5; // 100 cm
}

void USurfSettings::PostInitProperties()
{
	Super::PostInitProperties();

	// The config is loaded by now. Defaults that changed on purpose are
	// moved here, once: a saved config carries every property, so a new
	// default never reaches it otherwise.
	if (HasAnyFlags(RF_ClassDefaultObject) && SettingsRevision < 1)
	{
		// S56p: the edge pick tolerance, from the session records - six
		// clicks in a row missed edges by 24 to 28 pixels.
		if (EdgePickRadiusPx < 28.0f)
		{
			EdgePickRadiusPx = 28.0f;
		}
		SettingsRevision = 1;
	}
	if (HasAnyFlags(RF_ClassDefaultObject) && SettingsRevision < 2)
	{
		// S58: the gizmo's idle opacity, from the author's own setting.
		if (FMath::IsNearlyEqual(OwnGizmoIdleOpacity, 0.4f, 0.001f))
		{
			OwnGizmoIdleOpacity = 0.2f;
		}
		SettingsRevision = 2;
	}
	if (HasAnyFlags(RF_ClassDefaultObject) && SettingsRevision < 3)
	{
		// S245a: the line pick reach, from the record again - 67 clicks
		// landed 29 to 80 px from the nearest line at 28.
		if (EdgePickRadiusPx < 40.0f)
		{
			EdgePickRadiusPx = 40.0f;
		}
		SettingsRevision = 3;
	}
	if (HasAnyFlags(RF_ClassDefaultObject) && SettingsRevision < 4)
	{
		// S246c: a size under the grid step was never the size made (the
		// knob's floor was 10 cm, the patch came out a metre); the floor is
		// a metre now and a saved 10 goes back to the default.
		if (NewPatchSizeCm < 100.0)
		{
			NewPatchSizeCm = 1000.0;
		}
		SettingsRevision = 4;
	}
}

void FSurfPaintSet::Singles(TArray<ESurfPaintChannel, TInlineAllocator<4>>& Out) const
{
	Out.Reset();
	const ESurfPaintChannel Order[4] = { ESurfPaintChannel::R, ESurfPaintChannel::G, ESurfPaintChannel::B, ESurfPaintChannel::A };
	for (int32 I = IsColour() ? 3 : 0; I < 4; ++I)
	{
		if (Bits & (1 << I))
		{
			Out.Add(Order[I]);
		}
	}
}

void FSurfPaintSet::All(TArray<ESurfPaintChannel, TInlineAllocator<4>>& Out) const
{
	Out.Reset();
	const ESurfPaintChannel Order[4] = { ESurfPaintChannel::R, ESurfPaintChannel::G, ESurfPaintChannel::B, ESurfPaintChannel::A };
	for (int32 I = 0; I < 4; ++I)
	{
		if (Bits & (1 << I))
		{
			Out.Add(Order[I]);
		}
	}
	if (Out.Num() == 0)
	{
		Out.Add(ESurfPaintChannel::R);
	}
}

int32 FSurfPaintSet::ViewIndex() const
{
	if (IsColour())
	{
		return 4;
	}
	int32 Count = 0;
	int32 First = 0;
	for (int32 I = 3; I >= 0; --I)
	{
		if (Bits & (1 << I))
		{
			++Count;
			First = I;
		}
	}
	return Count == 1 ? First : (Count == 0 ? 0 : 4);
}

FString FSurfPaintSet::Name() const
{
	if (IsColour())
	{
		return (Bits & 8) ? TEXT("colour+A") : TEXT("colour");
	}
	static const TCHAR* Letters[4] = { TEXT("R"), TEXT("G"), TEXT("B"), TEXT("A") };
	FString Out;
	for (int32 I = 0; I < 4; ++I)
	{
		if (Bits & (1 << I))
		{
			if (!Out.IsEmpty())
			{
				Out += TEXT("+");
			}
			Out += Letters[I];
		}
	}
	return Out.IsEmpty() ? FString(TEXT("R")) : Out;
}

FSurfPaintSet USurfSettings::PaintSet() const
{
	FSurfPaintSet Set;
	if (PaintChannels & 15)
	{
		Set.Bits = PaintChannels & 15;
	}
	else
	{
		Set.Bits = PaintChannel == ESurfPaintChannel::Colour ? 7 : 1 << FMath::Clamp(static_cast<int32>(PaintChannel), 0, 3);
	}
	return Set;
}

void USurfSettings::SetPaintSet(int32 Bits)
{
	Bits &= 15;
	if (Bits == 0)
	{
		Bits = PaintSet().Bits;
	}
	PaintChannels = Bits;
	if ((Bits & 7) == 7)
	{
		PaintChannel = ESurfPaintChannel::Colour;
		return;
	}
	for (int32 I = 0; I < 4; ++I)
	{
		if (Bits & (1 << I))
		{
			PaintChannel = static_cast<ESurfPaintChannel>(I);
			return;
		}
	}
}

double USurfSettings::GetGridStepCm(int32 Index) const
{
	if (GridLadderCm.Num() == 0)
	{
		return 1.0;
	}

	const int32 Clamped = FMath::Clamp(Index, 0, GridLadderCm.Num() - 1);
	const double Step = GridLadderCm[Clamped];
	return Step > 0.0 ? Step : 1.0;
}

int32 USurfSettings::StepGridIndex(int32 Index, int32 Delta) const
{
	if (GridLadderCm.Num() == 0)
	{
		return 0;
	}

	return FMath::Clamp(Index + Delta, 0, GridLadderCm.Num() - 1);
}

double USurfSettings::SnapToStep(double Value, double StepCm)
{
	if (StepCm <= 0.0)
	{
		return Value;
	}

	return FMath::RoundToDouble(Value / StepCm) * StepCm;
}

FVector USurfSettings::SnapToStep(const FVector& Value, double StepCm)
{
	if (StepCm <= 0.0)
	{
		return Value;
	}

	return FVector(
		SnapToStep(Value.X, StepCm),
		SnapToStep(Value.Y, StepCm),
		SnapToStep(Value.Z, StepCm));
}

double USurfSettings::Falloff(ESurfFalloff Shape, double NormalisedDistance, int32 Seed)
{
	// Distance comes in as 0 at the selected point and 1 at the radius; the
	// curves are written on the amount that remains, which is how they read
	// when you look at the surface.
	const double T = 1.0 - FMath::Clamp(NormalisedDistance, 0.0, 1.0);

	switch (Shape)
	{
	case ESurfFalloff::Sphere:   return FMath::Sqrt(FMath::Max(0.0, 1.0 - (1.0 - T) * (1.0 - T)));
	case ESurfFalloff::Root:     return FMath::Sqrt(T);
	case ESurfFalloff::Sharp:    return T * T;
	case ESurfFalloff::Linear:   return T;
	case ESurfFalloff::Constant: return T > 0.0 ? 1.0 : 0.0;

	// Blender's inverse square: 1 - (1 - T)^2 read the other way - most of
	// the fall near the selection, a long tail to the radius.
	case ESurfFalloff::InverseSquare: return T * (2.0 - T);

	// A fixed share per point, from its seed, of a linear falloff: the same
	// point always follows by the same amount, so a drag reads as a rough
	// surface and not as noise that changes under the hand.
	case ESurfFalloff::Random:
	{
		const double Hash = FMath::Frac(FMath::Sin(static_cast<double>(Seed) * 12.9898 + 78.233) * 43758.5453);
		return T * Hash;
	}

	case ESurfFalloff::Smooth:
	default:
		// Smoothstep: flat at both ends, so a drag makes a hill with no crease
		// at the selection and no edge at the radius.
		return T * T * (3.0 - 2.0 * T);
	}
}

FString USurfSettings::FalloffName(ESurfFalloff Shape)
{
	switch (Shape)
	{
	case ESurfFalloff::Sphere:        return TEXT("sphere");
	case ESurfFalloff::Root:          return TEXT("root");
	case ESurfFalloff::Sharp:         return TEXT("sharp");
	case ESurfFalloff::Linear:        return TEXT("linear");
	case ESurfFalloff::Constant:      return TEXT("constant");
	case ESurfFalloff::InverseSquare: return TEXT("inverse square");
	case ESurfFalloff::Random:        return TEXT("random");
	case ESurfFalloff::Count:         return TEXT("smooth");
	case ESurfFalloff::Smooth:
	default:                          return TEXT("smooth");
	}
}

float USurfSettings::AdjustPaintValue(const USurfSettings& S, int32 Kind, float Was, float Mean, float Cavity)
{
	auto Blend = [&S](float A, float New)
	{
		switch (S.MaskBlend)
		{
		case ESurfMaskBlend::Add:      return FMath::Clamp(A + New, 0.0f, 1.0f);
		case ESurfMaskBlend::Subtract: return FMath::Clamp(A - New, 0.0f, 1.0f);
		case ESurfMaskBlend::Multiply: return A * New;
		case ESurfMaskBlend::Lighten:  return FMath::Max(A, New);
		case ESurfMaskBlend::Darken:   return FMath::Min(A, New);
		default:                       return New;
		}
	};
	switch (Kind)
	{
	case PaintAdjustCavity:
		return FMath::Clamp(FMath::Lerp(Was, Blend(Was, Cavity), FMath::Clamp(S.PaintCavityFactor, 0.0f, 1.0f)), 0.0f, 1.0f);
	case PaintAdjustSmooth:
		return FMath::Clamp(FMath::Lerp(Was, Mean, FMath::Clamp(S.PaintSmoothAmount, 0.0f, 1.0f)), 0.0f, 1.0f);
	case PaintAdjustContrast:
		return FMath::Clamp(0.5f + (Was - 0.5f) * (1.0f + FMath::Clamp(S.PaintContrast, -1.0f, 4.0f)), 0.0f, 1.0f);
	case PaintAdjustLevels:
	{
		const float Low = FMath::Clamp(S.PaintLevelsInLow, 0.0f, 1.0f);
		const float High = FMath::Max(Low + 0.001f, FMath::Clamp(S.PaintLevelsInHigh, 0.0f, 1.0f));
		float T = FMath::Clamp((Was - Low) / (High - Low), 0.0f, 1.0f);
		T = FMath::Pow(T, 1.0f / FMath::Clamp(S.PaintLevelsGamma, 0.1f, 10.0f));
		return FMath::Clamp(FMath::Lerp(FMath::Clamp(S.PaintLevelsOutLow, 0.0f, 1.0f), FMath::Clamp(S.PaintLevelsOutHigh, 0.0f, 1.0f), T), 0.0f, 1.0f);
	}
	case PaintAdjustInvert:
		return 1.0f - FMath::Clamp(Was, 0.0f, 1.0f);
	default:
		return Was;
	}
}
