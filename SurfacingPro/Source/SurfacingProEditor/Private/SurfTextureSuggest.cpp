// Copyright MapModelers. All Rights Reserved.

#include "SurfTextureSuggest.h"

#include "SurfMaterialsView.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/Texture.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Modules/ModuleManager.h"

namespace SurfTextureSuggest
{
	namespace
	{
		/** The suffixes a texture name can end in, upper-case. */
		const TCHAR* KnownSuffixes[] = {
			TEXT("N"), TEXT("NRM"), TEXT("NORMAL"), TEXT("R"), TEXT("ROUGH"), TEXT("ROUGHNESS"), TEXT("AO"), TEXT("M"), TEXT("METAL"), TEXT("METALLIC"),
			TEXT("H"), TEXT("HEIGHT"), TEXT("DISP"), TEXT("DISPLACEMENT"), TEXT("ORM"), TEXT("RMA"), TEXT("MASK"), TEXT("A"), TEXT("ALPHA"), TEXT("OP"), TEXT("OPACITY"), TEXT("E"), TEXT("O"),
			TEXT("D"), TEXT("C"), TEXT("BC"), TEXT("ALBEDO"), TEXT("DIFFUSE"), TEXT("COLOR"), TEXT("COLOUR"), TEXT("BASECOLOR"),
		};

		bool IsKnownSuffix(const FString& Upper)
		{
			for (const TCHAR* Known : KnownSuffixes)
			{
				if (Upper == Known)
				{
					return true;
				}
			}
			return false;
		}

		IAssetRegistry& SuggestRegistry()
		{
			return FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		}

		/** The texture on a parameter, overridden on the instance or not. */
		UTexture* TextureOn(const UMaterialInstanceConstant* Instance, FName Parameter, bool bOverriddenOnly)
		{
			UTexture* Texture = nullptr;
			if (Instance)
			{
				Instance->GetTextureParameterValue(FHashedMaterialParameterInfo(FMaterialParameterInfo(Parameter)), Texture, bOverriddenOnly);
			}
			return Texture;
		}

		/** The name to look for: the base alone for no suffix, else base_suffix. */
		FString NameFor(const FString& Base, const FString& Suffix)
		{
			return Suffix.IsEmpty() ? Base : Base + TEXT("_") + Suffix;
		}
	}

	void Split(const FString& AssetName, FString& OutBase, FString& OutSuffix)
	{
		OutBase = AssetName;
		OutSuffix.Reset();
		int32 Underscore = INDEX_NONE;
		if (AssetName.FindLastChar(TEXT('_'), Underscore) && Underscore > 0 && Underscore < AssetName.Len() - 1)
		{
			const FString Tail = AssetName.Mid(Underscore + 1).ToUpper();
			if (IsKnownSuffix(Tail))
			{
				OutBase = AssetName.Left(Underscore);
				OutSuffix = Tail;
			}
		}
	}

	void SuffixesFor(FName Parameter, TArray<FString>& Out)
	{
		Out.Reset();
		const FString Name = Parameter.ToString().ToLower();
		auto Has = [&Name](const TCHAR* Word) { return Name.Contains(Word); };

		if (Has(TEXT("normal")) || Has(TEXT("nrm")) || Has(TEXT("norm")))
		{
			Out = { TEXT("N"), TEXT("NRM"), TEXT("NORMAL") };
		}
		else if (Has(TEXT("orm")) || Has(TEXT("rma")) || Has(TEXT("packed")) || Has(TEXT("mask")))
		{
			Out = { TEXT("ORM"), TEXT("RMA"), TEXT("MASK") };
		}
		else if (Has(TEXT("rough")))
		{
			Out = { TEXT("R"), TEXT("ROUGH"), TEXT("ROUGHNESS"), TEXT("ORM"), TEXT("RMA") };
		}
		else if (Has(TEXT("metal")))
		{
			Out = { TEXT("M"), TEXT("METAL"), TEXT("METALLIC"), TEXT("ORM"), TEXT("RMA") };
		}
		else if (Has(TEXT("occlusion")) || Name == TEXT("ao") || Name.StartsWith(TEXT("ao")) || Name.EndsWith(TEXT("ao")) || Has(TEXT("_ao")) || Has(TEXT("ambient")))
		{
			Out = { TEXT("AO"), TEXT("ORM"), TEXT("RMA") };
		}
		else if (Has(TEXT("height")) || Has(TEXT("displace")) || Has(TEXT("bump")) || Has(TEXT("parallax")))
		{
			Out = { TEXT("H"), TEXT("HEIGHT"), TEXT("DISP"), TEXT("DISPLACEMENT") };
		}
		else if (Has(TEXT("emiss")) || Has(TEXT("glow")))
		{
			Out = { TEXT("E") };
		}
		else if (Has(TEXT("opacity")) || Has(TEXT("alpha")) || Has(TEXT("transp")))
		{
			Out = { TEXT("A"), TEXT("ALPHA"), TEXT("OP"), TEXT("OPACITY"), TEXT("O"), TEXT("MASK") };
		}
		else if (Has(TEXT("color")) || Has(TEXT("colour")) || Has(TEXT("albedo")) || Has(TEXT("diffuse")) || Has(TEXT("base")))
		{
			// _D is the diffuse (MapLink's T_Mk_Wall_D); the bare name next.
			Out = { TEXT("D"), TEXT(""), TEXT("C"), TEXT("BC"), TEXT("BASECOLOR"), TEXT("ALBEDO"), TEXT("DIFFUSE"), TEXT("COLOR"), TEXT("COLOUR") };
		}
	}

