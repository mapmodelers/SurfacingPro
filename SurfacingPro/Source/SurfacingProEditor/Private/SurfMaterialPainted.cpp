// Copyright MapModelers. All Rights Reserved.

#include "SurfMaterialPainted.h"

#include "SurfMaterialInfo.h"
#include "SurfMaterialSeed.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionAbs.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionAppendVector.h"
#include "Materials/MaterialExpressionDotProduct.h"
#include "Materials/MaterialExpressionOneMinus.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionSaturate.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionSubtract.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionVertexColor.h"
#include "Materials/MaterialFunction.h"
#include "Engine/EngineTypes.h"
#include "Engine/Texture.h"
#include "UObject/Package.h"

namespace SurfMaterialPainted
{
	FString FunctionPackage()
	{
		return TEXT("/SurfacingPro/Materials/Functions/MF_SurfPaintedMask");
	}

	FString MasterPackage()
	{
		return TEXT("/SurfacingPro/Materials/M_SurfPainted");
	}

	namespace
	{
		template<typename T>
		T* InFunction(UMaterialFunction* Function, int32 X, int32 Y)
		{
			return Cast<T>(UMaterialEditingLibrary::CreateMaterialExpressionInFunction(Function, T::StaticClass(), X, Y));
		}

		template<typename T>
		T* InMaster(UMaterial* Master, int32 X, int32 Y)
		{
			return Cast<T>(UMaterialEditingLibrary::CreateMaterialExpression(Master, T::StaticClass(), X, Y));
		}

		UMaterialExpressionFunctionInput* ScalarInput(UMaterialFunction* Function, const TCHAR* Name, float Default,
			const TCHAR* Description, int32 Priority, int32 Y)
		{
			UMaterialExpressionFunctionInput* In = InFunction<UMaterialExpressionFunctionInput>(Function, -1000, Y);
			if (In)
			{
				In->InputName = FName(Name);
				In->InputType = FunctionInput_Scalar;
				In->PreviewValue = FVector4f(Default, Default, Default, Default);
				In->bUsePreviewValueAsDefault = true;
				In->Description = Description;
				In->SortPriority = Priority;
				In->ConditionallyGenerateId(true);
				In->ValidateName();
			}
			return In;
		}

		UMaterialExpressionFunctionOutput* Output(UMaterialFunction* Function, const TCHAR* Name, const TCHAR* Description,
			int32 Priority, UMaterialExpression* From, int32 Y)
		{
			UMaterialExpressionFunctionOutput* Out = InFunction<UMaterialExpressionFunctionOutput>(Function, 500, Y);
			if (Out)
			{
				Out->OutputName = FName(Name);
				Out->Description = Description;
				Out->SortPriority = Priority;
				Out->A.Expression = From;
				Out->ConditionallyGenerateId(true);
				Out->ValidateName();
			}
			return Out;
		}

