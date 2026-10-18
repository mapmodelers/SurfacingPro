// Copyright MapModelers. All Rights Reserved.

#include "SurfPreviewMaterial.h"

#include "SurfGroup.h"
#include "SurfSettings.h"

#include "Engine/Texture2D.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionVertexNormalWS.h"
#include "Materials/MaterialExpressionTransform.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionDotProduct.h"
#include "Materials/MaterialExpressionSaturate.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionVertexColor.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "Materials/MaterialInterface.h"
#include "MaterialShared.h"
#include "MaterialEditingLibrary.h"
#include "UObject/Package.h"
#include "UObject/UObjectThreadContext.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionOneMinus.h"
#include "Materials/MaterialExpressionPower.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionSceneTexture.h"
#include "Materials/MaterialExpressionStep.h"
#include "Materials/MaterialExpressionConstant2Vector.h"
#include "Materials/MaterialExpressionScreenPosition.h"
#include "TextureResource.h"
#include "Engine/BlendableInterface.h"

namespace SurfPreviewMaterial
{
namespace
{
	/** One tile of texture. Big enough to read, small enough to build instantly. */
	constexpr int32 TextureSize = 256;
	constexpr int32 Squares = 8;

	TWeakObjectPtr<UMaterialInterface> CachedChecker;
	TWeakObjectPtr<UTexture2D> CachedTexture;

	/** The greys the checker was last filled between, so a changed setting can be told from an unchanged one. */
	float FilledDarkest = -1.0f;
	float FilledLightest = -1.0f;

	/** The darkest and lightest grey asked for, in order and apart. */
	void ReadLevels(float& OutDarkest, float& OutLightest)
	{
		const USurfSettings* Settings = GetDefault<USurfSettings>();
		OutDarkest = Settings ? FMath::Clamp(Settings->CheckerDarkest, 0.0f, 1.0f) : 0.30f;
		OutLightest = Settings ? FMath::Clamp(Settings->CheckerLightest, 0.0f, 1.0f) : 0.62f;
		if (OutLightest < OutDarkest + 0.05f)
		{
			OutLightest = FMath::Min(1.0f, OutDarkest + 0.05f);
		}
	}

	FColor PixelAt(int32 X, int32 Y, float Darkest, float Lightest)
	{
		/**
		 * Greyscale, on purpose. The first version had an orange tile edge
		 * and a red corner; both read as features of the surface rather than
		 * of the texture, and they fought every other colour on the screen.
		 *
		 * What has to be readable is texture space: which way U and V run,
		 * where the tile starts, how dense it is. A gradient does that - dark
		 * at the tile's origin, light at its far corner, multiplied over the
		 * checker - so a flip, a swap or a rotation shows as the gradient
		 * running the other way, and the tile edge shows as the jump from
		 * light back to dark.
		 */
		const int32 Square = TextureSize / Squares;
		const bool bOdd = (((X / Square) + (Y / Square)) % 2) != 0;
		const float Checker = bOdd ? 0.72f : 0.42f;

		const float U = (X + 0.5f) / TextureSize;
		const float V = (Y + 0.5f) / TextureSize;
		const float Gradient = 0.45f + 0.55f * (0.5f * (U + V));

		// Levelled: the raw checker runs from 0.42 * 0.45 to 0.72 * 1.0, which
		// is nearly black to nearly white - it glared in the light and vanished
		// in shadow. The same gradient is laid between the two greys asked
		// for, so it reads as texture space without shouting.
		constexpr float RawLow = 0.42f * 0.45f;
		constexpr float RawHigh = 0.72f * 1.0f;
		const float Raw = (Checker * Gradient - RawLow) / (RawHigh - RawLow);
		const float Levelled = FMath::Lerp(Darkest, Lightest, FMath::Clamp(Raw, 0.0f, 1.0f));

		const uint8 Grey = static_cast<uint8>(FMath::Clamp(Levelled * 255.0f, 0.0f, 255.0f));
		return FColor(Grey, Grey, Grey);
	}

	/** Write the checker's pixels into the texture's mip and source at the given levels. */
	bool FillChecker(UTexture2D* Texture, float Darkest, float Lightest)
	{
		FTexturePlatformData* Data = Texture ? Texture->GetPlatformData() : nullptr;
		if (!Data || Data->Mips.Num() == 0)
		{
			return false;
		}

		FTexture2DMipMap& Mip = Data->Mips[0];
		uint8* Bytes = static_cast<uint8*>(Mip.BulkData.Lock(LOCK_READ_WRITE));

		if (!Bytes)
		{
			Mip.BulkData.Unlock();
			return false;
		}

		for (int32 Y = 0; Y < TextureSize; ++Y)
		{
			for (int32 X = 0; X < TextureSize; ++X)
			{
				const FColor Colour = PixelAt(X, Y, Darkest, Lightest);
				uint8* Pixel = Bytes + ((Y * TextureSize) + X) * 4;

				Pixel[0] = Colour.B;
				Pixel[1] = Colour.G;
				Pixel[2] = Colour.R;
				Pixel[3] = 255;
			}
		}

		Mip.BulkData.Unlock();

#if WITH_EDITORONLY_DATA
		// The source is what the editor compiles from; without it the texture
		// rebuilds to white the first time anything touches it.
		if (!Texture->Source.IsValid())
		{
			Texture->Source.Init(TextureSize, TextureSize, 1, 1, TSF_BGRA8);
		}
		if (uint8* Source = Texture->Source.LockMip(0))
		{
			for (int32 Y = 0; Y < TextureSize; ++Y)
			{
				for (int32 X = 0; X < TextureSize; ++X)
				{
					const FColor Colour = PixelAt(X, Y, Darkest, Lightest);
					uint8* Pixel = Source + ((Y * TextureSize) + X) * 4;

					Pixel[0] = Colour.B;
					Pixel[1] = Colour.G;
					Pixel[2] = Colour.R;
					Pixel[3] = 255;
				}
			}
			Texture->Source.UnlockMip(0);
		}
#endif

		Texture->UpdateResource();
		FilledDarkest = Darkest;
		FilledLightest = Lightest;
		return true;
	}

	UTexture2D* BuildCheckerTexture()
	{
		UTexture2D* Texture = UTexture2D::CreateTransient(TextureSize, TextureSize, PF_B8G8R8A8);
		if (!Texture)
		{
			return nullptr;
		}

		Texture->AddToRoot();
		Texture->SRGB = true;
		Texture->AddressX = TA_Wrap;
		Texture->AddressY = TA_Wrap;
		Texture->CompressionSettings = TC_Default;
		Texture->MipGenSettings = TMGS_FromTextureGroup;
		Texture->NeverStream = true;

		float Darkest = 0.30f;
		float Lightest = 0.62f;
		ReadLevels(Darkest, Lightest);
		if (!FillChecker(Texture, Darkest, Lightest))
		{
			return nullptr;
		}

		CachedTexture = Texture;
		return Texture;
	}

	/** The shade the checker material was last built for. */
	ESurfPreviewShade BuiltShade = ESurfPreviewShade::Shaded;

	ESurfPreviewShade WantedShade()
	{
		const USurfSettings* Settings = GetDefault<USurfSettings>();
		return Settings ? Settings->PreviewShade : ESurfPreviewShade::Shaded;
	}

	UMaterialInterface* BuildChecker()
	{
		UTexture2D* Texture = CachedTexture.Get();
		if (!Texture)
		{
			Texture = BuildCheckerTexture();
		}
		if (!Texture)
		{
			return nullptr;
		}

		const ESurfPreviewShade Shade = WantedShade();
		BuiltShade = Shade;

		UMaterial* Material = NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Transient);
		if (!Material)
		{
			return nullptr;
		}

