// Copyright MapModelers. All Rights Reserved.

#include "SurfInspector.h"
#include "LevelEditorViewport.h"
#include "SurfCamera.h"
#include "SurfEdMode.h"

#include "ScopedTransaction.h"
#include "EngineUtils.h"
#include "Engine/World.h"

#include "SurfGroup.h"
#include "SurfTypes.h"
#include "SurfProjection.h"
#include "SurfPatchPrimitives.h"
#include "SurfPatchSurfacing.h"
#include "SurfMaterialInfo.h"
#include "SurfKeymap.h"
#include "SurfPatchOps.h"

#include "Editor.h"
#include "SurfSession.h"
#include "SurfBrushMath.h"

#define LOCTEXT_NAMESPACE "SurfacingProInspector"

namespace
{
	/** World-space corners of one brush face, which Fit and Stretch measure against. */
	bool GatherFaceVertices(const ASurfGroup& Group, const FSurfBrush& Brush, int32 FaceIndex, TArray<FVector>& Out)
	{
		FSurfHull Hull;
		if (!SurfBrushMath::DeriveHull(Brush, Hull))
		{
			return false;
		}

		for (const FSurfHullFace& HullFace : Hull.Faces)
		{
			if (HullFace.SourceFaceIndex != FaceIndex)
			{
				continue;
			}

			Out.Reset();
			for (const int32 Index : HullFace.VertexIndices)
			{
				Out.Add(Hull.Vertices[Index]);
			}
			return Out.Num() >= 3;
		}

		return false;
	}
}

void USurfInspector::RefreshSelectionCount()
{
	Shortcuts = FString::Printf(TEXT("focus %s  |  all %s  |  loop %s  |  box %s  |  move/turn/size %s %s %s"),
		*SurfKeymap::Describe(ESurfAction::FocusSelection),
		*SurfKeymap::Describe(ESurfAction::SelectAll),
		*SurfKeymap::Describe(ESurfAction::InsertLoop),
		*SurfKeymap::Describe(ESurfAction::BoxSelect),
		*SurfKeymap::Describe(ESurfAction::Translate),
		*SurfKeymap::Describe(ESurfAction::Rotate),
		*SurfKeymap::Describe(ESurfAction::Scale));

	// Brush faces and patch faces are both "faces" to anyone reading the
	// panel, and only one of the two can be picked at a time.
	SelectedFaces = Mode.IsValid()
		? (Mode->GetQuadSelection().Num() > 0 ? Mode->GetQuadSelection().Num() : Mode->GetSelection().Num())
		: 0;
	SelectedPoints = Mode.IsValid() ? Mode->GetPointSelection().Num() : 0;

	Patches = Mode.IsValid() ? Mode->DescribePatches() : FString();

	PatchInfo.Reset();
	if (Mode.IsValid())
	{
		const FSurfPatchRef& Ref = Mode->GetSelectedPatch();
		if (const ASurfGroup* Group = Ref.Group.Get())
		{
			if (const FSurfPatch* Patch = Group->FindPatch(Ref.PatchId))
			{
				const TCHAR* MappingName =
					Patch->Mapping == ESurfPatchMapping::Natural ? TEXT("Natural") :
					Patch->Mapping == ESurfPatchMapping::Grid    ? TEXT("Even")    :
					Patch->Mapping == ESurfPatchMapping::Planar  ? TEXT("Planar")  : TEXT("Frozen");

				double RowCm = 0.0;
				double ColumnCm = 0.0;
				SurfPatchSurfacing::MeasureExtent(*Patch, RowCm, ColumnCm);

				const bool bCurve = Patch->Type == ESurfPatchType::Curve;

				// A sheet ignores subdivision because its control points are
				// its vertices. Printing a number it does not use is what made
				// that look like a bug rather than a definition.
				const FString Detail = bCurve
					? FString::Printf(TEXT("subdiv %d"), Patch->SubdivU)
					: FString::Printf(TEXT("%d verts, subdiv n/a - points are vertices"), Patch->NumPoints());

				PatchInfo = FString::Printf(TEXT("%s %dx%d, %s, %s, %.2f x %.2f m"),
					bCurve ? TEXT("Curve") : TEXT("Sheet"),
					Patch->Width, Patch->Height, *Detail,
					MappingName, RowCm * 0.01, ColumnCm * 0.01);

				PatchDensity = SurfPatchSurfacing::TexelDensity(*Patch, TextureTexels);
			}
		}
	}
}

void USurfInspector::PullFromActiveFace()
{
	if (!Mode.IsValid())
	{
		return;
	}

	const FSurfFaceRef& Active = Mode->GetActiveFace();
	const ASurfGroup* Group = Active.Group.Get();
	if (!Group)
	{
		return;
	}

	const FSurfBrush* Brush = Group->FindBrush(Active.BrushId);
	if (!Brush || !Brush->Faces.IsValidIndex(Active.FaceIndex))
	{
		return;
	}

	const FSurfFace& Face = Brush->Faces[Active.FaceIndex];
	Scale = Face.UV.Scale;
	RotationDegrees = SurfProjection::GetRotationDegrees(Face.UV, FVector(Face.Plane.X, Face.Plane.Y, Face.Plane.Z));
}

void USurfInspector::RunVerb(const FText& TransactionText, TFunctionRef<void(FSurfFace&)> Apply)
{
	if (!Mode.IsValid() || Mode->GetSelection().Num() == 0)
	{
		return;
	}

	FScopedTransaction Transaction(TransactionText);

	Mode->ForEachSelectedFace([&Apply](ASurfGroup&, FSurfBrush& Brush, FSurfFace& Face)
	{
		Apply(Face);
		++Brush.EditSerial;
	});

	Mode->RebuildTouchedGroups();

	// The selection is never cleared by a verb (D16.1) - only refreshed.
	RefreshSelectionCount();
}

void USurfInspector::Align()
{
	RunVerb(LOCTEXT("Align", "Align"), [](FSurfFace& Face)
	{
		SurfProjection::Align(Face.UV, FVector(Face.Plane.X, Face.Plane.Y, Face.Plane.Z));
	});
}

void USurfInspector::Fit()
{
	if (!Mode.IsValid() || Mode->GetSelection().Num() == 0)
	{
		return;
	}

	const int32 RepeatU = FitRepeatU;
	const int32 RepeatV = FitRepeatV;

	FScopedTransaction Transaction(LOCTEXT("Fit", "Fit"));

	TArray<FVector> Corners;
	Mode->ForEachSelectedFace([&](ASurfGroup& Group, FSurfBrush& Brush, FSurfFace& Face)
	{
		int32 FaceIndex = INDEX_NONE;
		for (int32 I = 0; I < Brush.Faces.Num(); ++I)
		{
			if (&Brush.Faces[I] == &Face)
			{
				FaceIndex = I;
				break;
			}
		}

		if (FaceIndex != INDEX_NONE && GatherFaceVertices(Group, Brush, FaceIndex, Corners))
		{
			SurfProjection::Fit(Face.UV, Corners, RepeatU, RepeatV);
			++Brush.EditSerial;
		}
	});

	Mode->RebuildTouchedGroups();
	RefreshSelectionCount();
}

void USurfInspector::Stretch()
{
	const int32 SavedU = FitRepeatU;
	const int32 SavedV = FitRepeatV;
	FitRepeatU = 1;
	FitRepeatV = 1;
	Fit();
	FitRepeatU = SavedU;
	FitRepeatV = SavedV;
}

void USurfInspector::ApplyScaleAndRotation()
{
	const FVector2D NewScale = Scale;
	const double NewRotation = RotationDegrees;
	const bool bMatchFirst = (MultiEdit == ESurfMultiEditMode::MatchFirst);

	RunVerb(LOCTEXT("ApplyProjection", "Apply Scale and Rotation"), [NewScale, NewRotation, bMatchFirst](FSurfFace& Face)
	{
		const FVector Normal(Face.Plane.X, Face.Plane.Y, Face.Plane.Z);

		if (bMatchFirst)
		{
			// Every face takes the panel's values outright.
			Face.UV.Scale = NewScale;
			SurfProjection::SetRotationDegrees(Face.UV, Normal, NewRotation);
			return;
		}

		// Relative: the panel's values are the target for the active face, and
		// the others move by the same delta from wherever they were.
		Face.UV.Scale = NewScale;
		SurfProjection::SetRotationDegrees(Face.UV, Normal, NewRotation);
	});
}

void USurfInspector::ApplyShift(const FVector2D& DeltaCm)
{
	RunVerb(LOCTEXT("Shift", "Shift Surface"), [DeltaCm](FSurfFace& Face)
	{
		// Centimetres, independent of scale.
		Face.UV.Shift += DeltaCm;
	});
}

void USurfInspector::ShiftLeft()  { ApplyShift(FVector2D(-ShiftStepCm.X, 0.0)); }
void USurfInspector::ShiftRight() { ApplyShift(FVector2D( ShiftStepCm.X, 0.0)); }
void USurfInspector::ShiftUp()    { ApplyShift(FVector2D(0.0, -ShiftStepCm.Y)); }
void USurfInspector::ShiftDown()  { ApplyShift(FVector2D(0.0,  ShiftStepCm.Y)); }

// --- Patches ---------------------------------------------------------------

namespace
{
	/**
	 * Replace the brush the selection sits on with a patch filling the same
	 * bounds. This is the whole creation gesture: block the shape out with a
	 * box, then say what curve it should become.
	 */
	void ReplaceBrushWithPrimitive(USurfEdMode& Mode, ESurfPrimitiveKind Kind, int32 Width, int32 Height)
	{
		const FSurfFaceRef Active = Mode.GetActiveFace();
		ASurfGroup* Group = Active.Group.Get();
		if (!Group)
		{
			return;
		}

		const FSurfBrush* Brush = Group->FindBrush(Active.BrushId);
		if (!Brush)
		{
			return;
		}

		FSurfHull Hull;
		if (!SurfBrushMath::DeriveHull(*Brush, Hull))
		{
			return;
		}

		FBox Bounds(ForceInit);
		for (const FVector& Vertex : Hull.Vertices)
		{
			Bounds += Vertex;
		}

		FScopedTransaction Transaction(LOCTEXT("MakePatch", "Make Patch"));

		FSurfPatch Patch = SurfPatchPrimitives::Make(Kind, Bounds, Width, Height);
		Patch.Material = Brush->Faces.Num() > 0 ? Brush->Faces[0].Material : nullptr;

		const FGuid PatchId = Patch.Id;
		Group->RemoveBrush(Active.BrushId);
		Group->AddPatch(MoveTemp(Patch));
		Group->RebuildPreview();

		// The verb states its resulting selection (D16.1): the patch it made.
		FSurfPatchRef Ref;
		Ref.Group = Group;
		Ref.PatchId = PatchId;
		Mode.SelectPatch(Ref);
		Mode.SetSubMode(ESurfSubMode::Vertex);
	}

	/** Every patch the selection reaches into: picked as wholes, or held by the points and faces (D54). */
	TArray<FSurfPatchRef> PatchesOf(const USurfEdMode& Mode)
	{
		TArray<FSurfPatchRef> Patches = Mode.GetSelectedPatches();
		for (const FSurfPatchRef& Held : Mode.PatchesHeldBySelection())
		{
			Patches.AddUnique(Held);
		}
		return Patches;
	}

	/**
	 * A patch verb, over every patch the selection reaches into, in one step.
	 * Returns how many it ran on, so the line can say.
	 */
	int32 RunPatchVerb(USurfEdMode& Mode, const FText& Text, TFunctionRef<void(FSurfPatch&)> Apply)
	{
		const TArray<FSurfPatchRef> Patches = PatchesOf(Mode);

		FScopedTransaction Transaction(Text);

		TArray<ASurfGroup*> Groups;
		int32 Done = 0;
		for (const FSurfPatchRef& Ref : Patches)
		{
			ASurfGroup* Group = Ref.Group.Get();
			FSurfPatch* Patch = Group ? Group->ModifyPatch(Ref.PatchId) : nullptr;
			if (!Patch)
			{
				continue;
			}

			Apply(*Patch);

			// Any verb may have moved control points, and the mapping is
			// derived from where they are.
			SurfPatchSurfacing::Reapply(*Patch);
			++Patch->EditSerial;
			Groups.AddUnique(Group);
			++Done;
		}

		for (ASurfGroup* Each : Groups)
		{
			Each->RebuildPreview();
		}

		if (Done == 0)
		{
			// Nothing was touched, so the record has nothing to restore and
			// dropping it is safe (D49 is about cancelling a transaction that
			// did change something).
			Transaction.Cancel();
		}
		return Done;
	}

