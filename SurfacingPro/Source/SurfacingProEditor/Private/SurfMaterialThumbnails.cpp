// Copyright MapModelers. All Rights Reserved.

#include "SurfMaterialThumbnails.h"

#include "SurfMaterialManager.h"
#include "SurfTextureLib.h"

#include "CanvasTypes.h"
#include "Components/StaticMeshComponent.h"
#include "CoreGlobals.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "EngineModule.h"
#include "HAL/FileManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "ImageUtils.h"
#include "LegacyScreenPercentageDriver.h"
#include "Materials/MaterialInterface.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "RHI.h"
#include "RendererInterface.h"
#include "RenderingThread.h"
#include "SceneView.h"
#include "SceneViewExtension.h"
#include "ShowFlags.h"
#include "Styling/SlateBrush.h"
#include "TextureResource.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

// --- The scene ---------------------------------------------------------------------

FSurfMaterialThumbnailScene::FSurfMaterialThumbnailScene()
	: FThumbnailPreviewScene(FThumbnailPreviewScene::FConstructionValues().SetCreateFloorPlane(false))
{
	if (UStaticMesh* Mesh = SurfMaterialPreview::LoadMesh())
	{
		MeshComponent = NewObject<UStaticMeshComponent>(GetTransientPackage());
		MeshComponent->SetStaticMesh(Mesh);
		AddComponent(MeshComponent, FTransform::Identity);
	}
}

void FSurfMaterialThumbnailScene::SetMaterial(UMaterialInterface* Material)
{
	if (!MeshComponent)
	{
		return;
	}
	// The mesh its kind says: the denser cube for displacement.
	if (UStaticMesh* Mesh = SurfMaterialPreview::LoadMeshFor(Material))
	{
		if (MeshComponent->GetStaticMesh() != Mesh)
		{
			MeshComponent->SetStaticMesh(Mesh);
		}
	}
	const int32 Slot = SurfMaterialPreview::SlotFor(Material);
	const int32 NumSlots = MeshComponent->GetNumMaterials();
	for (int32 Index = 0; Index < NumSlots; ++Index)
	{
		MeshComponent->SetMaterial(Index, Material && Index == Slot ? Material : nullptr);
	}

	// Centred on its bounds, as the engine's material scene does, so a mesh with an off-centre pivot stays in frame.
	FTransform Transform = FTransform::Identity;
	MeshComponent->SetRelativeTransform(Transform);
	MeshComponent->UpdateBounds();
	Transform.SetLocation(-MeshComponent->Bounds.Origin + FVector(0.0, 0.0, GetBoundsZOffset(MeshComponent->Bounds)));
	MeshComponent->SetRelativeTransform(Transform);
	MeshComponent->RecreateRenderState_Concurrent();
}

void FSurfMaterialThumbnailScene::GetViewMatrixParameters(const float InFOVDegrees, FVector& OutOrigin, float& OutOrbitPitch, float& OutOrbitYaw, float& OutOrbitZoom) const
{
	// The engine's framing: tan(half FOV) = half size / distance, with headroom so the corners do not clip.
	const float HalfFOVRadians = FMath::DegreesToRadians<float>(InFOVDegrees) * 0.5f;
	float HalfMeshSize = 100.0f;
	float BoundsZOffset = 0.0f;
	if (MeshComponent)
	{
		HalfMeshSize = static_cast<float>(MeshComponent->Bounds.SphereRadius * 1.15);
		BoundsZOffset = GetBoundsZOffset(MeshComponent->Bounds);
	}
	OutOrigin = FVector(0.0, 0.0, -BoundsZOffset);
	// The manager looks from FRotator(-25, -135, 0); the thumbnail from the opposite corner, where the blend reads.
	OutOrbitPitch = static_cast<float>(SurfMaterialPreview::ViewRotation().Pitch);
	OutOrbitYaw = static_cast<float>(SurfMaterialPreview::ViewRotation().Yaw) + 180.0f;
	OutOrbitZoom = HalfMeshSize / FMath::Tan(HalfFOVRadians);
}

float FSurfMaterialThumbnailScene::GetFOV() const
{
	return SurfMaterialPreview::FOV();
}

// --- Rendering ------------------------------------------------------------------------

namespace SurfMaterialThumbnails
{
	namespace
	{
		constexpr int32 ThumbSize = 128;

		/** In memory for the session, keyed by object path and the asset file's time. */
		TMap<FString, TStrongObjectPtr<UTexture2D>> GPictures;
		TMap<FString, TSharedPtr<FSlateBrush>> GBrushes;
		TSharedPtr<FSurfMaterialThumbnailScene> GScene;