		Material->AddToRoot();

		UMaterialExpressionTextureSample* Sample = Cast<UMaterialExpressionTextureSample>(
			UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionTextureSample::StaticClass(), -350, 0));

		if (!Sample)
		{
			return nullptr;
		}

		// No UV input: the sample takes UV0, which is exactly the channel
		// every verb in the inspector writes. That is the whole point.
		Sample->Texture = Texture;
		Sample->SamplerType = SAMPLERTYPE_Color;

		if (Shade == ESurfPreviewShade::Lit)
		{
			UMaterialEditingLibrary::ConnectMaterialProperty(Sample, TEXT(""), MP_BaseColor);
		}
		else
		{
			/**
			 * Shaded by the view alone, the way a modelling package's solid
			 * view is: unlit, the checker multiplied by a shade from a fixed
			 * light that turns with the camera - up and to the left of the
			 * eye - so the shape reads and nothing in the level's lighting is
			 * paid for. Smooth, because the normal is the vertex normal.
			 */
			auto Make = [Material](UClass* Class, int32 X, int32 Y) -> UMaterialExpression*
			{
				return UMaterialEditingLibrary::CreateMaterialExpression(Material, Class, X, Y);
			};

			UMaterialExpressionVertexNormalWS* Normal = Cast<UMaterialExpressionVertexNormalWS>(Make(UMaterialExpressionVertexNormalWS::StaticClass(), -900, 200));
			UMaterialExpressionTransform* ToView = Cast<UMaterialExpressionTransform>(Make(UMaterialExpressionTransform::StaticClass(), -720, 200));
			UMaterialExpressionConstant3Vector* Light = Cast<UMaterialExpressionConstant3Vector>(Make(UMaterialExpressionConstant3Vector::StaticClass(), -720, 320));
			UMaterialExpressionDotProduct* Facing = Cast<UMaterialExpressionDotProduct>(Make(UMaterialExpressionDotProduct::StaticClass(), -540, 240));
			UMaterialExpressionSaturate* Clamped = Cast<UMaterialExpressionSaturate>(Make(UMaterialExpressionSaturate::StaticClass(), -420, 240));
			UMaterialExpressionLinearInterpolate* Shade_ = Cast<UMaterialExpressionLinearInterpolate>(Make(UMaterialExpressionLinearInterpolate::StaticClass(), -300, 240));
			UMaterialExpressionMultiply* Together = Cast<UMaterialExpressionMultiply>(Make(UMaterialExpressionMultiply::StaticClass(), -150, 60));

			if (!Normal || !ToView || !Light || !Facing || !Clamped || !Shade_ || !Together)
			{
				return nullptr;
			}

			ToView->TransformSourceType = TRANSFORMSOURCE_World;
			ToView->TransformType = TRANSFORM_View;
			ToView->Input.Connect(0, Normal);

			// View space: x right, y up, z into the screen. The light sits up
			// and to the left, a little in front of the eye.
			Light->Constant = FLinearColor(-0.35f, 0.55f, -0.76f, 1.0f);

			Facing->A.Connect(0, ToView);
			Facing->B.Connect(0, Light);
			Clamped->Input.Connect(0, Facing);

			// Never black: what faces away is still a readable grey.
			Shade_->ConstA = 0.42f;
			Shade_->ConstB = 1.08f;
			Shade_->Alpha.Connect(0, Clamped);

			Together->A.Connect(0, Sample);
			Together->B.Connect(0, Shade_);

			Material->SetShadingModel(MSM_Unlit);
			UMaterialEditingLibrary::ConnectMaterialProperty(Together, TEXT(""), MP_EmissiveColor);
		}

		/**
		 * Compiled the quiet way.
		 *
		 * UMaterialEditingLibrary::RecompileMaterial is the material editor's
		 * verb: after compiling it rebuilds texture streaming data, which runs
		 * CollectGarbage twice and pops a progress dialog. That is fine at a
		 * button in the material editor. It was fatal here: the first face
		 * ever picked built the highlight material from inside the viewport's
		 * draw, the garbage collector ran in the middle of the draw, and the
		 * editor went down - see the 2026-09-12 crash. PostEditChange inside
		 * an update context is the whole of what a new material needs.
		 */
		{
			FMaterialUpdateContext UpdateContext;
			UpdateContext.AddMaterial(Material);
			Material->PreEditChange(nullptr);
			Material->PostEditChange();
		}

		return Material;
	}
}

UMaterialInterface* GetChecker()
{
	if (CachedChecker.IsValid())
	{
		return CachedChecker.Get();
	}

	// Never from inside a draw or a load: compiling a material recreates the
	// render state of everything that uses one, and neither place can take
	// that. The mode warms the checker when it is entered, so this branch is
	// the fallback, not the plan.
	if (FUObjectThreadContext::Get().IsRoutingPostLoad || IsInRenderingThread())
	{
		return nullptr;
	}

	UMaterialInterface* Built = BuildChecker();
	CachedChecker = Built;
	return Built;
}

namespace
{
	TWeakObjectPtr<UMaterialInterface> CachedGizmoMaterial;

	UMaterialInterface* BuildGizmoMaterial()
	{
		UMaterial* Material = NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Transient);
		if (!Material)
		{
			return nullptr;
		}
		Material->AddToRoot();

		UMaterialExpressionVectorParameter* Colour = Cast<UMaterialExpressionVectorParameter>(
			UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionVectorParameter::StaticClass(), -300, 0));
		if (!Colour)
		{
			return nullptr;
		}
		Colour->ParameterName = FName(TEXT("Color"));
		Colour->DefaultValue = FLinearColor::White;

		Material->SetShadingModel(MSM_Unlit);
		Material->BlendMode = BLEND_Translucent;
		Material->TwoSided = true;
		UMaterialEditingLibrary::ConnectMaterialProperty(Colour, TEXT(""), MP_EmissiveColor);
		UMaterialEditingLibrary::ConnectMaterialProperty(Colour, TEXT("A"), MP_Opacity);

		{
			FMaterialUpdateContext UpdateContext;
			UpdateContext.AddMaterial(Material);
			Material->PreEditChange(nullptr);
			Material->PostEditChange();
		}
		return Material;
	}
}

namespace
{
	TWeakObjectPtr<UMaterialInterface> CachedPaintView[5];

	UMaterialInterface* BuildPaintView(int32 Channel)
	{
		UMaterial* Material = NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Transient);
		if (!Material)
		{
			return nullptr;
		}
		Material->AddToRoot();

		auto Make = [Material](UClass* Class, int32 X, int32 Y) -> UMaterialExpression*
		{
			return UMaterialEditingLibrary::CreateMaterialExpression(Material, Class, X, Y);
		};

		// The amount painted: the vertex colour's channel, straight off the
		// mesh - outputs 1 to 4 of the node are R, G, B and A.
		UMaterialExpressionVertexColor* Vertex = Cast<UMaterialExpressionVertexColor>(Make(UMaterialExpressionVertexColor::StaticClass(), -900, -100));
		UMaterialExpressionConstant3Vector* Dark = Cast<UMaterialExpressionConstant3Vector>(Make(UMaterialExpressionConstant3Vector::StaticClass(), -700, -200));
		UMaterialExpressionConstant3Vector* Bright = Cast<UMaterialExpressionConstant3Vector>(Make(UMaterialExpressionConstant3Vector::StaticClass(), -700, -60));
		UMaterialExpressionLinearInterpolate* Mix = Cast<UMaterialExpressionLinearInterpolate>(Make(UMaterialExpressionLinearInterpolate::StaticClass(), -480, -120));

