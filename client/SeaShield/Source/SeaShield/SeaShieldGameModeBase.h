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
	virtual void BeginPlay() override;
};