		/** UThumbnailRenderer::RenderViewFamily, which is protected: the screen-percentage driver and the view extensions are not optional. */
		void RenderFamily(FCanvas* Canvas, FSceneViewFamily* ViewFamily, FSceneView* View)
		{
			if (!ViewFamily || !View)
			{
				return;
			}
			ViewFamily->EngineShowFlags.ScreenPercentage = false;
			ViewFamily->SetScreenPercentageInterface(new FLegacyScreenPercentageDriver(*ViewFamily, 1.0f));
			if (GEngine && GEngine->ViewExtensions)
			{
				ViewFamily->ViewExtensions = GEngine->ViewExtensions->GatherActiveExtensions(FSceneViewExtensionContext(ViewFamily->Scene));
				for (const FSceneViewExtensionRef& Extension : ViewFamily->ViewExtensions)
				{
					Extension->SetupViewFamily(*ViewFamily);
					Extension->SetupView(*ViewFamily, *View);
				}
			}
			GetRendererModule().BeginRenderingViewFamily(Canvas, ViewFamily);
		}

		FString KeyFor(UMaterialInterface* Material)
		{
			const FString Package = Material->GetOutermost()->GetName();
			FString File;
			FDateTime Stamp;
			if (FPackageName::DoesPackageExist(Package, &File))
			{
				Stamp = IFileManager::Get().GetTimeStamp(*File);
			}
			return Material->GetPathName() + TEXT("#") + Stamp.ToString();
		}

		/** The asset's file time, or the epoch for one not saved. */
		FDateTime AssetTime(UMaterialInterface* Material)
		{
			FString File;
			if (FPackageName::DoesPackageExist(Material->GetOutermost()->GetName(), &File))
			{
				return IFileManager::Get().GetTimeStamp(*File);
			}
			return FDateTime();
		}

		UTexture2D* TextureFromPixels(const TArray<FColor>& Pixels, int32 Width, int32 Height)
		{
			UTexture2D* Texture = UTexture2D::CreateTransient(Width, Height, PF_B8G8R8A8, NAME_None,
				TConstArrayView64<uint8>(reinterpret_cast<const uint8*>(Pixels.GetData()), static_cast<int64>(Pixels.Num()) * sizeof(FColor)));
			if (Texture)
			{
				Texture->SRGB = true;
				Texture->UpdateResource();
			}
			return Texture;
		}

		bool WritePng(const FString& Path, const TArray<FColor>& Pixels, int32 Width, int32 Height)
		{
			IImageWrapperModule& Module = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
			TSharedPtr<IImageWrapper> Png = Module.CreateImageWrapper(EImageFormat::PNG);
			if (!Png.IsValid() || !Png->SetRaw(Pixels.GetData(), Pixels.Num() * sizeof(FColor), Width, Height, ERGBFormat::BGRA, 8))
			{
				return false;
			}
			const TArray64<uint8> Bytes = Png->GetCompressed();
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
			return FFileHelper::SaveArrayToFile(Bytes, *Path);
		}

		bool ReadPng(const FString& Path, TArray<FColor>& OutPixels, int32& OutWidth, int32& OutHeight)
		{
			TArray64<uint8> Bytes;
			if (!FFileHelper::LoadFileToArray(Bytes, *Path))
			{
				return false;
			}
			IImageWrapperModule& Module = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
			TSharedPtr<IImageWrapper> Png = Module.CreateImageWrapper(EImageFormat::PNG);
			if (!Png.IsValid() || !Png->SetCompressed(Bytes.GetData(), Bytes.Num()))
			{
				return false;
			}
			TArray64<uint8> Raw;
			if (!Png->GetRaw(ERGBFormat::BGRA, 8, Raw))
			{
				return false;
			}
			OutWidth = static_cast<int32>(Png->GetWidth());
			OutHeight = static_cast<int32>(Png->GetHeight());
			if (Raw.Num() < static_cast<int64>(OutWidth) * OutHeight * 4)
			{
				return false;
			}
			OutPixels.SetNumUninitialized(OutWidth * OutHeight);
			FMemory::Memcpy(OutPixels.GetData(), Raw.GetData(), static_cast<SIZE_T>(OutPixels.Num()) * sizeof(FColor));
			return true;
		}
	}

	int32 Size()
	{
		return ThumbSize;
	}

	FString CachePathFor(const FString& Root, const FString& AssetName)
	{
		return Root / TEXT("Thumbnails") / (AssetName + TEXT(".png"));
	}

	bool CanRender()
	{
		return GEditor != nullptr && GIsRHIInitialized && FApp::CanEverRender() && !IsRunningCommandlet();
	}

