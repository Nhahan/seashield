#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "SeaNetSubsystem.h"

#include "SeaWorldManager.generated.h"

class UMaterialInterface;
class UProceduralMeshComponent;

// Reconciles the interpolated entity sample into spawned actors each frame
// (charter §7 "스냅샷 → 보간 버퍼 → 액터 트랜스폼"). Per kind: a Blueprint
// class when assigned (VFX-laden actors later), else the bare procedural mesh
// (Tools/import_assets.py output) on a plain static-mesh actor — so the world
// is fully functional before any editor work.
//
// Engagement visuals are code-driven (no particle assets to hand-author):
//  - Rocket exhaust trails are camera-facing ribbons rebuilt per frame from
//    the rockets' position history; each history point DRIFTS WITH THE
//    SIMULATION WIND, so the bend of the smoke columns is the environment
//    model made visible (charter v2.1 차별점).
//  - Detonations raise a short-lived water column at the rocket's last
//    position, driven by the reliable engagement-event stream.
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

	UPROPERTY(EditAnywhere, Category = "SeaShield") TObjectPtr<UMaterialInterface> TrailMaterial;
	UPROPERTY(EditAnywhere, Category = "SeaShield") TObjectPtr<UMaterialInterface> SplashMaterial;
	UPROPERTY(EditAnywhere, Category = "SeaShield") TObjectPtr<UMaterialInterface> BurstMaterial;

	// Trail tuning: how long a smoke column lingers, how wide it grows.
	UPROPERTY(EditAnywhere, Category = "SeaShield") float TrailLifetimeS = 14.0f;
	UPROPERTY(EditAnywhere, Category = "SeaShield") float TrailWidthYoungCm = 60.0f;
	UPROPERTY(EditAnywhere, Category = "SeaShield") float TrailWidthOldCm = 900.0f;

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;
	virtual void Tick(float DeltaTime) override;

	UFUNCTION()
	void HandleEngagementEvent(const FSeaEngagementEvent& Event);

private:
	TSubclassOf<AActor> ClassFor(ESeaEntityKind Kind) const;
	UStaticMesh* MeshFor(ESeaEntityKind Kind) const;
	AActor* SpawnFor(const FSeaEntityState& Entity);

	struct FTrailPoint
	{
		FVector Position;  // Stage frame, at emission time.
		double Time = 0.0;
	};
	struct FRocketTrail
	{
		TArray<FTrailPoint> Points;
		TWeakObjectPtr<UProceduralMeshComponent> Ribbon;
		FVector LastRocketPosition = FVector::ZeroVector;
		double LastSampleTime = 0.0;
		bool bRocketAlive = true;
	};
	struct FSplash
	{
		TWeakObjectPtr<AActor> Column;
		double SpawnTime = 0.0;
		bool bAirburst = false;
	};

	void SampleTrail(int32 Key, const FVector& StagePosition, double Now);
	void RebuildTrails(double Now, const FVector& WindCms);
	void SpawnSplash(const FVector& StagePosition, double Now, bool bAirburst);
	void UpdateSplashes(double Now);
	// Draws every engagement material once as sub-pixel specks in front of
	// the camera, so first-use shader/PSO compiles happen during boot instead
	// of as a mid-salvo hitch (measured ~400 ms, client-design §8).
	void WarmupVfx();

	bool bLoggedFirstSpawn = false;
	bool bBurstCamFired = false;

	// Frame statistics for the K6 budget table (logged at EndPlay).
	int64 FrameCount = 0;
	double FrameTimeSumMs = 0.0;
	double FrameTimeMaxMs = 0.0;
	int64 FramesOver16ms = 0;

	// Key = (kind << 16) | id — the wire identity (rocket and track ids
	// share a number space).
	TMap<int32, TWeakObjectPtr<AActor>> Spawned;
	TMap<int32, FRocketTrail> Trails;
	TArray<FSplash> Splashes;
};
