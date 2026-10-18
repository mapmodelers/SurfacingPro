// Copyright MapModelers. All Rights Reserved.

#include "SurfBake.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/DynamicMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Materials/MaterialInterface.h"
#include "MeshDescription.h"
#include "PhysicsEngine/BodySetup.h"
#include "StaticMeshAttributes.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"
#include "UObject/SavePackage.h"
#include "Misc/PackageName.h"
#include "UObject/Linker.h"
#include "StaticMeshCompiler.h"
#include "HAL/FileManager.h"
#include "Engine/Level.h"

#include "SurfGroup.h"
#include "SurfLog.h"
#include "SurfPrefab.h"
#include "SurfPrefabInstance.h"
#include "SurfSettings.h"

using namespace UE::Geometry;

namespace
{
	// Triangles into a mesh description: one polygon group per material slot,
	// a vertex instance per corner carrying the corner's normal, UV and
	// colour from the overlays - which is exactly what the preview drew.
	void AppendTriangles(const FDynamicMesh3& Mesh, const TArray<int32>& Triangles, int32 SlotCount,
		FMeshDescription& Out, int32& OutTriangles)
	{
		FStaticMeshAttributes Attributes(Out);
		Attributes.Register();
		Attributes.GetVertexInstanceUVs().SetNumChannels(1);

		const FDynamicMeshNormalOverlay* Normals = Mesh.HasAttributes() ? Mesh.Attributes()->PrimaryNormals() : nullptr;
		const FDynamicMeshUVOverlay* UVs = Mesh.HasAttributes() ? Mesh.Attributes()->PrimaryUV() : nullptr;
		const FDynamicMeshColorOverlay* Colours = Mesh.HasAttributes() ? Mesh.Attributes()->PrimaryColors() : nullptr;
		const FDynamicMeshMaterialAttribute* MaterialIDs = Mesh.HasAttributes() ? Mesh.Attributes()->GetMaterialID() : nullptr;

		TArray<FPolygonGroupID> Groups;
		Groups.SetNum(FMath::Max(1, SlotCount));
		for (int32 Slot = 0; Slot < Groups.Num(); ++Slot)
		{
			Groups[Slot] = Out.CreatePolygonGroup();
			Attributes.GetPolygonGroupMaterialSlotNames()[Groups[Slot]] = *FString::Printf(TEXT("Slot_%d"), Slot);
		}

		TMap<int32, FVertexID> Vertices;
		auto VertexFor = [&](int32 Vid) -> FVertexID
		{
			if (const FVertexID* Found = Vertices.Find(Vid))
			{
				return *Found;
			}
			const FVertexID V = Out.CreateVertex();
			Attributes.GetVertexPositions()[V] = FVector3f(Mesh.GetVertex(Vid));
			Vertices.Add(Vid, V);
			return V;
		};

		for (const int32 Tid : Triangles)
		{
			if (!Mesh.IsTriangle(Tid))
			{
				continue;
			}
			int32 Slot = 0;
			if (MaterialIDs)
			{
				MaterialIDs->GetValue(Tid, &Slot);
			}
			Slot = FMath::Clamp(Slot, 0, Groups.Num() - 1);

			const FIndex3i Tri = Mesh.GetTriangle(Tid);
			const FIndex3i NTri = Normals && Normals->IsSetTriangle(Tid) ? Normals->GetTriangle(Tid) : FIndex3i::Invalid();
			const FIndex3i UTri = UVs && UVs->IsSetTriangle(Tid) ? UVs->GetTriangle(Tid) : FIndex3i::Invalid();
			const FIndex3i CTri = Colours && Colours->IsSetTriangle(Tid) ? Colours->GetTriangle(Tid) : FIndex3i::Invalid();

			FVertexInstanceID Corners[3];
			for (int32 K = 0; K < 3; ++K)
			{
				const FVertexInstanceID VI = Out.CreateVertexInstance(VertexFor(Tri[K]));
				Attributes.GetVertexInstanceNormals()[VI] = NTri[K] >= 0 ? Normals->GetElement(NTri[K]) : FVector3f(0.0f, 0.0f, 1.0f);
				Attributes.GetVertexInstanceUVs().Set(VI, 0, UTri[K] >= 0 ? UVs->GetElement(UTri[K]) : FVector2f::ZeroVector);
				Attributes.GetVertexInstanceColors()[VI] = CTri[K] >= 0 ? Colours->GetElement(CTri[K]) : FVector4f(0.0f, 0.0f, 0.0f, 0.0f);
				Corners[K] = VI;
			}
			Out.CreateTriangle(Groups[Slot], { Corners[0], Corners[1], Corners[2] });
			++OutTriangles;
		}
	}

