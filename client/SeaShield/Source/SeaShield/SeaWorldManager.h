#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "SeaNetSubsystem.h"

#include "SeaWorldManager.generated.h"

class UMaterialInterface;
class UMaterialInstanceDynamic;
class UProceduralMeshComponent;
class UWaterBodyComponent;

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
		// Puffs are emitted by DISTANCE travelled (not time) so a fast rocket can't
		// leave a dotted string of beads; this carries the sub-spacing remainder
		// between frames so the spacing stays exact across variable frame steps.
		float PuffDistAccumCm = 0.0f;
		bool bSeeded = false;  // first sample only fixes the origin; no puff line from (0,0,0).
		bool bRocketAlive = true;
	};
	// A lit billboard smoke puff — the real-exhaust particle stream that replaces the
	// flat ribbon as the BODY of the trail. Drifts with the sim wind, grows, fades.
	struct FPuff
	{
		TWeakObjectPtr<AActor> Sprite;
		TWeakObjectPtr<UMaterialInstanceDynamic> Mid;
		FVector SpawnPos = FVector::ZeroVector;
		FVector Jitter = FVector::ZeroVector;  // small per-puff lateral drift (cm/s)
		double SpawnTime = 0.0;
		float BaseHalfM = 0.0f;
		float RollDeg = 0.0f;
	};
	struct FSplash
	{
		TWeakObjectPtr<AActor> Column;
		double SpawnTime = 0.0;
		bool bAirburst = false;
		// Airbursts drive an "Age" scalar on a dynamic instance so the puff flashes
		// hot then decays to dark smoke (sea splashes leave this null).
		TWeakObjectPtr<UMaterialInstanceDynamic> Mid;
	};
	// A muzzle flash popped at a tube mouth when a rocket leaves the rail — a brief
	// bright emissive sphere expanded + snapped off in UpdateMuzzleFlashes.
	struct FMuzzleFlash
	{
		TWeakObjectPtr<AActor> Flash;
		TWeakObjectPtr<UMaterialInstanceDynamic> Mid;
		double SpawnTime = 0.0;
		float BaseScaleM = 7.0f;  // launch flash ~7 m; a detonation flash is bigger.
	};
	// Foam wake the own-ship churns: a flat ribbon rebuilt from the ship's recent
	// track, opacity per point = speed x age fade (carried in the vertex alpha).
	struct FWakePoint
	{
		FVector Position;  // Stage frame, pinned to the sea surface.
		double Time = 0.0;
		float SpeedFrac = 0.0f;  // 0..1 of full-speed wake at emission.
	};
	// Falling debris spawned on a kill — the dead missile's hull tumbling into
	// the sea, so a splash reads as a destroyed target, not just a puff.
	struct FWreckage
	{
		TWeakObjectPtr<AActor> Mesh;
		FVector VelCms = FVector::ZeroVector;   // cm/s, integrated under gravity
		FVector SpinDps = FVector::ZeroVector;  // deg/s tumble
		double SpawnTime = 0.0;
	};
	// A glowing fragment flung from an intercept detonation — a small additive spark
	// billboard on a ballistic arc (gravity), faded over its brief life.
	struct FDebris
	{
		TWeakObjectPtr<AActor> Sprite;
		TWeakObjectPtr<UMaterialInstanceDynamic> Mid;
		FVector VelCms = FVector::ZeroVector;
		double SpawnTime = 0.0;
		float SizeM = 0.6f;
	};

	void SampleTrail(int32 Key, const FVector& StagePosition, double Now);
	void RebuildTrails(double Now, const FVector& WindCms);
	// Billboard smoke particle stream (the real-exhaust body of the trail).
	void EmitPuff(const FVector& StagePosition, double Now);
	void UpdatePuffs(double Now, const FVector& WindCms);
	void SpawnSplash(const FVector& StagePosition, double Now, bool bAirburst);
	void UpdateSplashes(double Now);
	// Layered intercept detonation: flash + fireball core + radial spark debris +
	// lingering smoke. Composes the pieces below so a kill reads as a real airburst.
	void SpawnExplosion(const FVector& StagePosition, const FVector& InheritedVelCms, double Now);
	void SpawnDebris(const FVector& StagePosition, const FVector& InheritedVelCms, double Now);
	void UpdateDebris(double Now, float DeltaTime);
	// Shared hot flash billboard (T_Flash sprite); SpawnMuzzleFlash is launch-throttled,
	// SpawnExplosion calls a bigger one directly. Both ride UpdateMuzzleFlashes.
	void SpawnFlash(const FVector& StagePosition, double Now, float SizeM);
	void SpawnMuzzleFlash(const FVector& StagePosition, double Now);
	void UpdateMuzzleFlashes(double Now);
	// Own-ship foam wake: SampleWake records the ship's sea-level track; RebuildWake
	// emits a flat camera-independent foam ribbon along it.
	void SampleWake(const FVector& ShipStage, const FVector& VelCms, double Now);
	void RebuildWake(double Now);
	// Bow wave: a speed-gated foam V thrown off the bow as the hull cuts the water,
	// rebuilt each frame from the current ship pose (the astern wake is the trail).
	void RebuildBowWave(double Now);
	// Buoyancy: sample the live Gerstner ocean surface at a world XY -> wave height (cm)
	// + a damped tilt, so the visual hull rides the real waves (server owns XY/heading).
	void SampleSeaSurface(const FVector& WorldXY, float& OutHeightCm, FRotator& OutTilt) const;
	// World Z of the ocean surface at a world XY — pins the wake / bow-wave foam to the
	// live wave surface so it undulates with the swell instead of floating at mean sea Z.
	float SeaSurfaceWorldZ(const FVector& WorldXY) const;
	void SpawnWreckage(const FVector& StagePosition, const FVector& InheritedVelCms, double Now);
	void UpdateWreckage(double Now, float DeltaTime);
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
	TMap<int32, FRocketTrail> Trails;
	TArray<FPuff> Puffs;
	TArray<FSplash> Splashes;
	TArray<FMuzzleFlash> MuzzleFlashes;
	double LastMuzzleTime = 0.0;  // throttles a salvo's launch flashes to one pop.
	TArray<FWakePoint> WakePoints;
	TWeakObjectPtr<UProceduralMeshComponent> WakeRibbon;
	double LastWakeSampleTime = 0.0;
	// Current own-ship sea pose (refreshed every frame in SampleWake) — drives the bow
	// wave, which is GEOMETRY attached to the moving hull (not a track history).
	FVector LastShipSea = FVector::ZeroVector;
	FVector LastShipFwd = FVector::ForwardVector;
	float LastShipSpeedFrac = 0.0f;
	float ShipHalfLenCm = 6000.0f;  // hull forward half-length (queried from bounds at BeginPlay).
	TWeakObjectPtr<UProceduralMeshComponent> BowRibbon;
	TArray<FWreckage> Wreckage;
	TArray<FDebris> Debris;
	// Last seen stage position/velocity per entity — lets a kill spawn wreckage
	// that inherits the target's motion even though the entity is already gone
	// from the next snapshot.
	TMap<int32, FVector> LastEntityVelCms;
};
