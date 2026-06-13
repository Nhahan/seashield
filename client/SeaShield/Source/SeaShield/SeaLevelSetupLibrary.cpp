#include "SeaLevelSetupLibrary.h"

#include "GerstnerWaterWaves.h"
#include "WaterBodyActor.h"
#include "WaterWaves.h"

DEFINE_LOG_CATEGORY_STATIC(LogSeaLevelSetup, Log, All);

bool USeaLevelSetupLibrary::AssignOceanWaves(AWaterBody* Ocean, const FString& WavesAssetPath)
{
	if (Ocean == nullptr)
	{
		UE_LOG(LogSeaLevelSetup, Error, TEXT("AssignOceanWaves: null ocean"));
		return false;
	}
	UWaterWavesAsset* WavesAsset = LoadObject<UWaterWavesAsset>(nullptr, *WavesAssetPath);
	if (WavesAsset == nullptr)
	{
		UE_LOG(LogSeaLevelSetup, Error, TEXT("AssignOceanWaves: cannot load %s"), *WavesAssetPath);
		return false;
	}
	UWaterWavesAssetReference* Reference = NewObject<UWaterWavesAssetReference>(Ocean);
	Reference->SetWaterWavesAsset(WavesAsset);
	Ocean->SetWaterWaves(Reference);
	UE_LOG(LogSeaLevelSetup, Display, TEXT("AssignOceanWaves: %s -> %s"), *WavesAssetPath,
	       *Ocean->GetActorNameOrLabel());
	return true;
}

bool USeaLevelSetupLibrary::AssignGeneratedOceanWaves(AWaterBody* Ocean, int32 Seed, int32 NumWaves,
                                                      float MinWavelengthCm, float MaxWavelengthCm,
                                                      float MinAmplitudeCm, float MaxAmplitudeCm,
                                                      float WindAngleDeg, float DirectionSpreadDeg,
                                                      float SmallWaveSteepness, float LargeWaveSteepness)
{
	if (Ocean == nullptr)
	{
		UE_LOG(LogSeaLevelSetup, Error, TEXT("AssignGeneratedOceanWaves: null ocean"));
		return false;
	}
	UGerstnerWaterWaves* Waves = NewObject<UGerstnerWaterWaves>(Ocean);
	UGerstnerWaterWaveGeneratorSimple* Generator = NewObject<UGerstnerWaterWaveGeneratorSimple>(Waves);
	Generator->Seed = Seed;
	Generator->NumWaves = NumWaves;
	Generator->MinWavelength = MinWavelengthCm;
	Generator->MaxWavelength = MaxWavelengthCm;
	Generator->MinAmplitude = MinAmplitudeCm;
	Generator->MaxAmplitude = MaxAmplitudeCm;
	Generator->WindAngleDeg = WindAngleDeg;
	Generator->DirectionAngularSpreadDeg = DirectionSpreadDeg;
	Generator->SmallWaveSteepness = SmallWaveSteepness;
	Generator->LargeWaveSteepness = LargeWaveSteepness;
	Waves->GerstnerWaveGenerator = Generator;
	Waves->RecomputeWaves(/*bAllowBPScript=*/false);
	Ocean->SetWaterWaves(Waves);
	UE_LOG(LogSeaLevelSetup, Display,
	       TEXT("AssignGeneratedOceanWaves: seed=%d waves=%d wl=[%.0f, %.0f]cm amp=[%.0f, %.0f]cm "
	            "wind=%.0fdeg spread=%.0fdeg -> %s (max height %.1f cm)"),
	       Seed, NumWaves, MinWavelengthCm, MaxWavelengthCm, MinAmplitudeCm, MaxAmplitudeCm,
	       WindAngleDeg, DirectionSpreadDeg, *Ocean->GetActorNameOrLabel(), Waves->GetMaxWaveHeight());
	return true;
}