	/**
	 * The mesh on disk, there and then. A bake whose assets are only dirty
	 * in memory is a level that comes back pointing at nothing the next time
	 * it is opened - his "data was maybe corrupt: I had saved after bake"
	 * (S73a). Not while a test is running: a test leaves nothing behind.
	 */
	void SaveBakedPackage(UStaticMesh* Mesh)
	{
		if (!Mesh || GIsAutomationTesting)
		{
			return;
		}
		UPackage* Package = Mesh->GetOutermost();
		if (!Package || Package == GetTransientPackage())
		{
			return;
		}
		const FString File = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.SaveFlags = SAVE_NoError;
		const bool bSaved = UPackage::SavePackage(Package, Mesh, *File, Args);
		UE_LOG(LogSurfacingPro, Display, TEXT("bake: saved %s ok=%d"), *Package->GetName(), bSaved ? 1 : 0);
	}

	// A static mesh asset under the bake folder, made or remade in place so a
	// second bake does not pile up assets: the same name, the same package.
	UStaticMesh* BuildStaticMesh(const FString& Name, const FDynamicMesh3& Mesh, const TArray<int32>& Triangles,
		const TArray<TObjectPtr<UMaterialInterface>>& Slots, int32& OutTriangles)
	{
		const USurfSettings* Settings = GetDefault<USurfSettings>();
		FString Folder = Settings ? Settings->BakeFolder : TEXT("/Game/SurfacingProBaked/");
		Folder.RemoveFromEnd(TEXT("/"));
		const FString PackagePath = Folder / Name;

		UPackage* Package = CreatePackage(*PackagePath);
		if (!Package)
		{
			return nullptr;
		}
		Package->FullyLoad();

		UStaticMesh* StaticMesh = FindObject<UStaticMesh>(Package, *Name);
		/**
		 * The crash of S154a: "Assertion failed: AsyncTask == nullptr" in
		 * the engine's build, from a bake seconds after the one before. A
		 * mesh remade in place while its last build is still running on
		 * another thread has to wait for it; one the tidy let go of is
		 * never taken up again.
		 */
		if (StaticMesh && !IsValid(StaticMesh))
		{
			StaticMesh = nullptr;
		}
		if (StaticMesh && StaticMesh->IsCompiling())
		{
			FStaticMeshCompilingManager::Get().FinishCompilation({ StaticMesh });
		}
		const bool bNew = StaticMesh == nullptr;
		if (bNew)
		{
			StaticMesh = NewObject<UStaticMesh>(Package, *Name, RF_Public | RF_Standalone);
			if (!StaticMesh)
			{
				return nullptr;
			}
			StaticMesh->InitResources();
			StaticMesh->SetLightingGuid();
		}
		else
		{
			StaticMesh->Modify();
		}

		FMeshDescription Description;
		OutTriangles = 0;
		AppendTriangles(Mesh, Triangles, Slots.Num(), Description, OutTriangles);
		if (OutTriangles == 0)
		{
			return nullptr;
		}

		TArray<FStaticMaterial>& Materials = StaticMesh->GetStaticMaterials();
		Materials.Reset();
		for (int32 Slot = 0; Slot < FMath::Max(1, Slots.Num()); ++Slot)
		{
			const FName SlotName = *FString::Printf(TEXT("Slot_%d"), Slot);
			UMaterialInterface* Material = Slots.IsValidIndex(Slot) ? Slots[Slot].Get() : nullptr;
			Materials.Add(FStaticMaterial(Material, SlotName, SlotName));
		}

		// Nanite, with the fallback kept whole: the collision is the surface
		// as it is drawn, and Nanite's own proxy would thin it.
		StaticMesh->GetNaniteSettings().bEnabled = !Settings || Settings->bBakeNanite;
		StaticMesh->GetNaniteSettings().FallbackPercentTriangles = 1.0f;

		UStaticMesh::FBuildMeshDescriptionsParams Params;
		Params.bBuildSimpleCollision = false;
		Params.bCommitMeshDescription = true;
		Params.bMarkPackageDirty = true;
		Params.bFastBuild = false;
		if (!StaticMesh->BuildFromMeshDescriptions({ &Description }, Params))
		{
			return nullptr;
		}

		StaticMesh->CreateBodySetup();
		if (UBodySetup* Body = StaticMesh->GetBodySetup())
		{
			Body->CollisionTraceFlag = ECollisionTraceFlag::CTF_UseComplexAsSimple;
		}

		if (bNew)
		{
			FAssetRegistryModule::AssetCreated(StaticMesh);
		}
		Package->MarkPackageDirty();
		SaveBakedPackage(StaticMesh);
		return StaticMesh;
	}

