// Copyright MapModelers. All Rights Reserved.

/**
 * The brush on placed static meshes (S213a): "its possible to vertexpaint
 * them with surfacingpro feature(s)". The colours go where the editor's own
 * mesh paint puts them - the component's per-instance override colours
 * (LOD 0) - so they bake, cook and survive a reimport as usual, and two
 * placed copies can be painted differently. The mask for a mesh lives in
 * the session only (his: "dont bother saving ... unless it's free").
 */

#include "SurfEdMode.h"
#include "SurfInspector.h"
#include "SurfPickContext.h"
#include "SurfSession.h"
#include "SurfSettings.h"

#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/Selection.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "SurfPrefab.h"
#include "SurfPrefabInstance.h"
#include "EngineUtils.h"
#include "Materials/MaterialInterface.h"
#include "PrimitiveDrawInterface.h"
#include "SceneView.h"
#include "Rendering/ColorVertexBuffer.h"
#include "StaticMeshResources.h"
#include "StaticMeshComponentLODInfo.h"
#include "RenderingThread.h"

#define LOCTEXT_NAMESPACE "SurfacingProEdModeMeshPaint"

namespace
{
	const FStaticMeshLODResources* LodOf(const UStaticMeshComponent* Component)
	{
		const UStaticMesh* Mesh = Component ? Component->GetStaticMesh() : nullptr;
		if (!Mesh || !Mesh->GetRenderData() || Mesh->GetRenderData()->LODResources.Num() == 0)
		{
			return nullptr;
		}
		return &Mesh->GetRenderData()->LODResources[0];
	}

	/** The override colours of LOD 0, made if there are none; null when the mesh has no vertices. */
	FColorVertexBuffer* ColoursOf(UStaticMeshComponent* Component, bool bMake)
	{
		const FStaticMeshLODResources* Lod = LodOf(Component);
		if (!Lod)
		{
			return nullptr;
		}
		const int32 Num = Lod->GetNumVertices();
		if (Num <= 0)
		{
			return nullptr;
		}
		if (Component->LODData.Num() < 1)
		{
			if (!bMake)
			{
				return nullptr;
			}
			Component->SetLODDataCount(1, Component->LODData.Num());
		}
		FStaticMeshComponentLODInfo& Info = Component->LODData[0];
		if (Info.OverrideVertexColors && Info.OverrideVertexColors->GetNumVertices() != static_cast<uint32>(Num))
		{
			Info.ReleaseOverrideVertexColorsAndBlock();
		}
		if (!Info.OverrideVertexColors)
		{
			if (!bMake)
			{
				return nullptr;
			}
			Info.OverrideVertexColors = new FColorVertexBuffer();
			// The mesh's own colours to start from, when it has any; else clear.
			if (Lod->VertexBuffers.ColorVertexBuffer.GetNumVertices() == static_cast<uint32>(Num))
			{
				TArray<FColor> Own;
				Lod->VertexBuffers.ColorVertexBuffer.GetVertexColors(Own);
				Info.OverrideVertexColors->InitFromColorArray(Own);
			}
			else
			{
				Info.OverrideVertexColors->InitFromSingleColor(FColor(0, 0, 0, 0), Num);
			}
			BeginInitResource(Info.OverrideVertexColors);
		}
		return Info.OverrideVertexColors;
	}

	void Wrote(UStaticMeshComponent* Component)
	{
		if (Component->LODData.Num() > 0 && Component->LODData[0].OverrideVertexColors)
		{
			BeginUpdateResourceRHI(Component->LODData[0].OverrideVertexColors);
		}
		// Remembered by position, as the editor's mesh paint does, so a
		// reimport of the mesh keeps the paint.
		Component->CachePaintedDataIfNecessary();
		Component->MarkRenderStateDirty();
	}

	float ChannelOf(const FColor& C, ESurfPaintChannel Channel)
	{
		switch (Channel)
		{
		case ESurfPaintChannel::G: return C.G / 255.0f;
		case ESurfPaintChannel::B: return C.B / 255.0f;
		case ESurfPaintChannel::A: return C.A / 255.0f;
		default: return C.R / 255.0f;
		}
	}