	void IndexTextures(const FString& LibraryRoot, TMap<FString, FAssetData>& Out)
	{
		Out.Reset();
		const FString Root = SurfMaterialsLib::NormalisedRoot(LibraryRoot);
		if (Root.IsEmpty())
		{
			return;
		}
		TArray<FAssetData> Assets;
		SuggestRegistry().GetAssetsByPath(FName(*Root), Assets, /*bRecursive*/ true);
		for (const FAssetData& Asset : Assets)
		{
			const UClass* Class = Asset.GetClass();
			if (Class && Class->IsChildOf(UTexture::StaticClass()))
			{
				Out.Add(Asset.AssetName.ToString().ToUpper(), Asset);
			}
		}
	}

	void Suggest(UMaterialInstanceConstant* Instance, FName SetParameter, const FString& LibraryRoot, TArray<FSurfTextureSuggestion>& Out)
	{
		Out.Reset();
		if (!Instance)
		{
			return;
		}
		const UTexture* Picked = TextureOn(Instance, SetParameter, /*bOverriddenOnly*/ false);
		if (!Picked)
		{
			return;
		}
		FString Base;
		FString PickedSuffix;
		Split(Picked->GetName(), Base, PickedSuffix);
		if (Base.IsEmpty())
		{
			return;
		}

		TMap<FString, FAssetData> Textures;
		IndexTextures(LibraryRoot, Textures);
		if (Textures.Num() == 0)
		{
			return;
		}

		TArray<FMaterialParameterInfo> Infos;
		TArray<FGuid> Guids;
		Instance->GetAllTextureParameterInfo(Infos, Guids);

		// What the library's other instances pair with each parameter: for an
		// instance whose SetParameter holds a texture of the same role, the
		// suffix of the sibling on every other parameter, weighed by kinship.
		struct FSeen
		{
			int32 Weight = 0;
			FString InstanceName;
		};
		TMap<FName, TMap<FString, FSeen>> Learned;
		{
			const FString Root = SurfMaterialsLib::NormalisedRoot(LibraryRoot);
			TArray<FAssetData> Assets;
			if (!Root.IsEmpty())
			{
				SuggestRegistry().GetAssetsByPath(FName(*Root), Assets, /*bRecursive*/ true);
			}
			for (const FAssetData& Asset : Assets)
			{
				const UClass* Class = Asset.GetClass();
				if (!Class || !Class->IsChildOf(UMaterialInstanceConstant::StaticClass()))
				{
					continue;
				}
				const UMaterialInstanceConstant* Other = Cast<UMaterialInstanceConstant>(Asset.GetAsset());
				if (!Other || Other == Instance)
				{
					continue;
				}
				const UTexture* OtherPicked = TextureOn(Other, SetParameter, /*bOverriddenOnly*/ true);
				if (!OtherPicked)
				{
					continue;
				}
				FString OtherBase;
				FString OtherSuffix;
				Split(OtherPicked->GetName(), OtherBase, OtherSuffix);
				if (OtherSuffix != PickedSuffix)
				{
					continue;
				}
				const int32 Weight = Other->Parent == Instance->Parent ? 3 : 1;
				for (const FTextureParameterValue& Value : Other->TextureParameterValues)
				{
					if (Value.ParameterInfo.Name == SetParameter || !Value.ParameterValue)
					{
						continue;
					}
					FString SiblingBase;
					FString SiblingSuffix;
					Split(Value.ParameterValue->GetName(), SiblingBase, SiblingSuffix);
					if (SiblingBase != OtherBase)
					{
						continue;
					}
					FSeen& Seen = Learned.FindOrAdd(Value.ParameterInfo.Name).FindOrAdd(SiblingSuffix);
					Seen.Weight += Weight;
					if (Seen.InstanceName.IsEmpty() || Weight == 3)
					{
						Seen.InstanceName = Other->GetName();
					}
				}
			}
		}

		for (const FMaterialParameterInfo& Info : Infos)
		{
			if (Info.Name == SetParameter)
			{
				continue;
			}
			// Holding one of the set already: nothing to offer.
			if (const UTexture* Current = TextureOn(Instance, Info.Name, /*bOverriddenOnly*/ false))
			{
				FString CurrentBase;
				FString CurrentSuffix;
				Split(Current->GetName(), CurrentBase, CurrentSuffix);
				if (CurrentBase == Base)
				{
					continue;
				}
			}

			// Learned first, by weight; then the name's say.
			TArray<TPair<FString, FSeen>> Candidates;
			if (const TMap<FString, FSeen>* ForThis = Learned.Find(Info.Name))
			{
				for (const TPair<FString, FSeen>& Pair : *ForThis)
				{
					Candidates.Add(Pair);
				}
				Candidates.Sort([](const TPair<FString, FSeen>& A, const TPair<FString, FSeen>& B) { return A.Value.Weight > B.Value.Weight; });
			}
			TArray<FString> ByName;
			SuffixesFor(Info.Name, ByName);
			for (const FString& Suffix : ByName)
			{
				Candidates.Add(TPair<FString, FSeen>(Suffix, FSeen()));
			}

			for (const TPair<FString, FSeen>& Candidate : Candidates)
			{
				const FString Name = NameFor(Base, Candidate.Key);
				if (Name.Equals(Picked->GetName(), ESearchCase::IgnoreCase))
				{
					continue;
				}
				if (const FAssetData* Found = Textures.Find(Name.ToUpper()))
				{
					FSurfTextureSuggestion& Suggestion = Out.AddDefaulted_GetRef();
					Suggestion.Parameter = Info.Name;
					Suggestion.Texture = *Found;
					Suggestion.Why = Candidate.Value.Weight > 0
						? FString::Printf(TEXT("as in %s"), *Candidate.Value.InstanceName)
						: TEXT("by its name");
					break;
				}
			}
		}
	}
}