	/** A short suffix for a set of flags, so the meshes of one cell keep their own names. */
	FString FlagSuffix(int32 Flags)
	{
		FString Suffix;
		if (Flags & static_cast<int32>(ESurfPrimitiveFlags::Detail))       { Suffix += TEXT("_detail"); }
		if (Flags & static_cast<int32>(ESurfPrimitiveFlags::NonColliding)) { Suffix += TEXT("_nocol"); }
		if (Flags & static_cast<int32>(ESurfPrimitiveFlags::NoShadow))     { Suffix += TEXT("_noshadow"); }
		if (Flags & static_cast<int32>(ESurfPrimitiveFlags::NoDraw))       { Suffix += TEXT("_nodraw"); }
		return Suffix;
	}

	/**
	 * What a piece's flags mean once it is a static mesh (S151a; D92 said
	 * "should they drive the bake?" and the author said yes).
	 *
	 * Detail: never in the HLOD, so the far half of a map is not built from
	 * trim. Non-colliding: the component collides with nothing. No shadow:
	 * casts none. No draw: hidden in the game, kept for collision - a
	 * blocker the player walks into and never sees.
	 */
	void ApplyFlags(AStaticMeshActor& Actor, int32 Flags)
	{
		UStaticMeshComponent* Component = Actor.GetStaticMeshComponent();
		if (!Component)
		{
			return;
		}
		if (Flags & static_cast<int32>(ESurfPrimitiveFlags::Detail))
		{
			Actor.bEnableAutoLODGeneration = false;
			Actor.Tags.AddUnique(TEXT("SurfacingProDetail"));
		}
		if (Flags & static_cast<int32>(ESurfPrimitiveFlags::NonColliding))
		{
			Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		}
		if (Flags & static_cast<int32>(ESurfPrimitiveFlags::NoShadow))
		{
			Component->SetCastShadow(false);
		}
		if (Flags & static_cast<int32>(ESurfPrimitiveFlags::NoDraw))
		{
			Component->SetHiddenInGame(true);
			Component->SetCastShadow(false);
			Actor.Tags.AddUnique(TEXT("SurfacingProNoDraw"));
		}
	}

