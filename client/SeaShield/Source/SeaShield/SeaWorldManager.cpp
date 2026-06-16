#include "SeaWorldManager.h"

#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "Camera/CameraActor.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformMisc.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "HAL/PlatformTime.h"
#include "RenderTimer.h"
#include "TimerManager.h"
#include "UnrealClient.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "ProceduralMeshComponent.h"
#include "UObject/ConstructorHelpers.h"
#include "EngineUtils.h"  // TActorIterator
#include "WaterBodyActor.h"
#include "WaterBodyComponent.h"
#include "WaterWaves.h"
#include "WaterSubsystem.h"

#include "SeaWorldFrame.h"

DEFINE_LOG_CATEGORY_STATIC(LogSeaShieldWorld, Log, All);

namespace {
constexpr double kTrailSampleIntervalS = 0.035;  // ~29 Hz history -> a smooth, unfaceted column.
constexpr double kSplashLifetimeS = 1.6;
constexpr double kMuzzleLifetimeS = 0.18;        // a flash, not a fireball.
constexpr double kMuzzleThrottleS = 0.06;        // one pop per simultaneous salvo.
// Intercept detonation debris: glowing fragments flung radially, arcing under gravity.
constexpr int32 kDebrisPerBurst = 13;            // fragments per kill (sparse event).
constexpr int32 kDebrisMaxAlive = 180;           // global cap (multi-kill safety).
constexpr double kDebrisLifeS = 1.2;             // brief — sparks burn out fast.
constexpr float kDebrisSpeedMinCms = 2000.0f;    // radial ejection speed range.
constexpr float kDebrisSpeedMaxCms = 5500.0f;
constexpr float kDebrisGravityCms2 = 980.0f;     // arc back down like the wreckage.
constexpr double kWakeSampleIntervalS = 0.25;    // 4 Hz track, smooth enough flat.
constexpr double kWakeLifetimeS = 7.0;           // how long foam lingers astern.
constexpr double kWakeFullSpeedCms = 1200.0;     // speed at which the wake is full.
constexpr double kWakeSurfaceLiftCm = 60.0;      // ride just above the sea plane.
constexpr float kWakeHalfWidthYoungCm = 220.0f;  // ~ship beam at the stern.
constexpr float kWakeHalfWidthOldCm = 2600.0f;   // foam spreads astern.
// Bow wave: a foam V thrown off the bow, opening back at ~the Kelvin angle.
constexpr float kBowWaveAngleDeg = 22.0f;        // each wing's sweep back from the bow.
constexpr float kBowWaveLenMinCm = 2500.0f;      // wing length at low / full speed.
constexpr float kBowWaveLenMaxCm = 11000.0f;
constexpr float kBowWaveBandCm = 650.0f;         // foam band half-width across each wing.
constexpr int32 kBowWaveSegments = 10;           // strip resolution per wing.
// Buoyancy: the visual hull rides the live Gerstner surface (server owns XY/heading).
constexpr float kHullDraftCm = -40.0f;           // sit the waterline this far into the wave.
constexpr float kBuoyancyZSpeed = 4.0f;          // FInterpTo speed for the bob (anti-jitter).
constexpr float kBuoyancyTiltSpeed = 2.5f;       // FInterpTo speed for roll/pitch.
constexpr float kBuoyancyTiltDamp = 0.26f;       // a big hull rides the AVERAGE slope, not corks.
constexpr float kBuoyancyTiltMaxDeg = 5.0f;      // clamp so it stays a stately warship roll.
constexpr float kSeaDepthCm = 30000.0f;          // deep water for the wave query.
// Billboard smoke puffs ride the ribbon column as sparse 3D VOLUME. Emitted by
// DISTANCE travelled (not time) so a fast rocket can't leave a dotted bead string:
// every kPuffEmitDistanceCm of flight drops one puff, evenly, at any speed.
constexpr float kPuffEmitDistanceCm = 850.0f;    // one puff per ~8.5 m of flight.
constexpr int32 kPuffMaxPerCall = 2;             // bound the per-frame actor-spawn burst:
                                                 // a fast rocket covers many spacings in one
                                                 // frame, but spawning N actors per rocket per
                                                 // frame (inside reconcile) spikes the game
                                                 // thread. The wide ribbon covers the gap.
constexpr double kPuffLifeS = 6.5;               // long enough to disperse, then fade out.
constexpr int32 kPuffMaxAlive = 300;             // hard cap (translucent overdraw budget).
constexpr float kPuffYoungHalfM = 4.2f;           // half-size at emission (m) — bold fresh core.
constexpr float kPuffOldHalfM = 17.0f;           // balloons fat & diffuse as it dissipates.
// Per-puff TURBULENT DISPERSION: each puff gets its own random drift velocity so the
// column doesn't stay a rigid tube glued to the trajectory — it billows OUTWARD off
// the axis, breaks up, and thins as it ages (real smoke diffusing). Young puffs have
// barely drifted (Age small) so the fresh trail near the rocket stays coherent.
constexpr float kPuffDriftMinCms = 160.0f;
constexpr float kPuffDriftMaxCms = 360.0f;
}  // namespace

