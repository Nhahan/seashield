#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "SeaNetSubsystem.h"

#include "SeaEnvironmentController.generated.h"

class UMaterialInterface;
class UMaterialParameterCollection;
class UProceduralMeshComponent;

// Drives the battlefield visuals from the SIMULATION weather (ServerWelcome
// v3 scalars via USeaNetSubsystem) — the charter v2.1 differentiator: the sea
// state, fog, rain and trail bend are the environment model's visualization,
// not art-side mood dressing.
//   - The gerstner spectrum is regenerated from wind speed/bearing/gusts.
//   - Height-fog density scales with rain intensity (visibility loss).
//   - Rain itself is a code-built streak volume that rides the camera; the
//     streaks lean with the simulation wind vector (no particle assets, same
//     discipline as the trail ribbons).
UCLASS()
class SEASHIELD_API ASeaEnvironmentController : public AActor
{
	GENERATED_BODY()

public:
	ASeaEnvironmentController() { PrimaryActorTick.bCanEverTick = true; }

	UPROPERTY(EditAnywhere, Category = "SeaShield")
	TObjectPtr<UMaterialParameterCollection> WeatherParameters;

	// Streak count at rain_intensity = 1 inside the camera volume.
	UPROPERTY(EditAnywhere, Category = "SeaShield")
	int32 RainDropsAtFullIntensity = 1600;

	// Terminal fall speed of the streaks (cm/s); wind adds the lean.
	UPROPERTY(EditAnywhere, Category = "SeaShield")
	float RainFallSpeedCms = 900.0f;

	virtual void Tick(float DeltaTime) override;

private:
	// Wind speed/direction -> gerstner spectrum on the level's ocean. The sea
	// state IS the simulation weather (charter v2.1 differentiator).
	void ApplySeaState(const FSeaWeather& Weather) const;
	// Humidity + rain -> exponential height fog density (the visibility the
	// fire-control problem actually loses in weather; humid air hazes even
	// without rain, both seed-random per engagement).
	void ApplyFog(const FSeaWeather& Weather) const;
	void UpdateRain(float DeltaTime);

	UPROPERTY()
	TObjectPtr<UProceduralMeshComponent> RainMesh;
	UPROPERTY()
	TObjectPtr<UMaterialInterface> RainMaterial;

	// Camera-relative streak positions (cm) — wrapped back into the volume as
	// they fall out of it, so the volume follows any view target switch.
	TArray<FVector> Drops;

	FSeaWeather CachedWeather;
	bool bApplied = false;
};