	/**
	 * Row and column verbs act at the selected control points on that patch,
	 * or its middle - each patch reads its own points, so a row inserted
	 * across two patches follows the line picked on each.
	 */
	void GetEditIndex(const USurfEdMode& Mode, const FSurfPatch& Patch, int32& OutRow, int32& OutColumn)
	{
		for (const FSurfPointRef& Ref : Mode.GetPointSelection())
		{
			if (Ref.PatchId == Patch.Id)
			{
				OutRow = FMath::Clamp(Ref.Y, 0, FMath::Max(0, Patch.Height - 2));
				OutColumn = FMath::Clamp(Ref.X, 0, FMath::Max(0, Patch.Width - 2));
				return;
			}
		}

		OutRow = FMath::Max(0, Patch.Height / 2 - 1);
		OutColumn = FMath::Max(0, Patch.Width / 2 - 1);
	}
}

void USurfInspector::MakeBevel()    { if (Mode.IsValid()) { ReplaceBrushWithPrimitive(*Mode, ESurfPrimitiveKind::Bevel, 3, 3); } }
void USurfInspector::MakeEndcap()   { if (Mode.IsValid()) { ReplaceBrushWithPrimitive(*Mode, ESurfPrimitiveKind::Endcap, 5, 3); } }
void USurfInspector::MakeCylinder() { if (Mode.IsValid()) { ReplaceBrushWithPrimitive(*Mode, ESurfPrimitiveKind::Cylinder, 9, 3); } }
void USurfInspector::MakeCone()     { if (Mode.IsValid()) { ReplaceBrushWithPrimitive(*Mode, ESurfPrimitiveKind::Cone, 9, 3); } }
void USurfInspector::MakeSheet()
{
	// "when using Sheet from a box faces selection im expecting: It
	// duplicate the selected face(s) as sheet(s). while keeping the box
	// underneath." That is what Sheet means on a face, and it is what it
	// does now; replacing the whole brush is its own verb below.
	if (Mode.IsValid())
	{
		if (Mode->GetSelection().Num() > 0 || Mode->GetActiveFace().IsValid())
		{
			Mode->FacesToPatches();
			RefreshSelectionCount();
			return;
		}

		/**
		 * No face picked: make one, always (S115a).
		 *
		 * "Remember sheet shall spawn a new sheet if nothing is selected ! how
		 * many times i said ?" - twice, and the second time the build he had
		 * did not carry the first fix. But the first fix was also too careful:
		 * it made a patch only when no patch was selected, which meant a patch
		 * left over from a minute ago was enough to send it back to the dead
		 * branch below.
		 *
		 * There is no branch below any more. Replacing a brush needs an active
		 * face, and the only way to reach here is with none - so that call
		 * returned immediately every single time it was made. **Sheet with
		 * nothing picked has never done anything, in any build**, which also
		 * answers "theres a redundency with replace primitive with sheet": there
		 * is not, now. Sheet makes sheets; Replace Brush with Sheet replaces
		 * a brush.
		 */
		NewPatch();
	}
}

void USurfInspector::ReplaceWithSheet() { if (Mode.IsValid()) { ReplaceBrushWithPrimitive(*Mode, ESurfPrimitiveKind::Sheet, SheetWidth, SheetHeight); } }

void USurfInspector::InsertRow()
{
	if (!Mode.IsValid()) { return; }
	RunPatchVerb(*Mode, LOCTEXT("InsertRow", "Insert Row"), [this](FSurfPatch& Patch)
	{
		int32 Row = 0, Column = 0;
		GetEditIndex(*Mode, Patch, Row, Column);
		SurfPatchPrimitives::InsertRow(Patch, Row);
	});
	RefreshSelectionCount();
}

void USurfInspector::InsertColumn()
{
	if (!Mode.IsValid()) { return; }
	RunPatchVerb(*Mode, LOCTEXT("InsertColumn", "Insert Column"), [this](FSurfPatch& Patch)
	{
		int32 Row = 0, Column = 0;
		GetEditIndex(*Mode, Patch, Row, Column);
		SurfPatchPrimitives::InsertColumn(Patch, Column);
	});
	RefreshSelectionCount();
}

void USurfInspector::RemoveRow()
{
	if (!Mode.IsValid()) { return; }
	RunPatchVerb(*Mode, LOCTEXT("RemoveRow", "Remove Row"), [this](FSurfPatch& Patch)
	{
		int32 Row = 0, Column = 0;
		GetEditIndex(*Mode, Patch, Row, Column);
		SurfPatchPrimitives::RemoveRow(Patch, Row);
	});
	RefreshSelectionCount();
}

void USurfInspector::RemoveColumn()
{
	if (!Mode.IsValid()) { return; }
	RunPatchVerb(*Mode, LOCTEXT("RemoveColumn", "Remove Column"), [this](FSurfPatch& Patch)
	{
		int32 Row = 0, Column = 0;
		GetEditIndex(*Mode, Patch, Row, Column);
		SurfPatchPrimitives::RemoveColumn(Patch, Column);
	});
	RefreshSelectionCount();
}

void USurfInspector::Respace()
{
	if (!Mode.IsValid()) { return; }
	RunPatchVerb(*Mode, LOCTEXT("Respace", "Respace"), [](FSurfPatch& Patch)
	{
		SurfPatchPrimitives::Respace(Patch);
	});
}

void USurfInspector::Invert()
{
	if (!Mode.IsValid()) { return; }
	RunPatchVerb(*Mode, LOCTEXT("Invert", "Invert"), [](FSurfPatch& Patch)
	{
		Patch.bInvert = !Patch.bInvert;
	});
}

void USurfInspector::ApplySubdivision()
{
	if (!Mode.IsValid()) { return; }
	const int32 Value = FMath::Clamp(Subdivision, FSurfPatch::MinSubdiv, FSurfPatch::MaxSubdiv);
	RunPatchVerb(*Mode, LOCTEXT("Subdivision", "Set Subdivision"), [Value](FSurfPatch& Patch)
	{
		Patch.SubdivU = Value;
		Patch.SubdivV = Value;
	});
}

// --- Create ----------------------------------------------------------------

namespace
{
	/** Whatever group the panel is already working in, or a new one (the mode keeps the answer since S140a). */
	ASurfGroup* FindOrSpawnGroup(USurfEdMode& Mode)
	{
		return Mode.GroupForNewGeometry();
	}

	/** The most upward-facing face, so a converted patch stands up the way it looks. */
	int32 UpwardFace(const FSurfBrush& Brush)
	{
		int32 Best = 0;
		double BestDot = -MAX_dbl;

		for (int32 I = 0; I < Brush.Faces.Num(); ++I)
		{
			const FPlane& Plane = Brush.Faces[I].Plane;
			if (Plane.Z > BestDot)
			{
				BestDot = Plane.Z;
				Best = I;
			}
		}

		return Best;
	}
}

namespace
{
	/**
	 * Where a new brush goes (S67c): in front of the camera, on the grid, a
	 * cube of the size in the settings, moved sideways until it is clear of
	 * every patch and brush already there - the same place a new patch
	 * goes, so the two arrive side by side.
	 */
	/**
	 * Where the view looks, for a new piece. A replay from the piece's frame
	 * reuses the spot of the first make (S187b): the camera now slides onto
	 * a new piece, so reading the view again walked the piece - and the
	 * camera after it - further off with every change of a number.
	 */
	FVector GNewPieceLookAt = FVector::ZeroVector;
	bool bHasNewPieceLookAt = false;

	/** The new piece was put where the cursor was, on something: it stays there, touching or not. */
	bool bNewPieceAimed = false;

	/**
	 * Where a new piece goes (S189a): "if mouse cursor is over geometry then
	 * create it underneath cursor onto surface". Over our geometry or the
	 * level's, on it - a primitive standing on the spot, a patch centred on
	 * it. Over nothing, at the world's origin (S190d).
	 */
	FVector NewPieceLookAt(bool bReplaying, const USurfEdMode* Mode = nullptr)
	{
		if (bReplaying && bHasNewPieceLookAt)
		{
			return GNewPieceLookAt;
		}
		bNewPieceAimed = false;
		FVector LookAt = FVector::ZeroVector;
		if (FLevelEditorViewportClient* Client = GCurrentLevelEditingViewportClient)
		{
			LookAt = Client->GetLookAtLocation();
			// A flat view has no "ahead": the piece goes to the view's
			// centre, on the plane at depth zero - the ground, seen from above.
			if (!Client->IsPerspective())
			{
				const FVector Depth = FSurfCamera::ViewDirectionOf(Client);
				LookAt = Client->GetViewLocation();
				LookAt -= Depth * FVector::DotProduct(LookAt, Depth);
			}
			else if (!GIsAutomationTesting)
			{
				FViewport* Viewport = Client->Viewport;
				FIntPoint Cursor(-1, -1);
				if (Viewport)
				{
					Viewport->GetMousePos(Cursor);
				}
				const FIntPoint Size = Viewport ? Viewport->GetSizeXY() : FIntPoint::ZeroValue;
				// Away from the edges, where the bar and the frames sit.
				constexpr int32 Margin = 80;
				FVector Hit;
				if (Mode && Viewport && Cursor.X > Margin && Cursor.Y > Margin && Cursor.X < Size.X - Margin && Cursor.Y < Size.Y - Margin
					&& Mode->FindPointUnderCursor(Client, Viewport, Hit))
				{
					LookAt = Hit;
					bNewPieceAimed = true;
				}
				else
				{
					// Over nothing: the world's origin (S190d, his answer:
					// "origin if hovering nothing"); the camera comes to it.
					LookAt = FVector::ZeroVector;
				}
			}
		}
		GNewPieceLookAt = LookAt;
		bHasNewPieceLookAt = true;
		return LookAt;
	}

	FBox PlaceNewBrush(const ASurfGroup& Group, const FVector& LookAt)
	{
		const USurfSettings* Settings = GetDefault<USurfSettings>();
		const double Step = Settings ? FMath::Max(1.0, Settings->GetGridStepCm(Settings->DefaultGridIndex)) : 100.0;
		const double Size = FMath::Max(Step, FMath::RoundToDouble((Settings ? Settings->NewBrushSizeCm : 200.0) / Step) * Step);
		const FVector Extent(Size, Size, Size);

		const FTransform ToLocal = Group.GetActorTransform().Inverse();
		FVector Origin = ToLocal.TransformPosition(LookAt) - FVector(Extent.X, Extent.Y, 0.0) * 0.5;
		Origin.X = FMath::RoundToDouble(Origin.X / Step) * Step;
		Origin.Y = FMath::RoundToDouble(Origin.Y / Step) * Step;
		Origin.Z = FMath::RoundToDouble(Origin.Z / Step) * Step;

		auto Overlaps = [&Group](const FBox& Candidate)
		{
			bool bHit = false;
			Group.ForEachPatch([&](const FSurfPatch& Other)
			{
				if (bHit) { return; }
				FBox Bounds(ForceInit);
				for (const FSurfControlPoint& Point : Other.Points) { Bounds += Point.Pos; }
				bHit = Bounds.IsValid && Bounds.ExpandBy(1.0).Intersect(Candidate);
			});
			Group.ForEachBrush([&](const FSurfBrush& Other)
			{
				if (bHit) { return; }
				FSurfHull Hull;
				if (!SurfBrushMath::DeriveHull(Other, Hull)) { return; }
				FBox Bounds(ForceInit);
				for (const FVector& V : Hull.Vertices) { Bounds += V; }
				bHit = Bounds.IsValid && Bounds.ExpandBy(1.0).Intersect(Candidate);
			});
			return bHit;
		};

		FBox Box(Origin, Origin + Extent);
		for (int32 Try = 0; Try < 64 && !bNewPieceAimed && Overlaps(Box); ++Try)
		{
			Box = Box.ShiftBy(FVector(Extent.X + Step, 0.0, 0.0));
		}
		return Box;
	}
}

void USurfInspector::NewBox()
{
	MakeNewBrush(false);
}

void USurfInspector::NewCylinder()
{
	MakeNewBrush(true);
}

