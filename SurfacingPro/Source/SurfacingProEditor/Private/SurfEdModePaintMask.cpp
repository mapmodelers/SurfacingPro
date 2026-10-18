// Copyright MapModelers. All Rights Reserved.

/**
 * Masks and cavity for the paint (S212a): "Mask From Cavity / like blender's
 * sculpt mode does. so it masks some area and user can paint/fill the rest.
 * can invert mask too ... a photoshop like dual color picker ... a toggle so
 * the Masking is either per-object mask, either some brush-auto-mask".
 *
 * The cavity of a control point: how far its neighbours sit in front of it
 * along its normal - a dent reads high, a ridge low, a flat run half.
 * Smooth averages a ring or two in; Level is the gain about flat. A mask of
 * 1 keeps the brush off a point.
 */

#include "SurfEdMode.h"
#include "SurfGroup.h"
#include "SurfInspector.h"
#include "SurfSession.h"
#include "SurfSettings.h"
#include "SurfTypes.h"

#include "Editor.h"
#include "ScopedTransaction.h"

#define LOCTEXT_NAMESPACE "SurfacingProEdModePaintMask"

namespace
{
	FVector NormalAt(const FSurfPatch& Patch, int32 X, int32 Y)
	{
		const int32 X0 = FMath::Max(0, X - 1);
		const int32 X1 = FMath::Min(Patch.Width - 1, X + 1);
		const int32 Y0 = FMath::Max(0, Y - 1);
		const int32 Y1 = FMath::Min(Patch.Height - 1, Y + 1);
		const FVector DU = Patch.At(X1, Y).Pos - Patch.At(X0, Y).Pos;
		const FVector DV = Patch.At(X, Y1).Pos - Patch.At(X, Y0).Pos;
		FVector N = FVector::CrossProduct(DU, DV).GetSafeNormal();
		return Patch.bInvert ? -N : N;
	}

	float ChannelOf(const FSurfControlPoint& Point, int32 Channel)
	{
		switch (Channel)
		{
		case 1: return Point.Tint.G / 255.0f;
		case 2: return Point.Tint.B / 255.0f;
		case 3: return Point.Weight;
		default: return Point.Tint.R / 255.0f;
		}
	}

