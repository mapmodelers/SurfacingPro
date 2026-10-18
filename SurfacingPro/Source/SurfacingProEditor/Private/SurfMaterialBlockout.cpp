// Copyright MapModelers. All Rights Reserved.

#include "SurfMaterialBlockout.h"

#include "SurfMaterialInfo.h"
#include "SurfMaterialSeed.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Texture2D.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialInstanceConstant.h"
#include "UObject/Package.h"

namespace SurfMaterialBlockout
{
	FString Folder()
	{
		return TEXT("/SurfacingPro/Materials/Blockout");
	}

	FString MasterPackage()
	{
		return Folder() / TEXT("M_SurfBlockout");
	}

	const TArray<FSurfBlockoutKind>& Kinds()
	{
		static const TArray<FSurfBlockoutKind> All = []()
		{
			TArray<FSurfBlockoutKind> Out;
			auto Grid = [&Out](const TCHAR* Stem, double Metres, const FLinearColor& Colour)
			{
				FSurfBlockoutKind K;
				K.Stem = Stem;
				K.TileMetres = Metres;
				K.Colour = Colour;
				K.Shape = FSurfBlockoutKind::EShape::Grid;
				Out.Add(K);
			};
			// One hue per size, apart on the wheel, all light enough to read the lines on.
			Grid(TEXT("Grid_050"), 0.5, FLinearColor(0.42f, 0.62f, 0.86f));
			Grid(TEXT("Grid_100"), 1.0, FLinearColor(0.50f, 0.78f, 0.44f));
			Grid(TEXT("Grid_200"), 2.0, FLinearColor(0.92f, 0.70f, 0.36f));
			Grid(TEXT("Grid_400"), 4.0, FLinearColor(0.88f, 0.44f, 0.42f));

			FSurfBlockoutKind S25;
			S25.Stem = TEXT("Strip_025");
			S25.TileMetres = 1.0;
			S25.BandMetres = 0.25;
			S25.Colour = FLinearColor(0.86f, 0.30f, 0.30f);
			S25.Shape = FSurfBlockoutKind::EShape::Strip;
			Out.Add(S25);

			FSurfBlockoutKind S50;
			S50.Stem = TEXT("Strip_050");
			S50.TileMetres = 1.0;
			S50.BandMetres = 0.5;
			S50.Colour = FLinearColor(0.32f, 0.56f, 0.90f);
			S50.Shape = FSurfBlockoutKind::EShape::Strip;
			Out.Add(S50);

			FSurfBlockoutKind Fill;
			Fill.Stem = TEXT("Fill");
			Fill.TileMetres = 1.0;
			Fill.Colour = FLinearColor(0.55f, 0.55f, 0.55f);
			Fill.Shape = FSurfBlockoutKind::EShape::Fill;
			Out.Add(Fill);
			return Out;
		}();
		return All;
	}

	int32 TextureSize()
	{
		return 512;
	}

	// --- The font -------------------------------------------------------------------
	//
	// Five by seven, one row a byte, the high bit the left column. Digits, the
	// point and the m: everything a size label needs.

	namespace
	{
		const uint8* GlyphRows(TCHAR Glyph)
		{
			static const uint8 D0[7] = { 0x70, 0x88, 0x98, 0xA8, 0xC8, 0x88, 0x70 };
			static const uint8 D1[7] = { 0x20, 0x60, 0x20, 0x20, 0x20, 0x20, 0x70 };
			static const uint8 D2[7] = { 0x70, 0x88, 0x08, 0x10, 0x20, 0x40, 0xF8 };
			static const uint8 D3[7] = { 0xF8, 0x10, 0x20, 0x10, 0x08, 0x88, 0x70 };
			static const uint8 D4[7] = { 0x10, 0x30, 0x50, 0x90, 0xF8, 0x10, 0x10 };
			static const uint8 D5[7] = { 0xF8, 0x80, 0xF0, 0x08, 0x08, 0x88, 0x70 };
			static const uint8 D6[7] = { 0x30, 0x40, 0x80, 0xF0, 0x88, 0x88, 0x70 };
			static const uint8 D7[7] = { 0xF8, 0x08, 0x10, 0x20, 0x40, 0x40, 0x40 };
			static const uint8 D8[7] = { 0x70, 0x88, 0x88, 0x70, 0x88, 0x88, 0x70 };
			static const uint8 D9[7] = { 0x70, 0x88, 0x88, 0x78, 0x08, 0x10, 0x60 };
			static const uint8 Dot[7] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x60, 0x60 };
			static const uint8 M[7] = { 0x00, 0x00, 0xD0, 0xA8, 0xA8, 0xA8, 0xA8 };
			switch (Glyph)
			{
			case TEXT('0'): return D0;
			case TEXT('1'): return D1;
			case TEXT('2'): return D2;
			case TEXT('3'): return D3;
			case TEXT('4'): return D4;
			case TEXT('5'): return D5;
			case TEXT('6'): return D6;
			case TEXT('7'): return D7;
			case TEXT('8'): return D8;
			case TEXT('9'): return D9;
			case TEXT('.'): return Dot;
			case TEXT('m'): return M;
			default: return nullptr;
			}
		}