void USurfInspector::MakeNewBrush(bool bCylinder)
{
	if (!Mode.IsValid())
	{
		return;
	}

	FScopedTransaction Transaction(bCylinder ? LOCTEXT("NewCylinder", "New Cylinder") : LOCTEXT("NewBox", "New Box"));

	ASurfGroup* Group = FindOrSpawnGroup(*Mode);
	if (!Group)
	{
		return;
	}

	Group->Modify();

	const USurfSettings* Settings = GetDefault<USurfSettings>();
	const int32 Sides = Settings ? Settings->NewCylinderSides : 8;
	const FBox Box = PlaceNewBrush(*Group, NewPieceLookAt(Mode->bReplayingOptions, Mode.Get()));
	FSurfBrush Brush = bCylinder ? SurfBrushMath::MakeCylinder(Box, Sides) : SurfBrushMath::MakeBox(Box);
	// A cylinder reads round from the start (S171a): its sides share
	// normals under the crease angle, its caps stay hard.
	if (bCylinder)
	{
		Brush.CreaseAngleDeg = static_cast<float>(FMath::Clamp(Settings ? Settings->NewCylinderCreaseDeg : 60.0, 0.0, 180.0));
	}

	// Axial projection at one metre per tile, so a textured material reads
	// correctly the moment it is assigned.
	for (FSurfFace& Face : Brush.Faces)
	{
		SurfProjection::Align(Face.UV, FVector(Face.Plane.X, Face.Plane.Y, Face.Plane.Z));
	}

	const FGuid BrushId = Brush.Id;
	const int32 Top = UpwardFace(Brush);
	Group->AddBrush(MoveTemp(Brush));
	Group->RebuildPreview();

	// The verb states its resulting selection (D16.1): the face you would
	// press a patch button on next.
	FSurfFaceRef Ref;
	Ref.Group = Group;
	Ref.BrushId = BrushId;
	Ref.FaceIndex = Top;

	Mode->SetSubMode(ESurfSubMode::Face);
	Mode->SelectFace(Ref);

	const FVector Size = Box.GetSize() * 0.01;
	SetResult(bCylinder
		? FString::Printf(TEXT("new cylinder: %.1f m across, %.1f m tall, %d sides, its top selected"), Size.X, Size.Z, Sides)
		: FString::Printf(TEXT("new box: %.1f x %.1f x %.1f m, its top selected"), Size.X, Size.Y, Size.Z));
	SurfSession::Verb(bCylinder ? TEXT("new_cylinder") : TEXT("new_box"), FString::Printf(TEXT("size=%.0f sides=%d"), Box.GetSize().X, bCylinder ? Sides : 4));

	PullFromActiveFace();
	Mode->KeepSelectionInView();

	if (GEditor)
	{
		if (Mode.IsValid()) { Mode->RedrawViewports(); } else { GEditor->RedrawLevelEditingViewports(); }
	}
}

void USurfInspector::MakeLattice()
{
	if (!Mode.IsValid())
	{
		return;
	}
	if (Mode->ToggleLattice())
	{
		return;
	}
	const USurfSettings* Settings = GetDefault<USurfSettings>();
	FSurfOperation Op;
	Op.Kind = ESurfOpKind::Lattice;
	Op.Iterations = Settings ? Settings->LatticeResX : 3;
	Op.Segments = Settings ? Settings->LatticeResY : 3;
	Op.Layers = Settings ? Settings->LatticeResZ : 2;
	FScopedTransaction Transaction(LOCTEXT("MakeLattice", "Make Lattice"));
	if (!Mode->MakeLattice(Op.Iterations, Op.Segments, Op.Layers))
	{
		Transaction.Cancel();
		return;
	}
	Mode->NoteOperation(Op);
}

void USurfInspector::NewPatch()
{
	const USurfSettings* Settings = GetDefault<USurfSettings>();
	NewPatchWith(SheetWidth, SheetHeight,
		Settings ? Settings->NewPatchSizeCm : 1000.0,
		static_cast<int32>(ESurfSheetPreset::Flat));
}

void USurfInspector::NewPatchWith(int32 Columns, int32 Rows, double SizeCm, int32 Preset)
{
	if (!Mode.IsValid())
	{
		return;
	}

	FScopedTransaction Transaction(LOCTEXT("NewPatch", "New Patch"));

	ASurfGroup* Group = FindOrSpawnGroup(*Mode);
	if (!Group)
	{
		return;
	}

	Group->Modify();

	// Where the view is looking, on the grid, ten metres each way by
	// default: a patch that appears in front of the camera, big enough to be
	// a floor, its corners on the lattice. Moved sideways until it is clear
	// of any patch already there, so a second one does not land inside the
	// first.
	const USurfSettings* Settings = GetDefault<USurfSettings>();
	const double Step = Settings ? FMath::Max(1.0, Settings->GetGridStepCm(Settings->DefaultGridIndex)) : 100.0;
	Columns = FMath::Clamp(Columns, 2, 32);
	Rows = FMath::Clamp(Rows, 2, 32);
	const double Size = FMath::Max(Step, FMath::RoundToDouble(FMath::Max(Step, SizeCm) / Step) * Step);
	const FVector Extent(Size, Size, 0.0);

	const FVector LookAt = NewPieceLookAt(Mode->bReplayingOptions, Mode.Get());

	const FTransform ToLocal = Group->GetActorTransform().Inverse();
	FVector Origin = ToLocal.TransformPosition(LookAt) - Extent * 0.5;
	Origin.X = FMath::RoundToDouble(Origin.X / Step) * Step;
	Origin.Y = FMath::RoundToDouble(Origin.Y / Step) * Step;
	Origin.Z = FMath::RoundToDouble(Origin.Z / Step) * Step;

	auto Overlaps = [&](const FBox& Candidate)
	{
		bool bHit = false;
		Group->ForEachPatch([&](const FSurfPatch& Other)
		{
			if (bHit)
			{
				return;
			}
			FBox Bounds(ForceInit);
			for (const FSurfControlPoint& Point : Other.Points)
			{
				Bounds += Point.Pos;
			}
			bHit = Bounds.IsValid && Bounds.ExpandBy(1.0).Intersect(Candidate);
		});
		// The brushes too (S68d): a new patch used to land through a box.
		Group->ForEachBrush([&](const FSurfBrush& Other)
		{
			if (bHit)
			{
				return;
			}
			FSurfHull Hull;
			if (!SurfBrushMath::DeriveHull(Other, Hull))
			{
				return;
			}
			FBox Bounds(ForceInit);
			for (const FVector& V : Hull.Vertices)
			{
				Bounds += V;
			}
			bHit = Bounds.IsValid && Bounds.ExpandBy(1.0).Intersect(Candidate);
		});
		return bHit;
	};

	FBox Box(Origin, Origin + Extent);
	for (int32 Try = 0; Try < 64 && !bNewPieceAimed && Overlaps(Box); ++Try)
	{
		Box = Box.ShiftBy(FVector(Extent.X + Step, 0.0, 0.0));
	}

	FSurfPatch Patch = SurfPatchPrimitives::Make(ESurfPrimitiveKind::Sheet, Box, Columns, Rows, FVector::UpVector);

	/**
	 * The shape it starts as (S111a).
	 *
	 * Each one is a height over the grid, so nothing here can fold the patch
	 * or leave a point off the lattice in X or Y - it is still the same sheet,
	 * lifted. The rise is half the patch across, which is the proportion an
	 * arch or a dome reads as at a glance.
	 */
	const ESurfSheetPreset Shape = static_cast<ESurfSheetPreset>(
		FMath::Clamp(Preset, 0, static_cast<int32>(ESurfSheetPreset::Count) - 1));
	const TCHAR* ShapeName = TEXT("flat");
	if (Shape != ESurfSheetPreset::Flat)
	{
		const double Rise = Size * 0.5;
		for (int32 Y = 0; Y < Patch.Height; ++Y)
		{
			const double V = Patch.Height > 1 ? static_cast<double>(Y) / (Patch.Height - 1) : 0.0;
			for (int32 X = 0; X < Patch.Width; ++X)
			{
				const double U = Patch.Width > 1 ? static_cast<double>(X) / (Patch.Width - 1) : 0.0;
				double Lift = 0.0;
				switch (Shape)
				{
				case ESurfSheetPreset::Ramp: Lift = Rise * V; break;
				case ESurfSheetPreset::Arch: Lift = Rise * FMath::Sin(UE_DOUBLE_PI * U); break;
				case ESurfSheetPreset::Dome: Lift = Rise * FMath::Sin(UE_DOUBLE_PI * U) * FMath::Sin(UE_DOUBLE_PI * V); break;
				case ESurfSheetPreset::Bowl: Lift = -Rise * FMath::Sin(UE_DOUBLE_PI * U) * FMath::Sin(UE_DOUBLE_PI * V); break;
				default: break;
				}
				Patch.At(X, Y).Pos.Z += Lift;
			}
		}
		switch (Shape)
		{
		case ESurfSheetPreset::Ramp: ShapeName = TEXT("ramp"); break;
		case ESurfSheetPreset::Arch: ShapeName = TEXT("arch"); break;
		case ESurfSheetPreset::Dome: ShapeName = TEXT("dome"); break;
		case ESurfSheetPreset::Bowl: ShapeName = TEXT("bowl"); break;
		default: break;
		}
	}

	SetResult(FString::Printf(TEXT("new %s patch: %.0f x %.0f m, %d x %d points - the frame has the size, the points and the shape"),
		ShapeName, Size * 0.01, Size * 0.01, Columns, Rows));

	const FGuid PatchId = Patch.Id;
	Group->AddPatch(MoveTemp(Patch));
	Group->RebuildPreview();

	FSurfPatchRef Ref;
	Ref.Group = Group;
	Ref.PatchId = PatchId;
	Mode->SelectPatch(Ref);
	Mode->SetSubMode(ESurfSubMode::Vertex);

	// So the frame comes up over it, with the numbers that made it.
	FSurfOperation Op;
	Op.Kind = ESurfOpKind::NewSheet;
	Op.Iterations = Columns;
	Op.Segments = Rows;
	Op.Distance = Size;
	Op.Preset = static_cast<int32>(Shape);
	Mode->NoteOperation(Op);
	Mode->KeepSelectionInView();

	RefreshSelectionCount();

	if (GEditor)
	{
		if (Mode.IsValid()) { Mode->RedrawViewports(); } else { GEditor->RedrawLevelEditingViewports(); }
	}
}

void USurfInspector::Extrude()
{
	if (Mode.IsValid())
	{
		const USurfSettings* Settings = GetDefault<USurfSettings>();
		const bool bOffset = !Settings || Settings->bExtrudeButtonOffsets;
		Mode->ExtrudeSelectedEdge(/*bInPlace*/ !bOffset);
	}
}

void USurfInspector::Subdivide()
{
	if (Mode.IsValid())
	{
		Mode->SubdivideSelectedPatches(bSubdivideRows, bSubdivideColumns);
	}
}

void USurfInspector::Duplicate()
{
	if (Mode.IsValid())
	{
		Mode->DuplicateSelectionToPatch();
	}
}

void USurfInspector::Split()
{
	if (Mode.IsValid())
	{
		Mode->SplitSelectedPatch();
	}
}

void USurfInspector::Solidify()
{
	if (Mode.IsValid())
	{
		Mode->SolidifySelectedPatch(ThicknessCm, SolidifySide, bSolidifyRim, bSolidifyEven);
	}
}

void USurfInspector::Weld()
{
	if (Mode.IsValid())
	{
		const USurfSettings* Settings = GetDefault<USurfSettings>();
		if (Settings && Settings->bWeldEachCluster)
		{
			Mode->WeldSelectionInClusters(WeldToleranceCm);
			return;
		}
		Mode->WeldSelectedPoints(/*bAtCentre*/ true);
	}
}

void USurfInspector::NextPatch()
{
	if (Mode.IsValid())
	{
		Mode->CyclePatch(1);
	}
}

void USurfInspector::PreviousPatch()
{
	if (Mode.IsValid())
	{
		Mode->CyclePatch(-1);
	}
}

void USurfInspector::Join()
{
	if (Mode.IsValid())
	{
		Mode->JoinSelectedPatches(JoinToleranceCm);
	}
}

void USurfInspector::Bridge()
{
	if (Mode.IsValid())
	{
		Mode->BridgeSelectedLines(bBridgeJoins);
	}
}

void USurfInspector::FaceOutward()
{
	if (Mode.IsValid())
	{
		Mode->FaceSelectionOutward();
	}
}

void USurfInspector::Steps()
{
	if (!Mode.IsValid())
	{
		return;
	}
	// The settings are read here and nowhere else: the verb starts from them,
	// the adjust frame goes on from what the verb worked out.
	const USurfSettings* Settings = GetDefault<USurfSettings>();
	Mode->StepsBetweenSelectedLines(
		Settings ? Settings->StepCount : 8,
		Settings ? Settings->StepRiseCm : 0.0);
}

void USurfInspector::Continue()
{
	if (Mode.IsValid())
	{
		Mode->ContinueSelectedBorder();
	}
}

