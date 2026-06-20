#pragma once

#include "CoreMinimal.h"

// Per-effect VFX systems split out of ASeaWorldManager (the manager was a
// god-class running reconcile + trails + puffs + splashes + explosion + debris
// + muzzle + wake + bow-wave + buoyancy + wreckage + perf in one Tick). Each
// system below is a plain C++ class (NOT a UObject) that owns its own effect
// state and the methods that act on it, holding NON-OWNING raw pointers to the
// UPROPERTY components/materials it needs (the manager keeps GC ownership of the
// ISMs, ribbon meshes, FrigateActor and OceanComp).
//
// The manager builds an FSeaVfxContext ONCE per frame and runs each system's
// Tick(ctx); the reconcile/buoyancy/event-routing/dispatch/perf stays on the
// manager. Cross-system calls (SpawnExplosion -> SpawnSplash, the explosion /
// muzzle flash -> the trail puff emitter) are wired by passing system refs.
//
// Behavior is IDENTICAL to the pre-split manager: same spawn cadences, caps,
// transforms/age math, event routing, wave-reset clearing, and buoyancy — the
// code was MOVED, not rewritten.

class AActor;
class UStaticMesh;
class UMaterialInterface;
class UMaterialInstanceDynamic;
class UProceduralMeshComponent;
class UInstancedStaticMeshComponent;
class UWaterBodyComponent;

// Built ONCE per frame in ASeaWorldManager::Tick and passed to every system's
// Tick — this also hoists the camera / water-subsystem fetches that the
// per-function code used to repeat.
struct FSeaVfxContext
{
	double Now = 0.0;
	float Dt = 0.0f;
	FVector WindCms = FVector::ZeroVector;
	FVector CamLoc = FVector::ZeroVector;
	UWaterBodyComponent* Ocean = nullptr;
};

// Common interface so the manager can drive every effect uniformly each frame.
class ISeaVfxSystem
{
public:
	virtual ~ISeaVfxSystem() = default;
	virtual void Tick(const FSeaVfxContext& Ctx) = 0;
};

// Buoyancy and the wake both sample the live Gerstner ocean surface, so the two
// helpers live in one tiny shared sampler the manager owns and the wake system
// points at. It reads the live ocean component + the Water subsystem clock so
// the visual hull / foam ride the surface the renderer actually shows.
struct FSeaSurfaceSampler
{
	AActor* Owner = nullptr;                       // for GetWorld() (water subsystem time).
	TWeakObjectPtr<UWaterBodyComponent> Ocean;     // re-assigned on weather change; query each frame.

	// Sample the live Gerstner surface at a world XY -> wave height (cm) + a damped tilt.
	void SampleSeaSurface(const FVector& WorldXY, float& OutHeightCm, FRotator& OutTilt) const;
	// World Z of the ocean surface at a world XY — pins wake / bow-wave foam to the swell.
	float SeaSurfaceWorldZ(const FVector& WorldXY) const;
	// Footprint-averaged wave height (cm) over a disk of `RadiusCm` about WorldXY. A long hull
	// integrates the surface over its length, so sub-hull-length chop cancels and only swells
	// longer than the ship heave it — the physical, calmer bob (single-point over-corks).
	float SampleHullHeaveCm(const FVector& WorldXY, float RadiusCm) const;
};

// Rocket exhaust: camera-facing ribbons rebuilt per frame from each rocket's
// position history (every history point DRIFTS WITH THE SIM WIND), plus the
// billboard smoke puffs that carry the aged body of the trail. Owns Trails +
// Puffs. SampleTrail is called from the manager's reconcile; EmitPuff is also
// called by the explosion / muzzle systems.
class FRocketTrailSystem : public ISeaVfxSystem
{
public:
	FRocketTrailSystem(AActor* InOwner, UInstancedStaticMeshComponent* InSmokeISM,
	                   UMaterialInterface* InTrailMaterial, const float* InTrailLifetimeS,
	                   const float* InTrailWidthYoungCm, const float* InTrailWidthOldCm)
		: Owner(InOwner)
		, SmokeISM(InSmokeISM)
		, TrailMaterial(InTrailMaterial)
		, TrailLifetimeS(InTrailLifetimeS)
		, TrailWidthYoungCm(InTrailWidthYoungCm)
		, TrailWidthOldCm(InTrailWidthOldCm)
	{
	}

	virtual void Tick(const FSeaVfxContext& Ctx) override;

	void SampleTrail(int32 Key, const FVector& StagePosition, double Now);
	// Billboard smoke particle stream (the real-exhaust body of the trail).
	void EmitPuff(const FVector& StagePosition, double Now);