	void SetChannel(FColor& C, ESurfPaintChannel Channel, float Value)
	{
		const uint8 Byte = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(FMath::Clamp(Value, 0.0f, 1.0f) * 255.0f), 0, 255));
		switch (Channel)
		{
		case ESurfPaintChannel::G: C.G = Byte; break;
		case ESurfPaintChannel::B: C.B = Byte; break;
		case ESurfPaintChannel::A: C.A = Byte; break;
		default: C.R = Byte; break;
		}
	}

	FColor Towards(const FColor& Now, const FLinearColor& Target, float Reach)
	{
		const FLinearColor Was(Now.R / 255.0f, Now.G / 255.0f, Now.B / 255.0f, 1.0f);
		const FLinearColor Want = FMath::Lerp(Was, Target, Reach);
		return FColor(
			static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(Want.R * 255.0f), 0, 255)),
			static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(Want.G * 255.0f), 0, 255)),
			static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(Want.B * 255.0f), 0, 255)),
			Now.A);
	}
}

void USurfEdMode::PaintMeshTargets(TArray<UStaticMeshComponent*>& Out)
{
	Out.Reset();
	if (!GEditor)
	{
		return;
	}
	// A mesh inside an open prefab takes the brush only when the prefab
	// keeps paint (S237a): else the stroke would be lost on the first
	// rebuild of its instances, so it is refused and said.
	const ASurfPrefabInstance* Drilled = GetDrilledInstance();
	const bool bRoomPaints = !Drilled || !Drilled->Prefab || Drilled->Prefab->bMeshPaint;
	bool bRefused = false;
	for (FSelectionIterator It(GEditor->GetSelectedActorIterator()); It; ++It)
	{
		AStaticMeshActor* Actor = Cast<AStaticMeshActor>(*It);
		if (Actor && Actor->GetStaticMeshComponent() && Actor->GetStaticMeshComponent()->GetStaticMesh())
		{
			if (!bRoomPaints && IsRoomPartActor(Actor))
			{
				bRefused = true;
				continue;
			}
			Out.Add(Actor->GetStaticMeshComponent());
		}
	}
	if (bRefused && Inspector)
	{
		Inspector->SetResult(TEXT("this prefab keeps no mesh paint - turn Painted meshes on (right-click its tile in the Assets tab) to paint its meshes"));
	}
}

void USurfEdMode::MeshCavityOf(const UStaticMeshComponent* Component, TArray<float>& Out) const
{
	Out.Reset();
	const FStaticMeshLODResources* Lod = LodOf(Component);
	if (!Lod)
	{
		return;
	}
	const USurfSettings* S = GetDefault<USurfSettings>();
	const int32 Num = Lod->GetNumVertices();
	const FPositionVertexBuffer& Positions = Lod->VertexBuffers.PositionVertexBuffer;
	const FStaticMeshVertexBuffer& Vertices = Lod->VertexBuffers.StaticMeshVertexBuffer;
	// Vertices that share a position share their neighbours: a seam does
	// not cut the surface in two.
	TMap<FVector3f, int32> Family;
	TArray<int32> Rep;
	Rep.SetNumUninitialized(Num);
	for (int32 I = 0; I < Num; ++I)
	{
		const FVector3f P = Positions.VertexPosition(I);
		const int32* Found = Family.Find(P);
		Rep[I] = Found ? *Found : I;
		if (!Found) { Family.Add(P, I); }
	}
	TArray<FVector3f> Sum;
	TArray<double> Reach;
	TArray<int32> Count;
	Sum.SetNumZeroed(Num);
	Reach.SetNumZeroed(Num);
	Count.SetNumZeroed(Num);
	const FIndexArrayView Indices = Lod->IndexBuffer.GetArrayView();
	for (int32 T = 0; T + 2 < Indices.Num(); T += 3)
	{
		const int32 A = Indices[T], B = Indices[T + 1], C = Indices[T + 2];
		if (A >= Num || B >= Num || C >= Num) { continue; }
		const int32 Tri[3] = { A, B, C };
		for (int32 K = 0; K < 3; ++K)
		{
			const int32 From = Rep[Tri[K]];
			for (int32 J = 0; J < 3; ++J)
			{
				if (J == K) { continue; }
				const FVector3f D = Positions.VertexPosition(Tri[J]) - Positions.VertexPosition(Tri[K]);
				Sum[From] += D;
				Reach[From] += D.Size();
				++Count[From];
			}
		}
	}
	TArray<float> Raw;
	Raw.SetNumZeroed(Num);
	for (int32 I = 0; I < Num; ++I)
	{
		const int32 R = Rep[I];
		if (Count[R] == 0 || Reach[R] <= KINDA_SMALL_NUMBER) { continue; }
		const FVector3f Mean = Sum[R] / static_cast<float>(Count[R]);
		const float Scale = static_cast<float>(Reach[R] / Count[R]);
		const FVector3f N = Vertices.VertexTangentZ(I);
		Raw[I] = FVector3f::DotProduct(Mean, N) / Scale;
	}
	const float Smooth = S ? S->CavitySmooth : 0.3f;
	const float Level = S ? S->CavityLevel : 4.0f;
	const bool bInvert = S && S->bCavityInvert;
	// Smooth: the mean over the family's neighbours, once or twice.
	const int32 Passes = Smooth <= 0.0f ? 0 : (Smooth > 0.5f ? 2 : 1);
	const float Blend = Smooth <= 0.0f ? 0.0f : (Smooth > 0.5f ? FMath::Min(1.0f, Smooth) : Smooth * 2.0f);
	if (Passes > 0)
	{
		TArray<TArray<int32>> Around;
		Around.SetNum(Num);
		for (int32 T = 0; T + 2 < Indices.Num(); T += 3)
		{
			const int32 Tri[3] = { static_cast<int32>(Indices[T]), static_cast<int32>(Indices[T + 1]), static_cast<int32>(Indices[T + 2]) };
			for (int32 K = 0; K < 3; ++K)
			{
				for (int32 J = 0; J < 3; ++J)
				{
					if (J != K && Tri[K] < Num && Tri[J] < Num) { Around[Rep[Tri[K]]].AddUnique(Rep[Tri[J]]); }
				}
			}
		}
		for (int32 Pass = 0; Pass < Passes; ++Pass)
		{
			TArray<float> Was = Raw;
			for (int32 I = 0; I < Num; ++I)
			{
				const int32 R = Rep[I];
				double Total = Was[R];
				int32 N = 1;
				for (int32 Other : Around[R]) { Total += Was[Other]; ++N; }
				Raw[I] = FMath::Lerp(Was[I], static_cast<float>(Total / N), Blend);
			}
		}
	}
	Out.SetNumUninitialized(Num);
	for (int32 I = 0; I < Num; ++I)
	{
		float V = FMath::Clamp(Raw[I] * FMath::Max(0.25f, Level), -1.0f, 1.0f);
		if (bInvert) { V = -V; }
		// Dirt only (S248a): nothing on the flat, full in a crevice.
		Out[I] = FMath::Max(0.0f, V);
	}
}