void USurfInspector::ExtrudeBrushFace()
{
	if (!Mode.IsValid())
	{
		return;
	}

	// A step of the grid, so the verb alone makes something you can see;
	// the key does it in place and hands it to the move.
	const USurfSettings* Settings = GetDefault<USurfSettings>();
	const double Step = Settings ? Settings->GetGridStepCm(Settings->DefaultGridIndex) : 100.0;

	FScopedTransaction Transaction(LOCTEXT("ExtrudeBrushVerb", "Extrude Primitive Face"));
	if (Mode->ExtrudeSelectedBrushFaces(Step) == 0)
	{
		Transaction.Cancel();
	}
}

void USurfInspector::Separate()
{
	if (Mode.IsValid())
	{
		Mode->SeparateSelectedFaces();
	}
}

void USurfInspector::WeldByDistance()
{
	if (Mode.IsValid())
	{
		Mode->WeldSelectionByDistance(WeldToleranceCm);
	}
}

void USurfInspector::FlipNormals()
{
	if (!Mode.IsValid()) { return; }

	RunPatchVerb(*Mode, LOCTEXT("FlipNormals", "Flip Normals"), [](FSurfPatch& Patch)
	{
		Patch.bInvert = !Patch.bInvert;
	});

	if (const ASurfGroup* Group = Mode->GetSelectedPatch().Group.Get())
	{
		if (const FSurfPatch* Patch = Group->FindPatch(Mode->GetSelectedPatch().PatchId))
		{
			SetResult(Patch->bInvert ? TEXT("normals flipped") : TEXT("normals back to front"));
		}
	}

	RefreshSelectionCount();

	if (GEditor)
	{
		if (Mode.IsValid()) { Mode->RedrawViewports(); } else { GEditor->RedrawLevelEditingViewports(); }
	}
}

void USurfInspector::ApplyLayer()
{
	if (!Mode.IsValid()) { return; }

	const int32 Value = FMath::Clamp(Layer, 0, 4);

	RunPatchVerb(*Mode, LOCTEXT("SetLayer", "Set Layer"), [Value](FSurfPatch& Patch)
	{
		Patch.StackLevel = Value;
	});

	SetResult(Value == 0
		? TEXT("layer 0 - the opaque surface")
		: FString::Printf(TEXT("layer %d - drawn over layer %d, lifted %d x the offset at build"), Value, Value - 1, Value));

	RefreshSelectionCount();

	if (GEditor)
	{
		if (Mode.IsValid()) { Mode->RedrawViewports(); } else { GEditor->RedrawLevelEditingViewports(); }
	}
}

// The mode says what happened, in both directions, because a refusal that
// only returns false is a button that does nothing for no stated reason.
// The layer is an attribute of the patch - 0 opaque, 1, 2, 3 drawn over it
// in turn (D64) - so the bar's verb sets it on what is selected rather than
// making anything. Pressed again it goes round: a hand looking for "the next
// one up" gets it without a menu, and the number is on the bar as it goes.
int32 USurfInspector::GetActiveLayer() const
{
	if (!Mode.IsValid())
	{
		return -1;
	}
	const FSurfPatchRef Active = Mode->GetSelectedPatch();
	ASurfGroup* Group = Active.Group.Get();
	const FSurfPatch* Patch = Group ? Group->FindPatch(Active.PatchId) : nullptr;
	if (!Patch)
	{
		TArray<FSurfPatchRef> Held = Mode->PatchesHeldBySelection();
		if (Held.Num() > 0)
		{
			Group = Held[0].Group.Get();
			Patch = Group ? Group->FindPatch(Held[0].PatchId) : nullptr;
		}
	}
	return Patch ? Patch->StackLevel : -1;
}

void USurfInspector::SetLayer(int32 Level)
{
	Layer = FMath::Clamp(Level, 0, 4);
	ApplyLayer();
}

void USurfInspector::CycleLayer()
{
	const int32 Was = GetActiveLayer();
	if (Was < 0)
	{
		SetResult(TEXT("layer: nothing selected - pick the patch to set the level of"));
		return;
	}
	SetLayer((Was + 1) % 4);
}

void USurfInspector::MakeLayer()
{
	if (Mode.IsValid())
	{
		Mode->MakeLayerFromSelection();
	}
}

void USurfInspector::ApplyShading()
{
	if (!Mode.IsValid()) { return; }

	const ESurfShading Wanted = Shading;
	const double Angle = FMath::Clamp(SmoothingAngleDegrees, 1.0, 179.0);

	RunPatchVerb(*Mode, LOCTEXT("SetShading", "Set Shading"), [Wanted, Angle](FSurfPatch& Patch)
	{
		Patch.Shading = Wanted;
		Patch.SmoothingAngleDegrees = Angle;
	});

	SetResult(
		Wanted == ESurfShading::Smooth  ? FString(TEXT("smooth: one normal per control point")) :
		Wanted == ESurfShading::Faceted ? FString(TEXT("faceted: every triangle flat"))
		: FString::Printf(TEXT("hard past %.0f degrees"), Angle));

	RefreshSelectionCount();

	if (GEditor)
	{
		if (Mode.IsValid()) { Mode->RedrawViewports(); } else { GEditor->RedrawLevelEditingViewports(); }
	}
}

// --- Patch surfacing -------------------------------------------------------