	// Reconcile sets this when a rocket leaves the sample (its smoke lingers + fades).
	void MarkRocketDead(int32 Key);
	// Synthetic -SeaTestBurst seeds a trail's last position before firing the event.
	void SeedRocketPosition(int32 Key, const FVector& StagePosition);
	// Detonation routing: the rocket's last known stage position (the burst/splash
	// anchor). Returns false if this key was never sampled (the event then no-ops,
	// matching the pre-split "Trails.Find(Key) == nullptr" guard).
	bool TryGetRocketPosition(int32 Key, FVector& OutStagePosition) const;
	// kind-7 wave reset: drop every ribbon + instance and clear the index-locked arrays.
	void Clear();

private:
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
		float PuffDistAccumCm = 0.0f;
		bool bSeeded = false;
		bool bRocketAlive = true;
	};
	struct FPuff
	{
		FVector SpawnPos = FVector::ZeroVector;
		FVector Jitter = FVector::ZeroVector;  // small per-puff lateral drift (cm/s)
		double SpawnTime = 0.0;
		float BaseHalfM = 0.0f;
		float RollDeg = 0.0f;
	};

	void RebuildTrails(double Now, const FVector& WindCms, const FVector& CamLoc);
	void UpdatePuffs(double Now, const FVector& WindCms, const FVector& CamLoc);

	AActor* Owner = nullptr;
	UInstancedStaticMeshComponent* SmokeISM = nullptr;
	UMaterialInterface* TrailMaterial = nullptr;  // non-owning -> manager UPROPERTY.
	const float* TrailLifetimeS = nullptr;        // -> manager UPROPERTY (editable in-editor).
	const float* TrailWidthYoungCm = nullptr;
	const float* TrailWidthOldCm = nullptr;

	TMap<int32, FRocketTrail> Trails;
	TArray<FPuff> Puffs;
};

// Water columns + airbursts: SpawnSplash raises a short-lived sphere/cylinder;
// UpdateSplashes drives its scale + the airburst "Age" scalar. The explosion
// system calls SpawnSplash for the fireball core.
class FSplashSystem : public ISeaVfxSystem
{
public:
	FSplashSystem(AActor* InOwner, UMaterialInterface* InSplashMaterial,
	              UMaterialInterface* InBurstMaterial)
		: Owner(InOwner), SplashMaterial(InSplashMaterial), BurstMaterial(InBurstMaterial)
	{
	}

	virtual void Tick(const FSeaVfxContext& Ctx) override;

	void SpawnSplash(const FVector& StagePosition, double Now, bool bAirburst);
	// kind-7 wave reset: destroy live columns + clear the array.
	void Clear();

private:
	struct FSplash
	{
		TWeakObjectPtr<AActor> Column;
		double SpawnTime = 0.0;
		bool bAirburst = false;
		TWeakObjectPtr<UMaterialInstanceDynamic> Mid;
	};

	void UpdateSplashes(double Now);

	AActor* Owner = nullptr;
	UMaterialInterface* SplashMaterial = nullptr;  // non-owning -> manager UPROPERTY.
	UMaterialInterface* BurstMaterial = nullptr;   // non-owning -> manager UPROPERTY.

	TArray<FSplash> Splashes;
};

// Layered intercept detonation: a hot flash + the fireball core (delegated to the
// splash system) + a radial spark spray (debris) + lingering smoke (delegated to
// the trail puff emitter). Also owns the launch muzzle flash (throttled to one
// pop per simultaneous salvo). Owns MuzzleFlashes + Debris.
class FExplosionSystem : public ISeaVfxSystem
{
public:
	FExplosionSystem(AActor* InOwner, UInstancedStaticMeshComponent* InDebrisISM,
	                 UInstancedStaticMeshComponent* InFlashISM, FSplashSystem* InSplash,
	                 FRocketTrailSystem* InTrail)
		: Owner(InOwner)
		, DebrisISM(InDebrisISM)
		, FlashISM(InFlashISM)
		, Splash(InSplash)
		, Trail(InTrail)
	{
	}

	virtual void Tick(const FSeaVfxContext& Ctx) override;

	void SpawnExplosion(const FVector& StagePosition, const FVector& InheritedVelCms, double Now);
	void SpawnMuzzleFlash(const FVector& StagePosition, double Now);
	// kind-7 wave reset: drop every instance + clear the index-locked arrays.
	void Clear();

private:
	struct FMuzzleFlash
	{
		FVector Pos = FVector::ZeroVector;  // billboard anchor (was the actor location).
		double SpawnTime = 0.0;
		float BaseScaleM = 7.0f;  // launch flash ~7 m; a detonation flash is bigger.
	};
	struct FDebris
	{
		FVector Pos = FVector::ZeroVector;  // integrated from velocity (was the actor location).
		FVector VelCms = FVector::ZeroVector;
		double SpawnTime = 0.0;
		float SizeM = 0.6f;
	};

	void SpawnDebris(const FVector& StagePosition, const FVector& InheritedVelCms, double Now);
	void UpdateDebris(double Now, float DeltaTime, const FVector& CamLoc);
	// Shared hot flash billboard (T_Flash sprite); SpawnMuzzleFlash is launch-throttled,
	// SpawnExplosion calls a bigger one directly. Both ride UpdateMuzzleFlashes.
	void SpawnFlash(const FVector& StagePosition, double Now, float SizeM);
	void UpdateMuzzleFlashes(double Now, const FVector& CamLoc);

