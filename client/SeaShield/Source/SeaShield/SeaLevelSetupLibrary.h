#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"

#include "SeaLevelSetupLibrary.generated.h"

class AWaterBody;

// Level-construction helpers for Tools/setup_level.py. The Water plugin's
// waves wiring (WaterWavesAsset -> WaterWavesAssetReference -> body) uses
// classes that are not exposed to scripting, so the hop happens here in C++.
UCLASS()
class SEASHIELD_API USeaLevelSetupLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, Category = "SeaShield")
	static bool AssignOceanWaves(AWaterBody* Ocean, const FString& WavesAssetPath);

	// Seeded gerstner spectrum (UGerstnerWaterWaveGeneratorSimple). A wide
	// many-wave spectrum breaks the visible far-field repetition of the stock
	// asset, and the seed/wind parameters are the hook for the weather-driven
	// sea state (charter §5.6 환경 모델 시각화).
	UFUNCTION(BlueprintCallable, Category = "SeaShield")
	static bool AssignGeneratedOceanWaves(AWaterBody* Ocean, int32 Seed, int32 NumWaves,
	                                      float MinWavelengthCm, float MaxWavelengthCm,
	                                      float MinAmplitudeCm, float MaxAmplitudeCm,
	                                      float WindAngleDeg, float DirectionSpreadDeg,
	                                      float SmallWaveSteepness, float LargeWaveSteepness);
};