void USurfEdMode::RefreshMeshPaintView(UWorld* World, UMaterialInterface* PaintView)
{
	/**
	 * The channel on placed meshes (S236a). Their slots wear the paint
	 * view's material while it is on - the same one the patches wear, so
	 * a mesh and a patch read alike - and get their own back when it goes
	 * (the view off, the brush away, the mode left). What a slot had
	 * before is kept per component, and a slot that had nothing gets
	 * nothing back, which is the mesh's own material again.
	 */
	for (auto It = MeshViewRestore.CreateIterator(); It; ++It)
	{
		UStaticMeshComponent* Component = It.Key().Get();
		if (!Component)
		{
			It.RemoveCurrent();
			continue;
		}
		if (!PaintView)
		{
			for (int32 I = 0; I < It.Value().Num(); ++I)
			{
				Component->SetMaterial(I, It.Value()[I].Get());
			}
			It.RemoveCurrent();
		}
	}
	if (!PaintView || !World)
	{
		return;
	}
	TArray<UStaticMeshComponent*> Wearing;
	for (TActorIterator<AStaticMeshActor> It(World); It; ++It)
	{
		AStaticMeshActor* Actor = *It;
		UStaticMeshComponent* Component = Actor ? Actor->GetStaticMeshComponent() : nullptr;
		if (Component && Actor->Tags.Contains(PlacedMeshTag))
		{
			Wearing.AddUnique(Component);
		}
	}
	// And whatever is picked for the brush, placed by us or not.
	TArray<UStaticMeshComponent*> Picked;
	PaintMeshTargets(Picked);
	for (UStaticMeshComponent* Component : Picked)
	{
		Wearing.AddUnique(Component);
	}
	for (UStaticMeshComponent* Component : Wearing)
	{
		const int32 Slots = Component->GetNumMaterials();
		if (!MeshViewRestore.Contains(Component))
		{
			TArray<TWeakObjectPtr<UMaterialInterface>> Was;
			for (int32 I = 0; I < Slots; ++I)
			{
				Was.Add(Component->OverrideMaterials.IsValidIndex(I) ? Component->OverrideMaterials[I].Get() : nullptr);
			}
			MeshViewRestore.Add(Component, MoveTemp(Was));
		}
		for (int32 I = 0; I < Slots; ++I)
		{
			if (Component->GetMaterial(I) != PaintView)
			{
				Component->SetMaterial(I, PaintView);
			}
		}
	}
}