ASeaWorldManager::ASeaWorldManager()
{
	PrimaryActorTick.bCanEverTick = true;

	// Code-first defaults: the meshes Tools/import_assets.py lands in the
	// project and the materials Tools/setup_materials.py authors. The hostile
	// target is the procedural anti-ship missile.
	static ConstructorHelpers::FObjectFinder<UStaticMesh> MissileFinder(
		TEXT("/Game/SeaShield/Meshes/SM_Missile"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> RocketFinder(
		TEXT("/Game/SeaShield/Meshes/SM_Rocket"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> TrailFinder(
		TEXT("/Game/SeaShield/Materials/M_RocketTrail"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> SplashFinder(
		TEXT("/Game/SeaShield/Materials/M_Splash"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> BurstFinder(
		TEXT("/Game/SeaShield/Materials/M_Burst"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> MuzzleFinder(
		TEXT("/Game/SeaShield/Materials/M_Muzzle"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> WakeFinder(
		TEXT("/Game/SeaShield/Materials/M_Wake"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> SmokeFinder(
		TEXT("/Game/SeaShield/Materials/M_RocketSmoke"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> DebrisFinder(
		TEXT("/Game/SeaShield/Materials/M_Debris"));
	if (MissileFinder.Succeeded())
	{
		TargetMesh = MissileFinder.Object;
	}
	if (RocketFinder.Succeeded())
	{
		RocketMesh = RocketFinder.Object;
	}
	if (TrailFinder.Succeeded())
	{
		TrailMaterial = TrailFinder.Object;
	}
	if (SplashFinder.Succeeded())
	{
		SplashMaterial = SplashFinder.Object;
	}
	if (BurstFinder.Succeeded())
	{
		BurstMaterial = BurstFinder.Object;
	}
	if (MuzzleFinder.Succeeded())
	{
		MuzzleMaterial = MuzzleFinder.Object;
	}
	if (WakeFinder.Succeeded())
	{
		WakeMaterial = WakeFinder.Object;
	}
	if (SmokeFinder.Succeeded())
	{
		SmokeMaterial = SmokeFinder.Object;
	}
	if (DebrisFinder.Succeeded())
	{
		DebrisMaterial = DebrisFinder.Object;
	}
}

void ASeaWorldManager::BeginPlay()
{
	Super::BeginPlay();
	if (UGameInstance* GameInstance = GetGameInstance())
	{
		if (USeaNetSubsystem* Net = GameInstance->GetSubsystem<USeaNetSubsystem>())
		{
			Net->OnEngagementEvent.AddDynamic(this, &ASeaWorldManager::HandleEngagementEvent);
		}
	}
	// Locate the hand-placed frigate (setup_level.py) so the kOwnShip entity can
	// drive its pose. Match the SM_Frigate mesh — robust against actor renaming
	// and label stripping in packaged builds.
	{
		TArray<AActor*> MeshActors;
		UGameplayStatics::GetAllActorsOfClass(this, AStaticMeshActor::StaticClass(), MeshActors);
		for (AActor* Actor : MeshActors)
		{
			const AStaticMeshActor* SMA = Cast<AStaticMeshActor>(Actor);
			const UStaticMeshComponent* Comp = SMA ? SMA->GetStaticMeshComponent() : nullptr;
			const UStaticMesh* Mesh = Comp ? Comp->GetStaticMesh() : nullptr;
			if (Mesh != nullptr && Mesh->GetName().Contains(TEXT("Frigate")))
			{
				FrigateActor = Actor;
				break;
			}
		}
		if (!FrigateActor.IsValid())
		{
			UE_LOG(LogSeaShieldWorld, Warning,
			       TEXT("No SM_Frigate stage actor found; own-ship hull will not move (camera still follows)."));
		}
		else
		{
			// Forward half-length of the hull (its longest horizontal axis) — the bow
			// wave originates at the bow = ship centre + forward * this.
			FVector BoundsOrigin;
			FVector BoundsExtent;
			FrigateActor->GetActorBounds(/*bOnlyCollidingComponents=*/false, BoundsOrigin, BoundsExtent);
			ShipHalfLenCm =
			    FMath::Max(static_cast<float>(FMath::Max(BoundsExtent.X, BoundsExtent.Y)), 2000.0f);
		}
	}

	// Locate the ocean body so buoyancy can sample its live Gerstner surface. The
	// SeaEnvironmentController re-assigns the waves on weather changes, so we keep the
	// COMPONENT (and query GetWaterWaves() each frame) rather than caching the waves.
	for (TActorIterator<AWaterBody> It(GetWorld()); It; ++It)
	{
		if (UWaterBodyComponent* Comp = It->GetWaterBodyComponent())
		{
			OceanComp = Comp;
			break;
		}
	}

	// Pre-warm the engagement VFX pipeline once the player view exists.
	FTimerHandle WarmupTimer;
	GetWorld()->GetTimerManager().SetTimer(WarmupTimer, this, &ASeaWorldManager::WarmupVfx,
	                                       0.7f, false);
	// -SeaTestBurst: spawn a synthetic airburst + sea splash pair on a fixed
	// spot after a short delay — deterministic visual QA for the burst/splash
	// actors without depending on a live engagement's hit luck.
	if (FParse::Param(FCommandLine::Get(), TEXT("SeaTestBurst")))
	{
		FTimerHandle TestTimer;
		GetWorld()->GetTimerManager().SetTimer(
		    TestTimer,
		    [this]()
		    {
			    const FVector BurstSpot = SeaWorldFrame::Origin + FVector(60000.0, 25000.0, 50000.0);
			    FSeaEngagementEvent Synthetic;
			    Synthetic.bDetonated = true;
			    Trails.FindOrAdd((static_cast<int32>(ESeaEntityKind::Rocket) << 16) | 999)
			        .LastRocketPosition = BurstSpot;
			    Synthetic.SubjectId = 999;
			    HandleEngagementEvent(Synthetic);
			    SpawnSplash(SeaWorldFrame::Origin + FVector(64000.0, 21000.0, 0.0),
			                GetWorld()->GetTimeSeconds(), false);
		    },
		    8.0f, false);
	}
}

TSubclassOf<AActor> ASeaWorldManager::ClassFor(ESeaEntityKind Kind) const
{
	switch (Kind)
	{
	case ESeaEntityKind::Rocket: return RocketClass;
	case ESeaEntityKind::Track: return TrackClass;
	case ESeaEntityKind::Target: break;
	}
	return TargetClass;
}

UStaticMesh* ASeaWorldManager::MeshFor(ESeaEntityKind Kind) const
{
	switch (Kind)
	{
	case ESeaEntityKind::Rocket: return RocketMesh;
	case ESeaEntityKind::Track: return TrackMesh;
	case ESeaEntityKind::Target: break;
	}
	return TargetMesh;
}

AActor* ASeaWorldManager::SpawnFor(const FSeaEntityState& Entity)
{
	const FVector StagePosition = Entity.Position + SeaWorldFrame::Origin;
	if (const TSubclassOf<AActor> Class = ClassFor(Entity.Kind); Class != nullptr)
	{
		return GetWorld()->SpawnActor<AActor>(Class, StagePosition, FRotator::ZeroRotator);
	}
	UStaticMesh* Mesh = MeshFor(Entity.Kind);
	if (Mesh == nullptr)
	{
		return nullptr;  // Kind intentionally not visualized (e.g. tracks).
	}
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AStaticMeshActor* MeshActor = GetWorld()->SpawnActor<AStaticMeshActor>(
		StagePosition, FRotator::ZeroRotator, Params);
	if (MeshActor != nullptr)
	{
		MeshActor->SetMobility(EComponentMobility::Movable);
		MeshActor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
		// The generators model +Y as the nose (asset_lib.py render notes);
		// the actor convention below points +X along the velocity.
		MeshActor->GetStaticMeshComponent()->SetRelativeRotation(FRotator(0.0, -90.0, 0.0));
	}
	return MeshActor;
}

void ASeaWorldManager::EndPlay(const EEndPlayReason::Type Reason)
{
	if (FrameCount > 60)
	{
		// Conservative percentile: the upper edge (ms) of the 1 ms bucket that
		// holds the target rank — same "히스토그램 상계" convention as the server.
		const auto Percentile = [this](double Frac) -> int32
		{
			const int64 Target = FMath::Max<int64>(1, static_cast<int64>(FMath::CeilToDouble(Frac * FrameCount)));
			int64 Cumulative = 0;
			for (int32 Bucket = 0; Bucket < kFrameHistogramBuckets; ++Bucket)
			{
				Cumulative += FrameHistogramMs[Bucket];
				if (Cumulative >= Target)
				{
					return Bucket + 1;
				}
			}
			return kFrameHistogramBuckets;
		};
		const int32 P95 = Percentile(0.95);
		const int32 P99 = Percentile(0.99);
		const double AvgMs = FrameTimeSumMs / FrameCount;
		UE_LOG(LogSeaShieldWorld, Display,
		       TEXT("Frame stats: %lld frames avg=%.2f ms (%.1f fps) max=%.1f ms over-16.7ms=%lld"),
		       FrameCount, AvgMs, 1000.0 * FrameCount / FrameTimeSumMs, FrameTimeMaxMs, FramesOver16ms);
		// Machine-readable line for perf_summary.sh (stable "PERF:" prefix). The
		// budget verdict keys on over16.7%/p99, NOT avg (vsync pins avg @16.67).
		UE_LOG(LogSeaShieldWorld, Display,
		       TEXT("PERF: frames=%lld avg=%.2f ms p95=%d ms p99=%d ms max=%.1f ms fps_avg=%.1f "
		            "over16.7=%.1f%% over33.3=%.1f%%"),
		       FrameCount, AvgMs, P95, P99, FrameTimeMaxMs, 1000.0 * FrameCount / FrameTimeSumMs,
		       100.0 * FramesOver16ms / FrameCount, 100.0 * FramesOver33ms / FrameCount);
	}
	Super::EndPlay(Reason);
}

void ASeaWorldManager::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	// Steady-state only: the first seconds are boot hitches (shader compiles,
	// water info build, first spawns) that say nothing about the frame budget.
	if (GetWorld()->GetTimeSeconds() > 8.0)
	{
		const double FrameMs = DeltaTime * 1000.0;
		++FrameCount;
		FrameTimeSumMs += FrameMs;
		FrameTimeMaxMs = FMath::Max(FrameTimeMaxMs, FrameMs);
		++FrameHistogramMs[FMath::Clamp(static_cast<int32>(FrameMs), 0, kFrameHistogramBuckets - 1)];
		if (FrameMs > 16.7)
		{
			++FramesOver16ms;
		}
		if (FrameMs > 33.3)
		{
			++FramesOver33ms;
			// Spike forensics: attribute the over-budget frame to a thread.
			// game-thread high → game logic / actor churn (trail rebuild); render
			// high → render-proxy work; both low → GPU-bound. (G*ThreadTime are the
			// last completed frame's measured costs, RenderCore RenderTimer.h.)
			UE_LOG(LogSeaShieldWorld, Display,
			       TEXT("Spike: %.0f ms (game=%.1f render=%.1f | reconcile=%.1f trails=%.1f vfx=%.1f) at t=%.2f s"),
			       FrameMs, FPlatformTime::ToMilliseconds(GGameThreadTime),
			       FPlatformTime::ToMilliseconds(GRenderThreadTime), LastReconcileMs,
			       LastRebuildTrailsMs, LastVfxUpdateMs, GetWorld()->GetTimeSeconds());
		}
		if (FrameMs > 100.0)
		{
			// Hitch forensics: WHAT the worst frame is matters more than its
			// size (salvo? screenshot readback? water rebuild?).
			UE_LOG(LogSeaShieldWorld, Display, TEXT("Hitch: %.0f ms at t=%.2f s"), FrameMs,
			       GetWorld()->GetTimeSeconds());
		}
	}
	const UGameInstance* GameInstance = GetGameInstance();
	if (GameInstance == nullptr)
	{
		return;
	}
	USeaNetSubsystem* Net = GameInstance->GetSubsystem<USeaNetSubsystem>();
	if (Net == nullptr)
	{
		return;
	}

	const double TReconcile = FPlatformTime::Seconds();
	TArray<FSeaEntityState> Entities;
	Net->SampleEntities(Entities);
	const double Now = GetWorld()->GetTimeSeconds();

	TSet<int32> Alive;
	for (const FSeaEntityState& Entity : Entities)
	{
		const int32 Key = (static_cast<int32>(Entity.Kind) << 16) | Entity.Id;

		// Own ship: drive the hand-placed frigate hull rather than spawning a
		// generic actor. Yaw follows the course; the camera (gunner pawn) rides
		// the same pose independently.
		if (Entity.Kind == ESeaEntityKind::OwnShip)
		{
			const FVector ShipStage = Entity.Position + SeaWorldFrame::Origin;
			if (FrigateActor.IsValid())
			{
				// Buoyancy: the server owns XY + heading; the VISUAL hull rides the live
				// Gerstner surface — Z bob from the wave height, gentle roll/pitch from the
				// surface tilt, both low-pass smoothed so the hull never jitters or corks.
				float WaveHeightCm = 0.0f;
				FRotator WaveTilt = FRotator::ZeroRotator;
				SampleSeaSurface(ShipStage, WaveHeightCm, WaveTilt);

				FRotator Pose = FrigateActor->GetActorRotation();
				if (!Entity.Velocity.IsNearlyZero(50.0))  // >0.5 m/s of way on
				{
					Pose.Yaw = Entity.Velocity.Rotation().Yaw;
				}
				const float TargetZ =
				    static_cast<float>(SeaWorldFrame::Origin.Z) + WaveHeightCm + kHullDraftCm;
				const float SmoothZ =
				    FMath::FInterpTo(static_cast<float>(FrigateActor->GetActorLocation().Z), TargetZ,
				                     DeltaTime, kBuoyancyZSpeed);
				Pose.Pitch = FMath::FInterpTo(Pose.Pitch, WaveTilt.Pitch, DeltaTime, kBuoyancyTiltSpeed);
				Pose.Roll = FMath::FInterpTo(Pose.Roll, WaveTilt.Roll, DeltaTime, kBuoyancyTiltSpeed);
				FrigateActor->SetActorLocationAndRotation(FVector(ShipStage.X, ShipStage.Y, SmoothZ),
				                                          Pose);
			}
			// Lay foam astern as the hull makes way (independent of the frigate mesh).
			SampleWake(ShipStage, Entity.Velocity, Now);
			continue;
		}

		Alive.Add(Key);

		TWeakObjectPtr<AActor>& Slot = Spawned.FindOrAdd(Key);
		if (!Slot.IsValid())
		{
			Slot = SpawnFor(Entity);
			if (!Slot.IsValid())
			{
				continue;
			}
			if (!bLoggedFirstSpawn)
			{
				bLoggedFirstSpawn = true;
				UE_LOG(LogSeaShieldWorld, Display,
				       TEXT("First entity actor spawned: kind=%d id=%d at (%.0f, %.0f, %.0f) cm"),
				       static_cast<int32>(Entity.Kind), Entity.Id, Entity.Position.X,
				       Entity.Position.Y, Entity.Position.Z);
			}
			// A rocket appearing in the sample = a tube just fired: pop a launch
			// flash at its emergence point (throttled so a salvo reads as one pop).
			if (Entity.Kind == ESeaEntityKind::Rocket)
			{
				SpawnMuzzleFlash(Entity.Position + SeaWorldFrame::Origin, Now);
			}
		}
		// Face the velocity when it is meaningful; tracks keep identity
		// rotation (their symbology lives on the PPI, not in 3D pose).
		FRotator Rotation = Slot->GetActorRotation();
		if (Entity.Kind != ESeaEntityKind::Track && !Entity.Velocity.IsNearlyZero(1.0))
		{
			Rotation = Entity.Velocity.Rotation();
		}
		const FVector StagePosition = Entity.Position + SeaWorldFrame::Origin;
		Slot->SetActorLocationAndRotation(StagePosition, Rotation);

		if (Entity.Kind == ESeaEntityKind::Rocket)
		{
			SampleTrail(Key, StagePosition, Now);
		}
		else if (Entity.Kind == ESeaEntityKind::Target)
		{
			LastEntityVelCms.Add(Key, Entity.Velocity);  // for kill-wreckage inheritance
		}
	}

	// Entities gone from the sample (resolved rockets, dropped tracks,
	// destroyed target) leave the stage; their smoke lingers and fades.
	for (auto It = Spawned.CreateIterator(); It; ++It)
	{
		if (!Alive.Contains(It.Key()))
		{
			if (FRocketTrail* Trail = Trails.Find(It.Key()))
			{
				Trail->bRocketAlive = false;
			}
			if (It.Value().IsValid())
			{
				It.Value()->Destroy();
			}
			It.RemoveCurrent();
		}
	}

	const double TTrails = FPlatformTime::Seconds();
	LastReconcileMs = (TTrails - TReconcile) * 1000.0;
	RebuildTrails(Now, Net->GetWeather().WindCms);
	UpdatePuffs(Now, Net->GetWeather().WindCms);
	const double TVfx = FPlatformTime::Seconds();
	LastRebuildTrailsMs = (TVfx - TTrails) * 1000.0;
	UpdateSplashes(Now);
	UpdateMuzzleFlashes(Now);
	RebuildWake(Now);
	RebuildBowWave(Now);
	UpdateWreckage(Now, DeltaTime);
	UpdateDebris(Now, DeltaTime);
	LastVfxUpdateMs = (FPlatformTime::Seconds() - TVfx) * 1000.0;
}

void ASeaWorldManager::HandleEngagementEvent(const FSeaEngagementEvent& Event)
{
	UE_LOG(LogSeaShieldWorld, Display,
	       TEXT("Engagement event: kind=%d subject=%d detonated=%d killed=%d miss=%.1f"),
	       Event.Kind, Event.SubjectId, Event.bDetonated ? 1 : 0, Event.bKilled ? 1 : 0,
	       Event.MissDistanceM);

	// Survival game (protocol::EventKind): a new wave (7) wipes the previous
	// wave's actors/smoke so the reused entity ids never blend across waves; a
	// ship hit (6) bursts over the deck for feedback.
	if (Event.Kind == 7)
	{
		for (auto& Pair : Spawned)
		{
			if (Pair.Value.IsValid())
			{
				Pair.Value->Destroy();
			}
		}
		Spawned.Reset();
		for (auto& Pair : Trails)
		{
			if (Pair.Value.Ribbon.IsValid())
			{
				Pair.Value.Ribbon->DestroyComponent();
			}
		}
		Trails.Reset();
		for (FSplash& Sp : Splashes)
		{
			if (Sp.Column.IsValid())
			{
				Sp.Column->Destroy();
			}
		}
		Splashes.Reset();
		for (FWreckage& W : Wreckage)
		{
			if (W.Mesh.IsValid())
			{
				W.Mesh->Destroy();
			}
		}
		Wreckage.Reset();
		for (FDebris& D : Debris)
		{
			if (D.Sprite.IsValid())
			{
				D.Sprite->Destroy();
			}
		}
		Debris.Reset();
		LastEntityVelCms.Reset();
		bLoggedFirstSpawn = false;
		return;
	}
	if (Event.Kind == 6)
	{
		SpawnSplash(SeaWorldFrame::Origin + FVector(0.0, 0.0, 1200.0),
		            GetWorld()->GetTimeSeconds(), /*bAirburst=*/true);
		return;
	}
	if (Event.Kind == 2)  // kTargetDestroyed — a confirmed kill: burst + wreckage.
	{
		const int32 Key = (static_cast<int32>(ESeaEntityKind::Target) << 16) | Event.SubjectId;
		const TWeakObjectPtr<AActor>* Actor = Spawned.Find(Key);
		if (Actor != nullptr && Actor->IsValid())
		{
			const FVector Where = Actor->Get()->GetActorLocation();
			const double Now = GetWorld()->GetTimeSeconds();
			const FVector* Vel = LastEntityVelCms.Find(Key);
			const FVector VelCms = Vel != nullptr ? *Vel : FVector::ZeroVector;
			SpawnExplosion(Where, VelCms, Now);  // flash + fireball + debris + smoke
			SpawnWreckage(Where, VelCms, Now);   // the dead hull tumbling into the sea
		}
		return;
	}
	// Proximity bursts happen at altitude (airburst puff); rockets that never
	// fuzed come down into the sea (water column at the surface).
	const int32 Key = (static_cast<int32>(ESeaEntityKind::Rocket) << 16) | Event.SubjectId;
	if (const FRocketTrail* Trail = Trails.Find(Key))
	{
		FVector Where = Trail->LastRocketPosition;
		if (!Event.bDetonated)
		{
			Where.Z = 0.0;
		}
		// A fuzed proximity detonation is an airburst explosion; an un-fuzed rocket
		// that fell into the sea throws a water column instead.
		if (Event.bDetonated)
		{
			SpawnExplosion(Where, FVector::ZeroVector, GetWorld()->GetTimeSeconds());
		}
		else
		{
			SpawnSplash(Where, GetWorld()->GetTimeSeconds(), /*bAirburst=*/false);
		}

		// Dev capture: -SeaShotOnBurst frames the first airburst from a
		// chase offset and screenshots it — run-to-run timing differences
		// (server pre-roll vs client boot) make timer-based framing useless.
		const bool bWantBurst = FParse::Param(FCommandLine::Get(), TEXT("SeaShotOnBurst"));
		const bool bWantSplash = FParse::Param(FCommandLine::Get(), TEXT("SeaShotOnSplash"));
		if (!bBurstCamFired &&
		    ((Event.bDetonated && bWantBurst) || (!Event.bDetonated && bWantSplash)))
		{
			bBurstCamFired = true;
			const FVector Ship = SeaWorldFrame::Origin;
			const FVector Along = (Where - Ship).GetSafeNormal();
			// Stand abeam of the flight path, not in it — the trail ribbons
			// converge on the burst from the ship side.
			const FVector Side = FVector::CrossProduct(Along, FVector::UpVector).GetSafeNormal();
			// Frame from ABOVE and abeam, looking DOWN at the burst so the background is
			// the (darker) sea, not the backlit sun/sky that washes the explosion out.
			const FVector CameraSpot = Where + Side * 12000.0 - Along * 3000.0 + FVector(0, 0, 9000.0);
			ACameraActor* Camera = GetWorld()->SpawnActor<ACameraActor>(
			    CameraSpot, (Where - CameraSpot).Rotation());
			if (APlayerController* Controller = GetWorld()->GetFirstPlayerController())
			{
				Controller->SetViewTarget(Camera);
			}
			// Filmstrip the detonation: flash (0.1) -> fireball (0.3) -> debris spread
			// (0.6) -> smoke aftermath (1.1). One late shot misses the punchy phases.
			static const float kShotTimes[] = {0.10f, 0.30f, 0.60f, 1.10f};
			for (int32 ShotIdx = 0; ShotIdx < 4; ++ShotIdx)
			{
				FTimerHandle ShotTimer;
				const int32 Idx = ShotIdx;
				GetWorld()->GetTimerManager().SetTimer(
				    ShotTimer,
				    [Idx]() {
					    FScreenshotRequest::RequestScreenshot(
					        FString::Printf(TEXT("SeaBurst%d.png"), Idx), /*bShowUI=*/true, false);
				    },
				    kShotTimes[ShotIdx], false);
			}
			FTimerHandle QuitTimer;
			GetWorld()->GetTimerManager().SetTimer(
			    QuitTimer, []() { FPlatformMisc::RequestExit(false); }, 2.4f, false);
		}
	}
}

void ASeaWorldManager::WarmupVfx()
{
	APlayerCameraManager* Camera = UGameplayStatics::GetPlayerCameraManager(GetWorld(), 0);
	if (Camera == nullptr)
	{
		return;
	}
	// The specks must actually be IN the frustum: Metal builds the pipeline
	// state at first draw, so an occluded/culled warm-up warms nothing.
	const FVector Base = Camera->GetCameraLocation() + Camera->GetCameraRotation().Vector() * 500.0;

	TArray<TWeakObjectPtr<AActor>> Specks;
	auto SpawnSpeck = [&](const TCHAR* MeshPath, UMaterialInterface* Material, int32 Index)
	{
		UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, MeshPath);
		if (Mesh == nullptr)
		{
			return;
		}
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AStaticMeshActor* Actor = GetWorld()->SpawnActor<AStaticMeshActor>(
		    Base + FVector(0.0, Index * 8.0, 0.0), FRotator::ZeroRotator, Params);
		if (Actor == nullptr)
		{
			return;
		}
		Actor->SetMobility(EComponentMobility::Movable);
		Actor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
		if (Material != nullptr)
		{
			Actor->GetStaticMeshComponent()->SetMaterial(0, Material);
		}
		Actor->GetStaticMeshComponent()->SetCastShadow(false);
		Actor->SetActorScale3D(FVector(0.002));
		Specks.Add(Actor);
	};
	SpawnSpeck(TEXT("/Engine/BasicShapes/Cylinder"), SplashMaterial, 0);
	SpawnSpeck(TEXT("/Engine/BasicShapes/Sphere"), BurstMaterial, 1);
	SpawnSpeck(TEXT("/Game/SeaShield/Meshes/SM_Rocket"), nullptr, 2);
	SpawnSpeck(TEXT("/Engine/BasicShapes/Plane"), MuzzleMaterial, 3);  // flash billboard
	SpawnSpeck(TEXT("/Engine/BasicShapes/Plane"), SmokeMaterial, 4);   // smoke puff billboard
	SpawnSpeck(TEXT("/Engine/BasicShapes/Plane"), DebrisMaterial, 5);  // debris spark billboard

	// The vertex-color translucent procedural-mesh path: trail and wake share the
	// vertex factory but compile distinct material PSOs — warm each as a 1 cm quad.
	TArray<TWeakObjectPtr<UProceduralMeshComponent>> Quads;
	auto SpawnQuad = [&](UMaterialInterface* Material, int32 Index)
	{
		if (Material == nullptr)
		{
			return;
		}
		UProceduralMeshComponent* Quad = NewObject<UProceduralMeshComponent>(this);
		Quad->RegisterComponent();
		Quad->AttachToComponent(GetRootComponent(), FAttachmentTransformRules::KeepWorldTransform);
		Quad->SetWorldTransform(FTransform::Identity);
		Quad->SetCastShadow(false);
		Quad->SetMaterial(0, Material);
		const FVector Q = Base + FVector(0.0, 0.0, Index * 2.0);
		const TArray<FVector> Verts = {Q, Q + FVector(1, 0, 0), Q + FVector(0, 0, 1),
		                               Q + FVector(1, 0, 1)};
		const TArray<FLinearColor> Cols = {FLinearColor(1, 1, 1, 0.5f), FLinearColor(1, 1, 1, 0.5f),
		                                   FLinearColor(1, 1, 1, 0.5f), FLinearColor(1, 1, 1, 0.5f)};
		const TArray<FVector2D> Uv = {FVector2D(0, 0), FVector2D(1, 0), FVector2D(0, 1),
		                              FVector2D(1, 1)};
		Quad->CreateMeshSection_LinearColor(0, Verts, {0, 2, 1, 1, 2, 3}, {}, Uv, Cols, {}, false);
		Quads.Add(Quad);
	};
	SpawnQuad(TrailMaterial, 0);
	SpawnQuad(WakeMaterial, 1);

	FTimerHandle CleanupTimer;
	GetWorld()->GetTimerManager().SetTimer(
	    CleanupTimer,
	    [Specks, Quads]()
	    {
		    for (const TWeakObjectPtr<AActor>& Speck : Specks)
		    {
			    if (Speck.IsValid())
			    {
				    Speck->Destroy();
			    }
		    }
		    for (const TWeakObjectPtr<UProceduralMeshComponent>& Q : Quads)
		    {
			    if (Q.IsValid())
			    {
				    Q->DestroyComponent();
			    }
		    }
	    },
	    2.5f, false);
}

void ASeaWorldManager::SampleTrail(int32 Key, const FVector& StagePosition, double Now)
{
	FRocketTrail& Trail = Trails.FindOrAdd(Key);
	const FVector PrevPos = Trail.LastRocketPosition;
	const bool bWasSeeded = Trail.bSeeded;
	Trail.LastRocketPosition = StagePosition;
	Trail.bSeeded = true;
	Trail.bRocketAlive = true;
	// Drop puffs by DISTANCE along the segment flown this frame (interpolated), so
	// the column is continuous at any speed instead of a dotted string of beads.
	// (Skip the very first sample: PrevPos isn't a real prior position yet, so a
	// segment from it would streak puffs in from the stage origin.)
	const FVector Seg = StagePosition - PrevPos;
	const float SegLen = static_cast<float>(Seg.Size());
	if (bWasSeeded && SegLen > KINDA_SMALL_NUMBER)
	{
		const FVector Dir = Seg / SegLen;
		float D = Trail.PuffDistAccumCm;
		int32 Emitted = 0;
		while (D <= SegLen && Emitted < kPuffMaxPerCall)
		{
			EmitPuff(PrevPos + Dir * D, Now);
			D += kPuffEmitDistanceCm;
			++Emitted;
		}
		// If the rocket out-ran the per-call cap this frame, resync the accumulator to
		// the segment end (don't bank a huge debt that would burst next frame).
		Trail.PuffDistAccumCm = (D <= SegLen) ? kPuffEmitDistanceCm : (D - SegLen);
	}
	if (Now - Trail.LastSampleTime < kTrailSampleIntervalS)
	{
		return;
	}
	Trail.LastSampleTime = Now;
	Trail.Points.Add({StagePosition, Now});
}

void ASeaWorldManager::RebuildTrails(double Now, const FVector& WindCms)
{
	for (auto It = Trails.CreateIterator(); It; ++It)
	{
		FRocketTrail& Trail = It.Value();

		// Age out spent smoke; drop the whole column once the rocket is gone
		// and the youngest point has faded.
		while (!Trail.Points.IsEmpty() && Now - Trail.Points[0].Time > TrailLifetimeS)
		{
			Trail.Points.RemoveAt(0);
		}
		if (Trail.Points.Num() < 2)
		{
			if (!Trail.bRocketAlive)
			{
				if (Trail.Ribbon.IsValid())
				{
					Trail.Ribbon->DestroyComponent();
				}
				It.RemoveCurrent();
			}
			continue;
		}

		if (!Trail.Ribbon.IsValid())
		{
			UProceduralMeshComponent* Ribbon = NewObject<UProceduralMeshComponent>(this);
			Ribbon->RegisterComponent();
			Ribbon->AttachToComponent(GetRootComponent(),
			                          FAttachmentTransformRules::KeepWorldTransform);
			Ribbon->SetWorldTransform(FTransform::Identity);
			Ribbon->SetCastShadow(false);
			if (TrailMaterial != nullptr)
			{
				Ribbon->SetMaterial(0, TrailMaterial);
			}
			Trail.Ribbon = Ribbon;
		}

		const FVector CameraLocation =
		    UGameplayStatics::GetPlayerCameraManager(GetWorld(), 0) != nullptr
		        ? UGameplayStatics::GetPlayerCameraManager(GetWorld(), 0)->GetCameraLocation()
		        : FVector::ZeroVector;

		const int32 Count = Trail.Points.Num();
		TArray<FVector> Vertices;
		TArray<int32> Triangles;
		TArray<FLinearColor> Colors;
		TArray<FVector2D> UVs;
		Vertices.Reserve(Count * 2);
		Colors.Reserve(Count * 2);
		UVs.Reserve(Count * 2);
		Triangles.Reserve((Count - 1) * 6);

		for (int32 i = 0; i < Count; ++i)
		{
			const FTrailPoint& Point = Trail.Points[i];
			const double Age = Now - Point.Time;
			// The launch transient leaves the column; from then on each
			// sample rides the simulation wind — the bend IS the weather.
			const FVector Drifted = Point.Position + WindCms * Age;

			const FVector Along =
			    (Trail.Points[FMath::Min(i + 1, Count - 1)].Position -
			     Trail.Points[FMath::Max(i - 1, 0)].Position)
			        .GetSafeNormal();
			const FVector ToCamera = (CameraLocation - Drifted).GetSafeNormal();
			FVector Side = FVector::CrossProduct(Along, ToCamera);
			if (!Side.Normalize())
			{
				Side = FVector::RightVector;
			}

			const float AgeAlpha = FMath::Clamp(Age / TrailLifetimeS, 0.0, 1.0);
			// Billow fast then settle: sqrt grows the column most in its first second
			// (where exhaust expands hardest) so it reads as a fat plume, not a wedge.
			const float WidthAlpha = FMath::Sqrt(AgeAlpha);
			const float HalfWidth =
			    0.5f * FMath::Lerp(TrailWidthYoungCm, TrailWidthOldCm, WidthAlpha);
			const float Opacity = FMath::Square(1.0f - AgeAlpha);

			Vertices.Add(Drifted - Side * HalfWidth);
			Vertices.Add(Drifted + Side * HalfWidth);
			Colors.Add(FLinearColor(1.0f, 1.0f, 1.0f, Opacity));
			Colors.Add(FLinearColor(1.0f, 1.0f, 1.0f, Opacity));
			const float V = static_cast<float>(i) / (Count - 1);
			UVs.Add(FVector2D(0.0, V));
			UVs.Add(FVector2D(1.0, V));

			if (i > 0)
			{
				const int32 Base = (i - 1) * 2;
				Triangles.Append({Base, Base + 2, Base + 1, Base + 1, Base + 2, Base + 3});
			}
		}

		Trail.Ribbon->CreateMeshSection_LinearColor(
		    0, Vertices, Triangles, /*Normals=*/{}, UVs, Colors, /*Tangents=*/{},
		    /*bCreateCollision=*/false);
	}
}

void ASeaWorldManager::SpawnSplash(const FVector& StagePosition, double Now, bool bAirburst)
{
	UStaticMesh* Shape = LoadObject<UStaticMesh>(
	    nullptr, bAirburst ? TEXT("/Engine/BasicShapes/Sphere") : TEXT("/Engine/BasicShapes/Cylinder"));
	if (Shape == nullptr)
	{
		return;
	}
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AStaticMeshActor* Column = GetWorld()->SpawnActor<AStaticMeshActor>(
	    StagePosition, FRotator::ZeroRotator, Params);
	if (Column == nullptr)
	{
		return;
	}
	Column->SetMobility(EComponentMobility::Movable);
	Column->GetStaticMeshComponent()->SetStaticMesh(Shape);
	Column->GetStaticMeshComponent()->SetCastShadow(false);
	UMaterialInterface* Material = bAirburst ? BurstMaterial : SplashMaterial;
	TWeakObjectPtr<UMaterialInstanceDynamic> Mid;
	if (Material != nullptr)
	{
		// The airburst drives an "Age" scalar (flash -> dark smoke) via a dynamic
		// instance; the sea splash uses the static material directly.
		if (bAirburst)
		{
			UMaterialInstanceDynamic* Dyn =
			    Column->GetStaticMeshComponent()->CreateDynamicMaterialInstance(0, Material);
			if (Dyn != nullptr)
			{
				Dyn->SetScalarParameterValue(TEXT("Age"), 0.0f);
				Mid = Dyn;
			}
		}
		else
		{
			Column->GetStaticMeshComponent()->SetMaterial(0, Material);
		}
	}
	Column->SetActorScale3D(FVector(2.0, 2.0, 0.5));
	Splashes.Add({Column, Now, bAirburst, Mid});
	UE_LOG(LogSeaShieldWorld, Display, TEXT("%hs at (%.0f, %.0f, %.0f) cm t=%.1f"),
	       bAirburst ? "Airburst" : "Sea splash", StagePosition.X, StagePosition.Y,
	       StagePosition.Z, Now);
}

void ASeaWorldManager::UpdateSplashes(double Now)
{
	for (auto It = Splashes.CreateIterator(); It; ++It)
	{
		const double Age = Now - It->SpawnTime;
		if (!It->Column.IsValid() || Age > kSplashLifetimeS)
		{
			if (It->Column.IsValid())
			{
				It->Column->Destroy();
			}
			It.RemoveCurrent();
			continue;
		}
		// Fast rise, slow lateral spread, sink-back at the end — enough to
		// read as a shell splash at battle ranges without a particle system.
		const double T = Age / kSplashLifetimeS;
		if (It->bAirburst)
		{
			// Sharp flash-out: the puff swells and thins away; the Age scalar
			// fades the hot core to dark smoke over the same window.
			const double Diameter = 18.0 + 28.0 * T;  // m
			It->Column->SetActorScale3D(FVector(Diameter));
			if (It->Mid.IsValid())
			{
				It->Mid->SetScalarParameterValue(TEXT("Age"), static_cast<float>(T));
			}
		}
		else
		{
			const double Height = 14.0 * FMath::Sin(FMath::Min(T * 1.35, 1.0) * PI);  // m
			const double Radius = 2.0 + 5.0 * T;                                      // m
			It->Column->SetActorScale3D(
			    FVector(Radius, Radius, FMath::Max(Height, 0.05)));  // BasicShapes are 1 m.
			// The cylinder pivot is centered: lift it so the column grows
			// upward out of the water instead of straddling it.
			FVector Where = It->Column->GetActorLocation();
			Where.Z = FMath::Max(Height, 0.05) * 100.0 / 2.0;
			It->Column->SetActorLocation(Where);
		}
	}
}

void ASeaWorldManager::SpawnExplosion(const FVector& StagePosition, const FVector& InheritedVelCms,
                                      double Now)
{
	// Layered airburst: a hot flash punch, the expanding fireball -> smoke sphere, a
	// radial spray of glowing fragments, and a few smoke puffs that linger and drift.
	SpawnFlash(StagePosition, Now, 16.0f);                // bright detonation flash (blooms hard)
	SpawnSplash(StagePosition, Now, /*bAirburst=*/true);  // fireball core -> dark powder smoke
	SpawnDebris(StagePosition, InheritedVelCms, Now);
	for (int32 i = 0; i < 5; ++i)  // smoke aftermath: puffs that drift + disperse
	{
		EmitPuff(StagePosition + FMath::VRand() * FMath::FRandRange(100.0f, 700.0f), Now);
	}
}

void ASeaWorldManager::SpawnDebris(const FVector& StagePosition, const FVector& InheritedVelCms,
                                   double Now)
{
	if (DebrisMaterial == nullptr)
	{
		return;
	}
	UStaticMesh* Plane = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane"));
	if (Plane == nullptr)
	{
		return;
	}
	for (int32 i = 0; i < kDebrisPerBurst; ++i)
	{
		if (Debris.Num() >= kDebrisMaxAlive)  // global cap: recycle the oldest (multi-kill).
		{
			if (Debris[0].Sprite.IsValid())
			{
				Debris[0].Sprite->Destroy();
			}
			Debris.RemoveAt(0);
		}
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AStaticMeshActor* Spark =
		    GetWorld()->SpawnActor<AStaticMeshActor>(StagePosition, FRotator::ZeroRotator, Params);
		if (Spark == nullptr)
		{
			continue;
		}
		Spark->SetMobility(EComponentMobility::Movable);
		Spark->GetStaticMeshComponent()->SetStaticMesh(Plane);
		Spark->GetStaticMeshComponent()->SetCastShadow(false);
		UMaterialInstanceDynamic* Mid =
		    Spark->GetStaticMeshComponent()->CreateDynamicMaterialInstance(0, DebrisMaterial);
		if (Mid != nullptr)
		{
			Mid->SetScalarParameterValue(TEXT("Age"), 0.0f);
		}
		FDebris D;
		D.Sprite = Spark;
		D.Mid = Mid;
		// Radial ejection biased upward + a fraction of the target's inbound momentum.
		FVector Dir = FMath::VRand();
		Dir.Z = FMath::Abs(Dir.Z) * 0.6f + 0.25f;
		Dir.Normalize();
		D.VelCms = Dir * FMath::FRandRange(kDebrisSpeedMinCms, kDebrisSpeedMaxCms) +
		           InheritedVelCms * 0.25f;
		D.SpawnTime = Now;
		D.SizeM = FMath::FRandRange(0.9f, 2.2f);
		Debris.Add(D);
	}
}

void ASeaWorldManager::UpdateDebris(double Now, float DeltaTime)
{
	APlayerCameraManager* Cam = UGameplayStatics::GetPlayerCameraManager(GetWorld(), 0);
	const FVector CamLoc = Cam != nullptr ? Cam->GetCameraLocation() : FVector::ZeroVector;
	for (auto It = Debris.CreateIterator(); It; ++It)
	{
		const double Age = Now - It->SpawnTime;
		const float T = FMath::Clamp(static_cast<float>(Age / kDebrisLifeS), 0.0f, 1.0f);
		if (!It->Sprite.IsValid() || T >= 1.0f)
		{
			if (It->Sprite.IsValid())
			{
				It->Sprite->Destroy();
			}
			It.RemoveCurrent();
			continue;
		}
		It->VelCms.Z -= kDebrisGravityCms2 * DeltaTime;  // ballistic arc
		const FVector Pos = It->Sprite->GetActorLocation() + It->VelCms * DeltaTime;
		FVector ToCam = CamLoc - Pos;
		if (!ToCam.Normalize())
		{
			ToCam = FVector::UpVector;
		}
		const float S = It->SizeM * (1.0f - 0.5f * T);  // burn down as it cools
		It->Sprite->SetActorLocationAndRotation(Pos, FRotationMatrix::MakeFromZ(ToCam).Rotator());
		It->Sprite->SetActorScale3D(FVector(S, S, 1.0f));
		if (It->Mid.IsValid())
		{
			It->Mid->SetScalarParameterValue(TEXT("Age"), T);
		}
	}
}

void ASeaWorldManager::EmitPuff(const FVector& StagePosition, double Now)
{
	if (SmokeMaterial == nullptr)
	{
		return;
	}
	UStaticMesh* Plane = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane"));
	if (Plane == nullptr)
	{
		return;
	}
	// Hard cap on live puffs (translucent overdraw budget): recycle the oldest.
	if (Puffs.Num() >= kPuffMaxAlive)
	{
		if (Puffs[0].Sprite.IsValid())
		{
			Puffs[0].Sprite->Destroy();
		}
		Puffs.RemoveAt(0);
	}
	const FVector Off = FMath::VRand() * FMath::FRandRange(0.0f, 240.0f);
	const FVector Pos = StagePosition + FVector(Off.X, Off.Y, FMath::Abs(Off.Z) * 0.4f);
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AStaticMeshActor* Sprite =
	    GetWorld()->SpawnActor<AStaticMeshActor>(Pos, FRotator::ZeroRotator, Params);
	if (Sprite == nullptr)
	{
		return;
	}
	Sprite->SetMobility(EComponentMobility::Movable);
	Sprite->GetStaticMeshComponent()->SetStaticMesh(Plane);
	Sprite->GetStaticMeshComponent()->SetCastShadow(false);
	UMaterialInstanceDynamic* Mid =
	    Sprite->GetStaticMeshComponent()->CreateDynamicMaterialInstance(0, SmokeMaterial);
	if (Mid != nullptr)
	{
		Mid->SetScalarParameterValue(TEXT("Age"), 0.0f);
	}
	FPuff P;
	P.Sprite = Sprite;
	P.Mid = Mid;
	P.SpawnPos = Pos;
	P.SpawnTime = Now;
	P.BaseHalfM = FMath::FRandRange(kPuffYoungHalfM * 0.78f, kPuffYoungHalfM * 1.38f);
	P.RollDeg = FMath::FRandRange(0.0f, 360.0f);
	// Random drift direction biased OUTWARD (lateral) and UP (smoke is buoyant): the
	// Z range favours rising. Magnitude varies per puff so some barely move and some
	// fling wide -> irregular, natural dispersion instead of a uniform expanding tube.
	const FVector Dir = FVector(FMath::FRandRange(-1.f, 1.f), FMath::FRandRange(-1.f, 1.f),
	                            FMath::FRandRange(-0.15f, 0.85f))
	                        .GetSafeNormal();
	P.Jitter = Dir * FMath::FRandRange(kPuffDriftMinCms, kPuffDriftMaxCms);
	Puffs.Add(P);
}

void ASeaWorldManager::UpdatePuffs(double Now, const FVector& WindCms)
{
	APlayerCameraManager* Cam = UGameplayStatics::GetPlayerCameraManager(GetWorld(), 0);
	const FVector CamLoc = Cam != nullptr ? Cam->GetCameraLocation() : FVector::ZeroVector;
	for (auto It = Puffs.CreateIterator(); It; ++It)
	{
		const double Age = Now - It->SpawnTime;
		const float AgeFrac = FMath::Clamp(static_cast<float>(Age / kPuffLifeS), 0.0f, 1.0f);
		if (!It->Sprite.IsValid() || AgeFrac >= 1.0f)
		{
			if (It->Sprite.IsValid())
			{
				It->Sprite->Destroy();
			}
			It.RemoveCurrent();
			continue;
		}
		// Drift with the sim wind + a small per-puff lateral jitter (the bend IS weather).
		const FVector Pos = It->SpawnPos + (WindCms + It->Jitter) * Age;
		// Billboard: the plane (+Z normal) faces the camera, with a per-puff roll.
		FVector ToCam = CamLoc - Pos;
		if (!ToCam.Normalize())
		{
			ToCam = FVector::UpVector;
		}
		const FQuat Face = FRotationMatrix::MakeFromZ(ToCam).ToQuat();
		const FQuat Roll(ToCam, FMath::DegreesToRadians(It->RollDeg));
		// Balloon fast then settle (sqrt) so each puff visibly expands as it drifts off
		// the axis — the column reads as diffusing, not translating.
		const float HalfM = FMath::Lerp(It->BaseHalfM, kPuffOldHalfM, FMath::Sqrt(AgeFrac));
		It->Sprite->SetActorLocationAndRotation(Pos, (Roll * Face).Rotator());
		It->Sprite->SetActorScale3D(FVector(2.0f * HalfM, 2.0f * HalfM, 1.0f));  // Plane is 1 m
		if (It->Mid.IsValid())
		{
			It->Mid->SetScalarParameterValue(TEXT("Age"), AgeFrac);
		}
	}
}

void ASeaWorldManager::SpawnFlash(const FVector& StagePosition, double Now, float SizeM)
{
	UStaticMesh* Plane = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane"));
	if (Plane == nullptr || MuzzleMaterial == nullptr)
	{
		return;
	}
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AStaticMeshActor* Flash =
	    GetWorld()->SpawnActor<AStaticMeshActor>(StagePosition, FRotator::ZeroRotator, Params);
	if (Flash == nullptr)
	{
		return;
	}
	Flash->SetMobility(EComponentMobility::Movable);
	Flash->GetStaticMeshComponent()->SetStaticMesh(Plane);
	Flash->GetStaticMeshComponent()->SetCastShadow(false);
	UMaterialInstanceDynamic* Mid =
	    Flash->GetStaticMeshComponent()->CreateDynamicMaterialInstance(0, MuzzleMaterial);
	if (Mid != nullptr)
	{
		Mid->SetScalarParameterValue(TEXT("Age"), 0.0f);
	}
	Flash->SetActorScale3D(FVector(SizeM, SizeM, 1.0f));  // Plane is 1 m.
	MuzzleFlashes.Add({Flash, Mid, Now, SizeM});
}

void ASeaWorldManager::SpawnMuzzleFlash(const FVector& StagePosition, double Now)
{
	// One pop per simultaneous salvo: a ripple-fire volley spawns many rockets in
	// the same frame, so collapse them to a single flash to keep actor count + bloom sane.
	if (Now - LastMuzzleTime < kMuzzleThrottleS)
	{
		return;
	}
	LastMuzzleTime = Now;
	SpawnFlash(StagePosition, Now, 11.0f);  // ~11 m launch flash (blooms hard).
	// Ignition smoke kick: a burst of puffs punched out at the tube mouth so a launch
	// reads as fire + boiling smoke, not just a bare light. They drift/disperse like
	// the trail smoke (same puff system).
	for (int32 i = 0; i < 3; ++i)
	{
		EmitPuff(StagePosition + FMath::VRand() * FMath::FRandRange(60.0f, 320.0f), Now);
	}
}

void ASeaWorldManager::UpdateMuzzleFlashes(double Now)
{
	APlayerCameraManager* Cam = UGameplayStatics::GetPlayerCameraManager(GetWorld(), 0);
	const FVector CamLoc = Cam != nullptr ? Cam->GetCameraLocation() : FVector::ZeroVector;
	for (auto It = MuzzleFlashes.CreateIterator(); It; ++It)
	{
		const double Age = Now - It->SpawnTime;
		if (!It->Flash.IsValid() || Age > kMuzzleLifetimeS)
		{
			if (It->Flash.IsValid())
			{
				It->Flash->Destroy();
			}
			It.RemoveCurrent();
			continue;
		}
		// Billboard the flash plane at the camera, expand + fade (Age scalar drives
		// the emissive/opacity falloff so the star sprite punches then snaps off).
		const double T = Age / kMuzzleLifetimeS;
		FVector ToCam = CamLoc - It->Flash->GetActorLocation();
		if (!ToCam.Normalize())
		{
			ToCam = FVector::UpVector;
		}
		It->Flash->SetActorRotation(FRotationMatrix::MakeFromZ(ToCam).Rotator());
		// Expand to ~2.4x base over its life as it fades (launch ~7->17 m; bigger for a burst).
		const float S = It->BaseScaleM * (1.0f + 1.4f * static_cast<float>(T));
		It->Flash->SetActorScale3D(FVector(S, S, 1.0f));
		if (It->Mid.IsValid())
		{
			It->Mid->SetScalarParameterValue(TEXT("Age"), static_cast<float>(T));
		}
	}
}

void ASeaWorldManager::SampleSeaSurface(const FVector& WorldXY, float& OutHeightCm,
                                        FRotator& OutTilt) const
{
	OutHeightCm = 0.0f;
	OutTilt = FRotator::ZeroRotator;
	if (!OceanComp.IsValid())
	{
		return;
	}
	const UWaterWavesBase* Waves = OceanComp->GetWaterWaves();
	if (Waves == nullptr)
	{
		return;
	}
	// The renderer animates the Gerstner waves off the Water subsystem clock; query with
	// the SAME time so the hull rides the surface it actually sees (not a phantom phase).
	float Time = 0.0f;
	if (UWaterSubsystem* WaterSub = UWaterSubsystem::GetWaterSubsystem(GetWorld()))
	{
		Time = WaterSub->GetWaterTimeSeconds();
	}
	const FVector QueryPos(WorldXY.X, WorldXY.Y, SeaWorldFrame::Origin.Z);
	FVector Normal = FVector::UpVector;
	OutHeightCm = Waves->GetWaveHeightAtPosition(QueryPos, kSeaDepthCm, Time, Normal);
	// Damped + clamped tilt from the surface normal: a long hull rides the mean slope,
	// it doesn't snap flat to every crest like a cork.
	Normal = Normal.GetSafeNormal();
	const float NzSafe = FMath::Max(static_cast<float>(Normal.Z), 0.1f);
	const float Pitch =
	    FMath::RadiansToDegrees(FMath::Atan2(-static_cast<float>(Normal.X), NzSafe)) *
	    kBuoyancyTiltDamp;
	const float Roll =
	    FMath::RadiansToDegrees(FMath::Atan2(static_cast<float>(Normal.Y), NzSafe)) *
	    kBuoyancyTiltDamp;
	OutTilt.Pitch = FMath::Clamp(Pitch, -kBuoyancyTiltMaxDeg, kBuoyancyTiltMaxDeg);
	OutTilt.Roll = FMath::Clamp(Roll, -kBuoyancyTiltMaxDeg, kBuoyancyTiltMaxDeg);
}

float ASeaWorldManager::SeaSurfaceWorldZ(const FVector& WorldXY) const
{
	float HeightCm = 0.0f;
	FRotator UnusedTilt;
	SampleSeaSurface(WorldXY, HeightCm, UnusedTilt);
	return static_cast<float>(SeaWorldFrame::Origin.Z) + HeightCm;
}

void ASeaWorldManager::SampleWake(const FVector& ShipStage, const FVector& VelCms, double Now)
{
	const float Speed2D = static_cast<float>(VelCms.Size2D());
	const float SpeedFrac =
	    FMath::Clamp(Speed2D / static_cast<float>(kWakeFullSpeedCms), 0.0f, 1.0f);
	// Refresh the current ship pose EVERY frame (the bow wave is rebuilt from it),
	// even on frames where the astern-wake point recording is throttled below.
	LastShipSea = FVector(ShipStage.X, ShipStage.Y, SeaWorldFrame::Origin.Z + kWakeSurfaceLiftCm);
	LastShipSpeedFrac = SpeedFrac;
	{
		FVector Fwd = VelCms;
		Fwd.Z = 0.0;
		if (Fwd.Normalize())
		{
			LastShipFwd = Fwd;  // keep the last heading when momentarily stopped.
		}
	}
	if (Now - LastWakeSampleTime < kWakeSampleIntervalS)
	{
		return;
	}
	LastWakeSampleTime = Now;
	// Pin the foam to the sea surface: the hull rides its own pose, the wake lies
	// flat on the water just behind it (a hair above to avoid z-fighting the SLW).
	const FVector OnSea(ShipStage.X, ShipStage.Y, SeaWorldFrame::Origin.Z + kWakeSurfaceLiftCm);
	WakePoints.Add({OnSea, Now, SpeedFrac});
}

void ASeaWorldManager::RebuildWake(double Now)
{
	while (!WakePoints.IsEmpty() && Now - WakePoints[0].Time > kWakeLifetimeS)
	{
		WakePoints.RemoveAt(0);
	}
	if (WakePoints.Num() < 2 || WakeMaterial == nullptr)
	{
		if (WakeRibbon.IsValid())
		{
			WakeRibbon->ClearMeshSection(0);  // ship stopped long enough; foam gone.
		}
		return;
	}
	if (!WakeRibbon.IsValid())
	{
		UProceduralMeshComponent* Ribbon = NewObject<UProceduralMeshComponent>(this);
		Ribbon->RegisterComponent();
		Ribbon->AttachToComponent(GetRootComponent(),
		                          FAttachmentTransformRules::KeepWorldTransform);
		Ribbon->SetWorldTransform(FTransform::Identity);
		Ribbon->SetCastShadow(false);
		Ribbon->SetMaterial(0, WakeMaterial);
		WakeRibbon = Ribbon;
	}

	const int32 Count = WakePoints.Num();
	TArray<FVector> Vertices;
	TArray<int32> Triangles;
	TArray<FLinearColor> Colors;
	TArray<FVector2D> UVs;
	Vertices.Reserve(Count * 2);
	Colors.Reserve(Count * 2);
	UVs.Reserve(Count * 2);
	Triangles.Reserve((Count - 1) * 6);

	const FVector Up = FVector::UpVector;
	for (int32 i = 0; i < Count; ++i)
	{
		const FWakePoint& P = WakePoints[i];
		const double Age = Now - P.Time;
		const float AgeAlpha = FMath::Clamp(static_cast<float>(Age / kWakeLifetimeS), 0.0f, 1.0f);

		// Side = HORIZONTAL perpendicular to the track (NOT camera-facing — foam
		// lies flat on the water, unlike the smoke ribbon).
		FVector Along = (WakePoints[FMath::Min(i + 1, Count - 1)].Position -
		                 WakePoints[FMath::Max(i - 1, 0)].Position);
		Along.Z = 0.0;
		if (!Along.Normalize())
		{
			Along = FVector::ForwardVector;
		}
		FVector Side = FVector::CrossProduct(Along, Up);
		if (!Side.Normalize())
		{
			Side = FVector::RightVector;
		}
		const float HalfWidth = FMath::Lerp(kWakeHalfWidthYoungCm, kWakeHalfWidthOldCm, AgeAlpha);
		// Foam fades astern; the per-point ship speed gates whether there is any.
		const float Opacity = P.SpeedFrac * FMath::Pow(1.0f - AgeAlpha, 1.5f);

		// Pin each edge to the LIVE wave surface (per-vertex: a wide old wake spans
		// different wave phases) so the foam undulates with the swell, not at mean Z.
		FVector L = P.Position - Side * HalfWidth;
		FVector R = P.Position + Side * HalfWidth;
		L.Z = SeaSurfaceWorldZ(L) + static_cast<float>(kWakeSurfaceLiftCm);
		R.Z = SeaSurfaceWorldZ(R) + static_cast<float>(kWakeSurfaceLiftCm);
		Vertices.Add(L);
		Vertices.Add(R);
		Colors.Add(FLinearColor(1.0f, 1.0f, 1.0f, Opacity));
		Colors.Add(FLinearColor(1.0f, 1.0f, 1.0f, Opacity));
		const float V = static_cast<float>(i) / (Count - 1);
		UVs.Add(FVector2D(0.0, V));
		UVs.Add(FVector2D(1.0, V));

		if (i > 0)
		{
			const int32 Base = (i - 1) * 2;
			Triangles.Append({Base, Base + 2, Base + 1, Base + 1, Base + 2, Base + 3});
		}
	}

	WakeRibbon->CreateMeshSection_LinearColor(0, Vertices, Triangles, /*Normals=*/{}, UVs, Colors,
	                                          /*Tangents=*/{}, /*bCreateCollision=*/false);
}

void ASeaWorldManager::RebuildBowWave(double Now)
{
	// Only show when the hull is making way; a stopped ship throws no bow foam.
	if (WakeMaterial == nullptr || LastShipSpeedFrac < 0.03f || LastShipSea.IsNearlyZero())
	{
		if (BowRibbon.IsValid())
		{
			BowRibbon->ClearMeshSection(0);
		}
		return;
	}
	if (!BowRibbon.IsValid())
	{
		UProceduralMeshComponent* Ribbon = NewObject<UProceduralMeshComponent>(this);
		Ribbon->RegisterComponent();
		Ribbon->AttachToComponent(GetRootComponent(), FAttachmentTransformRules::KeepWorldTransform);
		Ribbon->SetWorldTransform(FTransform::Identity);
		Ribbon->SetCastShadow(false);
		Ribbon->SetMaterial(0, WakeMaterial);
		BowRibbon = Ribbon;
	}

	const FVector Up = FVector::UpVector;
	const FVector Fwd = LastShipFwd;
	const FVector Side = FVector::CrossProduct(Fwd, Up).GetSafeNormal();  // starboard
	const FVector Bow = LastShipSea + Fwd * ShipHalfLenCm;
	const float Length = FMath::Lerp(kBowWaveLenMinCm, kBowWaveLenMaxCm, LastShipSpeedFrac);
	const float SweepRad = FMath::DegreesToRadians(kBowWaveAngleDeg);

	TArray<FVector> Vertices;
	TArray<int32> Triangles;
	TArray<FLinearColor> Colors;
	TArray<FVector2D> UVs;

	// Two wings (port/starboard): each a tapering foam band from the bow, sweeping
	// back + outward at ~the Kelvin angle, brightest at the bow and fading aft.
	for (int32 s = -1; s <= 1; s += 2)
	{
		const FVector WingDir =
		    (-Fwd * FMath::Cos(SweepRad) + Side * (static_cast<float>(s) * FMath::Sin(SweepRad)))
		        .GetSafeNormal();
		const FVector Perp = FVector::CrossProduct(WingDir, Up).GetSafeNormal();
		const int32 Base = Vertices.Num();
		for (int32 i = 0; i <= kBowWaveSegments; ++i)
		{
			const float T = static_cast<float>(i) / kBowWaveSegments;
			const FVector Center = Bow + WingDir * (T * Length);
			const float HalfW = kBowWaveBandCm * (0.35f + 0.65f * T);     // narrow at bow
			const float Op = LastShipSpeedFrac * FMath::Pow(1.0f - T, 0.8f);  // fade aft
			// Pin to the live wave surface so the bow foam rides the swell with the hull.
			FVector Vi = Center - Perp * HalfW;
			FVector Vo = Center + Perp * HalfW;
			Vi.Z = SeaSurfaceWorldZ(Vi) + static_cast<float>(kWakeSurfaceLiftCm);
			Vo.Z = SeaSurfaceWorldZ(Vo) + static_cast<float>(kWakeSurfaceLiftCm);
			Vertices.Add(Vi);
			Vertices.Add(Vo);
			Colors.Add(FLinearColor(1.0f, 1.0f, 1.0f, Op));
			Colors.Add(FLinearColor(1.0f, 1.0f, 1.0f, Op));
			UVs.Add(FVector2D(0.0, T));
			UVs.Add(FVector2D(1.0, T));
			if (i > 0)
			{
				const int32 B = Base + (i - 1) * 2;
				Triangles.Append({B, B + 2, B + 1, B + 1, B + 2, B + 3});
			}
		}
	}

	BowRibbon->CreateMeshSection_LinearColor(0, Vertices, Triangles, /*Normals=*/{}, UVs, Colors,
	                                         /*Tangents=*/{}, /*bCreateCollision=*/false);
}

void ASeaWorldManager::SpawnWreckage(const FVector& StagePosition, const FVector& InheritedVelCms,
                                     double Now)
{
	if (TargetMesh == nullptr)
	{
		return;  // No hull mesh to throw; the airburst already sold the kill.
	}
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AStaticMeshActor* Hull =
	    GetWorld()->SpawnActor<AStaticMeshActor>(StagePosition, FRotator::ZeroRotator, Params);
	if (Hull == nullptr)
	{
		return;
	}
	Hull->SetMobility(EComponentMobility::Movable);
	Hull->GetStaticMeshComponent()->SetStaticMesh(TargetMesh);
	Hull->GetStaticMeshComponent()->SetCastShadow(false);
	Hull->SetActorScale3D(FVector(0.7));
	FWreckage W;
	W.Mesh = Hull;
	// Keep half the inbound momentum, add an upward/lateral scatter so the hull
	// tumbles up then arcs into the sea.
	W.VelCms = InheritedVelCms * 0.5 +
	           FVector(FMath::FRandRange(-1500.f, 1500.f), FMath::FRandRange(-1500.f, 1500.f),
	                   FMath::FRandRange(600.f, 2600.f));
	W.SpinDps = FVector(FMath::FRandRange(-220.f, 220.f), FMath::FRandRange(-220.f, 220.f),
	                    FMath::FRandRange(-220.f, 220.f));
	W.SpawnTime = Now;
	Wreckage.Add(W);
}

void ASeaWorldManager::UpdateWreckage(double Now, float DeltaTime)
{
	constexpr float kWreckLifeS = 5.0f;
	constexpr float kGravityCms2 = 980.0f;
	const float SeaZ = static_cast<float>(SeaWorldFrame::Origin.Z);
	for (auto It = Wreckage.CreateIterator(); It; ++It)
	{
		const double Age = Now - It->SpawnTime;
		if (!It->Mesh.IsValid() || Age > kWreckLifeS)
		{
			if (It->Mesh.IsValid())
			{
				It->Mesh->Destroy();
			}
			It.RemoveCurrent();
			continue;
		}
		It->VelCms.Z -= kGravityCms2 * DeltaTime;
		FVector Pos = It->Mesh->GetActorLocation() + It->VelCms * DeltaTime;
		if (Pos.Z < SeaZ)  // Hit the sea: kill momentum and let it sink under.
		{
			It->VelCms *= 0.2f;
			It->VelCms.Z = -60.0f;
			Pos.Z = FMath::Max(Pos.Z, SeaZ - 600.0f);
		}
		const FRotator Rot = It->Mesh->GetActorRotation() +
		                     FRotator(It->SpinDps.Y * DeltaTime, It->SpinDps.Z * DeltaTime,
		                              It->SpinDps.X * DeltaTime);
		It->Mesh->SetActorLocationAndRotation(Pos, Rot);
	}
}
