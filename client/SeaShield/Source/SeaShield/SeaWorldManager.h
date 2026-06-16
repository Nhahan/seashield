#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "SeaNetSubsystem.h"
#include "SeaVfxSystems.h"

#include "SeaWorldManager.generated.h"

class UMaterialInterface;
class UProceduralMeshComponent;
class UInstancedStaticMeshComponent;
class UWaterBodyComponent;

// Reconciles the interpolated entity sample into spawned actors each frame
// (charter §7 "스냅샷 → 보간 버퍼 → 액터 트랜스폼"). Per kind: a Blueprint
// class when assigned (VFX-laden actors later), else the bare procedural mesh
// (Tools/import_assets.py output) on a plain static-mesh actor — so the world
// is fully functional before any editor work.
//
// Engagement visuals are code-driven (no particle assets to hand-author). The
// per-effect VFX live in focused systems (SeaVfxSystems.h) the manager owns as
// TUniquePtr members; this class keeps only reconcile + buoyancy + event-routing
// + per-frame dispatch + perf-instrumentation:
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
	UPROPERTY(EditAnywhere, Category = "SeaShield") TObjectPtr<UMaterialInterface> MuzzleMaterial;
	UPROPERTY(EditAnywhere, Category = "SeaShield") TObjectPtr<UMaterialInterface> WakeMaterial;
	UPROPERTY(EditAnywhere, Category = "SeaShield") TObjectPtr<UMaterialInterface> SmokeMaterial;
	UPROPERTY(EditAnywhere, Category = "SeaShield") TObjectPtr<UMaterialInterface> DebrisMaterial;

	// Trail tuning: the ribbon marks ONLY the fresh, coherent exhaust right behind the
	// rocket (short-lived). The aged trail is carried by the billboard puffs, which
	// drift off-axis, balloon, and fade — so the old smoke DISPERSES naturally instead
	// of staying a rigid band glued to the trajectory. (Fresh = tight column near the
	// motor; old = breaking, thinning cloud.)
	UPROPERTY(EditAnywhere, Category = "SeaShield") float TrailLifetimeS = 2.6f;
	UPROPERTY(EditAnywhere, Category = "SeaShield") float TrailWidthYoungCm = 130.0f;
	UPROPERTY(EditAnywhere, Category = "SeaShield") float TrailWidthOldCm = 300.0f;

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;
	virtual void Tick(float DeltaTime) override;

	UFUNCTION()
	void HandleEngagementEvent(const FSeaEngagementEvent& Event);

private:
	TSubclassOf<AActor> ClassFor(ESeaEntityKind Kind) const;
	UStaticMesh* MeshFor(ESeaEntityKind Kind) const;
	AActor* SpawnFor(const FSeaEntityState& Entity);

	// Draws every engagement material once as sub-pixel specks in front of
	// the camera, so first-use shader/PSO compiles happen during boot instead
	// of as a mid-salvo hitch (measured ~400 ms, client-design §8).
	void WarmupVfx();

	bool bLoggedFirstSpawn = false;
	bool bBurstCamFired = false;

	// The hand-placed "Frigate" stage actor (setup_level.py). Located at
	// BeginPlay and driven from the kOwnShip entity so the hull moves/turns with
	// the player's helm. Weak: if the stage has no frigate we degrade gracefully.
	TWeakObjectPtr<AActor> FrigateActor;
	bool bLoggedNoFrigate = false;
	// The ocean body, located at BeginPlay; its current Gerstner waves drive buoyancy.
	TWeakObjectPtr<UWaterBodyComponent> OceanComp;

	// Buoyancy + the wake both query the live Gerstner surface — one shared sampler
	// the buoyancy reconcile uses directly and the wake system points at.
	FSeaSurfaceSampler SurfaceSampler;

	// Per-effect VFX systems (SeaVfxSystems.h). Typed so reconcile / events can call
	// their specific spawn methods; created in BeginPlay after the ISMs/materials exist.
	TUniquePtr<FRocketTrailSystem> TrailSystem;
	TUniquePtr<FSplashSystem> SplashSystem;
	TUniquePtr<FExplosionSystem> ExplosionSystem;
	TUniquePtr<FWakeSystem> WakeSystem;
	TUniquePtr<FWreckageSystem> WreckageSystem;

	// Frame statistics for the 1440p60 budget table (logged at EndPlay). Under
	// vsync the avg pins to 16.67 ms ("exactly 60 fps"), so the optimize-or-not
	// signal is the over-budget ratios and the tail (p95/p99) — a coarse 1 ms
	// histogram (mirrors the server's SimServerStats histogram) yields those
	// cheaply. perf_summary.sh greps the "PERF:" line this produces.
	int64 FrameCount = 0;
	double FrameTimeSumMs = 0.0;
	double FrameTimeMaxMs = 0.0;
	int64 FramesOver16ms = 0;
	int64 FramesOver33ms = 0;
	static constexpr int32 kFrameHistogramBuckets = 256;  // 1 ms each; last = overflow.
	int32 FrameHistogramMs[kFrameHistogramBuckets] = {};
	// Last frame's Tick sub-phase costs (ms) — logged on a spike to attribute the
	// over-budget frame to a phase (reconcile vs trail rebuild vs splash/wreckage).
	double LastReconcileMs = 0.0;
	double LastRebuildTrailsMs = 0.0;
	double LastVfxUpdateMs = 0.0;

	// Key = (kind << 16) | id — the wire identity (rocket and track ids
	// share a number space).
	TMap<int32, TWeakObjectPtr<AActor>> Spawned;
	// Each particle family is now ONE InstancedStaticMeshComponent drawing a /Engine
	// /BasicShapes/Plane per particle (was an AStaticMeshActor each). The Age 0..1 the
	// material used to read from a per-actor scalar parameter now rides custom-data
	// slot 0 (PerInstanceCustomData[0]). INVARIANT per family: the sim-state array and
	// its ISM stay index-locked (Array[i] <-> instance i), so a swap-remove on the array
	// is mirrored by popping the LAST instance. The ISMs stay manager UPROPERTYs (GC);
	// the VFX systems hold raw non-owning pointers to them.
	UPROPERTY() TObjectPtr<UStaticMesh> ParticlePlane;
	UPROPERTY() TObjectPtr<UInstancedStaticMeshComponent> SmokeISM;   // puffs (M_RocketSmoke)
	UPROPERTY() TObjectPtr<UInstancedStaticMeshComponent> DebrisISM;  // sparks (M_Debris)
	UPROPERTY() TObjectPtr<UInstancedStaticMeshComponent> FlashISM;   // muzzle/burst (M_Muzzle)
	// Last seen stage position/velocity per entity — lets a kill spawn its burst +
	// wreckage at the target's last pose and inherit its motion EVEN THOUGH reconcile
	// may already have destroyed the actor (the kill event and the snapshot drop race
	// inside the same net tick). These are intentionally NOT evicted on despawn — the
	// cached pose must outlive the actor for the kill event; the wave reset (kind 7)
	// bounds them, so growth is per-wave, not unbounded.
	TMap<int32, FVector> LastEntityVelCms;
	TMap<int32, FVector> LastEntityStagePos;
};