int32 USurfEdMode::PaintMeshWeightWith(const FSurfPickContext& Ctx, const FVector2D& At, bool bErase, bool bSmooth)
{
	TArray<UStaticMeshComponent*> Targets;
	PaintMeshTargets(Targets);
	if (Targets.Num() == 0 || !Ctx.IsValid())
	{
		return 0;
	}
	const USurfSettings* Settings = GetDefault<USurfSettings>();
	const double Radius = FMath::Max(1.0, Settings ? Settings->BrushSelectRadiusPx : 40.0);
	const double Strength = FMath::Clamp(Settings ? Settings->PaintStrength : 1.0, 0.05, 1.0);
	// The ticked channels (S248a): R, G and B together are the colour.
	const FSurfPaintSet Set = Settings ? Settings->PaintSet() : FSurfPaintSet();
	TArray<ESurfPaintChannel, TInlineAllocator<4>> Singles;
	Set.Singles(Singles);
	int32 Changed = 0;
	for (UStaticMeshComponent* Component : Targets)
	{
		const FStaticMeshLODResources* Lod = LodOf(Component);
		if (!Lod)
		{
			continue;
		}
		const int32 Num = Lod->GetNumVertices();
		const FPositionVertexBuffer& Positions = Lod->VertexBuffers.PositionVertexBuffer;
		const FTransform ToWorld = Component->GetComponentTransform();
		FColorVertexBuffer* Colours = nullptr;
		// Smoothing reads a copy, as the patches do.
		TArray<FColor> Was;
		if (bSmooth)
		{
			if (FColorVertexBuffer* Read = ColoursOf(Component, /*bMake*/ true)) { Read->GetVertexColors(Was); }
		}
		int32 Here = 0;
		for (int32 I = 0; I < Num; ++I)
		{
			FVector2D Screen;
			if (!Ctx.WorldToPixel(ToWorld.TransformPosition(FVector(Positions.VertexPosition(I))), Screen))
			{
				continue;
			}
			const double Distance = FVector2D::Distance(Screen, At);
			if (Distance > Radius)
			{
				continue;
			}
			const double T = 1.0 - Distance / Radius;
			const double Falloff = T * T * (3.0 - 2.0 * T);
			const float Reach = static_cast<float>(FMath::Clamp(Strength * Falloff, 0.0, 1.0));
			if (!Colours)
			{
				Component->Modify();
				Colours = ColoursOf(Component, /*bMake*/ true);
				if (!Colours) { break; }
			}
			FColor& C = Colours->VertexColor(I);
			const FColor Before = C;
			if (Set.IsColour())
			{
				FLinearColor Target = bErase ? (Settings ? Settings->PaintColourBack : FLinearColor::Black) : (Settings ? Settings->PaintColour : FLinearColor::Red);
				if (bSmooth && Was.Num() == Num)
				{
					// Towards the mean of the vertices sharing this position's
					// neighbourhood on screen is costly; the mean of the
					// colour of the nearest few is near enough: this one.
					Target = FLinearColor(Was[I].R / 255.0f, Was[I].G / 255.0f, Was[I].B / 255.0f, 1.0f);
				}
				C = Towards(C, Target, Reach);
			}
			// Every other ticked channel - A beside the colour.
			for (const ESurfPaintChannel Each : Singles)
			{
				const float Now = ChannelOf(C, Each);
				const float Want = bErase ? FMath::Min(Now, 1.0f - Reach) : FMath::Max(Now, Reach);
				SetChannel(C, Each, Want);
			}
			if (C != Before) { ++Here; }
		}
		if (Here > 0)
		{
			Wrote(Component);
			Changed += Here;
		}
	}
	return Changed;
}

int32 USurfEdMode::FillMeshPaint(bool bFull)
{
	TArray<UStaticMeshComponent*> Targets;
	PaintMeshTargets(Targets);
	if (Targets.Num() == 0)
	{
		return 0;
	}
	const USurfSettings* Settings = GetDefault<USurfSettings>();
	const FSurfPaintSet Set = Settings ? Settings->PaintSet() : FSurfPaintSet();
	TArray<ESurfPaintChannel, TInlineAllocator<4>> Singles;
	Set.Singles(Singles);
	const FLinearColor Colour = bFull ? (Settings ? Settings->PaintColour : FLinearColor::Red) : (Settings ? Settings->PaintColourBack : FLinearColor::Black);
	int32 Points = 0;
	for (UStaticMeshComponent* Component : Targets)
	{
		Component->Modify();
		FColorVertexBuffer* Colours = ColoursOf(Component, /*bMake*/ true);
		if (!Colours)
		{
			continue;
		}
		for (uint32 I = 0; I < Colours->GetNumVertices(); ++I)
		{
			const float Through = 1.0f;
			FColor& C = Colours->VertexColor(I);
			if (Set.IsColour())
			{
				C = Towards(C, Colour, Through);
			}
			for (const ESurfPaintChannel Each : Singles)
			{
				SetChannel(C, Each, FMath::Lerp(ChannelOf(C, Each), bFull ? 1.0f : 0.0f, Through));
			}
			++Points;
		}
		Wrote(Component);
	}
	return Points;
}

