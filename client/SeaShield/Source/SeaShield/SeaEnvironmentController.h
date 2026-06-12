#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "SeaEnvironmentController.generated.h"

class UMaterialParameterCollection;
class UNiagaraComponent;
class UNiagaraSystem;

// Drives the battlefield visuals from the SIMULATION weather (ServerWelcome
// v3 scalars via USeaNetSubsystem) — the charter v2.1 differentiator: the sea
// state, rain and trail bend are the environment model's visualization, not
// art-side mood dressing. Editor assets plug in via the UPROPERTYs:
//   - WeatherParameters (MPC) feeds the ocean/sky materials
//     (scalars "WindSpeed", "RainIntensity", "GustSigma"; vector "WindDir").
//   - RainSystem spawns when the simulation says it rains.
UCLASS()
class SEASHIELD_API ASeaEnvironmentController : public AActor
{
	GENERATED_BODY()

public:
	ASeaEnvironmentController() { PrimaryActorTick.bCanEverTick = true; }

	UPROPERTY(EditAnywhere, Category = "SeaShield")
	TObjectPtr<UMaterialParameterCollection> WeatherParameters;

	UPROPERTY(EditAnywhere, Category = "SeaShield")
	TObjectPtr<UNiagaraSystem> RainSystem;

	// Rain particle spawn rate at rain_intensity = 1.
	UPROPERTY(EditAnywhere, Category = "SeaShield")
	float RainRateAtFullIntensity = 6000.0f;

	virtual void Tick(float DeltaTime) override;

private:
	UPROPERTY()
	TObjectPtr<UNiagaraComponent> RainComponent;

	bool bApplied = false;
};
