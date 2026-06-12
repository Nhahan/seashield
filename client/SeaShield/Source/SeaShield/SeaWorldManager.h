#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "SeaNetSubsystem.h"

#include "SeaWorldManager.generated.h"

// Reconciles the interpolated entity sample into spawned actors each frame
// (charter §7 "스냅샷 → 보간 버퍼 → 액터 트랜스폼"). Per kind: a Blueprint
// class when assigned (VFX-laden actors later), else the bare procedural mesh
// (Tools/import_assets.py output) on a plain static-mesh actor — so the world
// is fully functional before any editor work.
UCLASS()
class SEASHIELD_API ASeaWorldManager : public AActor
{
	GENERATED_BODY()

public:
	ASeaWorldManager();

	UPROPERTY(EditAnywhere, Category = "SeaShield") TSubclassOf<AActor> TargetClass;
	UPROPERTY(EditAnywhere, Category = "SeaShield") TSubclassOf<AActor> RocketClass;
	UPROPERTY(EditAnywhere, Category = "SeaShield") TSubclassOf<AActor> TrackClass;

	// Fallbacks when the class above is unset. Tracks default to none — their
	// symbology lives on the PPI, not in 3D (client-design.md §6).
	UPROPERTY(EditAnywhere, Category = "SeaShield") TObjectPtr<UStaticMesh> TargetMesh;
	UPROPERTY(EditAnywhere, Category = "SeaShield") TObjectPtr<UStaticMesh> RocketMesh;
	UPROPERTY(EditAnywhere, Category = "SeaShield") TObjectPtr<UStaticMesh> TrackMesh;

	virtual void Tick(float DeltaTime) override;

private:
	TSubclassOf<AActor> ClassFor(ESeaEntityKind Kind) const;
	UStaticMesh* MeshFor(ESeaEntityKind Kind) const;
	AActor* SpawnFor(const FSeaEntityState& Entity);

	bool bLoggedFirstSpawn = false;

	// Key = (kind << 16) | id — the wire identity (rocket and track ids
	// share a number space).
	TMap<int32, TWeakObjectPtr<AActor>> Spawned;
};
