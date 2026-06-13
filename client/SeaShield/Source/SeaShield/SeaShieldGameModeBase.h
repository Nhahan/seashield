#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"

#include "SeaShieldGameModeBase.generated.h"

// Guarantees the two presentation drivers (world manager, environment
// controller) exist in whatever map is loaded — levels carry visual dressing,
// never required logic, so the core loop also runs in empty/test maps.
UCLASS()
class SEASHIELD_API ASeaShieldGameModeBase : public AGameModeBase
{
	GENERATED_BODY()

public:
	ASeaShieldGameModeBase();

	virtual void BeginPlay() override;

	// The level carries no PlayerStart (it is fully script-generated); without
	// one the engine spawns at world zero — inside the Metal water defect the
	// stage offset exists to avoid (SeaWorldFrame.h). Spawn a bridge-wing
	// vantage near the ownship instead.
	virtual AActor* ChoosePlayerStart_Implementation(AController* Player) override;

private:
	TWeakObjectPtr<AActor> LazyPlayerStart;
};
