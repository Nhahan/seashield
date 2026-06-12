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
};