	bool Render(UMaterialInterface* Material, int32 InSize, TArray<FColor>& OutPixels, FString& OutError)
	{
		OutError.Reset();
		OutPixels.Reset();
		if (!Material)
		{
			OutError = TEXT("render: no material");
			return false;
		}
		if (!CanRender())
		{
			OutError = TEXT("render: this editor cannot render (no RHI)");
			return false;
		}
		if (IsCompiling(Material))
		{
			OutError = TEXT("compiling - a moment");
			return false;
		}
		if (!GScene.IsValid())
		{
			GScene = MakeShared<FSurfMaterialThumbnailScene>();
		}
		if (!GScene->HasMesh())
		{
			OutError = TEXT("render: no preview mesh");
			return false;
		}
		UTextureRenderTarget2D* Target = GEditor->GetScratchRenderTarget(InSize);
		FTextureRenderTargetResource* Resource = Target ? Target->GameThread_GetRenderTargetResource() : nullptr;
		if (!Resource)
		{
			OutError = TEXT("render: no render target");
			return false;
		}

		// The editor's on-screen messages are drawn into a view when they are on; not into a picture of a material.
		TGuardValue<bool> NoScreenMessages(GAreScreenMessagesEnabled, false);

		FCanvas Canvas(Resource, nullptr, FGameTime::GetTimeSinceAppStart(), GMaxRHIFeatureLevel);
		Canvas.Clear(FLinearColor::Black);
		GScene->SetMaterial(Material);
		{
			FSceneViewFamilyContext ViewFamily(FSceneViewFamily::ConstructionValues(Resource, GScene->GetScene(), FEngineShowFlags(ESFIM_Game))
				.SetTime(FGameTime::GetTimeSinceAppStart()));
			ViewFamily.EngineShowFlags.DisableAdvancedFeatures();
			ViewFamily.EngineShowFlags.MotionBlur = 0;
			ViewFamily.EngineShowFlags.AntiAliasing = 0;
			FSceneView* View = GScene->CreateView(&ViewFamily, 0, 0, InSize, InSize);
			RenderFamily(&Canvas, &ViewFamily, View);
		}
		GScene->SetMaterial(nullptr);
		Canvas.Flush_GameThread();
		FlushRenderingCommands();

		OutPixels.SetNumUninitialized(InSize * InSize);
		if (!Resource->ReadPixelsPtr(OutPixels.GetData(), FReadSurfaceDataFlags(), FIntRect(0, 0, InSize, InSize)))
		{
			OutPixels.Reset();
			OutError = TEXT("render: the pixels could not be read back");
			return false;
		}
		for (FColor& Pixel : OutPixels)
		{
			Pixel.A = 255;
		}
		return true;
	}

	UTexture2D* PictureFor(UMaterialInterface* Material, FString& OutError)
	{
		OutError.Reset();
		if (!Material)
		{
			OutError = TEXT("picture: no material");
			return nullptr;
		}
		const FString Key = KeyFor(Material);
		if (const TStrongObjectPtr<UTexture2D>* Known = GPictures.Find(Key))
		{
			return Known->Get();
		}

		// The file under the source root, when it is newer than the asset.
		const FString Path = CachePathFor(SurfTextures::SourceRoot(), Material->GetName());
		TArray<FColor> Pixels;
		int32 Width = 0;
		int32 Height = 0;
		bool bHave = false;
		if (FPaths::FileExists(Path) && IFileManager::Get().GetTimeStamp(*Path) >= AssetTime(Material))
		{
			bHave = ReadPng(Path, Pixels, Width, Height);
		}
		if (!bHave)
		{
			if (!Render(Material, ThumbSize, Pixels, OutError))
			{
				return nullptr;
			}
			Width = ThumbSize;
			Height = ThumbSize;
			WritePng(Path, Pixels, Width, Height);
		}

		UTexture2D* Texture = TextureFromPixels(Pixels, Width, Height);
		if (!Texture)
		{
			OutError = TEXT("picture: no texture could be made");
			return nullptr;
		}
		if (GPictures.Num() > 512)
		{
			Forget();
		}
		GPictures.Add(Key, TStrongObjectPtr<UTexture2D>(Texture));
		return Texture;
	}

	const FSlateBrush* BrushFor(UMaterialInterface* Material, FString& OutError)
	{
		UTexture2D* Texture = PictureFor(Material, OutError);
		if (!Texture)
		{
			return nullptr;
		}
		const FString Key = KeyFor(Material);
		if (const TSharedPtr<FSlateBrush>* Known = GBrushes.Find(Key))
		{
			return Known->Get();
		}
		TSharedPtr<FSlateBrush> Brush = MakeShared<FSlateBrush>();
		Brush->SetResourceObject(Texture);
		Brush->ImageSize = FVector2D(Texture->GetSizeX(), Texture->GetSizeY());
		Brush->DrawAs = ESlateBrushDrawType::Image;
		GBrushes.Add(Key, Brush);
		return Brush.Get();
	}

	void Forget()
	{
		GBrushes.Empty();
		GPictures.Empty();
		GScene.Reset();
	}

	int32 ForgetFiles()
	{
		Forget();
		const FString Dir = FPaths::GetPath(CachePathFor(SurfTextures::SourceRoot(), TEXT("x")));
		TArray<FString> Files;
		IFileManager::Get().FindFiles(Files, *(Dir / TEXT("*.png")), /*Files*/ true, /*Directories*/ false);
		int32 Gone = 0;
		for (const FString& File : Files)
		{
			if (IFileManager::Get().Delete(*(Dir / File), false, true, true))
			{
				++Gone;
			}
		}
		return Gone;
	}

	bool IsCompiling(const UMaterialInterface* Material)
	{
		return Material && (Material->IsCompiling() || !Material->IsComplete());
	}
}