		// The same shade from a fixed light that the checker wears, so the
		// shape reads under the paint.
		UMaterialExpressionVertexNormalWS* Normal = Cast<UMaterialExpressionVertexNormalWS>(Make(UMaterialExpressionVertexNormalWS::StaticClass(), -900, 200));
		UMaterialExpressionTransform* ToView = Cast<UMaterialExpressionTransform>(Make(UMaterialExpressionTransform::StaticClass(), -720, 200));
		UMaterialExpressionConstant3Vector* Light = Cast<UMaterialExpressionConstant3Vector>(Make(UMaterialExpressionConstant3Vector::StaticClass(), -720, 320));
		UMaterialExpressionDotProduct* Facing = Cast<UMaterialExpressionDotProduct>(Make(UMaterialExpressionDotProduct::StaticClass(), -540, 240));
		UMaterialExpressionSaturate* Clamped = Cast<UMaterialExpressionSaturate>(Make(UMaterialExpressionSaturate::StaticClass(), -420, 240));
		UMaterialExpressionLinearInterpolate* Shade = Cast<UMaterialExpressionLinearInterpolate>(Make(UMaterialExpressionLinearInterpolate::StaticClass(), -300, 240));
		UMaterialExpressionMultiply* Together = Cast<UMaterialExpressionMultiply>(Make(UMaterialExpressionMultiply::StaticClass(), -150, 60));

		if (!Vertex || !Dark || !Bright || !Mix || !Normal || !ToView || !Light || !Facing || !Clamped || !Shade || !Together)
		{
			return nullptr;
		}

		// Black where nothing is painted (S237a: "why it shows some gray
		// instead of black when its all set to 0" - "Black").
		Dark->Constant = FLinearColor(0.0f, 0.0f, 0.0f, 1.0f);
		// The channel's own colour, pure (S248a: "are we sure the view mode
		// shows red as full red?" - it was an orange-red, a green-blue and a
		// sky blue, shaded down to four tenths).
		static const FLinearColor Colours[4] =
		{
			FLinearColor(1.0f, 0.0f, 0.0f, 1.0f),
			FLinearColor(0.0f, 1.0f, 0.0f, 1.0f),
			FLinearColor(0.0f, 0.0f, 1.0f, 1.0f),
			FLinearColor(1.0f, 1.0f, 1.0f, 1.0f),
		};
		Bright->Constant = Colours[FMath::Clamp(Channel, 0, 3)];

		Mix->A.Connect(0, Dark);
		Mix->B.Connect(0, Bright);
		Mix->Alpha.Connect(FMath::Clamp(Channel, 0, 3) + 1, Vertex);

		ToView->TransformSourceType = TRANSFORMSOURCE_World;
		ToView->TransformType = TRANSFORM_View;
		ToView->Input.Connect(0, Normal);
		Light->Constant = FLinearColor(-0.35f, 0.55f, -0.76f, 1.0f);
		Facing->A.Connect(0, ToView);
		Facing->B.Connect(0, Light);
		Clamped->Input.Connect(0, Facing);
		// A light shade, so the shape reads and a face towards the light
		// shows the value exactly; one turned away three quarters of it.
		Shade->ConstA = 0.75f;
		Shade->ConstB = 1.0f;
		Shade->Alpha.Connect(0, Clamped);

		// The view is drawn gamma only (the view extension turns the
		// tonemapper off while it shows): a channel's value is a value, so it
		// goes out through the inverse of the screen's gamma and reads on the
		// screen as painted - 1 is 255, a half is 128. The colour view (4) is
		// the vertex colour itself, stored linear, shaded the same way.
		UMaterialExpressionPower* Gamma = Cast<UMaterialExpressionPower>(Make(UMaterialExpressionPower::StaticClass(), 0, 60));
		UMaterialExpressionPower* ShadeGamma = Cast<UMaterialExpressionPower>(Make(UMaterialExpressionPower::StaticClass(), -150, 260));
		if (!Gamma || !ShadeGamma)
		{
			return nullptr;
		}
		Gamma->ConstExponent = 2.2f;
		ShadeGamma->ConstExponent = 2.2f;
		UMaterialExpression* Out = Together;
		if (Channel >= 4)
		{
			ShadeGamma->Base.Connect(0, Shade);
			Together->A.Connect(0, Vertex);
			Together->B.Connect(0, ShadeGamma);
		}
		else
		{
			Together->A.Connect(0, Mix);
			Together->B.Connect(0, Shade);
			Gamma->Base.Connect(0, Together);
			Out = Gamma;
		}

		Material->SetShadingModel(MSM_Unlit);
		Material->TwoSided = true;
		UMaterialEditingLibrary::ConnectMaterialProperty(Out, TEXT(""), MP_EmissiveColor);

		{
			FMaterialUpdateContext UpdateContext;
			UpdateContext.AddMaterial(Material);
			Material->PreEditChange(nullptr);
			Material->PostEditChange();
		}
		return Material;
	}
}

namespace
{
	TWeakObjectPtr<UMaterialInstanceDynamic> CachedMatcap;
	TWeakObjectPtr<UMaterialInstanceDynamic> CachedBackdrop;
	TWeakObjectPtr<UMaterialInstanceDynamic> CachedMatcapPost;
	TWeakObjectPtr<UMaterialInstanceDynamic> CachedBackdropPost;