	AStaticMeshActor* PlaceMesh(UWorld& World, UStaticMesh* Mesh, const FTransform& Transform, const FString& Label, AActor* Parent)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		// In the level the group is in, not the current one (S153c): a
		// group in a sublevel or a scene being edited keeps its stand-ins
		// with it, and an attachment never crosses levels.
		Params.OverrideLevel = Parent ? Parent->GetLevel() : nullptr;
		// Not listed in the outliner (S219a): "maybe Bake can not be visible
		// to user? why would it be?" The bake is ours to make and remake; an
		// edit brings the previews back and the next bake replaces it.
		Params.bHideFromSceneOutliner = true;
		AStaticMeshActor* Actor = World.SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(), Transform, Params);
		if (!Actor)
		{
			return nullptr;
		}
		Actor->SetActorLabel(Label);
		Actor->Tags.AddUnique(TEXT("SurfacingProBaked"));
		// No folder (S251i): the outliner does not list it, and a drawer for
		// it only ever showed empty.
		if (UStaticMeshComponent* Component = Actor->GetStaticMeshComponent())
		{
			Component->SetMobility(EComponentMobility::Static);
			Component->SetStaticMesh(Mesh);
		}
		// Attached, so the bake moves with what it stands in for.
		if (Parent)
		{
			Actor->AttachToActor(Parent, FAttachmentTransformRules::KeepWorldTransform);
		}
		return Actor;
	}

	FString CleanName(const FString& In)
	{
		FString Out;
		for (const TCHAR C : In)
		{
			Out.AppendChar(FChar::IsAlnum(C) ? C : TEXT('_'));
		}
		return Out.IsEmpty() ? FString(TEXT("Group")) : Out;
	}

	/**
	 * The bake folder tidied after a group's bake (S153c): meshes wearing
	 * this group's name that this bake did not make - the cells and flag
	 * splits of an earlier bake - and that no stand-in anywhere in the world
	 * still uses, deleted. "has created a shitload of sm?" was also the
	 * ones left behind. Answers how many went.
	 */
	int32 TidyAfter(const ASurfGroup& Group, const FString& Base)
	{
		const UWorld* World = Group.GetWorld();
		if (!World)
		{
			return 0;
		}
		const USurfSettings* Settings = GetDefault<USurfSettings>();
		FString Folder = Settings ? Settings->BakeFolder : TEXT("/Game/SurfacingProBaked/");
		Folder.RemoveFromEnd(TEXT("/"));

		// Every mesh a stand-in in the world uses: the ones that stay.
		TSet<const UStaticMesh*> InUse;
		for (TActorIterator<ASurfGroup> It(const_cast<UWorld*>(World)); It; ++It)
		{
			for (const TObjectPtr<AActor>& Actor : It->BakedActors)
			{
				const AStaticMeshActor* StandIn = Cast<AStaticMeshActor>(Actor.Get());
				if (StandIn && StandIn->GetStaticMeshComponent())
				{
					InUse.Add(StandIn->GetStaticMeshComponent()->GetStaticMesh());
				}
			}
		}
		for (TActorIterator<ASurfPrefabInstance> It(const_cast<UWorld*>(World)); It; ++It)
		{
			const AStaticMeshActor* StandIn = Cast<AStaticMeshActor>(It->BakedActor.Get());
			if (StandIn && StandIn->GetStaticMeshComponent())
			{
				InUse.Add(StandIn->GetStaticMeshComponent()->GetStaticMesh());
			}
		}

		const FAssetRegistryModule& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		TArray<FAssetData> Assets;
		Registry.Get().GetAssetsByPath(FName(*Folder), Assets, /*bRecursive*/ false);

		TArray<UObject*> Stale;
		for (const FAssetData& Asset : Assets)
		{
			const FString Name = Asset.AssetName.ToString();
			if (Name != Base && !Name.StartsWith(Base + TEXT("_")))
			{
				continue;
			}
			// Anything else on disk that points at it - another level's
			// stand-ins, say - keeps it; only a mesh nothing refers to goes.
			TArray<FName> Referencers;
			Registry.Get().GetReferencers(Asset.PackageName, Referencers);
			bool bHeld = false;
			for (const FName& Referencer : Referencers)
			{
				// A stand-in this bake just took down still counts on disk
				// until the level is saved; loaded and dead, it holds nothing.
				UPackage* Package = FindPackage(nullptr, *Referencer.ToString());
				if (!Package)
				{
					bHeld = true;
					break;
				}
				bool bAlive = false;
				ForEachObjectWithPackage(Package, [&bAlive](UObject* Object)
				{
					if (Cast<AActor>(Object) && IsValid(Object))
					{
						bAlive = true;
						return false;
					}
					return true;
				}, EGetObjectsFlags::None);
				if (bAlive)
				{
					bHeld = true;
					break;
				}
			}
			if (bHeld)
			{
				continue;
			}
			UStaticMesh* Mesh = Cast<UStaticMesh>(Asset.GetAsset());
			if (Mesh && !InUse.Contains(Mesh))
			{
				Stale.Add(Mesh);
			}
		}
		// Let go gently: no forced garbage collection in the middle of a
		// bake that may itself be running inside a save. The object is
		// marked garbage and the file removed; the next collection takes
		// the rest.
		int32 Gone = 0;
		for (UObject* Object : Stale)
		{
			UPackage* Package = Object->GetOutermost();
			FString File;
			if (!FPackageName::TryConvertLongPackageNameToFilename(Package->GetName(), File, FPackageName::GetAssetPackageExtension()))
			{
				continue;
			}
			// A build still running on it finishes first; then it and its
			// package are renamed out of the way, so a bake that wants the
			// name back a moment later makes a fresh one and never meets
			// the corpse (S154a).
			if (UStaticMesh* Mesh = Cast<UStaticMesh>(Object))
			{
				if (Mesh->IsCompiling())
				{
					FStaticMeshCompilingManager::Get().FinishCompilation({ Mesh });
				}
			}
			ResetLoaders(Package);
			FAssetRegistryModule::AssetDeleted(Object);
			const ERenameFlags Flags = REN_DontCreateRedirectors | REN_NonTransactional | REN_DoNotDirty;
			Object->ClearFlags(RF_Standalone | RF_Public);
			Object->Rename(*MakeUniqueObjectName(GetTransientPackage(), Object->GetClass(), TEXT("SP_Stale")).ToString(), GetTransientPackage(), Flags);
			Object->MarkAsGarbage();
			Package->SetDirtyFlag(false);
			Package->ClearFlags(RF_Standalone);
			Package->Rename(*MakeUniqueObjectName(nullptr, UPackage::StaticClass(), TEXT("/Temp/SP_Stale")).ToString(), nullptr, Flags);
			Package->MarkAsGarbage();
			IFileManager::Get().Delete(*File, /*bRequireExists*/ false, /*bEvenReadOnly*/ true, /*bQuiet*/ true);
			++Gone;
		}
		return Gone;
	}
}