	AActor* Owner = nullptr;
	UInstancedStaticMeshComponent* DebrisISM = nullptr;
	UInstancedStaticMeshComponent* FlashISM = nullptr;
	FSplashSystem* Splash = nullptr;       // SpawnExplosion calls SpawnSplash.
	FRocketTrailSystem* Trail = nullptr;   // SpawnExplosion / SpawnMuzzleFlash call EmitPuff.

	double LastMuzzleTime = 0.0;  // throttles a salvo's launch flashes to one pop.
	TArray<FMuzzleFlash> MuzzleFlashes;
	TArray<FDebris> Debris;
};

// Own-ship foam: the astern wake ribbon (SampleWake records the sea-level track,
// RebuildWake emits the flat foam band), plus the bow-wave V thrown off the bow,
// both pinned to the live wave surface via the shared FSeaSurfaceSampler. The
// manager calls SampleWake from reconcile with the ownship pose.
class FWakeSystem : public ISeaVfxSystem
{
public:
	FWakeSystem(AActor* InOwner, UMaterialInterface* InWakeMaterial,
	            UInstancedStaticMeshComponent* InSprayISM,
	            const FSeaSurfaceSampler* InSurface)
		: Owner(InOwner), WakeMaterial(InWakeMaterial), SprayISM(InSprayISM), Surface(InSurface)
	{
	}

	virtual void Tick(const FSeaVfxContext& Ctx) override;

	void SampleWake(const FVector& ShipStage, const FVector& VelCms, double Now);
	// Forward half-length of the hull — the bow wave originates at ship centre + fwd * this.
	void SetShipHalfLenCm(float InShipHalfLenCm) { ShipHalfLenCm = InShipHalfLenCm; }

private:
	struct FWakePoint
	{
		FVector Position;  // Stage frame, pinned to the sea surface.
		double Time = 0.0;
		float SpeedFrac = 0.0f;  // 0..1 of full-speed wake at emission.
	};
	struct FSpray
	{
		FVector SpawnPos = FVector::ZeroVector;
		FVector Vel = FVector::ZeroVector;
		double SpawnTime = 0.0;
		float BaseHalfM = 0.6f;
	};

	void RebuildWake(double Now);
	void RebuildBowWave(double Now);
	// Thin foam band hugging the hull waterline outline (an ellipse footprint from the
	// ship pose + ShipHalfLenCm) — the disturbed water a hull always shows, faint at
	// rest and stronger making way. Flat, surface-pinned, reuses WakeMaterial.
	void RebuildHullFoam(double Now);
	// Hull-waterline spray puffs: billboard ISM particles emitted where waves slap the hull.
	void EmitSpray(double Now);
	void UpdateSpray(double Now, const FVector& CamLoc);

	AActor* Owner = nullptr;
	UMaterialInterface* WakeMaterial = nullptr;          // non-owning -> manager UPROPERTY.
	UInstancedStaticMeshComponent* SprayISM = nullptr;  // non-owning -> manager UPROPERTY.
	const FSeaSurfaceSampler* Surface = nullptr;

	TArray<FWakePoint> WakePoints;
	TWeakObjectPtr<UProceduralMeshComponent> WakeRibbon;
	double LastWakeSampleTime = 0.0;
	// Current own-ship sea pose (refreshed every frame in SampleWake) — drives the bow wave.
	FVector LastShipSea = FVector::ZeroVector;
	FVector LastShipFwd = FVector::ForwardVector;
	float LastShipSpeedFrac = 0.0f;
	float ShipHalfLenCm = 6000.0f;  // hull forward half-length (queried from bounds at BeginPlay).
	TWeakObjectPtr<UProceduralMeshComponent> BowRibbon;
	TWeakObjectPtr<UProceduralMeshComponent> HullFoamRibbon;

	TArray<FSpray> Sprays;
};

// Falling debris on a kill — the dead hull tumbling into the sea under gravity,
// so a splash reads as a destroyed target, not just a puff. Owns Wreckage.
class FWreckageSystem : public ISeaVfxSystem
{
public:
	FWreckageSystem(AActor* InOwner, UStaticMesh* InTargetMesh)
		: Owner(InOwner), TargetMesh(InTargetMesh)
	{
	}

	virtual void Tick(const FSeaVfxContext& Ctx) override;

	void SpawnWreckage(const FVector& StagePosition, const FVector& InheritedVelCms, double Now);
	// kind-7 wave reset: destroy live hulls + clear the array.
	void Clear();

private:
	struct FWreckage
	{
		TWeakObjectPtr<AActor> Mesh;
		FVector VelCms = FVector::ZeroVector;   // cm/s, integrated under gravity
		FVector SpinDps = FVector::ZeroVector;  // deg/s tumble
		double SpawnTime = 0.0;
	};

	void UpdateWreckage(double Now, float DeltaTime);

	AActor* Owner = nullptr;
	UStaticMesh* TargetMesh = nullptr;  // non-owning -> manager UPROPERTY.

	TArray<FWreckage> Wreckage;
};