		UMaterialFunction* AuthorFunction(FString& OutError)
		{
			UPackage* Package = CreatePackage(*FunctionPackage());
			if (!Package)
			{
				OutError = TEXT("could not make the function's package");
				return nullptr;
			}
			Package->FullyLoad();

			UMaterialFunction* Function = NewObject<UMaterialFunction>(Package, TEXT("MF_SurfPaintedMask"), RF_Public | RF_Standalone | RF_Transactional);
			Function->Description = TEXT("The painted weight of a SurfacingPro layer (vertex alpha) as a mask for Opacity Mask. Paint is the weight as painted; Mask is the same, sharpened against the material's own height when Sharpen is turned up.");
			Function->bExposeToLibrary = true;

			// The inputs, all optional: which channel the paint is in, a height to sharpen against, how much, how hard.
			UMaterialExpressionFunctionInput* Channel = ScalarInput(Function, TEXT("Channel"), 0.0f,
				TEXT("Which channel the paint is in: 0 red (the brush's default), 1 green, 2 blue, 3 alpha."), 0, -400);
			UMaterialExpressionFunctionInput* Height = ScalarInput(Function, TEXT("Height"), 0.5f,
				TEXT("The material's own height, 0 to 1, for the sharpened edge. Unplugged: flat."), 1, -200);
			UMaterialExpressionFunctionInput* Sharpen = ScalarInput(Function, TEXT("Sharpen"), 0.0f,
				TEXT("0: the paint as painted. 1: the edge sharpened against the height."), 2, 0);
			UMaterialExpressionFunctionInput* Contrast = ScalarInput(Function, TEXT("Contrast"), 1.0f,
				TEXT("How hard the sharpened edge is: 1 and up."), 3, 200);

			// The weight: the one channel of the vertex colour the paint is in
			// (D71). A pick per channel - saturate(1 - |Channel - k|) is 1 for
			// that channel and 0 for the others - then a dot with the colour
			// and the alpha's share added.
			UMaterialExpressionVertexColor* Vertex = InFunction<UMaterialExpressionVertexColor>(Function, -1000, -900);
			UMaterialExpression* Picks[4] = { nullptr, nullptr, nullptr, nullptr };
			for (int32 K = 0; K < 4; ++K)
			{
				UMaterialExpressionSubtract* Off = InFunction<UMaterialExpressionSubtract>(Function, -1000, -1300 + K * 100);
				UMaterialExpressionAbs* Distance = InFunction<UMaterialExpressionAbs>(Function, -870, -1300 + K * 100);
				UMaterialExpressionOneMinus* Near = InFunction<UMaterialExpressionOneMinus>(Function, -740, -1300 + K * 100);
				UMaterialExpressionSaturate* Pick = InFunction<UMaterialExpressionSaturate>(Function, -610, -1300 + K * 100);
				if (!Off || !Distance || !Near || !Pick)
				{
					OutError = TEXT("could not make the channel picks");
					return nullptr;
				}
				Off->A.Expression = Channel;
				Off->ConstB = static_cast<float>(K);
				Distance->Input.Expression = Off;
				Near->Input.Expression = Distance;
				Pick->Input.Expression = Near;
				Picks[K] = Pick;
			}
			UMaterialExpressionAppendVector* PickRG = InFunction<UMaterialExpressionAppendVector>(Function, -480, -1250);
			UMaterialExpressionAppendVector* PickRGB = InFunction<UMaterialExpressionAppendVector>(Function, -350, -1200);
			UMaterialExpressionDotProduct* Colour = InFunction<UMaterialExpressionDotProduct>(Function, -800, -700);
			UMaterialExpressionMultiply* Alpha = InFunction<UMaterialExpressionMultiply>(Function, -800, -550);
			UMaterialExpressionAdd* Weight = InFunction<UMaterialExpressionAdd>(Function, -650, -600);
			if (!PickRG || !PickRGB || !Colour || !Alpha || !Weight)
			{
				OutError = TEXT("could not make the weight");
				return nullptr;
			}
			PickRG->A.Expression = Picks[0];
			PickRG->B.Expression = Picks[1];
			PickRGB->A.Expression = PickRG;
			PickRGB->B.Expression = Picks[2];
			Colour->A.Expression = Vertex;
			Colour->A.OutputIndex = 0;
			Colour->B.Expression = PickRGB;
			Alpha->A.Expression = Vertex;
			Alpha->A.OutputIndex = 4;
			Alpha->B.Expression = Picks[3];
			Weight->A.Expression = Colour;
			Weight->B.Expression = Alpha;

			// Sharpened: saturate((weight * 2 - 1 + height) * contrast) - D14's formula.
			UMaterialExpressionMultiply* Twice = InFunction<UMaterialExpressionMultiply>(Function, -600, -350);
			UMaterialExpressionSubtract* Centred = InFunction<UMaterialExpressionSubtract>(Function, -450, -350);
			UMaterialExpressionAdd* Lifted = InFunction<UMaterialExpressionAdd>(Function, -300, -300);
			UMaterialExpressionMultiply* Hardened = InFunction<UMaterialExpressionMultiply>(Function, -150, -250);
			UMaterialExpressionSaturate* Clamped = InFunction<UMaterialExpressionSaturate>(Function, 0, -250);

			// Mask: the paint as painted, sliding to the sharpened one as Sharpen goes up.
			UMaterialExpressionLinearInterpolate* Mixed = InFunction<UMaterialExpressionLinearInterpolate>(Function, 200, -100);

			if (!Channel || !Height || !Sharpen || !Contrast || !Vertex || !Twice || !Centred || !Lifted || !Hardened || !Clamped || !Mixed)
			{
				OutError = TEXT("could not make the function's nodes");
				return nullptr;
			}

			Twice->A.Expression = Weight;
			Twice->ConstB = 2.0f;
			Centred->A.Expression = Twice;
			Centred->ConstB = 1.0f;
			Lifted->A.Expression = Centred;
			Lifted->B.Expression = Height;
			Hardened->A.Expression = Lifted;
			Hardened->B.Expression = Contrast;
			Clamped->Input.Expression = Hardened;

			Mixed->A.Expression = Weight;
			Mixed->B.Expression = Clamped;
			Mixed->Alpha.Expression = Sharpen;

			UMaterialExpressionFunctionOutput* Mask = Output(Function, TEXT("Mask"),
				TEXT("The weight, sharpened against the height as far as Sharpen says. Plug into Opacity Mask."), 0, Mixed, -100);
			UMaterialExpressionFunctionOutput* Paint = Output(Function, TEXT("Paint"),
				TEXT("The weight exactly as painted, 0 to 1, from the channel chosen."), 1, Weight, 100);
			if (!Mask || !Paint)
			{
				OutError = TEXT("could not make the function's outputs");
				return nullptr;
			}

			Function->PreEditChange(nullptr);
			Function->PostEditChange();
			Function->MarkPackageDirty();
			FAssetRegistryModule::AssetCreated(Function);
			return Function;
		}

