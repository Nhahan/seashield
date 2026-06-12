#include "SeaLevelSetupLibrary.h"

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