	void SetChannel(FSurfControlPoint& Point, int32 Channel, float Value)
	{
		const float Clamped = FMath::Clamp(Value, 0.0f, 1.0f);
		const uint8 Byte = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(Clamped * 255.0f), 0, 255));
		switch (Channel)
		{
		case 1: Point.Tint.G = Byte; break;
		case 2: Point.Tint.B = Byte; break;
		case 3: Point.Weight = Clamped; break;
		default: Point.Tint.R = Byte; break;
		}
	}

	/**
	 * Every open patch's points welded where they stand on one another
	 * (S248a: "from cavity: can we consider overlapped verts/edges as if
	 * they were welded?"): one node per spot in the world - the 0.1 cm cells
	 * the overlap dots count by - its neighbours the grid neighbours of every
	 * point standing there, whichever patch they belong to. A floor meeting
	 * a wall is one crease then, not two flat borders.
	 */
	struct FSurfWeld
	{
		TArray<FVector> Position;
		TArray<FVector> Normal;
		TArray<TArray<int32>> Ring8;
		TArray<TArray<int32>> Ring4;
		TArray<TPair<const ASurfGroup*, FGuid>> Members;
		TMap<TPair<const ASurfGroup*, FGuid>, TArray<int32>> NodesOf;

		const TArray<int32>* Nodes(const ASurfGroup* Group, const FGuid& Id) const
		{
			return NodesOf.Find(TPair<const ASurfGroup*, FGuid>(Group, Id));
		}
	};

	void BuildWeld(const TArray<FSurfPatchRef>& Refs, FSurfWeld& Out)
	{
		auto CellOf = [](const FVector& World)
		{
			return FIntVector(FMath::RoundToInt(World.X * 10.0), FMath::RoundToInt(World.Y * 10.0), FMath::RoundToInt(World.Z * 10.0));
		};
		TMap<FIntVector, int32> Cells;
		TArray<const FSurfPatch*> Patches;
		for (const FSurfPatchRef& Ref : Refs)
		{
			const ASurfGroup* Group = Ref.Group.Get();
			const FSurfPatch* Patch = Group ? Group->FindPatch(Ref.PatchId) : nullptr;
			if (!Patch || !Patch->HasValidDimensions())
			{
				continue;
			}
			const TPair<const ASurfGroup*, FGuid> Key(Group, Patch->Id);
			if (Out.NodesOf.Contains(Key))
			{
				continue;
			}
			const FTransform ToWorld = Group->GetActorTransform();
			TArray<int32>& Nodes = Out.NodesOf.Add(Key);
			Out.Members.Add(Key);
			Patches.Add(Patch);
			const int32 W = Patch->Width;
			const int32 H = Patch->Height;
			Nodes.SetNumUninitialized(W * H);
			for (int32 Y = 0; Y < H; ++Y)
			{
				for (int32 X = 0; X < W; ++X)
				{
					const FVector World = ToWorld.TransformPosition(Patch->At(X, Y).Pos);
					const FIntVector Cell = CellOf(World);
					int32 Node = INDEX_NONE;
					if (const int32* Found = Cells.Find(Cell))
					{
						Node = *Found;
					}
					else
					{
						Node = Out.Position.Add(World);
						Out.Normal.Add(FVector::ZeroVector);
						Out.Ring8.AddDefaulted();
						Out.Ring4.AddDefaulted();
						Cells.Add(Cell, Node);
					}
					Nodes[Y * W + X] = Node;
					Out.Normal[Node] += ToWorld.TransformVectorNoScale(NormalAt(*Patch, X, Y));
				}
			}
		}
		for (int32 K = 0; K < Out.Members.Num(); ++K)
		{
			const FSurfPatch& Patch = *Patches[K];
			const TArray<int32>& Nodes = Out.NodesOf[Out.Members[K]];
			const int32 W = Patch.Width;
			const int32 H = Patch.Height;
			for (int32 Y = 0; Y < H; ++Y)
			{
				for (int32 X = 0; X < W; ++X)
				{
					const int32 Node = Nodes[Y * W + X];
					for (int32 DY = -1; DY <= 1; ++DY)
					{
						for (int32 DX = -1; DX <= 1; ++DX)
						{
							if ((DX == 0 && DY == 0) || X + DX < 0 || Y + DY < 0 || X + DX >= W || Y + DY >= H)
							{
								continue;
							}
							const int32 Other = Nodes[(Y + DY) * W + X + DX];
							if (Other == Node)
							{
								continue;
							}
							Out.Ring8[Node].AddUnique(Other);
							if (DX == 0 || DY == 0)
							{
								Out.Ring4[Node].AddUnique(Other);
							}
						}
					}
				}
			}
		}
		for (FVector& Each : Out.Normal)
		{
			Each = Each.GetSafeNormal();
		}
	}

	/**
	 * The cavity of every node (S248a): the mean of its neighbours' offsets
	 * along its normal over their mean distance, smoothed, leveled - and
	 * dirt only: nothing on the flat, up to full in a crevice (the tooltip's
	 * "dirt in every crevice"; it was 0.5 on the flat, so a filled channel
	 * never read as full). Inverted, the ridges instead.
	 */
	void WeldCavity(const FSurfWeld& Weld, float Smooth, float Level, bool bInvert, TArray<float>& Out)
	{
		const int32 Num = Weld.Position.Num();
		TArray<float> Raw;
		Raw.SetNumZeroed(Num);
		for (int32 N = 0; N < Num; ++N)
		{
			const FVector& Up = Weld.Normal[N];
			const TArray<int32>& Ring = Weld.Ring8[N];
			if (Up.IsNearlyZero() || Ring.Num() == 0)
			{
				continue;
			}
			FVector Sum = FVector::ZeroVector;
			double Reach = 0.0;
			for (const int32 M : Ring)
			{
				const FVector D = Weld.Position[M] - Weld.Position[N];
				Sum += D;
				Reach += D.Size();
			}
			if (Reach <= KINDA_SMALL_NUMBER)
			{
				continue;
			}
			const double Count = static_cast<double>(Ring.Num());
			Raw[N] = static_cast<float>(FVector::DotProduct(Sum / Count, Up) / (Reach / Count));
		}
		const int32 Passes = Smooth <= 0.0f ? 0 : (Smooth > 0.5f ? 2 : 1);
		const float Blend = Smooth <= 0.0f ? 0.0f : (Smooth > 0.5f ? FMath::Min(1.0f, Smooth) : Smooth * 2.0f);
		for (int32 Pass = 0; Pass < Passes; ++Pass)
		{
			const TArray<float> Was = Raw;
			for (int32 N = 0; N < Num; ++N)
			{
				double Total = Was[N];
				int32 Count = 1;
				for (const int32 M : Weld.Ring8[N])
				{
					Total += Was[M];
					++Count;
				}
				Raw[N] = FMath::Lerp(Was[N], static_cast<float>(Total / Count), Blend);
			}
		}
		Out.SetNumUninitialized(Num);
		for (int32 N = 0; N < Num; ++N)
		{
			float V = FMath::Clamp(Raw[N] * FMath::Max(0.25f, Level), -1.0f, 1.0f);
			if (bInvert)
			{
				V = -V;
			}
			Out[N] = FMath::Max(0.0f, V);
		}
	}

	const TCHAR* BlendName(ESurfMaskBlend How)
	{
		switch (How)
		{
		case ESurfMaskBlend::Add: return TEXT("add");
		case ESurfMaskBlend::Subtract: return TEXT("subtract");
		case ESurfMaskBlend::Multiply: return TEXT("multiply");
		case ESurfMaskBlend::Lighten: return TEXT("lighten");
		case ESurfMaskBlend::Darken: return TEXT("darken");
		default: return TEXT("replace");
		}
	}
}