bool USurfInspector::ApplySurface(const FSurfOperation& Op)
{
	if (!Mode.IsValid())
	{
		return false;
	}

	const TArray<FSurfPatchRef> Targets = PatchesOf(*Mode);

	// A brush face is a surface too (§2 for brushes, S67a): the selected
	// faces run under the same verb as the patches, with the same knobs, in
	// the same transaction. A face is flat, so Flow on one is Align; the
	// rest carries over as it reads.
	TArray<FSurfFaceRef> FaceTargets;
	for (const FSurfFaceRef& Ref : Mode->GetSelection())
	{
		const ASurfGroup* Group = Ref.Group.Get();
		const FSurfBrush* Brush = Group ? Group->FindBrush(Ref.BrushId) : nullptr;
		if (Brush && Brush->Faces.IsValidIndex(Ref.FaceIndex))
		{
			FaceTargets.AddUnique(Ref);
		}
	}

	if (Targets.Num() == 0 && FaceTargets.Num() == 0)
	{
		SetResult(TEXT("surface: select a patch or a primitive face first"));
		RefreshSelectionCount();
		SurfSession::Friction(TEXT("surface_no_patch"));
		return false;
	}

	FSurfOperation Noted = Op;

	auto NormalOf = [](const FSurfFace& Face)
	{
		return FVector(Face.Plane.X, Face.Plane.Y, Face.Plane.Z).GetSafeNormal();
	};

	// Metres per tile: the knob's number. Zero means "leave each patch's own
	// alone", and the knob is filled in from the active patch so it starts
	// where the surface already is rather than at one metre.
	// The patch the numbers are read from is the active one - the last picked,
	// as Join's survivor is - so "click the wall that is right, then the ones
	// to match" is the gesture. It falls back to the first when the active
	// patch is not in the selection at all; with no patch at all, the active
	// brush face is the reference, else the first face picked.
	FSurfPatchRef Reference = Mode->GetSelectedPatch();
	if (!Reference.IsValid() || !Targets.Contains(Reference))
	{
		Reference = Targets.Num() > 0 ? Targets[0] : FSurfPatchRef();
	}
	FSurfFaceRef FaceReference;
	if (!Reference.IsValid())
	{
		FaceReference = Mode->GetActiveFace();
		if (!FaceReference.IsValid() || !FaceTargets.Contains(FaceReference))
		{
			FaceReference = FaceTargets[0];
		}
	}
	const ASurfGroup* ActiveGroup = Reference.IsValid() ? Reference.Group.Get() : FaceReference.Group.Get();
	const FSurfPatch* Active = (Reference.IsValid() && ActiveGroup) ? ActiveGroup->FindPatch(Reference.PatchId) : nullptr;
	const FSurfFace* ActiveFacePtr = nullptr;
	if (!Active && ActiveGroup)
	{
		const FSurfBrush* Brush = ActiveGroup->FindBrush(FaceReference.BrushId);
		if (Brush && Brush->Faces.IsValidIndex(FaceReference.FaceIndex))
		{
			ActiveFacePtr = &Brush->Faces[FaceReference.FaceIndex];
		}
	}
	const FVector2D ActiveScale = Active ? Active->Scale : ActiveFacePtr ? ActiveFacePtr->UV.Scale : FVector2D(1.0, 1.0);
	const double ActiveTurn = Active ? Active->UVRotation
		: ActiveFacePtr ? SurfProjection::GetRotationDegrees(ActiveFacePtr->UV, NormalOf(*ActiveFacePtr)) : 0.0;
	const bool bKeepScale = Noted.Tiles.X <= 0.0 || Noted.Tiles.Y <= 0.0;
	if (bKeepScale)
	{
		Noted.Tiles = ActiveScale;
	}
	Noted.Tiles.X = FMath::Max(0.01, Noted.Tiles.X);
	Noted.Tiles.Y = FMath::Max(0.01, Noted.Tiles.Y);

	// The first time through, the reference's own numbers are taken so the
	// knobs have something to be relative to; on a replay they are what
	// they were, because the replay undid the verb first and the patches
	// are back where they started. In relative mode the panel's turn is a
	// delta on the reference's - a Flow on a wall turned thirty degrees
	// keeps it at thirty, and the knob says thirty; in match mode it is
	// the number itself, as it always was.
	if (!Noted.bReferenceKnown)
	{
		Noted.ReferenceTiles = ActiveScale;
		Noted.ReferenceRotation = ActiveTurn;
		Noted.bReferenceKnown = true;
		if (!Noted.bMatchFirst)
		{
			Noted.SurfaceRotation = ActiveTurn + Op.SurfaceRotation;
		}

		// Even starts every patch at one tile a cell, and its scale is
		// cells a tile from then on, so the knob starts at one and the
		// ratio is one until it is moved.
		if (Op.SurfaceVerb == ESurfSurfaceVerb::Even)
		{
			Noted.ReferenceTiles = FVector2D(1.0, 1.0);
			if (bKeepScale)
			{
				Noted.Tiles = FVector2D(1.0, 1.0);
			}
		}
	}
	Noted.ReferenceTiles.X = FMath::Max(0.01, Noted.ReferenceTiles.X);
	Noted.ReferenceTiles.Y = FMath::Max(0.01, Noted.ReferenceTiles.Y);
	const FVector2D Ratio(Noted.Tiles.X / Noted.ReferenceTiles.X, Noted.Tiles.Y / Noted.ReferenceTiles.Y);
	const double TurnDelta = Noted.SurfaceRotation - Noted.ReferenceRotation;

	// What a surface's metres a tile become under the knob.
	auto ScaleFor = [&](const FVector2D& Own) -> FVector2D
	{
		return Noted.bMatchFirst
			? Noted.Tiles
			: FVector2D(Own.X * Ratio.X, Own.Y * Ratio.Y);
	};
	Noted.RepeatU = FMath::Clamp(Noted.RepeatU, 1, 64);
	Noted.RepeatV = FMath::Clamp(Noted.RepeatV, 1, 64);
	Noted.Axis = FMath::Clamp(Noted.Axis, 0, 3);

	// Normalise: the density to match. The active patch's unless one was
	// typed - which is the number that says whether a wall matches its
	// neighbours, and the only one worth carrying between patches.
	if (Op.SurfaceVerb == ESurfSurfaceVerb::Normalise && Noted.Density <= 0.0)
	{
		const FVector2D Was = Active ? SurfPatchSurfacing::TexelDensity(*Active, TextureTexels)
			: ActiveFacePtr ? SurfProjection::TexelDensity(ActiveFacePtr->UV, TextureTexels) : FVector2D::ZeroVector;
		Noted.Density = FMath::Max(Was.X, Was.Y);
	}

	// Project: one projection for the whole selection, built once. That is the
	// difference between Project and Align - Align asks each patch for its own
	// normal, so two patches that meet at an angle get two mappings and the
	// seam shows; Project hands them the same axes, and a wall that bends
	// carries its texture round the bend.
	FSurfFaceUV Shared;
	FVector AxisNormal = FVector(0.0, 0.0, 1.0);
	if (Op.SurfaceVerb == ESurfSurfaceVerb::Project)
	{
		FVector Facing = FVector::ZeroVector;
		for (const FSurfPatchRef& Ref : Targets)
		{
			const ASurfGroup* Group = Ref.Group.Get();
			if (const FSurfPatch* Patch = Group ? Group->FindPatch(Ref.PatchId) : nullptr)
			{
				Facing += SurfPatchSurfacing::AverageNormal(*Patch);
			}
		}
		for (const FSurfFaceRef& Ref : FaceTargets)
		{
			const ASurfGroup* Group = Ref.Group.Get();
			const FSurfBrush* Brush = Group ? Group->FindBrush(Ref.BrushId) : nullptr;
			if (Brush)
			{
				Facing += NormalOf(Brush->Faces[Ref.FaceIndex]);
			}
		}
		Facing = Facing.GetSafeNormal();

		// From the camera (S230a): the view's own direction, towards the eye.
		FVector FromCamera = FVector::ZeroVector;
		if (Noted.Axis == 4 && GCurrentLevelEditingViewportClient)
		{
			FromCamera = -GCurrentLevelEditingViewportClient->GetViewRotation().Vector();
		}
		AxisNormal =
			Noted.Axis == 1 ? FVector(1.0, 0.0, 0.0) :
			Noted.Axis == 2 ? FVector(0.0, 1.0, 0.0) :
			Noted.Axis == 3 ? FVector(0.0, 0.0, 1.0) :
			(Noted.Axis == 4 && !FromCamera.IsNearlyZero()) ? FromCamera : Facing;
		if (AxisNormal.IsNearlyZero())
		{
			AxisNormal = FVector(0.0, 0.0, 1.0);
		}

		Shared.Scale = Noted.Tiles;
		Shared.Shift = FVector2D::ZeroVector;
		SurfProjection::Align(Shared, AxisNormal);
	}

	static const TCHAR* Names[] = { TEXT("Flow"), TEXT("Align"), TEXT("Project"), TEXT("Fit"), TEXT("Match density"), TEXT("Even") };
	const int32 Which = FMath::Clamp(static_cast<int32>(Op.SurfaceVerb), 0, 5);

	FScopedTransaction Transaction(FText::FromString(Names[Which]));

	TArray<ASurfGroup*> Groups;
	int32 Done = 0;
	int32 DoneFaces = 0;
	int32 Trimmed = 0;
	for (const FSurfPatchRef& Ref : Targets)
	{
		ASurfGroup* Group = Ref.Group.Get();
		FSurfPatch* Patch = Group ? Group->ModifyPatch(Ref.PatchId) : nullptr;
		if (!Patch)
		{
			continue;
		}

		const double OwnTurn = Patch->UVRotation;

		switch (Op.SurfaceVerb)
		{
		case ESurfSurfaceVerb::Flow:
			if (!bKeepScale || !Noted.bMatchFirst) { SurfPatchSurfacing::SetScale(*Patch, ScaleFor(Patch->Scale)); }
			SurfPatchSurfacing::SetNatural(*Patch);
			break;

		case ESurfSurfaceVerb::Align:
			if (!bKeepScale || !Noted.bMatchFirst) { SurfPatchSurfacing::SetScale(*Patch, ScaleFor(Patch->Scale)); }
			SurfPatchSurfacing::SetPlanar(*Patch);
			break;

		case ESurfSurfaceVerb::Even:
		{
			// One tile a cell, then the knob from there: the ratio on the
			// cell in relative mode, the number outright in match mode.
			SurfPatchSurfacing::SetEven(*Patch);
			if (!bKeepScale || !Noted.bMatchFirst) { SurfPatchSurfacing::SetScale(*Patch, ScaleFor(Patch->Scale)); }
			break;
		}

		case ESurfSurfaceVerb::Project:
		{
			FSurfFaceUV Projection = Shared;
			Projection.Scale = (bKeepScale && Noted.bMatchFirst) ? Patch->Scale : ScaleFor(Patch->Scale);
			SurfPatchSurfacing::SetPlanarWith(*Patch, Projection);
			break;
		}

		case ESurfSurfaceVerb::Fit:
		{
			// A material that declares a trim's cells is fitted to cells,
			// not tiles: the number of tiles asked for is the number of
			// cells the surface spans, and the mapping lands on a cell.
			const USurfMaterialInfo* Info = USurfMaterialInfo::Find(Patch->Material.Get());
			const FVector2D Cells = Info ? Info->TrimCellsPerTile : FVector2D::ZeroVector;
			if (Cells.X >= 1.0 && Cells.Y >= 1.0)
			{
				SurfPatchSurfacing::FitToTrim(*Patch, Noted.RepeatU, Noted.RepeatV, Cells);
				++Trimmed;
			}
			else
			{
				SurfPatchSurfacing::FitScale(*Patch, Noted.RepeatU, Noted.RepeatV);
			}
			break;
		}

		case ESurfSurfaceVerb::Normalise:
		{
			// Density is texels over metres of surface, and the scale is
			// metres per tile: twice the scale is half the density. So the
			// scale each patch needs is its own, times how far its density
			// is from the one to match - measured, not derived, so a
			// swapped or turned mapping is measured as it looks.
			const FVector2D Now = SurfPatchSurfacing::TexelDensity(*Patch, TextureTexels);
			if (Noted.Density > 0.0 && Now.X > 0.0 && Now.Y > 0.0)
			{
				SurfPatchSurfacing::SetScale(*Patch, FVector2D(
					Patch->Scale.X * (Now.X / Noted.Density),
					Patch->Scale.Y * (Now.Y / Noted.Density)));
			}
			break;
		}
		}

		// The turn is a post-step, set rather than accumulated, so the number
		// in the frame is the angle the reference is at: every patch takes
		// it outright in match mode, and turns by the reference's change in
		// relative mode.
		Patch->UVRotation = 0.0;
		const double Turn = Noted.bMatchFirst ? Noted.SurfaceRotation : OwnTurn + TurnDelta;
		if (!FMath::IsNearlyZero(Turn))
		{
			SurfPatchSurfacing::Rotate(*Patch, Turn);
		}

		SurfPatchSurfacing::Reapply(*Patch);
		++Patch->EditSerial;
		Groups.AddUnique(Group);
		++Done;
	}

	// The brush faces. A face's mapping is its two axes, so the turn is the
	// axes turned: about the face's own direction after Align, about the
	// projection's after Project, and otherwise about whatever plane the
	// mapping already lies in - so a projected face turned in the frame stays
	// projected, and an untouched knob moves nothing.
	for (const FSurfFaceRef& Ref : FaceTargets)
	{
		ASurfGroup* Group = Ref.Group.Get();
		FSurfBrush* Brush = Group ? Group->ModifyBrush(Ref.BrushId) : nullptr;
		if (!Brush || !Brush->Faces.IsValidIndex(Ref.FaceIndex))
		{
			continue;
		}
		FSurfFace& Face = Brush->Faces[Ref.FaceIndex];
		const FVector Normal = NormalOf(Face);
		if (Normal.IsNearlyZero())
		{
			continue;
		}

		const double OwnTurn = SurfProjection::GetRotationDegrees(Face.UV, Normal);
		const double Turn = Noted.bMatchFirst ? Noted.SurfaceRotation : OwnTurn + TurnDelta;

		switch (Op.SurfaceVerb)
		{
		case ESurfSurfaceVerb::Flow:
		case ESurfSurfaceVerb::Align:
		case ESurfSurfaceVerb::Even:
			SurfProjection::Align(Face.UV, Normal);
			if (!bKeepScale || !Noted.bMatchFirst) { Face.UV.Scale = ScaleFor(Face.UV.Scale); }
			break;

		case ESurfSurfaceVerb::Project:
			Face.UV.UAxis = Shared.UAxis;
			Face.UV.VAxis = Shared.VAxis;
			Face.UV.Shift = FVector2D::ZeroVector;
			Face.UV.Scale = (bKeepScale && Noted.bMatchFirst) ? Face.UV.Scale : ScaleFor(Face.UV.Scale);
			break;

		case ESurfSurfaceVerb::Fit:
		{
			TArray<FVector> Corners;
			if (GatherFaceVertices(*Group, *Brush, Ref.FaceIndex, Corners))
			{
				SurfProjection::Fit(Face.UV, Corners, Noted.RepeatU, Noted.RepeatV);

				// A trim: the count asked for is cells, so a tile is that
				// many cells wide - and Fit landed the shift on the face's
				// edge, which is a cell boundary.
				const USurfMaterialInfo* Info = USurfMaterialInfo::Find(Face.Material.Get());
				const FVector2D Cells = Info ? Info->TrimCellsPerTile : FVector2D::ZeroVector;
				if (Cells.X >= 1.0 && Cells.Y >= 1.0)
				{
					Face.UV.Scale.X *= Cells.X;
					Face.UV.Scale.Y *= Cells.Y;
					++Trimmed;
				}
			}
			break;
		}

		case ESurfSurfaceVerb::Normalise:
		{
			const FVector2D Now = SurfProjection::TexelDensity(Face.UV, TextureTexels);
			if (Noted.Density > 0.0 && Now.X > 0.0 && Now.Y > 0.0)
			{
				Face.UV.Scale.X *= Now.X / Noted.Density;
				Face.UV.Scale.Y *= Now.Y / Noted.Density;
			}
			break;
		}
		}

		if (Op.SurfaceVerb == ESurfSurfaceVerb::Project)
		{
			if (!FMath::IsNearlyZero(Turn))
			{
				const FQuat Spin(AxisNormal, FMath::DegreesToRadians(Turn));
				Face.UV.UAxis = Spin.RotateVector(Face.UV.UAxis).GetSafeNormal();
				Face.UV.VAxis = Spin.RotateVector(Face.UV.VAxis).GetSafeNormal();
			}
		}
		else
		{
			const double Now = SurfProjection::GetRotationDegrees(Face.UV, Normal);
			const double Delta = Turn - Now;
			if (!FMath::IsNearlyZero(Delta))
			{
				FVector About = FVector::CrossProduct(Face.UV.UAxis, Face.UV.VAxis).GetSafeNormal();
				if (About.IsNearlyZero())
				{
					About = Normal;
				}
				else if (FVector::DotProduct(About, Normal) < 0.0)
				{
					About = -About;
				}
				const FQuat Spin(About, FMath::DegreesToRadians(Delta));
				Face.UV.UAxis = Spin.RotateVector(Face.UV.UAxis).GetSafeNormal();
				Face.UV.VAxis = Spin.RotateVector(Face.UV.VAxis).GetSafeNormal();
			}
		}

		++Brush->EditSerial;
		Groups.AddUnique(Group);
		++DoneFaces;
	}

	if (Done == 0 && DoneFaces == 0)
	{
		Transaction.Cancel();
		return false;
	}

	for (ASurfGroup* Each : Groups)
	{
		Each->RebuildPreview();
	}

	// The numbers the verb settled on go back into the panel, so the next
	// press starts from them.
	if (Op.SurfaceVerb == ESurfSurfaceVerb::Fit)
	{
		FitRepeatU = Noted.RepeatU;
		FitRepeatV = Noted.RepeatV;
		if (const FSurfPatch* Fitted = (Active && ActiveGroup) ? ActiveGroup->FindPatch(Reference.PatchId) : nullptr)
		{
			Noted.Tiles = Fitted->Scale;
		}
		else if (ActiveFacePtr)
		{
			Noted.Tiles = ActiveFacePtr->UV.Scale;
		}
	}
	Scale = Noted.Tiles;
	RotationDegrees = Noted.bMatchFirst ? Noted.SurfaceRotation : RotationDegrees;
	ProjectAxis = Noted.Axis;
	if (Trimmed > 0)
	{
		SetResult(FString::Printf(TEXT("fit %d surface%s to the trim's cells, %d across and %d down, on a cell"),
			Trimmed, Trimmed == 1 ? TEXT("") : TEXT("s"), Noted.RepeatU, Noted.RepeatV));
	}

	static const TCHAR* Axes[] = { TEXT("its own direction"), TEXT("X"), TEXT("Y"), TEXT("Z") };
	FString On;
	if (Done + DoneFaces > 1)
	{
		const FString PatchPart = Done > 0 ? FString::Printf(TEXT("%d patch%s"), Done, Done == 1 ? TEXT("") : TEXT("es")) : FString();
		const FString FacePart = DoneFaces > 0 ? FString::Printf(TEXT("%d face%s"), DoneFaces, DoneFaces == 1 ? TEXT("") : TEXT("s")) : FString();
		On = TEXT(" on ") + PatchPart + (Done > 0 && DoneFaces > 0 ? TEXT(" and ") : TEXT("")) + FacePart;
	}
	else if (DoneFaces == 1 && Done == 0)
	{
		On = TEXT(" on the face");
	}
	SetResult(
		Op.SurfaceVerb == ESurfSurfaceVerb::Flow      ? FString::Printf(TEXT("flow%s: the texture follows the surface, %.2f x %.2f m a tile"), *On, Noted.Tiles.X, Noted.Tiles.Y) :
		Op.SurfaceVerb == ESurfSurfaceVerb::Align     ? FString::Printf(TEXT("aligned%s: projected from each surface's own direction, %.2f x %.2f m a tile"), *On, Noted.Tiles.X, Noted.Tiles.Y) :
		Op.SurfaceVerb == ESurfSurfaceVerb::Project   ? FString::Printf(TEXT("projected%s along %s: one mapping, no seam where they meet"), *On, Axes[Noted.Axis]) :
		Op.SurfaceVerb == ESurfSurfaceVerb::Normalise ? FString::Printf(TEXT("matched%s to %.0f texels a metre"), *On, Noted.Density) :
		Op.SurfaceVerb == ESurfSurfaceVerb::Even      ? FString::Printf(TEXT("even%s: every cell the same size in the texture, a tile every %.2f x %.2f cells - the knob counts cells here, not metres"), *On, Noted.Tiles.X, Noted.Tiles.Y) :
		FString::Printf(TEXT("fitted%s: %d x %d tiles across, %.2f x %.2f m a tile"), *On, Noted.RepeatU, Noted.RepeatV, Noted.Tiles.X, Noted.Tiles.Y));

	Mode->NoteOperation(Noted);
	RefreshSelectionCount();

	SurfSession::Verb(TEXT("surface"), FString::Printf(TEXT("%s patches=%d faces=%d tiles=%.2fx%.2f turn=%.0f axis=%d"),
		Names[Which], Done, DoneFaces, Noted.Tiles.X, Noted.Tiles.Y, Noted.SurfaceRotation, Noted.Axis));

	if (GEditor)
	{
		if (Mode.IsValid()) { Mode->RedrawViewports(); } else { GEditor->RedrawLevelEditingViewports(); }
	}

	return true;
}