	UMaterial* NewTransientMaterial()
	{
		UMaterial* Material = NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Transient);
		if (Material)
		{
			Material->AddToRoot();
		}
		return Material;
	}

	void Compile(UMaterial* Material)
	{
		FMaterialUpdateContext UpdateContext;
		UpdateContext.AddMaterial(Material);
		Material->PreEditChange(nullptr);
		Material->PostEditChange();
	}

	/**
	 * What ours wear while the clay is on (S174a): a plain lit grey, two
	 * sided, so the scene pass below lights our pixels exactly as it lights
	 * every mesh in the level - one clay, not two that disagree.
	 */
	UMaterialInstanceDynamic* BuildMatcap()
	{
		UMaterial* Material = NewTransientMaterial();
		if (!Material)
		{
			return nullptr;
		}
		auto* Base = Cast<UMaterialExpressionVectorParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionVectorParameter::StaticClass(), -300, 0));
		if (!Base)
		{
			return nullptr;
		}
		Base->ParameterName = TEXT("Clay");
		Base->DefaultValue = FLinearColor(0.5f, 0.5f, 0.5f, 1.0f);
		Material->SetShadingModel(MSM_DefaultLit);
		Material->TwoSided = true;
		UMaterialEditingLibrary::ConnectMaterialProperty(Base, TEXT(""), MP_BaseColor);
		Compile(Material);
		UMaterialInstanceDynamic* Instance = UMaterialInstanceDynamic::Create(Material, GetTransientPackage());
		if (Instance)
		{
			Instance->AddToRoot();
		}
		return Instance;
	}

	/**
	 * The studio sphere the clay is read from (S179a): "no matcap is just
	 * flat colour right now - could we have a decent matcap like the one
	 * from the image". A grey ball lit from the upper left - a soft wrapped
	 * key, a faint fill from the right, a highlight, the lower edge a little
	 * lifted by bounce - painted once into a 256 texture. Stored already
	 * display-encoded (the icon and the pass read it decoded since S184a), where
	 * colours are the screen's.
	 */
	constexpr int32 MatcapSize = 256;

	constexpr int32 MatcapLookCount = static_cast<int32>(ESurfMatcapLook::Count);

	/** The textures that show the ball, so a new look is painted into them in place (S188a). */
	TWeakObjectPtr<UTexture2D> MatcapSphereScreen;
	TWeakObjectPtr<UTexture2D> MatcapSphereIcon;
	int32 AppliedMatcapLook = -1;

	int32 CurrentMatcapLook()
	{
		const USurfSettings* S = GetDefault<USurfSettings>();
		return S ? FMath::Clamp(static_cast<int32>(S->MatcapLook), 0, MatcapLookCount - 1) : 0;
	}

	double MatcapStep(double Edge0, double Edge1, double X)
	{
		const double T = FMath::Clamp((X - Edge0) / (Edge1 - Edge0), 0.0, 1.0);
		return T * T * (3.0 - 2.0 * T);
	}

	/**
	 * The balls (S188a): "here some matcap i like - could user have to choose
	 * from? (similar looking ones)". The grey clay of S180a; a softer, lighter
	 * grey; terracotta; a contrasted studio ball - a white top, a grey band, a
	 * dark underside with a cold rim; red wax. All lit from the upper left.
	 */
	FColor MatcapPixel(int32 X, int32 Y, int32 Look)
	{
		double U = (X + 0.5) / MatcapSize * 2.0 - 1.0;
		double V = 1.0 - (Y + 0.5) / MatcapSize * 2.0;
		double R2 = U * U + V * V;
		// Transparent outside the ball, a pixel of softening at its edge: the
		// pass never reads out there, the menu icon does (S182a).
		const uint8 Alpha = static_cast<uint8>(FMath::Clamp((1.0 - FMath::Sqrt(R2)) * MatcapSize * 0.5 + 0.5, 0.0, 1.0) * 255.0);
		if (R2 > 0.998)
		{
			const double K = FMath::Sqrt(0.998 / R2);
			U *= K;
			V *= K;
			R2 = U * U + V * V;
		}
		const FVector N(U, V, FMath::Sqrt(FMath::Max(0.0, 1.0 - R2)));
		const FVector Key = FVector(-0.45, 0.62, 0.64).GetSafeNormal();
		const FVector Fill = FVector(0.75, -0.10, 0.45).GetSafeNormal();
		const double Facing = FVector::DotProduct(N, Key);
		const double KeyLight = FMath::Pow(FMath::Clamp(Facing * 0.8 + 0.2, 0.0, 1.0), 1.5);
		const double FillLight = FMath::Clamp(FVector::DotProduct(N, Fill), 0.0, 1.0);
		const FVector Reflected = 2.0 * N.Z * N - FVector(0.0, 0.0, 1.0);
		const double Mirror = FMath::Clamp(FVector::DotProduct(Reflected, Key), 0.0, 1.0);
		const double Edge = FMath::Pow(1.0 - N.Z, 3.0);
		FLinearColor C;
		switch (Look)
		{
		case 1: // Soft grey: lighter, a broad sheen.
		{
			const double L = 0.02 + 0.52 * KeyLight + 0.07 * FillLight + 0.18 * FMath::Pow(Mirror, 30.0) + 0.06 * FMath::Pow(Mirror, 4.0)
				+ 0.04 * Edge * FMath::Clamp(0.2 - N.Y, 0.0, 1.0);
			C = FLinearColor(L, L, L);
			break;
		}
		case 2: // Terracotta: warm clay, a dry highlight.
		{
			const FLinearColor Base(0.60f, 0.26f, 0.15f);
			const double D = 0.03 + 0.66 * KeyLight + 0.08 * FillLight + 0.05 * Edge;
			const double H = 0.10 * FMath::Pow(Mirror, 10.0);
			C = FLinearColor(Base.R * D + H, Base.G * D + H * 0.85, Base.B * D + H * 0.75);
			break;
		}
		case 3: // Studio: white top, grey band, dark underside, a cold rim.
		{
			const double L = 0.012 + 0.16 * MatcapStep(-0.05, 0.05, Facing) + 0.55 * MatcapStep(0.30, 0.38, Facing)
				+ 0.14 * MatcapStep(0.40, 0.95, Facing) + 0.35 * FMath::Pow(Mirror, 40.0);
			const double Rim = FMath::Pow(1.0 - N.Z, 5.0) * FMath::Clamp(0.4 * N.X - 0.8 * N.Y, 0.0, 1.0);
			C = FLinearColor(L + 0.03 * Rim, L + 0.35 * Rim, L + 0.45 * Rim);
			break;
		}
		case 4: // Red wax: a wrapped light, a soft glossy highlight.
		{
			const FLinearColor Base(0.42f, 0.045f, 0.03f);
			const double Wrap = FMath::Clamp((Facing + 0.45) / 1.45, 0.0, 1.0);
			const double D = 0.05 + 0.85 * Wrap * Wrap + 0.06 * FillLight + 0.25 * Edge;
			const double H = 0.16 * FMath::Pow(Mirror, 8.0);
			C = FLinearColor(Base.R * D + H, Base.G * D + H * 0.55, Base.B * D + H * 0.5);
			break;
		}
		default: // Clay (S180a): "darker." - about two thirds of the first ball.
		{
			const double L = 0.006 + 0.36 * KeyLight + 0.03 * FillLight + 0.14 * FMath::Pow(Mirror, 20.0)
				+ 0.035 * Edge * FMath::Clamp(0.2 - N.Y, 0.0, 1.0) + 0.02 * FMath::Clamp(N.Y, 0.0, 1.0);
			C = FLinearColor(L, L, L);
			break;
		}
		}
		FColor Out = C.ToFColor(/*bSRGB*/ true);
		Out.A = Alpha;
		return Out;
	}

	/** The ball painted into a texture already made, in place. */
	void WriteMatcapPixels(UTexture2D* Texture, int32 Look)
	{
		FTexturePlatformData* Data = Texture ? Texture->GetPlatformData() : nullptr;
		if (!Data || Data->Mips.Num() == 0)
		{
			return;
		}
		TArray<FColor> Pixels;
		Pixels.SetNumUninitialized(MatcapSize * MatcapSize);
		for (int32 Y = 0; Y < MatcapSize; ++Y)
		{
			for (int32 X = 0; X < MatcapSize; ++X)
			{
				Pixels[Y * MatcapSize + X] = MatcapPixel(X, Y, Look);
			}
		}

		FTexture2DMipMap& Mip = Data->Mips[0];
		if (void* Bytes = Mip.BulkData.Lock(LOCK_READ_WRITE))
		{
			FMemory::Memcpy(Bytes, Pixels.GetData(), Pixels.Num() * sizeof(FColor));
		}
		Mip.BulkData.Unlock();
#if WITH_EDITORONLY_DATA
		if (!Texture->Source.IsValid())
		{
			Texture->Source.Init(MatcapSize, MatcapSize, 1, 1, TSF_BGRA8);
		}
		if (uint8* Source = Texture->Source.LockMip(0))
		{
			FMemory::Memcpy(Source, Pixels.GetData(), Pixels.Num() * sizeof(FColor));
			Texture->Source.UnlockMip(0);
		}
#endif
		Texture->UpdateResource();
	}

	UTexture2D* BuildMatcapTexture(bool bForScreen = true, int32 Look = -1)
	{
		UTexture2D* Texture = UTexture2D::CreateTransient(MatcapSize, MatcapSize, PF_B8G8R8A8);
		FTexturePlatformData* Data = Texture ? Texture->GetPlatformData() : nullptr;
		if (!Data || Data->Mips.Num() == 0)
		{
			return nullptr;
		}
		Texture->AddToRoot();
		// Read as the numbers written: they are the screen's already. The
		// icon is drawn by Slate, which wants them decoded.
		Texture->SRGB = !bForScreen;
		Texture->AddressX = TA_Clamp;
		Texture->AddressY = TA_Clamp;
		Texture->CompressionSettings = TC_Default;
		Texture->MipGenSettings = TMGS_NoMipmaps;
		Texture->NeverStream = true;

		WriteMatcapPixels(Texture, Look < 0 ? CurrentMatcapLook() : Look);
		return Texture;
	}

	/**
	 * Clay on everything (S173a): "matcap shall be enabled for anything in
	 * the scene. everything." A pass before depth of field that reads each
	 * pixel's normal from the scene, turns it to the camera, and looks the
	 * colour up on the studio sphere (S179a) - where the normal faces on the
	 * screen is where on the ball it reads. The sky, and pixels that are
	 * unlit, keep what they were.
	 */
	UMaterialInstanceDynamic* BuildMatcapPost()
	{
		// Read decoded (S184a): the pass now runs before the upscale, in the
		// scene's own linear colours.
		UTexture2D* Sphere = BuildMatcapTexture(/*bForScreen*/ false);
		MatcapSphereScreen = Sphere;
		AppliedMatcapLook = CurrentMatcapLook();
		UMaterial* Material = NewTransientMaterial();
		if (!Material || !Sphere)
		{
			return nullptr;
		}
		auto Make = [Material](UClass* Class, int32 X, int32 Y) -> UMaterialExpression*
		{
			return UMaterialEditingLibrary::CreateMaterialExpression(Material, Class, X, Y);
		};
		auto Texture = [&Make](ESceneTextureId Id, int32 Y) -> UMaterialExpressionSceneTexture*
		{
			auto* Node = Cast<UMaterialExpressionSceneTexture>(Make(UMaterialExpressionSceneTexture::StaticClass(), -1300, Y));
			if (Node)
			{
				Node->SceneTextureId = Id;
			}
			return Node;
		};
		auto Mask = [&Make](UMaterialExpression* From, bool R, bool G, bool B, int32 Y) -> UMaterialExpressionComponentMask*
		{
			auto* Node = Cast<UMaterialExpressionComponentMask>(Make(UMaterialExpressionComponentMask::StaticClass(), -1150, Y));
			if (Node && From)
			{
				Node->Input.Connect(0, From);
				Node->R = R; Node->G = G; Node->B = B; Node->A = false;
			}
			return Node;
		};
		UMaterialExpressionSceneTexture* NormalTex = Texture(PPI_WorldNormal, 0);
		UMaterialExpressionSceneTexture* SceneTex = Texture(PPI_PostProcessInput0, -300);
		UMaterialExpressionSceneTexture* DepthTex = Texture(PPI_SceneDepth, 300);
		UMaterialExpressionSceneTexture* ModelTex = Texture(PPI_ShadingModelID, 450);
		UMaterialExpressionComponentMask* Normal = Mask(NormalTex, true, true, true, 0);
		UMaterialExpressionComponentMask* Scene = Mask(SceneTex, true, true, true, -300);
		UMaterialExpressionComponentMask* Depth = Mask(DepthTex, true, false, false, 300);
		UMaterialExpressionComponentMask* Model = Mask(ModelTex, true, false, false, 450);
		auto* ToView = Cast<UMaterialExpressionTransform>(Make(UMaterialExpressionTransform::StaticClass(), -1000, 0));
		auto* Across = Cast<UMaterialExpressionComponentMask>(Make(UMaterialExpressionComponentMask::StaticClass(), -880, 0));
		auto* Half = Cast<UMaterialExpressionConstant2Vector>(Make(UMaterialExpressionConstant2Vector::StaticClass(), -880, 100));
		auto* Scaled = Cast<UMaterialExpressionMultiply>(Make(UMaterialExpressionMultiply::StaticClass(), -760, 40));
		auto* Middle = Cast<UMaterialExpressionConstant2Vector>(Make(UMaterialExpressionConstant2Vector::StaticClass(), -760, 140));
		auto* UV = Cast<UMaterialExpressionAdd>(Make(UMaterialExpressionAdd::StaticClass(), -640, 60));
		// A parameter (S188b): a new look is another texture handed to the
		// instance. Painting the ball in place under a running pass crashed
		// the renderer (its resource was recreated while in use).
		auto* Sample = Cast<UMaterialExpressionTextureSampleParameter2D>(Make(UMaterialExpressionTextureSampleParameter2D::StaticClass(), -520, 60));
		auto* SampleRGB = Cast<UMaterialExpressionComponentMask>(Make(UMaterialExpressionComponentMask::StaticClass(), -400, 60));
		auto* Clay = Cast<UMaterialExpressionVectorParameter>(Make(UMaterialExpressionVectorParameter::StaticClass(), -520, -120));
		auto* Neutral = Cast<UMaterialExpressionMultiply>(Make(UMaterialExpressionMultiply::StaticClass(), -400, -120));
		auto* Tinted = Cast<UMaterialExpressionMultiply>(Make(UMaterialExpressionMultiply::StaticClass(), -280, 0));
		auto* Near = Cast<UMaterialExpressionStep>(Make(UMaterialExpressionStep::StaticClass(), -850, 300));
		auto* LitModel = Cast<UMaterialExpressionStep>(Make(UMaterialExpressionStep::StaticClass(), -850, 450));
		auto* Both = Cast<UMaterialExpressionMultiply>(Make(UMaterialExpressionMultiply::StaticClass(), -600, 380));
		auto* Out = Cast<UMaterialExpressionLinearInterpolate>(Make(UMaterialExpressionLinearInterpolate::StaticClass(), -100, 0));
		// The backdrop where the sky is left black (S183a): "matcap mat and
		// background with the gradient" - the gradient behind the scene did
		// not survive the pass, so the pass paints it into the black sky.
		auto* Screen = Cast<UMaterialExpressionScreenPosition>(Make(UMaterialExpressionScreenPosition::StaticClass(), -1000, 700));
		auto* Down = Cast<UMaterialExpressionComponentMask>(Make(UMaterialExpressionComponentMask::StaticClass(), -880, 700));
		auto* Top = Cast<UMaterialExpressionVectorParameter>(Make(UMaterialExpressionVectorParameter::StaticClass(), -880, 800));
		auto* Bottom = Cast<UMaterialExpressionVectorParameter>(Make(UMaterialExpressionVectorParameter::StaticClass(), -880, 900));
		auto* Gradient = Cast<UMaterialExpressionLinearInterpolate>(Make(UMaterialExpressionLinearInterpolate::StaticClass(), -700, 800));
		auto* Weights = Cast<UMaterialExpressionConstant3Vector>(Make(UMaterialExpressionConstant3Vector::StaticClass(), -1000, 1000));
		auto* Luma = Cast<UMaterialExpressionDotProduct>(Make(UMaterialExpressionDotProduct::StaticClass(), -850, 1000));
		auto* Black = Cast<UMaterialExpressionStep>(Make(UMaterialExpressionStep::StaticClass(), -700, 1000));
		auto* Far = Cast<UMaterialExpressionOneMinus>(Make(UMaterialExpressionOneMinus::StaticClass(), -700, 1100));
		auto* EmptySky = Cast<UMaterialExpressionMultiply>(Make(UMaterialExpressionMultiply::StaticClass(), -550, 1050));
		auto* Behind = Cast<UMaterialExpressionLinearInterpolate>(Make(UMaterialExpressionLinearInterpolate::StaticClass(), -400, 800));
		if (!NormalTex || !SceneTex || !DepthTex || !ModelTex || !Normal || !Scene || !Depth || !Model || !ToView || !Across || !Half || !Scaled
			|| !Middle || !UV || !Sample || !SampleRGB || !Clay || !Neutral || !Tinted || !Near || !LitModel || !Both || !Out
			|| !Screen || !Down || !Top || !Bottom || !Gradient || !Weights || !Luma || !Black || !Far || !EmptySky || !Behind)
		{
			return nullptr;
		}
		Down->Input.Connect(0, Screen);
		Down->R = false; Down->G = true; Down->B = false; Down->A = false;
		Top->ParameterName = TEXT("BackTop");
		Top->DefaultValue = FLinearColor(0.32f, 0.33f, 0.34f, 1.0f);
		Bottom->ParameterName = TEXT("BackBottom");
		Bottom->DefaultValue = FLinearColor(0.14f, 0.14f, 0.15f, 1.0f);
		Gradient->A.Connect(0, Top);
		Gradient->B.Connect(0, Bottom);
		Gradient->Alpha.Connect(0, Down);
		Weights->Constant = FLinearColor(0.3f, 0.59f, 0.11f, 1.0f);
		Luma->A.Connect(0, Scene);
		Luma->B.Connect(0, Weights);
		// Black: nothing brighter than a hair.
		Black->Y.Connect(0, Luma);
		Black->ConstX = 0.0005f;
		// The normal as the camera sees it: x right, y up.
		ToView->TransformSourceType = TRANSFORMSOURCE_World;
		ToView->TransformType = TRANSFORM_View;
		ToView->Input.Connect(0, Normal);
		Across->Input.Connect(0, ToView);
		Across->R = true; Across->G = true; Across->B = false; Across->A = false;
		// Onto the ball: x to u, y up to v down.
		Half->R = 0.5f;
		Half->G = -0.5f;
		Scaled->A.Connect(0, Across);
		Scaled->B.Connect(0, Half);
		Middle->R = 0.5f;
		Middle->G = 0.5f;
		UV->A.Connect(0, Scaled);
		UV->B.Connect(0, Middle);
		Sample->Texture = Sphere;
		Sample->ParameterName = TEXT("Ball");
		Sample->SamplerType = SAMPLERTYPE_Color;
		Sample->Coordinates.Connect(0, UV);
		SampleRGB->Input.Connect(0, Sample);
		SampleRGB->R = true; SampleRGB->G = true; SampleRGB->B = true; SampleRGB->A = false;
		// The ball is painted for a clay of 0.6; any other colour tints it.
		Clay->ParameterName = TEXT("Clay");
		Clay->DefaultValue = FLinearColor(0.60f, 0.60f, 0.60f, 1.0f);
		Neutral->A.Connect(0, Clay);
		Neutral->ConstB = 1.0f / 0.6f;
		Tinted->A.Connect(0, SampleRGB);
		Tinted->B.Connect(0, Neutral);
		// Near: anything nearer than 50 km is geometry, the sky is not.
		Near->Y.Connect(0, Depth);
		Near->ConstX = 5.0e6f;
		// Lit: a shading model other than unlit (0).
		LitModel->ConstY = 0.5f;
		LitModel->X.Connect(0, Model);
		Both->A.Connect(0, Near);
		Both->B.Connect(0, LitModel);
		Far->Input.Connect(0, Near);
		EmptySky->A.Connect(0, Black);
		EmptySky->B.Connect(0, Far);
		Behind->A.Connect(0, Scene);
		Behind->B.Connect(0, Gradient);
		Behind->Alpha.Connect(0, EmptySky);
		Out->A.Connect(0, Behind);
		Out->B.Connect(0, Tinted);
		Out->Alpha.Connect(0, Both);
		Material->MaterialDomain = MD_PostProcess;
		/**
		 * Before depth of field (S184a): "super weird black artifact". After
		 * tonemapping the pass ran on the upscaled image while the depth and
		 * the normals it reads are still at the render's own size - edges
		 * read the wrong pixel. Here all three agree, the anti-aliasing
		 * smooths the result, and the translucent things drawn after (the
		 * grid, the backdrop) land on top of it.
		 */
		Material->BlendableLocation = BL_SceneColorBeforeDOF;
		UMaterialEditingLibrary::ConnectMaterialProperty(Out, TEXT(""), MP_EmissiveColor);
		Compile(Material);
		UMaterialInstanceDynamic* Instance = UMaterialInstanceDynamic::Create(Material, GetTransientPackage());
		if (Instance)
		{
			Instance->AddToRoot();
		}
		return Instance;
	}

	/**
	 * The gradient behind the plain unlit view (S250a: "while in unlit, the
	 * ghost mode shades wrong - only visible where it overdraws. else
	 * unvisible"). Drawn among the editor's own things, the gradient landed
	 * over the see-through ghosts wherever nothing solid stood behind them.
	 * As a pass before the see-through things - the way the clay's pass
	 * paints its sky - they are drawn over it.
	 */
	UMaterialInstanceDynamic* BuildBackdropPost()
	{
		UMaterial* Material = NewTransientMaterial();
		if (!Material)
		{
			return nullptr;
		}
		auto Make = [Material](UClass* Class, int32 X, int32 Y) -> UMaterialExpression*
		{
			return UMaterialEditingLibrary::CreateMaterialExpression(Material, Class, X, Y);
		};
		auto* SceneTex = Cast<UMaterialExpressionSceneTexture>(Make(UMaterialExpressionSceneTexture::StaticClass(), -1000, -200));
		auto* DepthTex = Cast<UMaterialExpressionSceneTexture>(Make(UMaterialExpressionSceneTexture::StaticClass(), -1000, 100));
		auto* Scene = Cast<UMaterialExpressionComponentMask>(Make(UMaterialExpressionComponentMask::StaticClass(), -850, -200));
		auto* Depth = Cast<UMaterialExpressionComponentMask>(Make(UMaterialExpressionComponentMask::StaticClass(), -850, 100));
		auto* Near = Cast<UMaterialExpressionStep>(Make(UMaterialExpressionStep::StaticClass(), -700, 100));
		auto* Far = Cast<UMaterialExpressionOneMinus>(Make(UMaterialExpressionOneMinus::StaticClass(), -550, 100));
		auto* Screen = Cast<UMaterialExpressionScreenPosition>(Make(UMaterialExpressionScreenPosition::StaticClass(), -1000, 300));
		auto* Down = Cast<UMaterialExpressionComponentMask>(Make(UMaterialExpressionComponentMask::StaticClass(), -850, 300));
		auto* Top = Cast<UMaterialExpressionVectorParameter>(Make(UMaterialExpressionVectorParameter::StaticClass(), -850, 400));
		auto* Bottom = Cast<UMaterialExpressionVectorParameter>(Make(UMaterialExpressionVectorParameter::StaticClass(), -850, 500));
		auto* Gradient = Cast<UMaterialExpressionLinearInterpolate>(Make(UMaterialExpressionLinearInterpolate::StaticClass(), -650, 400));
		auto* Out = Cast<UMaterialExpressionLinearInterpolate>(Make(UMaterialExpressionLinearInterpolate::StaticClass(), -300, 0));
		if (!SceneTex || !DepthTex || !Scene || !Depth || !Near || !Far || !Screen || !Down || !Top || !Bottom || !Gradient || !Out)
		{
			return nullptr;
		}
		SceneTex->SceneTextureId = PPI_PostProcessInput0;
		DepthTex->SceneTextureId = PPI_SceneDepth;
		Scene->Input.Connect(0, SceneTex);
		Scene->R = true; Scene->G = true; Scene->B = true; Scene->A = false;
		Depth->Input.Connect(0, DepthTex);
		Depth->R = true; Depth->G = false; Depth->B = false; Depth->A = false;
		// Near: anything nearer than 50 km is geometry, the sky is not.
		Near->Y.Connect(0, Depth);
		Near->ConstX = 5.0e6f;
		Far->Input.Connect(0, Near);
		Down->Input.Connect(0, Screen);
		Down->R = false; Down->G = true; Down->B = false; Down->A = false;
		Top->ParameterName = TEXT("BackTop");
		Top->DefaultValue = FLinearColor(0.085f, 0.088f, 0.095f, 1.0f);
		Bottom->ParameterName = TEXT("BackBottom");
		Bottom->DefaultValue = FLinearColor(0.018f, 0.018f, 0.021f, 1.0f);
		Gradient->A.Connect(0, Top);
		Gradient->B.Connect(0, Bottom);
		Gradient->Alpha.Connect(0, Down);
		Out->A.Connect(0, Scene);
		Out->B.Connect(0, Gradient);
		Out->Alpha.Connect(0, Far);
		Material->MaterialDomain = MD_PostProcess;
		Material->BlendableLocation = BL_SceneColorBeforeDOF;
		UMaterialEditingLibrary::ConnectMaterialProperty(Out, TEXT(""), MP_EmissiveColor);
		Compile(Material);
		UMaterialInstanceDynamic* Instance = UMaterialInstanceDynamic::Create(Material, GetTransientPackage());
		if (Instance)
		{
			Instance->AddToRoot();
		}
		return Instance;
	}

	/** Two colours down the screen: the quad's UV.y is 0 at the top, 1 at the bottom. */
	UMaterialInstanceDynamic* BuildBackdrop()
	{
		UMaterial* Material = NewTransientMaterial();
		if (!Material)
		{
			return nullptr;
		}
		auto Make = [Material](UClass* Class, int32 X, int32 Y) -> UMaterialExpression*
		{
			return UMaterialEditingLibrary::CreateMaterialExpression(Material, Class, X, Y);
		};
		auto* UV = Cast<UMaterialExpressionTextureCoordinate>(Make(UMaterialExpressionTextureCoordinate::StaticClass(), -700, 100));
		auto* Down = Cast<UMaterialExpressionComponentMask>(Make(UMaterialExpressionComponentMask::StaticClass(), -550, 100));
		auto* Top = Cast<UMaterialExpressionVectorParameter>(Make(UMaterialExpressionVectorParameter::StaticClass(), -550, -150));
		auto* Bottom = Cast<UMaterialExpressionVectorParameter>(Make(UMaterialExpressionVectorParameter::StaticClass(), -550, -20));
		auto* Mix = Cast<UMaterialExpressionLinearInterpolate>(Make(UMaterialExpressionLinearInterpolate::StaticClass(), -300, 0));
		if (!UV || !Down || !Top || !Bottom || !Mix)
		{
			return nullptr;
		}
		Down->Input.Connect(0, UV);
		Down->R = false;
		Down->G = true;
		Down->B = false;
		Down->A = false;
		Top->ParameterName = TEXT("Top");
		Top->DefaultValue = FLinearColor(0.085f, 0.088f, 0.095f, 1.0f);
		Bottom->ParameterName = TEXT("Bottom");
		Bottom->DefaultValue = FLinearColor(0.018f, 0.018f, 0.021f, 1.0f);
		Mix->A.Connect(0, Top);
		Mix->B.Connect(0, Bottom);
		Mix->Alpha.Connect(0, Down);
		Material->SetShadingModel(MSM_Unlit);
		Material->TwoSided = true;
		// Drawn the way the gizmo is (S173a): an opaque material in the
		// editor's own primitives never showed - "pitch black background".
		Material->BlendMode = BLEND_Translucent;
		Material->bUsedWithEditorCompositing = true;
		UMaterialEditingLibrary::ConnectMaterialProperty(Mix, TEXT(""), MP_EmissiveColor);
		auto* Solid = Cast<UMaterialExpressionConstant>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionConstant::StaticClass(), -300, 150));
		if (Solid)
		{
			Solid->R = 1.0f;
			UMaterialEditingLibrary::ConnectMaterialProperty(Solid, TEXT(""), MP_Opacity);
		}
		Compile(Material);
		UMaterialInstanceDynamic* Instance = UMaterialInstanceDynamic::Create(Material, GetTransientPackage());
		if (Instance)
		{
			Instance->AddToRoot();
		}
		return Instance;
	}
}