void USurfEdMode::CavityOf(const FSurfPatch& Patch, TArray<float>& Out, float Smooth, float Level, bool bInvert)
{
	Out.Reset();
	if (!Patch.HasValidDimensions())
	{
		return;
	}
	const int32 W = Patch.Width;
	const int32 H = Patch.Height;
	TArray<float> Raw;
	Raw.SetNumZeroed(W * H);

	// Signed: the mean of the neighbours' offsets, along the normal, over
	// the mean neighbour distance - so a dent of any size reads the same.
	for (int32 Y = 0; Y < H; ++Y)
	{
		for (int32 X = 0; X < W; ++X)
		{
			const FVector P = Patch.At(X, Y).Pos;
			const FVector N = NormalAt(Patch, X, Y);
			FVector Sum = FVector::ZeroVector;
			double Reach = 0.0;
			int32 Count = 0;
			for (int32 DY = -1; DY <= 1; ++DY)
			{
				for (int32 DX = -1; DX <= 1; ++DX)
				{
					if ((DX == 0 && DY == 0) || X + DX < 0 || Y + DY < 0 || X + DX >= W || Y + DY >= H)
					{
						continue;
					}
					const FVector D = Patch.At(X + DX, Y + DY).Pos - P;
					Sum += D;
					Reach += D.Size();
					++Count;
				}
			}
			if (Count == 0 || Reach <= KINDA_SMALL_NUMBER)
			{
				continue;
			}
			const FVector Mean = Sum / static_cast<double>(Count);
			const double Scale = Reach / static_cast<double>(Count);
			// Dents positive: the neighbours sit in front of the surface.
			Raw[Y * W + X] = static_cast<float>(FVector::DotProduct(Mean, N) / Scale);
		}
	}

	// Smooth: one ring at Smooth 0.5, two at 1 - a blend with the ring's
	// mean, twice when asked.
	const int32 Passes = Smooth <= 0.0f ? 0 : (Smooth > 0.5f ? 2 : 1);
	const float Blend = Smooth <= 0.0f ? 0.0f : (Smooth > 0.5f ? FMath::Min(1.0f, Smooth) : Smooth * 2.0f);
	for (int32 Pass = 0; Pass < Passes; ++Pass)
	{
		TArray<float> Was = Raw;
		for (int32 Y = 0; Y < H; ++Y)
		{
			for (int32 X = 0; X < W; ++X)
			{
				double Sum = 0.0;
				int32 Count = 0;
				for (int32 DY = -1; DY <= 1; ++DY)
				{
					for (int32 DX = -1; DX <= 1; ++DX)
					{
						if (X + DX < 0 || Y + DY < 0 || X + DX >= W || Y + DY >= H)
						{
							continue;
						}
						Sum += Was[(Y + DY) * W + X + DX];
						++Count;
					}
				}
				const float Mean = Count > 0 ? static_cast<float>(Sum / Count) : Was[Y * W + X];
				Raw[Y * W + X] = FMath::Lerp(Was[Y * W + X], Mean, Blend);
			}
		}
	}

	// Level about flat; dirt only - nothing on the flat, full in a crevice
	// (S248a); inverted, the ridges.
	Out.SetNumUninitialized(W * H);
	for (int32 I = 0; I < W * H; ++I)
	{
		float V = FMath::Clamp(Raw[I] * FMath::Max(0.25f, Level), -1.0f, 1.0f);
		if (bInvert)
		{
			V = -V;
		}
		Out[I] = FMath::Max(0.0f, V);
	}
}