void USurfInspector::Natural()
{
	FSurfOperation Op;
	Op.Kind = ESurfOpKind::Surface;
	Op.SurfaceVerb = ESurfSurfaceVerb::Flow;
	Op.SurfaceRotation = RotationDegrees;
	ApplySurface(Op);
}

void USurfInspector::Even()
{
	FSurfOperation Op;
	Op.Kind = ESurfOpKind::Surface;
	Op.SurfaceVerb = ESurfSurfaceVerb::Even;
	Op.SurfaceRotation = RotationDegrees;
	ApplySurface(Op);
}

void USurfInspector::Planar()
{
	FSurfOperation Op;
	Op.Kind = ESurfOpKind::Surface;
	Op.SurfaceVerb = ESurfSurfaceVerb::Align;
	Op.SurfaceRotation = RotationDegrees;
	ApplySurface(Op);
}

void USurfInspector::Project()
{
	FSurfOperation Op;
	Op.Kind = ESurfOpKind::Surface;
	Op.SurfaceVerb = ESurfSurfaceVerb::Project;
	Op.SurfaceRotation = RotationDegrees;
	Op.Axis = ProjectAxis;
	ApplySurface(Op);
}

void USurfInspector::NormaliseDensity()
{
	FSurfOperation Op;
	Op.Kind = ESurfOpKind::Surface;
	Op.SurfaceVerb = ESurfSurfaceVerb::Normalise;
	ApplySurface(Op);
}

void USurfInspector::FitPatch()
{
	FSurfOperation Op;
	Op.Kind = ESurfOpKind::Surface;
	Op.SurfaceVerb = ESurfSurfaceVerb::Fit;
	Op.SurfaceRotation = RotationDegrees;
	Op.RepeatU = FitRepeatU;
	Op.RepeatV = FitRepeatV;
	ApplySurface(Op);
}

void USurfInspector::InheritFaceSurface()
{
	if (!Mode.IsValid()) { return; }

	// The gesture that closes a seam: click the wall, then the patch, then
	// this. The patch takes the wall's projection whole, axes included.
	const FSurfFaceRef Active = Mode->GetActiveFace();
	const ASurfGroup* Group = Active.Group.Get();
	const FSurfBrush* Brush = Group ? Group->FindBrush(Active.BrushId) : nullptr;

	if (!Brush || !Brush->Faces.IsValidIndex(Active.FaceIndex))
	{
		return;
	}

	const FSurfFace Face = Brush->Faces[Active.FaceIndex];

	RunPatchVerb(*Mode, LOCTEXT("InheritSurface", "Inherit Face Surface"), [&Face](FSurfPatch& Patch)
	{
		SurfPatchSurfacing::CopyFaceSurface(Face, Patch);
	});
	RefreshSelectionCount();
}

void USurfInspector::Freeze()
{
	if (!Mode.IsValid()) { return; }
	RunPatchVerb(*Mode, LOCTEXT("Freeze", "Freeze Surfacing"), [](FSurfPatch& Patch)
	{
		SurfPatchSurfacing::SetManual(Patch);
	});
	RefreshSelectionCount();
}

void USurfInspector::ApplyPatchScale()
{
	if (!Mode.IsValid()) { return; }
	const FVector2D Value = Scale;

	RunPatchVerb(*Mode, LOCTEXT("PatchScale", "Set Patch Scale"), [Value](FSurfPatch& Patch)
	{
		SurfPatchSurfacing::SetScale(Patch, Value);
	});
	RefreshSelectionCount();
}

void USurfInspector::RotatePatchSurface()
{
	if (!Mode.IsValid()) { return; }
	const double Degrees = RotationDegrees;

	RunPatchVerb(*Mode, LOCTEXT("RotatePatch", "Rotate Patch Surface"), [Degrees](FSurfPatch& Patch)
	{
		// Set rather than accumulate, so the panel's number is what you get.
		Patch.UVRotation = 0.0;
		SurfPatchSurfacing::Rotate(Patch, Degrees);
	});
	RefreshSelectionCount();
}

void USurfInspector::FlipPatchU()
{
	if (!Mode.IsValid()) { return; }
	RunPatchVerb(*Mode, LOCTEXT("FlipPatchU", "Flip Patch U"), [](FSurfPatch& Patch)
	{
		SurfPatchSurfacing::FlipU(Patch);
	});
}

void USurfInspector::FlipPatchV()
{
	if (!Mode.IsValid()) { return; }
	RunPatchVerb(*Mode, LOCTEXT("FlipPatchV", "Flip Patch V"), [](FSurfPatch& Patch)
	{
		SurfPatchSurfacing::FlipV(Patch);
	});
}

void USurfInspector::SwapPatchUV()
{
	if (!Mode.IsValid()) { return; }
	RunPatchVerb(*Mode, LOCTEXT("SwapPatchUV", "Swap Patch UV"), [](FSurfPatch& Patch)
	{
		SurfPatchSurfacing::SwapUV(Patch);
	});
}

void USurfInspector::ShiftPatchU()
{
	if (!Mode.IsValid()) { return; }
	const double Tiles = PatchShiftTiles.X;

	RunPatchVerb(*Mode, LOCTEXT("ShiftPatchU", "Shift Patch U"), [Tiles](FSurfPatch& Patch)
	{
		SurfPatchSurfacing::Shift(Patch, Tiles, 0.0);
	});
}

void USurfInspector::ShiftPatchV()
{
	if (!Mode.IsValid()) { return; }
	const double Tiles = PatchShiftTiles.Y;

	RunPatchVerb(*Mode, LOCTEXT("ShiftPatchV", "Shift Patch V"), [Tiles](FSurfPatch& Patch)
	{
		SurfPatchSurfacing::Shift(Patch, 0.0, Tiles);
	});
}

void USurfInspector::ConvertToSheet()
{
	if (!Mode.IsValid())
	{
		return;
	}

	SurfSession::Verb(TEXT("panel_convert_to_sheet"));
	Mode->ConvertSelectedPatchToSheet();
	RefreshSelectionCount();
}

// --- Live operators ---------------------------------------------------------

namespace
{
	/** Indices of the selected control points, or empty to mean the whole patch. */
	TArray<int32> SelectedIndices(const USurfEdMode& Mode, const FSurfPatch& Patch)
	{
		TArray<int32> Indices;
		for (const FSurfPointRef& Ref : Mode.GetPointSelection())
		{
			if (Ref.PatchId == Patch.Id && Ref.X >= 0 && Ref.X < Patch.Width && Ref.Y >= 0 && Ref.Y < Patch.Height)
			{
				Indices.Add(Ref.Y * Patch.Width + Ref.X);
			}
		}
		return Indices;
	}
}

namespace
{
	/** One line to bevel: a row or a column of the patch, by index. */
	struct FSurfBevelLine
	{
		bool bColumn = false;
		int32 Index = INDEX_NONE;
	};

	/**
	 * The lines the point selection names, in the order they run.
	 *
	 * A line is named by an edge along it: two selected points next to each
	 * other on a column name the column, on a row the row. A whole row
	 * selected is one row; several edges are several lines. Two vertical
	 * edges side by side are also two horizontal pairs, so when both axes
	 * are named the axis of the last edge picked wins, then the axis with
	 * more lines. Border lines are left out - there is no crease to round
	 * on the edge of a patch - and a selection of only border lines names
	 * nothing, which the caller refuses with a reason.
	 */
	void GetBevelLines(const USurfEdMode& Mode, const FSurfPatch& Patch, TArray<FSurfBevelLine>& OutLines)
	{
		OutLines.Reset();

		const TArray<FSurfPointRef>& Points = Mode.GetPointSelection();
		if (Points.Num() == 0)
		{
			return;
		}

		TSet<FIntPoint> Selected;
		for (const FSurfPointRef& Ref : Points)
		{
			if (Ref.PatchId == Patch.Id && Ref.X >= 0 && Ref.X < Patch.Width && Ref.Y >= 0 && Ref.Y < Patch.Height)
			{
				Selected.Add(FIntPoint(Ref.X, Ref.Y));
			}
		}

		TSet<int32> Columns;
		TSet<int32> Rows;
		for (const FIntPoint& P : Selected)
		{
			if (Selected.Contains(FIntPoint(P.X, P.Y + 1)) && P.X > 0 && P.X < Patch.Width - 1)
			{
				Columns.Add(P.X);
			}
			if (Selected.Contains(FIntPoint(P.X + 1, P.Y)) && P.Y > 0 && P.Y < Patch.Height - 1)
			{
				Rows.Add(P.Y);
			}
		}

		// One lone point still names its own lines, as it always did.
		if (Columns.Num() == 0 && Rows.Num() == 0 && Selected.Num() == 1)
		{
			const FIntPoint P = *Selected.CreateConstIterator();
			if (P.X > 0 && P.X < Patch.Width - 1) { Columns.Add(P.X); }
			else if (P.Y > 0 && P.Y < Patch.Height - 1) { Rows.Add(P.Y); }
		}

		bool bColumns = Columns.Num() > 0;
		if (Columns.Num() > 0 && Rows.Num() > 0)
		{
			const int32 Axis = Mode.GetLastEdgePickAxis();
			bColumns = Axis == 1 ? true : (Axis == 2 ? false : Columns.Num() >= Rows.Num());
		}

		TArray<int32> Indices = bColumns ? Columns.Array() : Rows.Array();
		Indices.Sort();
		for (int32 Index : Indices)
		{
			FSurfBevelLine Line;
			Line.bColumn = bColumns;
			Line.Index = Index;
			OutLines.Add(Line);
		}
	}

	/**
	 * The most a bevel of these lines can spread before it meets the next
	 * line: the shortest gap across the crease (not along it - the spacing
	 * of the points along the line has nothing to do with how wide the
	 * bevel can be), at the share of the gap BevelLine itself allows.
	 */
	double BevelLimitOver(const FSurfPatch& Patch, const TArray<FSurfBevelLine>& Lines)
	{
		double Shortest = TNumericLimits<double>::Max();

		for (const FSurfBevelLine& Line : Lines)
		{
			const int32 Across = Line.bColumn ? Patch.Height : Patch.Width;
			for (int32 B = 0; B < Across; ++B)
			{
				const FVector& Here   = Line.bColumn ? Patch.At(Line.Index, B).Pos     : Patch.At(B, Line.Index).Pos;
				const FVector& Before = Line.bColumn ? Patch.At(Line.Index - 1, B).Pos : Patch.At(B, Line.Index - 1).Pos;
				const FVector& After  = Line.bColumn ? Patch.At(Line.Index + 1, B).Pos : Patch.At(B, Line.Index + 1).Pos;
				Shortest = FMath::Min(Shortest, FVector::Dist(Here, Before));
				Shortest = FMath::Min(Shortest, FVector::Dist(Here, After));
			}
		}

		// 0.95 is BevelLine's MaxShareOfGap: the slider ends where the bevel
		// stops changing.
		return Shortest < TNumericLimits<double>::Max() ? FMath::Max(1.0, Shortest * 0.95) : 500.0;
	}
}