UMaterialInterface* GetMatcap()
{
	return CachedMatcap.Get();
}

UMaterialInterface* GetBackdrop()
{
	return CachedBackdrop.Get();
}

UMaterialInterface* GetMatcapPost()
{
	return CachedMatcapPost.Get();
}

UMaterialInterface* GetBackdropPost()
{
	return CachedBackdropPost.Get();
}

UTexture2D* GetCheckerTexture()
{
	return CachedTexture.Get();
}

UTexture2D* GetMatcapIcon()
{
	if (!MatcapSphereIcon.IsValid() && !IsInRenderingThread() && !FUObjectThreadContext::Get().IsRoutingPostLoad)
	{
		MatcapSphereIcon = BuildMatcapTexture(/*bForScreen*/ false);
	}
	return MatcapSphereIcon.Get();
}

UTexture2D* GetMatcapLookIcon(int32 Look)
{
	static TWeakObjectPtr<UTexture2D> Icons[MatcapLookCount];
	if (Look < 0 || Look >= MatcapLookCount)
	{
		return nullptr;
	}
	if (!Icons[Look].IsValid() && !IsInRenderingThread() && !FUObjectThreadContext::Get().IsRoutingPostLoad)
	{
		Icons[Look] = BuildMatcapTexture(/*bForScreen*/ false, Look);
	}
	return Icons[Look].Get();
}