int32 USurfEdMode::AdjustPaint(int32 Kind)
{
	static const TCHAR* Names[5] = { TEXT("From Cavity"), TEXT("Smooth Paint"), TEXT("Contrast"), TEXT("Levels"), TEXT("Invert Paint") };
	static const TCHAR* Verbs[5] = { TEXT("paint_cavity"), TEXT("paint_smooth"), TEXT("paint_contrast"), TEXT("paint_levels"), TEXT("paint_invert") };
	Kind = FMath::Clamp(Kind, 0, 4);
	TArray<FSurfPatchRef> Targets;
	PaintTargets(Targets);
	const USurfSettings* S = GetDefault<USurfSettings>();
	if (!S)
	{
		return 0;
	}
	// The tab's channels (S248a): every ticked one - R, G and B as the colour, A beside them.
	TArray<int32> Channels;
	{
		TArray<ESurfPaintChannel, TInlineAllocator<4>> All;
		S->PaintSet().All(All);
		for (const ESurfPaintChannel Each : All) { Channels.Add(static_cast<int32>(Each)); }
	}
	FScopedTransaction Transaction(FText::FromString(Names[Kind]));
	const int32 OnMeshes = MeshAdjustPaint(Kind);
	if (Targets.Num() == 0 && OnMeshes == 0)
	{
		Transaction.Cancel();
		if (Inspector)
		{
			Inspector->SetResult(FString::Printf(TEXT("%s: select a patch or a mesh first"), Names[Kind]));
		}
		return 0;
	}
	const int32 Times = Kind == USurfSettings::PaintAdjustSmooth ? FMath::Clamp(S->PaintSmoothTimes, 1, 20) : 1;

	// The two that read neighbours read across patches that meet (S248a):
	// every open patch welded where its points stand on another's.
	const bool bWelded = Kind == USurfSettings::PaintAdjustCavity || Kind == USurfSettings::PaintAdjustSmooth;
	FSurfWeld Weld;
	TArray<float> NodeCavity;
	if (bWelded)
	{
		TArray<FSurfPatchRef> Around = GetAllPatches();
		for (const FSurfPatchRef& Ref : Targets)
		{
			Around.AddUnique(Ref);
		}
		BuildWeld(Around, Weld);
		if (Kind == USurfSettings::PaintAdjustCavity)
		{
			WeldCavity(Weld, S->CavitySmooth, S->CavityLevel, S->bCavityInvert, NodeCavity);
		}
	}

	// Each target with its picked points - all of it when none is picked.
	struct FTarget
	{
		ASurfGroup* Group = nullptr;
		FSurfPatch* Patch = nullptr;
		TArray<bool> Picked;
		int32 PickedCount = 0;
		const TArray<int32>* Nodes = nullptr;
	};
	TArray<FTarget> Work;
	TArray<ASurfGroup*> Groups;
	int32 Points = 0;
	for (const FSurfPatchRef& Ref : Targets)
	{
		ASurfGroup* Group = Ref.Group.Get();
		FSurfPatch* Patch = Group ? Group->ModifyPatch(Ref.PatchId) : nullptr;
		if (!Patch || !Patch->HasValidDimensions())
		{
			continue;
		}
		FTarget& Target = Work.AddDefaulted_GetRef();
		Target.Group = Group;
		Target.Patch = Patch;
		const int32 W = Patch->Width;
		const int32 H = Patch->Height;
		Target.Picked.Init(false, W * H);
		for (const FSurfPointRef& Each : PointSelection)
		{
			if (Each.PatchId == Ref.PatchId && Each.Group == Ref.Group && Each.X >= 0 && Each.X < W && Each.Y >= 0 && Each.Y < H)
			{
				Target.Picked[Each.Y * W + Each.X] = true;
				++Target.PickedCount;
			}
		}
		if (Target.PickedCount == 0)
		{
			for (bool& Each : Target.Picked) { Each = true; }
		}
		Target.Nodes = bWelded ? Weld.Nodes(Group, Patch->Id) : nullptr;
		Points += Target.PickedCount > 0 ? Target.PickedCount : W * H;
		++Patch->EditSerial;
		Groups.AddUnique(Group);
	}

	for (const int32 C : Channels)
	{
		for (int32 Pass = 0; Pass < Times; ++Pass)
		{
			// A smooth reads the pass's start everywhere, welded: a node is
			// the mean of the points standing in it, every patch's.
			TArray<float> NodeWas;
			if (Kind == USurfSettings::PaintAdjustSmooth && bWelded)
			{
				TArray<double> Sum;
				TArray<int32> Count;
				Sum.SetNumZeroed(Weld.Position.Num());
				Count.SetNumZeroed(Weld.Position.Num());
				for (const TPair<const ASurfGroup*, FGuid>& Member : Weld.Members)
				{
					const FSurfPatch* Read = Member.Key ? Member.Key->FindPatch(Member.Value) : nullptr;
					const TArray<int32>* Nodes = Weld.Nodes(Member.Key, Member.Value);
					if (!Read || !Nodes || Nodes->Num() != Read->Points.Num())
					{
						continue;
					}
					for (int32 I = 0; I < Read->Points.Num(); ++I)
					{
						Sum[(*Nodes)[I]] += ChannelOf(Read->Points[I], C);
						++Count[(*Nodes)[I]];
					}
				}
				NodeWas.SetNumUninitialized(Sum.Num());
				for (int32 N = 0; N < Sum.Num(); ++N)
				{
					NodeWas[N] = Count[N] > 0 ? static_cast<float>(Sum[N] / Count[N]) : 0.0f;
				}
			}
			for (FTarget& Target : Work)
			{
				FSurfPatch* Patch = Target.Patch;
				const int32 W = Patch->Width;
				const int32 H = Patch->Height;
				const bool bNodes = Target.Nodes && Target.Nodes->Num() == W * H;
				// Read before write, so a smooth reads the pass's start everywhere.
				TArray<float> Was;
				Was.SetNumUninitialized(W * H);
				for (int32 I = 0; I < W * H; ++I)
				{
					Was[I] = ChannelOf(Patch->Points[I], C);
				}
				for (int32 Y = 0; Y < H; ++Y)
				{
					for (int32 X = 0; X < W; ++X)
					{
						const int32 I = Y * W + X;
						if (!Target.Picked[I])
						{
							continue;
						}
						float Own = Was[I];
						float Mean = Own;
						float Cavity = 0.0f;
						if (Kind == USurfSettings::PaintAdjustSmooth)
						{
							if (bNodes && NodeWas.Num() > 0)
							{
								const int32 Node = (*Target.Nodes)[I];
								Own = NodeWas[Node];
								const TArray<int32>& Ring = Weld.Ring4[Node];
								float Sum = 0.0f;
								for (const int32 M : Ring) { Sum += NodeWas[M]; }
								Mean = Ring.Num() > 0 ? Sum / Ring.Num() : Own;
							}
							else
							{
								float Sum = 0.0f;
								int32 Count = 0;
								if (X > 0) { Sum += Was[I - 1]; ++Count; }
								if (X + 1 < W) { Sum += Was[I + 1]; ++Count; }
								if (Y > 0) { Sum += Was[I - W]; ++Count; }
								if (Y + 1 < H) { Sum += Was[I + W]; ++Count; }
								Mean = Count > 0 ? Sum / Count : Own;
							}
						}
						else if (Kind == USurfSettings::PaintAdjustCavity && bNodes)
						{
							Cavity = NodeCavity[(*Target.Nodes)[I]];
						}
						SetChannel(Patch->At(X, Y), C, USurfSettings::AdjustPaintValue(*S, Kind, Own, Mean, Cavity));
					}
				}
			}
		}
	}
	Points += OnMeshes;
	if (Points == 0)
	{
		Transaction.Cancel();
		return 0;
	}
	for (ASurfGroup* Each : Groups) { Each->RebuildPreview(); }
	if (Inspector)
	{
		FString Which;
		static const TCHAR* Letters[4] = { TEXT("R"), TEXT("G"), TEXT("B"), TEXT("A") };
		for (const int32 C : Channels) { Which += Letters[C]; }
		Inspector->SetResult(FString::Printf(TEXT("%s: %s on %d point%s"), Names[Kind], *Which, Points, Points == 1 ? TEXT("") : TEXT("s")));
		Inspector->RefreshSelectionCount();
	}
	SurfSession::Verb(Verbs[Kind], FString::Printf(TEXT("channels=%d points=%d blend=%s"), Channels.Num(), Points, BlendName(S->MaskBlend)));
	RefreshPaintView();
	if (GEditor) { GEditor->RedrawLevelEditingViewports(); }
	return Points;
}

void USurfEdMode::SwapPaintColours()
{
	if (USurfSettings* S = GetMutableDefault<USurfSettings>())
	{
		Swap(S->PaintColour, S->PaintColourBack);
		S->SaveConfig();
	}
	SurfSession::Verb(TEXT("paint_colours"), TEXT("swapped"));
}

#undef LOCTEXT_NAMESPACE