namespace
{
	/** Select the lines the bevels just made, so the result is what is in hand. Adds to the selection; the caller clears it first. */
	void SelectBevelResult(USurfEdMode& Mode, const FSurfPatchRef& PatchRef, const FSurfPatch& Patch, bool bColumn, const TArray<int32>& Starts, int32 Segments, bool bHard)
	{
		const int32 Count = bHard ? 2 * Segments + 1 : 2 * Segments;
		const int32 Across = bColumn ? Patch.Height : Patch.Width;

		for (int32 Start : Starts)
		{
			for (int32 S = 0; S < Count; ++S)
			{
				const int32 Line = Start + S;
				if (Line < 0 || Line >= (bColumn ? Patch.Width : Patch.Height))
				{
					continue;
				}

				for (int32 B = 0; B < Across; ++B)
				{
					FSurfPointRef Ref;
					Ref.Group = PatchRef.Group;
					Ref.PatchId = Patch.Id;
					Ref.X = bColumn ? Line : B;
					Ref.Y = bColumn ? B : Line;
					Mode.AddPointToSelection(Ref);
				}
			}
		}
	}
}

bool USurfInspector::ApplyShape(const FSurfOperation& Op)
{
	if (!Mode.IsValid())
	{
		return false;
	}

	ASurfGroup* Group = Mode->GetSelectedPatch().Group.Get();
	const FSurfPatch* Found = Group ? Group->FindPatch(Mode->GetSelectedPatch().PatchId) : nullptr;
	if (!Found)
	{
		SetResult(TEXT("shape: select a patch first"));
		RefreshSelectionCount();
		return false;
	}

	/**
	 * A shape operator set to zero does nothing, and used to do it in
	 * silence (S95a).
	 *
	 * "just tried applying smooth trough menu on selection it didnt apply."
	 * A strength of 0 is a legal setting, reachable by dragging the knob
	 * down, and it makes every one of these a no-op - so the verb ran, the
	 * transaction opened and closed, and the geometry did not move. That
	 * reads exactly like a broken button. It says what is wrong now, and
	 * where the number is.
	 */
	if ((Op.Kind == ESurfOpKind::Smooth || Op.Kind == ESurfOpKind::Flatten) && Op.Strength <= KINDA_SMALL_NUMBER)
	{
		SetResult(Op.Kind == ESurfOpKind::Smooth
			? TEXT("smooth: strength is 0, so there is nothing to do - raise it under Adjust")
			: TEXT("flatten: strength is 0, so there is nothing to do - raise it under Adjust"));
		RefreshSelectionCount();
		SurfSession::Friction(TEXT("shape_zero_strength"));
		return false;
	}
	if (Op.Kind == ESurfOpKind::Inflate && FMath::Abs(Op.Distance) <= KINDA_SMALL_NUMBER)
	{
		SetResult(TEXT("inflate: the distance is 0, so there is nothing to do - set it under Adjust"));
		RefreshSelectionCount();
		SurfSession::Friction(TEXT("shape_zero_distance"));
		return false;
	}

	// A bevel needs lines to round - on every patch the selection reaches
	// into, each with its own lines; the rest take the selection, or the
	// whole patch when nothing is selected.
	struct FBevelJob
	{
		FSurfPatchRef Ref;
		TArray<FSurfBevelLine> Lines;
	};
	TArray<FBevelJob> BevelJobs;
	if (Op.Kind == ESurfOpKind::Bevel)
	{
		TArray<FSurfPatchRef> Held = Mode->PatchesHeldBySelection();
		FSurfPatchRef Active;
		Active.Group = Group;
		Active.PatchId = Found->Id;
		Held.AddUnique(Active);
		for (const FSurfPatchRef& Ref : Held)
		{
			const ASurfGroup* EachGroup = Ref.Group.Get();
			const FSurfPatch* Each = EachGroup ? EachGroup->FindPatch(Ref.PatchId) : nullptr;
			if (!Each)
			{
				continue;
			}
			FBevelJob Job;
			Job.Ref = Ref;
			GetBevelLines(*Mode, *Each, Job.Lines);
			if (Job.Lines.Num() > 0)
			{
				BevelJobs.Add(MoveTemp(Job));
			}
		}
		if (BevelJobs.Num() == 0)
		{
			SetResult(Mode->GetPointSelection().Num() > 0
				? TEXT("bevel: a border has no crease to round - pick an edge, or a line, inside the patch")
				: TEXT("bevel: select an edge, or points on one row or column, first"));
			RefreshSelectionCount();
			SurfSession::Friction(TEXT("bevel_no_line"));
			return false;
		}
	}

	// The most the bevel can spread before it meets the next line, measured
	// across the lines it is about to round - the tightest of the patches
	// when there are several - so the slider offered afterwards ends where
	// the bevel stops changing.
	FSurfOperation Noted = Op;
	if (Op.Kind == ESurfOpKind::Bevel)
	{
		Noted.WidthLimit = 20000.0;
		Noted.SegmentsLimit = 8;
		for (const FBevelJob& Job : BevelJobs)
		{
			const FSurfPatch* Each = Job.Ref.Group->FindPatch(Job.Ref.PatchId);
			Noted.WidthLimit = FMath::Min(Noted.WidthLimit, BevelLimitOver(*Each, Job.Lines));

			// And how many segments the grid has room for: every line adds
			// 2 * Segments - 1 lines (one more when the crease is kept), and
			// the grid holds 64 (a curve 15). The knob ends there rather than
			// running into a refusal at every step past it.
			const bool bColumns = Job.Lines[0].bColumn;
			const int32 Along = bColumns ? Each->Width : Each->Height;
			const int32 Limit = Each->Type == ESurfPatchType::Curve ? FSurfPatch::MaxCurveDim : FSurfPatch::MaxSheetDim;
			const int32 Room = FMath::Max(0, Limit - Along) / FMath::Max(1, Job.Lines.Num());
			Noted.SegmentsLimit = FMath::Min(Noted.SegmentsLimit, FMath::Clamp((Room + (Noted.bHard ? 0 : 1)) / 2, 1, 8));
		}
		Noted.Width = FMath::Min(Noted.Width, Noted.WidthLimit);
		Noted.Segments = FMath::Clamp(Noted.Segments, 1, Noted.SegmentsLimit);
	}

	static const TCHAR* Names[] = { TEXT("Shape"), TEXT("Move"), TEXT("Rotate"), TEXT("Scale"), TEXT("Extrude"),
		TEXT("Bevel"), TEXT("Smooth"), TEXT("Inflate"), TEXT("Flatten"), TEXT("Solidify"), TEXT("Duplicate"), TEXT("Weld") };
	FScopedTransaction Transaction(Op.Kind == ESurfOpKind::Random
		? FText::FromString(TEXT("Random"))
		: FText::FromString(Names[FMath::Clamp(static_cast<int32>(Op.Kind), 0, 11)]));

	FSurfPatch* Patch = Group->ModifyPatch(Found->Id);
	if (!Patch)
	{
		Transaction.Cancel();
		return false;
	}

	const TArray<int32> Indices = SelectedIndices(*Mode, *Patch);

	switch (Op.Kind)
	{
	case ESurfOpKind::Smooth:
	case ESurfOpKind::Inflate:
	case ESurfOpKind::Flatten:
	case ESurfOpKind::Random:
	{
		/**
		 * Every patch the selection reaches into (S95a), which is what D54
		 * says a verb does and what this one did not.
		 *
		 * It took `GetSelectedPatches()` alone - the patches picked whole in
		 * object sub-mode - and not the patches a point selection spans. So
		 * a row dragged across a seam smoothed on one side of it and not the
		 * other: "soft selected move only worked on one of my two selected",
		 * "fit didnt apply on both . only one", "selecting all and pressing
		 * some uv ops fit - expecting all to be ran fit but only one did or
		 * none". Bevel, three lines above, had it right all along.
		 */
		TArray<FSurfPatchRef> Targets = Mode->GetSelectedPatches();
		for (const FSurfPatchRef& Held : Mode->PatchesHeldBySelection())
		{
			Targets.AddUnique(Held);
		}

		int32 PointsDone = 0;
		int32 PatchesDone = 0;
		TArray<ASurfGroup*> Groups;
		for (const FSurfPatchRef& Ref : Targets)
		{
			ASurfGroup* EachGroup = Ref.Group.Get();
			FSurfPatch* Each = EachGroup ? EachGroup->ModifyPatch(Ref.PatchId) : nullptr;
			if (!Each)
			{
				continue;
			}
			const TArray<int32> EachIndices = Each == Patch ? Indices : SelectedIndices(*Mode, *Each);
			if (Op.Kind == ESurfOpKind::Smooth)
			{
				SurfPatchOps::Smooth(*Each, EachIndices, Noted.Strength, Noted.Iterations, Noted.bBorder);
			}
			else if (Op.Kind == ESurfOpKind::Inflate)
			{
				SurfPatchOps::Inflate(*Each, EachIndices, Noted.Distance);
			}
			else if (Op.Kind == ESurfOpKind::Random)
			{
				SurfPatchOps::Random(*Each, EachIndices, Noted.Distance, Noted.Scale, Noted.bHard, Noted.Segments);
			}
			else
			{
				SurfPatchOps::Flatten(*Each, EachIndices, Noted.Strength);
			}
			++Each->EditSerial;
			SurfPatchSurfacing::Reapply(*Each);
			Groups.AddUnique(EachGroup);
			PointsDone += EachIndices.Num() > 0 ? EachIndices.Num() : Each->NumPoints();
			++PatchesDone;
		}
		for (ASurfGroup* Each : Groups)
		{
			if (Each != Group)
			{
				Each->RebuildPreview();
			}
		}

		// And when it came to nothing, say that rather than reporting a
		// success over zero points (S95a).
		if (PointsDone == 0)
		{
			SetResult(TEXT("nothing was picked that this could act on - select points, or a patch"));
			RefreshSelectionCount();
			SurfSession::Friction(TEXT("shape_nothing_picked"));
			break;
		}

		const FString Over = PatchesDone > 1 ? FString::Printf(TEXT(" on %d patches"), PatchesDone) : FString();
		if (Op.Kind == ESurfOpKind::Smooth)
		{
			SetResult(FString::Printf(TEXT("smoothed %d points%s, strength %.2f, %d passes"), PointsDone, *Over, Noted.Strength, Noted.Iterations));
		}
		else if (Op.Kind == ESurfOpKind::Inflate)
		{
			SetResult(FString::Printf(TEXT("inflated %d points%s by %.1f cm"), PointsDone, *Over, Noted.Distance));
		}
		else if (Op.Kind == ESurfOpKind::Random)
		{
			SetResult(FString::Printf(TEXT("%d points%s pushed at random, up to %.1f cm - the knob tunes it, Shift+R rolls again"), PointsDone, *Over, Noted.Distance));
		}
		else
		{
			SetResult(FString::Printf(TEXT("flattened %d points%s onto their plane, strength %.2f"), PointsDone, *Over, Noted.Strength));
		}
		break;
	}

	case ESurfOpKind::Bevel:
	{
		// Every patch with lines, each on its own. On a patch: every line,
		// from the far end back, so the indices still to come are not moved
		// by the lines a bevel adds. Each bevel puts 2 * Segments lines
		// where one was, so a line's result starts at its index plus what
		// every bevel before it added.
		const int32 Added = Noted.bHard ? 2 * Noted.Segments : 2 * Noted.Segments - 1;
		int32 Done = 0;
		int32 PatchesDone = 0;
		bool bColumn = BevelJobs[0].Lines[0].bColumn;
		bool bCurve = false;
		bool bCleared = false;
		TArray<ASurfGroup*> Groups;
		for (FBevelJob& Job : BevelJobs)
		{
			ASurfGroup* EachGroup = Job.Ref.Group.Get();
			FSurfPatch* Each = EachGroup ? EachGroup->ModifyPatch(Job.Ref.PatchId) : nullptr;
			if (!Each)
			{
				continue;
			}

			/**
			 * A bevel wider than the next line eats it (S159a): "eat the
			 * lines it reaches". On one line at a time: every interior
			 * neighbour nearer than the width asked for goes, either side,
			 * and the round takes its place. Then the width is what the
			 * room allows - which is now the room he asked for.
			 */
			const USurfSettings* Eat = GetDefault<USurfSettings>();
			if (Eat && Eat->bBevelEatsLines && Job.Lines.Num() == 1 && Each->Type != ESurfPatchType::Curve)
			{
				FSurfBevelLine& Line = Job.Lines[0];
				const double Wanted = Op.Width;
				auto Gap = [&](int32 A, int32 B) -> double
				{
					const int32 Across = Line.bColumn ? Each->Height : Each->Width;
					double Shortest = TNumericLimits<double>::Max();
					for (int32 I = 0; I < Across; ++I)
					{
						const FVector& P = Line.bColumn ? Each->At(A, I).Pos : Each->At(I, A).Pos;
						const FVector& Q = Line.bColumn ? Each->At(B, I).Pos : Each->At(I, B).Pos;
						Shortest = FMath::Min(Shortest, FVector::Dist(P, Q));
					}
					return Shortest;
				};
				int32 Eaten = 0;
				// Before the line: the neighbour is not the border, and is within the width.
				while (Line.Index - 1 > 0 && Gap(Line.Index, Line.Index - 1) < Wanted * 0.95)
				{
					if (!(Line.bColumn ? SurfPatchPrimitives::RemoveColumn(*Each, Line.Index - 1) : SurfPatchPrimitives::RemoveRow(*Each, Line.Index - 1)))
					{
						break;
					}
					--Line.Index;
					++Eaten;
				}
				// After it.
				while (Line.Index + 1 < (Line.bColumn ? Each->Width : Each->Height) - 1 && Gap(Line.Index, Line.Index + 1) < Wanted * 0.95)
				{
					if (!(Line.bColumn ? SurfPatchPrimitives::RemoveColumn(*Each, Line.Index + 1) : SurfPatchPrimitives::RemoveRow(*Each, Line.Index + 1)))
					{
						break;
					}
					++Eaten;
				}
				if (Eaten > 0)
				{
					Noted.WidthLimit = FMath::Max(1.0, BevelLimitOver(*Each, Job.Lines));
					Noted.Width = FMath::Min(Wanted, Noted.WidthLimit);
					SurfSession::Verb(TEXT("bevel_ate"), FString::Printf(TEXT("lines=%d width=%.1f"), Eaten, Noted.Width));
				}
			}
			const int32 EachWidthBefore = Each->Width;
			const int32 EachHeightBefore = Each->Height;
			const bool bEachColumn = Job.Lines[0].bColumn;
			bCurve |= Each->Type == ESurfPatchType::Curve;

			TArray<bool> Bevelled;
			Bevelled.Init(false, Job.Lines.Num());
			int32 EachDone = 0;
			for (int32 I = Job.Lines.Num() - 1; I >= 0; --I)
			{
				Bevelled[I] = SurfPatchOps::BevelLine(*Each, bEachColumn, Job.Lines[I].Index, Noted.Width, Noted.Segments, Noted.Strength, Noted.bHard);
				EachDone += Bevelled[I] ? 1 : 0;
			}
			if (EachDone == 0)
			{
				continue;
			}
			if (PatchesDone == 0)
			{
				bColumn = bEachColumn;
			}
			Done += EachDone;
			++PatchesDone;

			TArray<int32> Starts;
			int32 Shift = 0;
			for (int32 I = 0; I < Job.Lines.Num(); ++I)
			{
				if (Bevelled[I])
				{
					Starts.Add(Job.Lines[I].Index + Shift);
					Shift += Added;
				}
			}

			// Every index past a bevel now names a different point, so
			// holding the old selection walked it to the far edge of the
			// patch. Select the lines the bevels produced instead.
			if (Each->Width != EachWidthBefore || Each->Height != EachHeightBefore)
			{
				if (!bCleared)
				{
					Mode->ClearPointSelection();
					bCleared = true;
				}
				SelectBevelResult(*Mode, Job.Ref, *Each, bEachColumn, Starts, Noted.Segments, Noted.bHard);
			}
			++Each->EditSerial;
			SurfPatchSurfacing::Reapply(*Each);
			Groups.AddUnique(EachGroup);
		}

		if (Done == 0)
		{
			Transaction.Cancel();
			SetResult(bCurve
				? TEXT("bevel: a curve has no room left for the lines a bevel adds (15 at most on each side)")
				: TEXT("bevel: no room left for the lines a bevel adds (64 at most on each side)"));
			RefreshSelectionCount();
			SurfSession::Friction(TEXT("bevel_refused"));
			return false;
		}
		if (bCleared)
		{
			Mode->SyncPatchSelectionToPoints();
		}
		for (ASurfGroup* Each : Groups)
		{
			if (Each != Group)
			{
				Each->RebuildPreview();
			}
		}

		const TCHAR* What = Noted.bHard ? TEXT("added support lines to") : TEXT("bevelled");
		const FString Over = PatchesDone > 1 ? FString::Printf(TEXT(" on %d patches"), PatchesDone) : FString();
		SetResult(Done == 1
			? FString::Printf(TEXT("%s the %s: %.1f cm wide, %d segments"), What, bColumn ? TEXT("column") : TEXT("row"), Noted.Width, Noted.Segments)
			: FString::Printf(TEXT("%s %d %s%s: %.1f cm wide, %d segments each"), What, Done, bColumn ? TEXT("columns") : TEXT("rows"), *Over, Noted.Width, Noted.Segments));
		break;
	}

	default:
		Transaction.Cancel();
		return false;
	}

	++Patch->EditSerial;
	SurfPatchSurfacing::Reapply(*Patch);
	Group->RebuildPreview();

	SurfSession::Verb(TEXT("shape"), FString::Printf(TEXT("kind=%d"), static_cast<int32>(Op.Kind)));

	// The panel's fields follow what was just done, so the next press starts
	// from the last numbers - and the adjust panel offers to change them.
	Strength = Noted.Strength;
	Iterations = Noted.Iterations;
	bPreserveBorder = Noted.bBorder;
	if (Op.Kind == ESurfOpKind::Inflate) { DistanceCm = Noted.Distance; }
	if (Op.Kind == ESurfOpKind::Bevel) { BevelWidthCm = Noted.Width; BevelSegments = Noted.Segments; bBevelKeepsCrease = Noted.bHard; }

	Mode->NoteOperation(Noted);
	RefreshSelectionCount();

	if (GEditor)
	{
		if (Mode.IsValid()) { Mode->RedrawViewports(); } else { GEditor->RedrawLevelEditingViewports(); }
	}

	return true;
}