void RefreshViewColours()
{
	if (IsInRenderingThread() || FUObjectThreadContext::Get().IsRoutingPostLoad)
	{
		return;
	}
	if (!CachedMatcap.IsValid())
	{
		CachedMatcap = BuildMatcap();
	}
	if (!CachedBackdrop.IsValid())
	{
		CachedBackdrop = BuildBackdrop();
	}
	if (!CachedMatcapPost.IsValid())
	{
		CachedMatcapPost = BuildMatcapPost();
	}
	if (!CachedBackdropPost.IsValid())
	{
		CachedBackdropPost = BuildBackdropPost();
	}
	// The warm clay of S172b becomes the classic grey (S174a) unless it was
	// changed by hand.
	if (USurfSettings* Mutable = GetMutableDefault<USurfSettings>())
	{
		if (Mutable->MatcapColour.Equals(FLinearColor(0.62f, 0.60f, 0.56f, 1.0f), 0.002f))
		{
			Mutable->MatcapColour = FLinearColor(0.60f, 0.60f, 0.60f, 1.0f);
			Mutable->SaveConfig();
		}
	}
	// A new ball is handed to the pass (S188b); textures are never repainted once shown.
	const int32 Look = CurrentMatcapLook();
	if (Look != AppliedMatcapLook)
	{
		UMaterialInstanceDynamic* Post = CachedMatcapPost.Get();
		UTexture2D* Ball = GetMatcapLookIcon(Look);
		if (Post && Ball)
		{
			Post->SetTextureParameterValue(TEXT("Ball"), Ball);
			AppliedMatcapLook = Look;
		}
	}
	const USurfSettings* Settings = GetDefault<USurfSettings>();
	if (!Settings)
	{
		return;
	}
	if (UMaterialInstanceDynamic* Matcap = CachedMatcap.Get())
	{
		Matcap->SetVectorParameterValue(TEXT("Clay"), Settings->MatcapColour);
	}
	if (UMaterialInstanceDynamic* Post = CachedMatcapPost.Get())
	{
		Post->SetVectorParameterValue(TEXT("Clay"), Settings->MatcapColour);
		// The backdrop's own linear colours (the pass runs before the
		// tonemapper since S184a), black when the gradient is off.
		auto Encoded = [&Settings](const FLinearColor& Linear)
		{
			return Settings->bBackdropGradient ? Linear : FLinearColor::Black;
		};
		Post->SetVectorParameterValue(TEXT("BackTop"), Encoded(Settings->BackdropTop));
		Post->SetVectorParameterValue(TEXT("BackBottom"), Encoded(Settings->BackdropBottom));
	}
	if (UMaterialInstanceDynamic* Backdrop = CachedBackdrop.Get())
	{
		Backdrop->SetVectorParameterValue(TEXT("Top"), Settings->BackdropTop);
		Backdrop->SetVectorParameterValue(TEXT("Bottom"), Settings->BackdropBottom);
	}
	if (UMaterialInstanceDynamic* BackPost = CachedBackdropPost.Get())
	{
		BackPost->SetVectorParameterValue(TEXT("BackTop"), Settings->BackdropTop);
		BackPost->SetVectorParameterValue(TEXT("BackBottom"), Settings->BackdropBottom);
	}
}

