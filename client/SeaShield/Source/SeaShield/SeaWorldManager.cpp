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
#include "Components/InstancedStaticMeshComponent.h"
#include "ProceduralMeshComponent.h"
#include "UObject/ConstructorHelpers.h"
#include "EngineUtils.h"  // TActorIterator
#include "WaterBodyActor.h"
#include "WaterBodyComponent.h"

#include "SeaWorldFrame.h"

DEFINE_LOG_CATEGORY_STATIC(LogSeaShieldWorld, Log, All);

namespace {
// Buoyancy: the visual hull rides the live Gerstner surface (server owns XY/heading).
constexpr float kHullDraftCm = -40.0f;           // sit the waterline this far into the wave.
constexpr float kBuoyancyZSpeed = 4.0f;          // FInterpTo speed for the bob (anti-jitter).
constexpr float kBuoyancyTiltSpeed = 2.5f;       // FInterpTo speed for roll/pitch.
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
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> SprayFinder(
		TEXT("/Game/SeaShield/Materials/M_Spray"));
	if (SprayFinder.Succeeded())
	{
		SprayMaterial = SprayFinder.Object;
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
	// Hull forward half-length — defaulted, refined from the located frigate's bounds below.
	float ShipHalfLenCm = 6000.0f;
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
			// Buoyancy + helm drive the hull's pose every tick (SetActorLocationAndRotation
			// below). setup_level.py spawns a default StaticMeshActor (Static mobility), so
			// moving it spams a per-frame "has to be 'Movable'" warning that renders as
			// on-screen red text in captures (and would ruin trailer stills). Promote it to
			// Movable here — the level is fully dynamic (Lumen, no baked lighting), so the
			// Static->Movable switch has no lighting cost. (setup_level.py also sets this at
			// authoring; this keeps it correct regardless of how the level was built.)
			if (UPrimitiveComponent* Root = Cast<UPrimitiveComponent>(FrigateActor->GetRootComponent()))
			{
				Root->SetMobility(EComponentMobility::Movable);
			}

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

	// Particle families now draw through one InstancedStaticMeshComponent each (a
	// /Engine/BasicShapes/Plane per particle) instead of an AStaticMeshActor per
	// particle. Custom-data slot 0 carries the Age 0..1 the materials read.
	ParticlePlane = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane"));
	{
		auto MakeISM = [&](UMaterialInterface* Mat) -> UInstancedStaticMeshComponent*
		{
			UInstancedStaticMeshComponent* C = NewObject<UInstancedStaticMeshComponent>(this);
			C->SetupAttachment(GetRootComponent());
			C->RegisterComponent();
			C->SetStaticMesh(ParticlePlane);
			if (Mat != nullptr)
			{
				C->SetMaterial(0, Mat);
			}
			C->NumCustomDataFloats = 1;  // [0] = Age 0..1 (was the per-actor "Age" scalar).
			C->SetCastShadow(false);
			C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			C->SetMobility(EComponentMobility::Movable);
			C->SetCanEverAffectNavigation(false);
			C->bDisableCollision = true;
			return C;
		};
		SmokeISM = MakeISM(SmokeMaterial);
		DebrisISM = MakeISM(DebrisMaterial);
		FlashISM = MakeISM(MuzzleMaterial);
		SprayISM = MakeISM(SprayMaterial);
	}

	// Build the per-effect VFX systems now that the ISMs + materials exist. They hold
	// raw non-owning pointers to the manager's UPROPERTY components/materials (GC stays
	// with the manager) and refs to one another for the cross-system spawn calls.
	SurfaceSampler.Owner = this;
	SurfaceSampler.Ocean = OceanComp;
	TrailSystem = MakeUnique<FRocketTrailSystem>(this, SmokeISM, TrailMaterial, &TrailLifetimeS,
	                                             &TrailWidthYoungCm, &TrailWidthOldCm);
	SplashSystem = MakeUnique<FSplashSystem>(this, SplashMaterial, BurstMaterial);
	ExplosionSystem = MakeUnique<FExplosionSystem>(this, DebrisISM, FlashISM, SplashSystem.Get(),
	                                               TrailSystem.Get());
	WakeSystem = MakeUnique<FWakeSystem>(this, WakeMaterial, SprayISM, &SurfaceSampler);
	WakeSystem->SetShipHalfLenCm(ShipHalfLenCm);
	WreckageSystem = MakeUnique<FWreckageSystem>(this, TargetMesh);

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
			    TrailSystem->SeedRocketPosition(
			        (static_cast<int32>(ESeaEntityKind::Rocket) << 16) | 999, BurstSpot);
			    Synthetic.SubjectId = 999;
			    HandleEngagementEvent(Synthetic);
			    SplashSystem->SpawnSplash(SeaWorldFrame::Origin + FVector(64000.0, 21000.0, 0.0),
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

	// Build the per-frame VFX context ONCE (hoists the camera / ocean fetches the
	// per-effect code used to repeat). Every system Tick reads from this.
	FSeaVfxContext Ctx;
	Ctx.Dt = DeltaTime;
	Ctx.WindCms = Net->GetWeather().WindCms;
	if (const APlayerCameraManager* Cam = UGameplayStatics::GetPlayerCameraManager(GetWorld(), 0))
	{
		Ctx.CamLoc = Cam->GetCameraLocation();
	}
	Ctx.Ocean = OceanComp.Get();
	// Keep the shared sampler pointed at the live ocean (re-assigned on weather change).
	SurfaceSampler.Ocean = OceanComp;

	const double TReconcile = FPlatformTime::Seconds();
	TArray<FSeaEntityState> Entities;
	Net->SampleEntities(Entities);
	const double Now = GetWorld()->GetTimeSeconds();
	Ctx.Now = Now;

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
				SurfaceSampler.SampleSeaSurface(ShipStage, WaveHeightCm, WaveTilt);

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
			WakeSystem->SampleWake(ShipStage, Entity.Velocity, Now);
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
				ExplosionSystem->SpawnMuzzleFlash(Entity.Position + SeaWorldFrame::Origin, Now);
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
			TrailSystem->SampleTrail(Key, StagePosition, Now);
		}
		else if (Entity.Kind == ESeaEntityKind::Target)
		{
			// Cache pose + motion so a kill can place its burst/wreckage even after the
			// actor is destroyed by the reconcile drop in the same tick.
			LastEntityVelCms.Add(Key, Entity.Velocity);
			LastEntityStagePos.Add(Key, StagePosition);
		}
	}