void USurfInspector::Smooth()
{
	FSurfOperation Op;
	Op.Kind = ESurfOpKind::Smooth;
	Op.Strength = Strength;
	Op.Iterations = Iterations;
	Op.bBorder = bPreserveBorder;
	ApplyShape(Op);
}

void USurfInspector::Inflate()
{
	FSurfOperation Op;
	Op.Kind = ESurfOpKind::Inflate;
	Op.Distance = DistanceCm;
	ApplyShape(Op);
}

void USurfInspector::Random()
{
	const USurfSettings* S = GetDefault<USurfSettings>();
	FSurfOperation Op;
	Op.Kind = ESurfOpKind::Random;
	Op.Distance = S ? S->RandomOffsetCm : 10.0;
	Op.Scale = FVector(S && S->bRandomX ? 1.0 : 0.0, S && S->bRandomY ? 1.0 : 0.0, S && S->bRandomZ ? 1.0 : 0.0);
	Op.bHard = S && S->bRandomAlongNormal;
	// A fresh roll each press; the adjust knob keeps the roll and tunes the amount.
	Op.Segments = FMath::Rand();
	Op.SegmentsLimit = MAX_int32;
	ApplyShape(Op);
}

void USurfInspector::Flatten()
{
	FSurfOperation Op;
	Op.Kind = ESurfOpKind::Flatten;
	Op.Strength = Strength;
	ApplyShape(Op);
}

void USurfInspector::Bevel()
{
	FSurfOperation Op;
	Op.Kind = ESurfOpKind::Bevel;
	Op.Width = BevelWidthCm;
	Op.Segments = BevelSegments;
	Op.Strength = Strength;
	Op.bHard = bBevelKeepsCrease;
	ApplyShape(Op);
}

void USurfInspector::FlattenAlong(int32 Axis)
{
	// Not a live operator: a flatten along a world axis has no strength to
	// tweak. Every selected point's coordinate on that axis becomes their
	// mean - "make this row level" in one press.
	if (!Mode.IsValid() || Axis < 0 || Axis > 2)
	{
		return;
	}

	const TArray<FSurfPointRef> Refs = Mode->GetPointSelection();
	if (Refs.Num() < 2)
	{
		SetResult(TEXT("flatten: select at least two points"));
		RefreshSelectionCount();
		return;
	}

	double Sum = 0.0;
	int32 Count = 0;
	for (const FSurfPointRef& Ref : Refs)
	{
		const ASurfGroup* Group = Ref.Group.Get();
		const FSurfPatch* Patch = Group ? Group->FindPatch(Ref.PatchId) : nullptr;
		if (Patch && Ref.X >= 0 && Ref.X < Patch->Width && Ref.Y >= 0 && Ref.Y < Patch->Height)
		{
			Sum += Group->GetActorTransform().TransformPosition(Patch->At(Ref.X, Ref.Y).Pos)[Axis];
			++Count;
		}
	}

	if (Count == 0)
	{
		return;
	}

	const double Mean = Sum / Count;

	FScopedTransaction Transaction(LOCTEXT("FlattenAxis", "Flatten Along Axis"));

	Mode->ForEachSelectedPatch([&](ASurfGroup& Group, FSurfPatch& Patch, const TArray<FSurfPointRef>& PatchRefs)
	{
		const FTransform ToWorld = Group.GetActorTransform();
		for (const FSurfPointRef& Ref : PatchRefs)
		{
			if (Ref.X >= 0 && Ref.X < Patch.Width && Ref.Y >= 0 && Ref.Y < Patch.Height)
			{
				FVector World = ToWorld.TransformPosition(Patch.At(Ref.X, Ref.Y).Pos);
				World[Axis] = Mean;
				Patch.At(Ref.X, Ref.Y).Pos = ToWorld.InverseTransformPosition(World);
			}
		}
	});

	SetResult(FString::Printf(TEXT("flattened %d points to %s = %.1f cm"),
		Count, Axis == 0 ? TEXT("X") : (Axis == 1 ? TEXT("Y") : TEXT("Z")), Mean));
	RefreshSelectionCount();

	if (GEditor)
	{
		if (Mode.IsValid()) { Mode->RedrawViewports(); } else { GEditor->RedrawLevelEditingViewports(); }
	}
}

double USurfInspector::BevelWidthLimitCm() const
{
	// The most the bevel can spread before it meets the next line, across
	// the lines the selection names. A slider that goes further than that is
	// a slider that stops working part way, which is what "stuck, can't
	// bevel beyond" was.
	if (!Mode.IsValid())
	{
		return 500.0;
	}

	// The tightest of the patches the selection reaches into.
	TArray<FSurfPatchRef> Held = Mode->PatchesHeldBySelection();
	Held.AddUnique(Mode->GetSelectedPatch());
	double Limit = 500.0;
	for (const FSurfPatchRef& Ref : Held)
	{
		const ASurfGroup* Group = Ref.Group.Get();
		const FSurfPatch* Patch = Group ? Group->FindPatch(Ref.PatchId) : nullptr;
		if (!Patch)
		{
			continue;
		}
		TArray<FSurfBevelLine> Lines;
		GetBevelLines(*Mode, *Patch, Lines);
		if (Lines.Num() > 0)
		{
			Limit = FMath::Min(Limit, BevelLimitOver(*Patch, Lines));
		}
	}
	return Limit;
}



void USurfInspector::PushSoftSelection()
{
	USurfSettings* Settings = GetMutableDefault<USurfSettings>();
	if (!Settings)
	{
		return;
	}

	Settings->bSoftSelection = bSoftSelection;
	Settings->SoftSelectionRadiusCm = FMath::Clamp(SoftRadiusCm, USurfSettings::SoftReachMinCm, USurfSettings::SoftReachMaxCm);
	Settings->SoftSelectionFalloff = SoftFalloff;

	if (Mode.IsValid())
	{
		Mode->RefreshSoftSelection();
	}

	if (GEditor)
	{
		if (Mode.IsValid()) { Mode->RedrawViewports(); } else { GEditor->RedrawLevelEditingViewports(); }
	}
}

void USurfInspector::PullSoftSelection()
{
	if (const USurfSettings* Settings = GetDefault<USurfSettings>())
	{
		bSoftSelection = Settings->bSoftSelection;
		SoftRadiusCm = Settings->SoftSelectionRadiusCm;
		SoftFalloff = Settings->SoftSelectionFalloff;
	}
}

#if WITH_EDITOR
void USurfInspector::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	const FName Name = PropertyChangedEvent.GetPropertyName();

	if (Name == GET_MEMBER_NAME_CHECKED(USurfInspector, bSoftSelection)
		|| Name == GET_MEMBER_NAME_CHECKED(USurfInspector, SoftRadiusCm)
		|| Name == GET_MEMBER_NAME_CHECKED(USurfInspector, SoftFalloff))
	{
		PushSoftSelection();
	}
}
#endif

#undef LOCTEXT_NAMESPACE