UMaterialInterface* GetPaintView(int32 Channel)
{
	Channel = FMath::Clamp(Channel, 0, 4);
	if (!CachedPaintView[Channel].IsValid())
	{
		CachedPaintView[Channel] = BuildPaintView(Channel);
	}
	return CachedPaintView[Channel].Get();
}

UMaterialInterface* GetGizmoMaterial()
{
	if (CachedGizmoMaterial.IsValid())
	{
		return CachedGizmoMaterial.Get();
	}
	if (FUObjectThreadContext::Get().IsRoutingPostLoad || IsInRenderingThread())
	{
		return nullptr;
	}
	UMaterialInterface* Built = BuildGizmoMaterial();
	CachedGizmoMaterial = Built;
	return Built;
}

namespace
{
	constexpr int32 GizmoTintCount = static_cast<int32>(ESurfGizmoTint::Count);
	TWeakObjectPtr<UMaterialInstanceDynamic> GizmoTints[GizmoTintCount];
	FLinearColor GizmoTintColours[GizmoTintCount];
}

UMaterialInterface* GetGizmoMaterial(ESurfGizmoTint Tint, const FLinearColor& Colour)
{
	const int32 Slot = static_cast<int32>(Tint);
	if (Slot < 0 || Slot >= GizmoTintCount)
	{
		return nullptr;
	}

	static const FName ColourName(TEXT("Color"));
	UMaterialInstanceDynamic* Instance = GizmoTints[Slot].Get();
	if (!Instance)
	{
		UMaterialInterface* Base = GetGizmoMaterial();
		if (!Base)
		{
			return nullptr;
		}
		Instance = UMaterialInstanceDynamic::Create(Base, GetTransientPackage());
		if (!Instance)
		{
			return nullptr;
		}
		Instance->AddToRoot();
		Instance->SetVectorParameterValue(ColourName, Colour);
		GizmoTints[Slot] = Instance;
		GizmoTintColours[Slot] = Colour;
		return Instance;
	}

	// Recoloured only when asked for a colour it does not have: the render
	// thread hears about a parameter every time it is set.
	if (!GizmoTintColours[Slot].Equals(Colour))
	{
		Instance->SetVectorParameterValue(ColourName, Colour);
		GizmoTintColours[Slot] = Colour;
	}
	return Instance;
}

