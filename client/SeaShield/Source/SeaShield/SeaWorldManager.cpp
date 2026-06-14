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
#include "TimerManager.h"
#include "UnrealClient.h"
#include "Materials/MaterialInterface.h"
#include "ProceduralMeshComponent.h"
#include "UObject/ConstructorHelpers.h"

#include "SeaWorldFrame.h"

DEFINE_LOG_CATEGORY_STATIC(LogSeaShieldWorld, Log, All);

namespace {
constexpr double kTrailSampleIntervalS = 0.06;  // ~17 Hz history, plenty for a ribbon.
constexpr double kSplashLifetimeS = 1.6;
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
		UE_LOG(LogSeaShieldWorld, Display,
		       TEXT("Frame stats: %lld frames avg=%.2f ms (%.1f fps) max=%.1f ms over-16.7ms=%lld"),
		       FrameCount, FrameTimeSumMs / FrameCount, 1000.0 * FrameCount / FrameTimeSumMs,
		       FrameTimeMaxMs, FramesOver16ms);
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
		if (FrameMs > 16.7)
		{
			++FramesOver16ms;
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
			if (FrigateActor.IsValid())
			{
				const FVector ShipStage = Entity.Position + SeaWorldFrame::Origin;
				FRotator Yaw = FrigateActor->GetActorRotation();
				if (!Entity.Velocity.IsNearlyZero(50.0))  // >0.5 m/s of way on
				{
					Yaw.Yaw = Entity.Velocity.Rotation().Yaw;
				}
				FrigateActor->SetActorLocationAndRotation(
				    FVector(ShipStage.X, ShipStage.Y, FrigateActor->GetActorLocation().Z), Yaw);
			}
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

	RebuildTrails(Now, Net->GetWeather().WindCms);
	UpdateSplashes(Now);
	UpdateWreckage(Now, DeltaTime);
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
			SpawnSplash(Where, Now, /*bAirburst=*/true);
			const FVector* Vel = LastEntityVelCms.Find(Key);
			SpawnWreckage(Where, Vel != nullptr ? *Vel : FVector::ZeroVector, Now);
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
		SpawnSplash(Where, GetWorld()->GetTimeSeconds(), Event.bDetonated);

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
			const FVector CameraSpot = Where + Side * 20000.0 - Along * 5000.0 + FVector(0, 0, 1500.0);
			ACameraActor* Camera = GetWorld()->SpawnActor<ACameraActor>(
			    CameraSpot, (Where - CameraSpot).Rotation());
			if (APlayerController* Controller = GetWorld()->GetFirstPlayerController())
			{
				Controller->SetViewTarget(Camera);
			}
			FTimerHandle ShotTimer;
			GetWorld()->GetTimerManager().SetTimer(
			    ShotTimer,
			    []() { FScreenshotRequest::RequestScreenshot(TEXT("SeaShot.png"), /*bShowUI=*/true, false); },
			    0.6f, false);
			FTimerHandle QuitTimer;
			GetWorld()->GetTimerManager().SetTimer(
			    QuitTimer, []() { FPlatformMisc::RequestExit(false); }, 3.5f, false);
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

	// The trail ribbon's vertex-color translucent path, as a 1 cm quad.
	UProceduralMeshComponent* Quad = NewObject<UProceduralMeshComponent>(this);
	Quad->RegisterComponent();
	Quad->AttachToComponent(GetRootComponent(), FAttachmentTransformRules::KeepWorldTransform);
	Quad->SetWorldTransform(FTransform::Identity);
	Quad->SetCastShadow(false);
	if (TrailMaterial != nullptr)
	{
		Quad->SetMaterial(0, TrailMaterial);
	}
	const TArray<FVector> Vertices = {Base, Base + FVector(1, 0, 0), Base + FVector(0, 0, 1),
	                                  Base + FVector(1, 0, 1)};
	const TArray<FLinearColor> Colors = {FLinearColor(1, 1, 1, 0.5f), FLinearColor(1, 1, 1, 0.5f),
	                                     FLinearColor(1, 1, 1, 0.5f), FLinearColor(1, 1, 1, 0.5f)};
	Quad->CreateMeshSection_LinearColor(0, Vertices, {0, 2, 1, 1, 2, 3}, {}, {}, Colors, {},
	                                    false);

	FTimerHandle CleanupTimer;
	GetWorld()->GetTimerManager().SetTimer(
	    CleanupTimer,
	    [Specks, WeakQuad = TWeakObjectPtr<UProceduralMeshComponent>(Quad)]()
	    {
		    for (const TWeakObjectPtr<AActor>& Speck : Specks)
		    {
			    if (Speck.IsValid())
			    {
				    Speck->Destroy();
			    }
		    }
		    if (WeakQuad.IsValid())
		    {
			    WeakQuad->DestroyComponent();
		    }
	    },
	    2.5f, false);
}

void ASeaWorldManager::SampleTrail(int32 Key, const FVector& StagePosition, double Now)
{
	FRocketTrail& Trail = Trails.FindOrAdd(Key);
	Trail.LastRocketPosition = StagePosition;
	Trail.bRocketAlive = true;
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
			const float HalfWidth =
			    0.5f * FMath::Lerp(TrailWidthYoungCm, TrailWidthOldCm, AgeAlpha);
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
	if (Material != nullptr)
	{
		Column->GetStaticMeshComponent()->SetMaterial(0, Material);
	}
	Column->SetActorScale3D(FVector(2.0, 2.0, 0.5));
	Splashes.Add({Column, Now, bAirburst});
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
			// Sharp flash-out: the puff swells and thins away.
			const double Diameter = 14.0 + 22.0 * T;  // m
			It->Column->SetActorScale3D(FVector(Diameter));
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