namespace SurfBake
{
	bool BakeGroup(ASurfGroup& Group, FReport& Report)
	{
		UWorld* World = Group.GetWorld();
		if (!World || !Group.Preview)
		{
			Report.Problems.Add(FString::Printf(TEXT("%s: no preview to bake"), *Group.GetActorLabel()));
			return false;
		}

		Group.Unbake();

		const USurfSettings* Settings = GetDefault<USurfSettings>();
		const double CellCm = Settings ? Settings->BakeCellCm : 0.0;
		const FString Base = FString::Printf(TEXT("SM_SP_%s"), *CleanName(Group.GetActorLabel()));

		/**
		 * Triangles by cell, in the group's own space: a cell is a square of
		 * the floor plan, so a long wall becomes a run of meshes that stream
		 * in as the player reaches them rather than one that is always in.
		 * And by flags within the cell (S151a): a piece that casts no shadow
		 * cannot share a mesh with one that does.
		 *
		 * **A piece is never cut by a cell** (S151b): "has created a
		 * shitload of sm?" - fourteen triangles in ten meshes, because the
		 * cell was decided per triangle and a ten-metre patch straddling a
		 * corner fell into four, the two-metre box on the origin into four
		 * more. The cell is the piece's now, by the middle of its
		 * triangles, so a piece is one mesh however it lies on the grid; a
		 * piece longer than a cell simply streams in a little early.
		 */
		struct FBucket
		{
			FIntPoint Cell;
			int32 Flags = 0;
			bool operator==(const FBucket& Other) const { return Cell == Other.Cell && Flags == Other.Flags; }
		};
		TArray<TPair<FBucket, TArray<int32>>> Cells;
		Group.Preview->ProcessMesh([&](const FDynamicMesh3& Mesh)
		{
			// Where each piece's middle is, by its triangle group.
			TMap<int32, FVector> PieceSum;
			TMap<int32, int32> PieceCount;
			for (const int32 Tid : Mesh.TriangleIndicesItr())
			{
				const int32 Piece = Mesh.GetTriangleGroup(Tid);
				PieceSum.FindOrAdd(Piece) += Mesh.GetTriCentroid(Tid);
				PieceCount.FindOrAdd(Piece) += 1;
			}

			for (const int32 Tid : Mesh.TriangleIndicesItr())
			{
				const int32 Piece = Mesh.GetTriangleGroup(Tid);
				const FVector Centre = PieceCount.FindRef(Piece) > 0
					? PieceSum.FindRef(Piece) / PieceCount.FindRef(Piece)
					: Mesh.GetTriCentroid(Tid);
				FBucket Key;
				Key.Cell = CellCm > 1.0
					? FIntPoint(FMath::FloorToInt(Centre.X / CellCm), FMath::FloorToInt(Centre.Y / CellCm))
					: FIntPoint(0, 0);
				Key.Flags = Group.FlagsOfTriangleGroup(Piece);
				TPair<FBucket, TArray<int32>>* Found = Cells.FindByPredicate([&Key](const TPair<FBucket, TArray<int32>>& Each)
				{
					return Each.Key == Key;
				});
				if (!Found)
				{
					Found = &Cells.Emplace_GetRef(Key, TArray<int32>());
				}
				Found->Value.Add(Tid);
			}
		});
		if (Cells.Num() == 0)
		{
			Report.Problems.Add(FString::Printf(TEXT("%s: nothing in it"), *Group.GetActorLabel()));
			return false;
		}

		Group.Modify();
		int32 Made = 0;
		Group.Preview->ProcessMesh([&](const FDynamicMesh3& Mesh)
		{
			for (const TPair<FBucket, TArray<int32>>& Cell : Cells)
			{
				const FString Name = (Cells.Num() == 1
					? Base
					: FString::Printf(TEXT("%s_%d_%d"), *Base, Cell.Key.Cell.X, Cell.Key.Cell.Y))
					+ FlagSuffix(Cell.Key.Flags);
				int32 Triangles = 0;
				UStaticMesh* Baked = BuildStaticMesh(Name, Mesh, Cell.Value, Group.PreviewMaterials, Triangles);
				if (!Baked)
				{
					continue;
				}
				if (AStaticMeshActor* Actor = PlaceMesh(*World, Baked, Group.GetActorTransform(), Name, &Group))
				{
					ApplyFlags(*Actor, Cell.Key.Flags);
					Group.BakedActors.Add(Actor);
					++Report.Actors;
				}
				++Report.Meshes;
				Report.Triangles += Triangles;
				++Made;
			}
		});

		if (Made == 0)
		{
			Report.Problems.Add(FString::Printf(TEXT("%s: no mesh could be built from it"), *Group.GetActorLabel()));
			return false;
		}
		Group.bBaked = true;
		// The game's copy: the editor keeps drawing the preview (S153a).
		Group.ApplyBakeLook();
		++Report.Groups;
		Report.Tidied += TidyAfter(Group, Base);
		return true;
	}