namespace
{
	TWeakObjectPtr<UMaterialInstanceDynamic> CachedGhost;
}

UMaterialInterface* GetGhost()
{
	if (CachedGhost.IsValid())
	{
		return CachedGhost.Get();
	}
	// The gizmo's unlit see-through material, in a grey that stays a grey
	// over any level: there, and plainly not what is being edited.
	UMaterialInterface* Base = GetGizmoMaterial();
	if (!Base)
	{
		return nullptr;
	}
	UMaterialInstanceDynamic* Instance = UMaterialInstanceDynamic::Create(Base, GetTransientPackage());
	if (!Instance)
	{
		return nullptr;
	}
	Instance->AddToRoot();
	// Plainer (S176a): "the context didn't show... the other object disappeared" -
	// a fifth-opaque grey over the grey backdrop was nothing at all.
	Instance->SetVectorParameterValue(FName(TEXT("Color")), FLinearColor(0.72f, 0.76f, 0.84f, 0.42f));
	CachedGhost = Instance;
	return Instance;
}

void WarmUp()
{
	GetChecker();
	RefreshLevels();
	RefreshShade();
	GetGizmoMaterial();
	for (int32 Slot = 0; Slot < GizmoTintCount; ++Slot)
	{
		GetGizmoMaterial(static_cast<ESurfGizmoTint>(Slot), FLinearColor::White);
	}
	RefreshViewColours();
}

void RefreshShade()
{
	if (!CachedChecker.IsValid() || BuiltShade == WantedShade()
		|| FUObjectThreadContext::Get().IsRoutingPostLoad || IsInRenderingThread())
	{
		return;
	}

	// A new material for the new shade; every group without a material of
	// its own is handed it. The old one stays rooted - a material in use by
	// a render proxy is not something to pull from under it.
	UMaterialInterface* Built = BuildChecker();
	if (!Built)
	{
		return;
	}
	CachedChecker = Built;

	if (GEditor)
	{
		if (UWorld* World = GEditor->GetEditorWorldContext().World())
		{
			for (TActorIterator<ASurfGroup> It(World); It; ++It)
			{
				if (ASurfGroup* Group = *It)
				{
					Group->RefreshPreviewMaterial();
				}
			}
		}
	}
}

void RefreshLevels()
{
	UTexture2D* Texture = CachedTexture.Get();
	if (!Texture || FUObjectThreadContext::Get().IsRoutingPostLoad || IsInRenderingThread())
	{
		return;
	}

	float Darkest = 0.30f;
	float Lightest = 0.62f;
	ReadLevels(Darkest, Lightest);

	if (!FMath::IsNearlyEqual(Darkest, FilledDarkest, 1.e-4f) || !FMath::IsNearlyEqual(Lightest, FilledLightest, 1.e-4f))
	{
		FillChecker(Texture, Darkest, Lightest);
	}
}

void GetLevels(float& OutDarkest, float& OutLightest)
{
	OutDarkest = FilledDarkest;
	OutLightest = FilledLightest;
}

void RegisterAsGroupDefault()
{
	ASurfGroup::DefaultMaterialProvider.BindStatic(&GetChecker);
	ASurfGroup::GhostMaterialProvider.BindStatic(&GetGhost);
}

void UnregisterAsGroupDefault()
{
	ASurfGroup::DefaultMaterialProvider.Unbind();
	ASurfGroup::GhostMaterialProvider.Unbind();
	CachedChecker.Reset();
	CachedTexture.Reset();
}

} // namespace SurfPreviewMaterial