	// Entities gone from the sample (resolved rockets, dropped tracks,
	// destroyed target) leave the stage; their smoke lingers and fades.
	for (auto It = Spawned.CreateIterator(); It; ++It)
	{
		if (!Alive.Contains(It.Key()))
		{
			TrailSystem->MarkRocketDead(It.Key());
			if (It.Value().IsValid())
			{
				It.Value()->Destroy();
			}
			It.RemoveCurrent();
		}
	}

	const double TTrails = FPlatformTime::Seconds();
	LastReconcileMs = (TTrails - TReconcile) * 1000.0;
	TrailSystem->Tick(Ctx);
	const double TVfx = FPlatformTime::Seconds();
	LastRebuildTrailsMs = (TVfx - TTrails) * 1000.0;
	SplashSystem->Tick(Ctx);
	ExplosionSystem->Tick(Ctx);
	WakeSystem->Tick(Ctx);
	WreckageSystem->Tick(Ctx);
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
		// Clear every effect system's state (ribbons/actors/instances + index-locked
		// arrays) so reused entity ids never blend across waves.
		TrailSystem->Clear();
		SplashSystem->Clear();
		ExplosionSystem->Clear();
		WreckageSystem->Clear();
		LastEntityVelCms.Reset();
		LastEntityStagePos.Reset();
		bLoggedFirstSpawn = false;
		return;
	}
	if (Event.Kind == 6)
	{
		SplashSystem->SpawnSplash(SeaWorldFrame::Origin + FVector(0.0, 0.0, 1200.0),
		                          GetWorld()->GetTimeSeconds(), /*bAirburst=*/true);
		return;
	}
	if (Event.Kind == 2)  // kTargetDestroyed — a confirmed kill: burst + wreckage.
	{
		const int32 Key = (static_cast<int32>(ESeaEntityKind::Target) << 16) | Event.SubjectId;
		// Prefer the cached last pose (survives the reconcile drop); fall back to a
		// still-live actor only if no cache exists yet. Either way the kill ALWAYS
		// produces its burst — it no longer silently no-ops on the tick-order race.
		const FVector* PosP = LastEntityStagePos.Find(Key);
		const TWeakObjectPtr<AActor>* Actor = Spawned.Find(Key);
		FVector Where;
		if (PosP != nullptr)
		{
			Where = *PosP;
		}
		else if (Actor != nullptr && Actor->IsValid())
		{
			Where = Actor->Get()->GetActorLocation();
		}
		else
		{
			return;  // never saw this target — nothing to anchor the burst to.
		}
		const double Now = GetWorld()->GetTimeSeconds();
		const FVector* Vel = LastEntityVelCms.Find(Key);
		const FVector VelCms = Vel != nullptr ? *Vel : FVector::ZeroVector;
		ExplosionSystem->SpawnExplosion(Where, VelCms, Now);  // flash + fireball + debris + smoke
		WreckageSystem->SpawnWreckage(Where, VelCms, Now);    // the dead hull tumbling into the sea
		return;
	}
	// Proximity bursts happen at altitude (airburst puff); rockets that never
	// fuzed come down into the sea (water column at the surface).
	const int32 Key = (static_cast<int32>(ESeaEntityKind::Rocket) << 16) | Event.SubjectId;
	FVector Where;
	if (!TrailSystem->TryGetRocketPosition(Key, Where))
	{
		return;
	}
	{
		if (!Event.bDetonated)
		{
			Where.Z = 0.0;
		}
		// A fuzed proximity detonation is an airburst explosion; an un-fuzed rocket
		// that fell into the sea throws a water column instead.
		if (Event.bDetonated)
		{
			ExplosionSystem->SpawnExplosion(Where, FVector::ZeroVector, GetWorld()->GetTimeSeconds());
		}
		else
		{
			SplashSystem->SpawnSplash(Where, GetWorld()->GetTimeSeconds(), /*bAirburst=*/false);
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