	bool BakeInstance(ASurfPrefabInstance& Instance, FReport& Report)
	{
		UWorld* World = Instance.GetWorld();
		if (!World || !Instance.Preview || !Instance.Prefab)
		{
			Report.Problems.Add(FString::Printf(TEXT("%s: no prefab asset behind it"), *Instance.GetActorLabel()));
			return false;
		}

		Instance.Unbake();

		// One mesh per asset, whatever the count of instances: the second
		// instance of a door frame finds the mesh the first one made and
		// places it again, which is what a prefab was for.
		// Its own paint (S251j): a mesh of its own.
		const FString Name = Instance.bOverridePaint && Instance.PaintOverrides.Num() > 0
			? FString::Printf(TEXT("SM_SP_%s_%s"), *CleanName(Instance.Prefab->GetName()), *CleanName(Instance.GetActorLabel()))
			: FString::Printf(TEXT("SM_SP_%s"), *CleanName(Instance.Prefab->GetName()));
		int32 Triangles = 0;
		UStaticMesh* Baked = nullptr;
		Instance.Preview->ProcessMesh([&](const FDynamicMesh3& Mesh)
		{
			TArray<int32> All;
			All.Reserve(Mesh.TriangleCount());
			for (const int32 Tid : Mesh.TriangleIndicesItr())
			{
				All.Add(Tid);
			}
			Baked = BuildStaticMesh(Name, Mesh, All, Instance.PreviewMaterials, Triangles);
		});
		if (!Baked)
		{
			Report.Problems.Add(FString::Printf(TEXT("%s: the prefab's mesh could not be built"), *Instance.GetActorLabel()));
			return false;
		}

		Instance.Modify();
		if (AStaticMeshActor* Actor = PlaceMesh(*World, Baked, Instance.GetActorTransform(), Name, &Instance))
		{
			// The placement's own flags, over the whole of it (S201a).
			if (Instance.bOverrideFlags)
			{
				ApplyFlags(*Actor, Instance.OverrideFlags);
			}
			Instance.BakedActor = Actor;
			++Report.Actors;
		}
		Instance.bBaked = true;
		Instance.ApplyBakeLook();
		++Report.Instances;
		++Report.Meshes;
		Report.Triangles += Triangles;
		return true;
	}