		void Put(TArray<FColor>& Pixels, int32 Side, int32 X, int32 Y, const FColor& Colour)
		{
			if (X >= 0 && Y >= 0 && X < Side && Y < Side)
			{
				Pixels[Y * Side + X] = Colour;
			}
		}

		FColor ToColor(const FLinearColor& Linear)
		{
			return Linear.ToFColor(/*bSRGB*/ true);
		}

		/** The label a grid prints: "0.5", "1", "2", "4", then " m". */
		FString LabelFor(double Metres)
		{
			if (FMath::IsNearlyEqual(Metres, FMath::RoundToDouble(Metres)))
			{
				return FString::Printf(TEXT("%dm"), FMath::RoundToInt(Metres));
			}
			return FString::Printf(TEXT("%.1fm"), Metres);
		}
	}

	int32 PrintGlyph(TCHAR Glyph, int32 X, int32 Y, int32 Scale, const FColor& Colour, TArray<FColor>& Pixels, int32 Side)
	{
		const uint8* Rows = GlyphRows(Glyph);
		if (!Rows)
		{
			return 0;
		}
		// The point and the m are narrower than a digit; the cells they use say so.
		const int32 Columns = Glyph == TEXT('.') ? 3 : 5;
		for (int32 Row = 0; Row < 7; ++Row)
		{
			for (int32 Column = 0; Column < Columns; ++Column)
			{
				if (Rows[Row] & (0x80 >> Column))
				{
					for (int32 DY = 0; DY < Scale; ++DY)
					{
						for (int32 DX = 0; DX < Scale; ++DX)
						{
							Put(Pixels, Side, X + Column * Scale + DX, Y + Row * Scale + DY, Colour);
						}
					}
				}
			}
		}
		return (Columns + 1) * Scale;
	}

	void Draw(const FSurfBlockoutKind& Kind, TArray<FColor>& OutPixels)
	{
		const int32 Side = TextureSize();
		OutPixels.Init(ToColor(Kind.Colour), Side * Side);

		if (Kind.Shape == FSurfBlockoutKind::EShape::Fill)
		{
			return;
		}

		const FColor Light = ToColor(Kind.Colour * 1.18f);
		const FColor Dark = ToColor(Kind.Colour * 0.55f);
		const FColor Ink = ToColor(Kind.Colour * 0.28f);

		if (Kind.Shape == FSurfBlockoutKind::EShape::Strip)
		{
			// Bands along V, the height of BandMetres each, alternating; the band's edge a dark line.
			const int32 Band = FMath::Max(1, FMath::RoundToInt(Side * Kind.BandMetres / Kind.TileMetres));
			for (int32 Y = 0; Y < Side; ++Y)
			{
				const bool bAlternate = ((Y / Band) % 2) == 1;
				const bool bEdge = (Y % Band) == 0;
				for (int32 X = 0; X < Side; ++X)
				{
					Put(OutPixels, Side, X, Y, bEdge ? Dark : (bAlternate ? Light : ToColor(Kind.Colour)));
				}
			}
			return;
		}

		// A grid: fine lines every 10 cm, firm ones every 50 cm, the tile's edge firmest;
		// a tile under a metre is finer - 5 cm and 10 cm - so it has lines of its own to show.
		const double PixelsPerMetre = Side / Kind.TileMetres;
		const bool bSmall = Kind.TileMetres < 1.0;
		const int32 Fine = FMath::Max(1, FMath::RoundToInt(PixelsPerMetre * (bSmall ? 0.05 : 0.1)));
		const int32 Firm = FMath::Max(1, FMath::RoundToInt(PixelsPerMetre * (bSmall ? 0.1 : 0.5)));
		for (int32 Y = 0; Y < Side; ++Y)
		{
			for (int32 X = 0; X < Side; ++X)
			{
				const bool bFineX = (X % Fine) == 0;
				const bool bFineY = (Y % Fine) == 0;
				const bool bFirmX = (X % Firm) < 2;
				const bool bFirmY = (Y % Firm) < 2;
				const bool bEdge = X < 3 || Y < 3;
				if (bEdge)
				{
					Put(OutPixels, Side, X, Y, Ink);
				}
				else if (bFirmX || bFirmY)
				{
					Put(OutPixels, Side, X, Y, Dark);
				}
				else if (bFineX || bFineY)
				{
					Put(OutPixels, Side, X, Y, Light);
				}
			}
		}

		// The size, in the middle, on a plate the lines do not cross.
		const FString Label = LabelFor(Kind.TileMetres);
		const int32 Scale = Side / 40;
		int32 Width = 0;
		for (TCHAR C : Label)
		{
			Width += (C == TEXT('.') ? 4 : 6) * Scale;
		}
		Width -= Scale;
		const int32 Height = 7 * Scale;
		const int32 X0 = (Side - Width) / 2;
		const int32 Y0 = (Side - Height) / 2;
		const int32 Pad = Scale * 2;
		for (int32 Y = Y0 - Pad; Y < Y0 + Height + Pad; ++Y)
		{
			for (int32 X = X0 - Pad; X < X0 + Width + Pad; ++X)
			{
				Put(OutPixels, Side, X, Y, ToColor(Kind.Colour));
			}
		}
		int32 X = X0;
		for (TCHAR C : Label)
		{
			X += PrintGlyph(C, X, Y0, Scale, Ink, OutPixels, Side);
		}
	}

	// --- The assets -------------------------------------------------------------------

	namespace
	{
		template<typename T>
		T* BlockNode(UMaterial* Master, int32 X, int32 Y)
		{
			return Cast<T>(UMaterialEditingLibrary::CreateMaterialExpression(Master, T::StaticClass(), X, Y));
		}

		FString TextureName(const FSurfBlockoutKind& Kind)
		{
			return TEXT("T_Sp_Block_") + Kind.Stem;
		}

		FString InstanceName(const FSurfBlockoutKind& Kind)
		{
			return TEXT("MI_Block_") + Kind.Stem;
		}

		UTexture2D* AuthorTexture(const FSurfBlockoutKind& Kind, FString& OutError)
		{
			const FString Package = Folder() / TextureName(Kind);
			UPackage* Pkg = CreatePackage(*Package);
			if (!Pkg)
			{
				OutError = FString::Printf(TEXT("could not make %s"), *Package);
				return nullptr;
			}
			Pkg->FullyLoad();

			TArray<FColor> Pixels;
			Draw(Kind, Pixels);
			const int32 Side = TextureSize();

			UTexture2D* Texture = NewObject<UTexture2D>(Pkg, FName(*TextureName(Kind)), RF_Public | RF_Standalone | RF_Transactional);
			Texture->Source.Init(Side, Side, 1, 1, TSF_BGRA8, reinterpret_cast<const uint8*>(Pixels.GetData()));
			Texture->SRGB = true;
			Texture->CompressionSettings = TC_Default;
			Texture->MipGenSettings = TMGS_FromTextureGroup;
			Texture->LODGroup = TEXTUREGROUP_World;
			Texture->AddressX = TA_Wrap;
			Texture->AddressY = TA_Wrap;
			Texture->UpdateResource();
			Texture->PostEditChange();
			Texture->MarkPackageDirty();
			FAssetRegistryModule::AssetCreated(Texture);
			return Texture;
		}

		UMaterial* AuthorMaster(UTexture2D* Default, FString& OutError)
		{
			UPackage* Package = CreatePackage(*MasterPackage());
			if (!Package)
			{
				OutError = TEXT("could not make the master's package");
				return nullptr;
			}
			Package->FullyLoad();

			UMaterial* Master = NewObject<UMaterial>(Package, TEXT("M_SurfBlockout"), RF_Public | RF_Standalone | RF_Transactional);
			Master->BlendMode = BLEND_Opaque;
			Master->TwoSided = false;

			UMaterialExpressionTextureSampleParameter2D* Grid = BlockNode<UMaterialExpressionTextureSampleParameter2D>(Master, -700, -200);
			UMaterialExpressionVectorParameter* Tint = BlockNode<UMaterialExpressionVectorParameter>(Master, -700, 100);
			UMaterialExpressionMultiply* Times = BlockNode<UMaterialExpressionMultiply>(Master, -350, -100);
			UMaterialExpressionScalarParameter* Roughness = BlockNode<UMaterialExpressionScalarParameter>(Master, -700, 350);
			if (!Grid || !Tint || !Times || !Roughness)
			{
				OutError = TEXT("the master's nodes could not be made");
				return nullptr;
			}
			Grid->ParameterName = FName(TEXT("Grid"));
			Grid->Texture = Default;
			Grid->SamplerType = SAMPLERTYPE_Color;
			Tint->ParameterName = FName(TEXT("Tint"));
			Tint->DefaultValue = FLinearColor::White;
			Roughness->ParameterName = FName(TEXT("Roughness"));
			Roughness->DefaultValue = 0.9f;
			Times->A.Expression = Grid;
			Times->A.OutputIndex = 0;
			Times->B.Expression = Tint;
			Times->B.OutputIndex = 0;

			UMaterialEditorOnlyData* Editor = Master->GetEditorOnlyData();
			if (!Editor)
			{
				OutError = TEXT("the master has no editor data");
				return nullptr;
			}
			Editor->BaseColor.Expression = Times;
			Editor->Roughness.Expression = Roughness;

			if (USurfMaterialInfo* Info = USurfMaterialInfo::FindOrAdd(Master))
			{
				Info->DefaultScale = FVector2D(1.0, 1.0);
			}

			Master->PreEditChange(nullptr);
			Master->PostEditChange();
			Master->MarkPackageDirty();
			FAssetRegistryModule::AssetCreated(Master);
			return Master;
		}

		UMaterialInstanceConstant* AuthorInstance(const FSurfBlockoutKind& Kind, UMaterial* Master, UTexture2D* Texture, FString& OutError)
		{
			const FString Package = Folder() / InstanceName(Kind);
			UPackage* Pkg = CreatePackage(*Package);
			if (!Pkg)
			{
				OutError = FString::Printf(TEXT("could not make %s"), *Package);
				return nullptr;
			}
			Pkg->FullyLoad();

			UMaterialInstanceConstant* Instance = NewObject<UMaterialInstanceConstant>(Pkg, FName(*InstanceName(Kind)), RF_Public | RF_Standalone | RF_Transactional);
			Instance->SetParentEditorOnly(Master);
			Instance->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Grid")), Texture);
			// The tile is the size it prints: the scale is the kind's metres.
			if (USurfMaterialInfo* Info = USurfMaterialInfo::FindOrAdd(Instance))
			{
				Info->DefaultScale = FVector2D(Kind.TileMetres, Kind.TileMetres);
			}
			Instance->PreEditChange(nullptr);
			Instance->PostEditChange();
			Instance->MarkPackageDirty();
			FAssetRegistryModule::AssetCreated(Instance);
			return Instance;
		}
	}

	FSurfBlockoutAssets Author(bool bSave, FString& OutError)
	{
		FSurfBlockoutAssets Out;
		OutError.Reset();

		for (const FSurfBlockoutKind& Kind : Kinds())
		{
			UTexture2D* Texture = AuthorTexture(Kind, OutError);
			if (!Texture)
			{
				return Out;
			}
			Out.Textures.Add(Texture);
		}

		// The one-metre grid is the master's default.
		UTexture2D* Default = Out.Textures.IsValidIndex(1) ? Out.Textures[1] : Out.Textures[0];
		Out.Master = AuthorMaster(Default, OutError);
		if (!Out.Master)
		{
			return Out;
		}

		const TArray<FSurfBlockoutKind>& All = Kinds();
		for (int32 Index = 0; Index < All.Num(); ++Index)
		{
			UMaterialInstanceConstant* Instance = AuthorInstance(All[Index], Out.Master, Out.Textures[Index], OutError);
			if (!Instance)
			{
				return Out;
			}
			Out.Instances.Add(Instance);
		}

		if (bSave)
		{
			for (UTexture2D* Texture : Out.Textures)
			{
				if (!SurfMaterialSeed::SaveAsset(Texture))
				{
					OutError = FString::Printf(TEXT("%s could not be saved"), *Texture->GetName());
					return Out;
				}
			}
			if (!SurfMaterialSeed::SaveAsset(Out.Master))
			{
				OutError = TEXT("the master could not be saved");
				return Out;
			}
			for (UMaterialInstanceConstant* Instance : Out.Instances)
			{
				if (!SurfMaterialSeed::SaveAsset(Instance))
				{
					OutError = FString::Printf(TEXT("%s could not be saved"), *Instance->GetName());
					return Out;
				}
			}
		}
		return Out;
	}

	FSurfBlockoutAssets Load()
	{
		FSurfBlockoutAssets Out;
		Out.Master = LoadObject<UMaterial>(nullptr, *(MasterPackage() + TEXT(".M_SurfBlockout")));
		for (const FSurfBlockoutKind& Kind : Kinds())
		{
			const FString T = Folder() / TextureName(Kind) + TEXT(".") + TextureName(Kind);
			const FString I = Folder() / InstanceName(Kind) + TEXT(".") + InstanceName(Kind);
			Out.Textures.Add(LoadObject<UTexture2D>(nullptr, *T));
			Out.Instances.Add(LoadObject<UMaterialInstanceConstant>(nullptr, *I));
		}
		return Out;
	}
}
