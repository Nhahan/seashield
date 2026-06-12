#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "SeaNetSubsystem.h"

#include "SeaWorldManager.generated.h"

// Reconciles the interpolated entity sample into spawned actors each frame
// (charter §7 "스냅샷 → 보간 버퍼 → 액터 트랜스폼"). The actor classes are
// assigned in the editor (procedural-asset blueprints from tools/assets/).
UCLASS()
class SEASHIELD_API ASeaWorldManager : public AActor
{
	GENERATED_BODY()

public:
	ASeaWorldManager() { PrimaryActorTick.bCanEverTick = true; }

	UPROPERTY(EditAnywhere, Category = "SeaShield") TSubclassOf<AActor> TargetClass;
	UPROPERTY(EditAnywhere, Category = "SeaShield") TSubclassOf<AActor> RocketClass;
	UPROPERTY(EditAnywhere, Category = "SeaShield") TSubclassOf<AActor> TrackClass;

	virtual void Tick(float DeltaTime) override;

private:
	TSubclassOf<AActor> ClassFor(ESeaEntityKind Kind) const;

	// Key = (kind << 16) | id — the wire identity (rocket and track ids
	// share a number space).
	TMap<int32, TWeakObjectPtr<AActor>> Spawned;
};