		UMaterial* AuthorMaster(UMaterialFunction* Function, FString& OutError)
		{
			UTexture* White = LoadObject<UTexture>(nullptr, TEXT("/SurfacingPro/Materials/32_grayscale_white.32_grayscale_white"));
			UTexture* Flat = LoadObject<UTexture>(nullptr, TEXT("/SurfacingPro/Materials/32_normal.32_normal"));
			if (!White || !Flat)
			{
				OutError = TEXT("the plugin's textures are not there to default the master's parameters to");
				return nullptr;
			}

			UPackage* Package = CreatePackage(*MasterPackage());
			if (!Package)
			{
				OutError = TEXT("could not make the master's package");
				return nullptr;
			}
			Package->FullyLoad();

			UMaterial* Master = NewObject<UMaterial>(Package, TEXT("M_SurfPainted"), RF_Public | RF_Standalone | RF_Transactional);
			Master->BlendMode = BLEND_Masked;

			auto TextureParameter = [Master](const TCHAR* Name, UTexture* Default, EMaterialSamplerType Sampler, int32 Y)
			{
				UMaterialExpressionTextureSampleParameter2D* Parameter = InMaster<UMaterialExpressionTextureSampleParameter2D>(Master, -700, Y);
				if (Parameter)
				{
					Parameter->ParameterName = FName(Name);
					Parameter->Texture = Default;
					Parameter->SamplerType = Sampler;
				}
				return Parameter;
			};
			auto ScalarParameter = [Master](const TCHAR* Name, float Default, int32 Y)
			{
				UMaterialExpressionScalarParameter* Parameter = InMaster<UMaterialExpressionScalarParameter>(Master, -700, Y);
				if (Parameter)
				{
					Parameter->ParameterName = FName(Name);
					Parameter->DefaultValue = Default;
				}
				return Parameter;
			};

			UMaterialExpressionTextureSampleParameter2D* BaseColor = TextureParameter(TEXT("BaseColor"), White, SAMPLERTYPE_Color, -400);
			UMaterialExpressionTextureSampleParameter2D* NormalMap = TextureParameter(TEXT("Normal"), Flat, SAMPLERTYPE_Normal, -100);
			UMaterialExpressionScalarParameter* Roughness = ScalarParameter(TEXT("Roughness"), 0.8f, 150);
			UMaterialExpressionScalarParameter* Channel = ScalarParameter(TEXT("Channel"), 0.0f, 500);
			UMaterialExpressionTextureSampleParameter2D* Height = TextureParameter(TEXT("Height"), White, SAMPLERTYPE_Color, 300);
			UMaterialExpressionScalarParameter* Sharpen = ScalarParameter(TEXT("Sharpen"), 0.0f, 650);
			UMaterialExpressionScalarParameter* Contrast = ScalarParameter(TEXT("Contrast"), 1.0f, 750);
			UMaterialExpressionMaterialFunctionCall* Call = InMaster<UMaterialExpressionMaterialFunctionCall>(Master, -300, 500);
			if (!BaseColor || !NormalMap || !Roughness || !Channel || !Height || !Sharpen || !Contrast || !Call)
			{
				OutError = TEXT("could not make the master's nodes");
				return nullptr;
			}

			Call->SetMaterialFunction(Function);
			for (FFunctionExpressionInput& In : Call->FunctionInputs)
			{
				const FName Name = In.ExpressionInput ? In.ExpressionInput->InputName : NAME_None;
				if (Name == FName(TEXT("Channel")))
				{
					In.Input.Expression = Channel;
				}
				else if (Name == FName(TEXT("Height")))
				{
					// The height's red channel.
					In.Input.Expression = Height;
					In.Input.OutputIndex = 1;
				}
				else if (Name == FName(TEXT("Sharpen")))
				{
					In.Input.Expression = Sharpen;
				}
				else if (Name == FName(TEXT("Contrast")))
				{
					In.Input.Expression = Contrast;
				}
			}

			UMaterialEditorOnlyData* Editor = Master->GetEditorOnlyData();
			if (!Editor)
			{
				OutError = TEXT("the master has no editor data");
				return nullptr;
			}
			Editor->BaseColor.Expression = BaseColor;
			Editor->Normal.Expression = NormalMap;
			Editor->Roughness.Expression = Roughness;
			Editor->OpacityMask.Expression = Call;
			Editor->OpacityMask.OutputIndex = 0;

			// A layer material: its mask is the paint, so it belongs above another patch.
			if (USurfMaterialInfo* Info = USurfMaterialInfo::FindOrAdd(Master))
			{
				Info->Use = ESurfMaterialUse::Layer;
				Info->DefaultScale = FVector2D(1.0, 1.0);
			}

			Master->PreEditChange(nullptr);
			Master->PostEditChange();
			Master->MarkPackageDirty();
			FAssetRegistryModule::AssetCreated(Master);
			return Master;
		}
	}

	FSurfPaintedAssets Author(bool bSave, FString& OutError)
	{
		FSurfPaintedAssets Out;
		OutError.Reset();

		Out.Function = AuthorFunction(OutError);
		if (!Out.Function)
		{
			return Out;
		}
		Out.Master = AuthorMaster(Out.Function, OutError);
		if (!Out.Master)
		{
			return Out;
		}

		if (bSave)
		{
			if (!SurfMaterialSeed::SaveAsset(Out.Function))
			{
				OutError = TEXT("the function could not be saved");
			}
			else if (!SurfMaterialSeed::SaveAsset(Out.Master))
			{
				OutError = TEXT("the master could not be saved");
			}
		}
		return Out;
	}

	FSurfPaintedAssets Load()
	{
		FSurfPaintedAssets Out;
		Out.Function = LoadObject<UMaterialFunction>(nullptr, *(FunctionPackage() + TEXT(".MF_SurfPaintedMask")));
		Out.Master = LoadObject<UMaterial>(nullptr, *(MasterPackage() + TEXT(".M_SurfPainted")));
		return Out;
	}
}