	TSharedRef<FJob> PlanLevelBake(UWorld* World, bool bSkipStanding, const ULevel* OnlyLevel)
	{
		TSharedRef<FJob> Job = MakeShared<FJob>();
		Job->Started = FPlatformTime::Seconds();
		if (!World)
		{
			return Job;
		}

		// A bake that is standing is current - an edit would have dropped
		// it - so the automatic bake leaves it and does the rest (S153a).
		for (TActorIterator<ASurfGroup> It(World); It; ++It)
		{
			if (*It && It->NumPrimitives() > 0 && !(bSkipStanding && It->bBaked && It->BakeIsShowing())
				&& (!OnlyLevel || It->GetLevel() == OnlyLevel))
			{
				Job->Groups.Add(*It);
			}
		}
		for (TActorIterator<ASurfPrefabInstance> It(World); It; ++It)
		{
			if (*It && It->Prefab && !(bSkipStanding && It->bBaked && It->BakeIsShowing())
				&& (!OnlyLevel || It->GetLevel() == OnlyLevel))
			{
				Job->Instances.Add(*It);
			}
		}
		return Job;
	}

	bool StepBake(FJob& Job, double BudgetSeconds)
	{
		// Assets are built once per name: the instances of one prefab share
		// theirs, so a pass that has already built one finds it again.
		const double Until = FPlatformTime::Seconds() + FMath::Max(0.0, BudgetSeconds);
		const int32 Total = Job.Total();

		while (!Job.bStopped && Job.Next < Total)
		{
			if (Job.Next < Job.Groups.Num())
			{
				if (ASurfGroup* Group = Job.Groups[Job.Next].Get())
				{
					BakeGroup(*Group, Job.Report);
				}
			}
			else if (ASurfPrefabInstance* Instance = Job.Instances[Job.Next - Job.Groups.Num()].Get())
			{
				BakeInstance(*Instance, Job.Report);
			}
			++Job.Next;

			// The budget is checked after a piece, never during one: a group
			// half-baked is not a state this can be left in.
			if (BudgetSeconds > 0.0 && FPlatformTime::Seconds() >= Until)
			{
				break;
			}
		}

		Job.Report.Seconds = FPlatformTime::Seconds() - Job.Started;
		if (Job.IsFinished())
		{
			SURF_METRIC_N("bake.seconds", Job.Report.Seconds, "s", FMath::Max(1, Job.Report.Meshes));
			return false;
		}
		return true;
	}

	FReport BakeLevel(UWorld* World, bool bSkipStanding, const ULevel* OnlyLevel)
	{
		// The foreground bake is the same job with no budget: one slice, and
		// the slice is all of it. Two paths through one piece of code, so the
		// background bake cannot drift away from the one the tests run.
		TSharedRef<FJob> Job = PlanLevelBake(World, bSkipStanding, OnlyLevel);
		StepBake(*Job, 0.0);
		return Job->Report;
	}

	int32 UnbakeLevel(UWorld* World)
	{
		int32 Count = 0;
		if (!World)
		{
			return 0;
		}
		for (TActorIterator<ASurfGroup> It(World); It; ++It)
		{
			if (*It && It->bBaked)
			{
				It->Modify();
				It->Unbake();
				++Count;
			}
		}
		for (TActorIterator<ASurfPrefabInstance> It(World); It; ++It)
		{
			if (*It && It->bBaked)
			{
				It->Modify();
				It->Unbake();
				++Count;
			}
		}
		return Count;
	}
}