int32 USurfEdMode::MeshAdjustPaint(int32 Kind)
{
	// The adjustments on the picked meshes (S238a): the tab's channels of
	// LOD 0's override colours, vertices sharing a position sharing their
	// neighbours for Smooth, as the cavity does.
	TArray<UStaticMeshComponent*> Targets;
	PaintMeshTargets(Targets);
	const USurfSettings* S = GetDefault<USurfSettings>();
	if (Targets.Num() == 0 || !S)
	{
		return 0;
	}
	// Every ticked channel (S248a): R, G and B as the colour, A beside them.
	TArray<ESurfPaintChannel> Channels;
	{
		TArray<ESurfPaintChannel, TInlineAllocator<4>> All;
		S->PaintSet().All(All);
		Channels.Append(All);
	}
	const int32 Times = Kind == USurfSettings::PaintAdjustSmooth ? FMath::Clamp(S->PaintSmoothTimes, 1, 20) : 1;
	int32 Points = 0;
	for (UStaticMeshComponent* Component : Targets)
	{
		const FStaticMeshLODResources* Lod = LodOf(Component);
		if (!Lod) { continue; }
		const int32 Num = Lod->GetNumVertices();
		Component->Modify();
		FColorVertexBuffer* Colours = ColoursOf(Component, true);
		if (!Colours) { continue; }
		TArray<float> Cavity;
		if (Kind == USurfSettings::PaintAdjustCavity)
		{
			MeshCavityOf(Component, Cavity);
		}
		// Neighbours by shared position, for Smooth.
		TArray<int32> Rep;
		TArray<TArray<int32>> Around;
		if (Kind == USurfSettings::PaintAdjustSmooth)
		{
			const FPositionVertexBuffer& Positions = Lod->VertexBuffers.PositionVertexBuffer;
			TMap<FVector3f, int32> Family;
			Rep.SetNumUninitialized(Num);
			for (int32 I = 0; I < Num; ++I)
			{
				const FVector3f P = Positions.VertexPosition(I);
				const int32* Found = Family.Find(P);
				Rep[I] = Found ? *Found : I;
				if (!Found) { Family.Add(P, I); }
			}
			Around.SetNum(Num);
			const FIndexArrayView Indices = Lod->IndexBuffer.GetArrayView();
			for (int32 T = 0; T + 2 < Indices.Num(); T += 3)
			{
				const int32 A = Indices[T], B = Indices[T + 1], C = Indices[T + 2];
				if (A >= Num || B >= Num || C >= Num) { continue; }
				const int32 RA = Rep[A], RB = Rep[B], RC = Rep[C];
				Around[RA].AddUnique(RB); Around[RA].AddUnique(RC);
				Around[RB].AddUnique(RA); Around[RB].AddUnique(RC);
				Around[RC].AddUnique(RA); Around[RC].AddUnique(RB);
			}
		}
		for (const ESurfPaintChannel C : Channels)
		{
			for (int32 Pass = 0; Pass < Times; ++Pass)
			{
				TArray<float> Was;
				Was.SetNumUninitialized(Num);
				for (int32 I = 0; I < Num; ++I) { Was[I] = ChannelOf(Colours->VertexColor(I), C); }
				for (int32 I = 0; I < Num; ++I)
				{
					float Mean = Was[I];
					if (Kind == USurfSettings::PaintAdjustSmooth)
					{
						const TArray<int32>& Near = Around[Rep[I]];
						if (Near.Num() > 0)
						{
							float Sum = 0.0f;
							for (const int32 N : Near) { Sum += Was[N]; }
							Mean = Sum / Near.Num();
						}
					}
					SetChannel(Colours->VertexColor(I), C, USurfSettings::AdjustPaintValue(*S, Kind, Was[I], Mean, Cavity.IsValidIndex(I) ? Cavity[I] : 0.5f));
				}
			}
		}
		Points += Num;
		Wrote(Component);
	}
	if (GEditor) { GEditor->RedrawLevelEditingViewports(); }
	return Points;
}

#undef LOCTEXT_NAMESPACE
