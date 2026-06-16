#include "SeaVfxSystems.h"

#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "ProceduralMeshComponent.h"
#include "WaterBodyComponent.h"
#include "WaterWaves.h"
#include "WaterSubsystem.h"

#include "SeaWorldFrame.h"

DEFINE_LOG_CATEGORY_STATIC(LogSeaShieldVfx, Log, All);

namespace {
constexpr double kTrailSampleIntervalS = 0.035;  // ~29 Hz history -> a smooth, unfaceted column.
constexpr double kSplashLifetimeS = 1.6;
constexpr double kMuzzleLifetimeS = 0.18;        // a flash, not a fireball.
constexpr double kMuzzleThrottleS = 0.06;        // one pop per simultaneous salvo.
// Intercept detonation debris: glowing fragments flung radially, arcing under gravity.
constexpr int32 kDebrisPerBurst = 18;            // fragments per kill (sparse event; streaked sparks).
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
// Hull foam collar: a thin foam ring hugging the hull waterline outline (an ellipse
// from the ship pose + ShipHalfLenCm), pinned to the live surface like the wake. Faint
// at rest (a hull always disturbs the water it sits in) and brighter making way.
constexpr int32 kHullFoamSegments = 36;          // ring resolution around the hull.
constexpr float kHullFoamBeamRatio = 0.12f;      // beam half / length half (frigate L/B ~8.7).
constexpr float kHullFoamBandCm = 320.0f;        // foam band width OUTSIDE the hull edge.
constexpr float kHullFoamInsetCm = 70.0f;        // band starts a hair inside the edge (no gap).
constexpr float kHullFoamBaseOp = 0.28f;         // always-on faint foam at rest.
constexpr float kHullFoamSpeedOp = 0.55f;        // extra foam when making way.
// Buoyancy: the visual hull rides the live Gerstner surface (server owns XY/heading).
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

// ============================ FSeaSurfaceSampler ============================

void FSeaSurfaceSampler::SampleSeaSurface(const FVector& WorldXY, float& OutHeightCm,
                                          FRotator& OutTilt) const
{
	OutHeightCm = 0.0f;
	OutTilt = FRotator::ZeroRotator;
	if (!Ocean.IsValid())
	{
		return;
	}
	const UWaterWavesBase* Waves = Ocean->GetWaterWaves();
	if (Waves == nullptr)
	{
		return;
	}
	// The renderer animates the Gerstner waves off the Water subsystem clock; query with
	// the SAME time so the hull rides the surface it actually sees (not a phantom phase).
	float Time = 0.0f;
	UWorld* World = Owner != nullptr ? Owner->GetWorld() : nullptr;
	if (UWaterSubsystem* WaterSub = UWaterSubsystem::GetWaterSubsystem(World))
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

float FSeaSurfaceSampler::SeaSurfaceWorldZ(const FVector& WorldXY) const
{
	float HeightCm = 0.0f;
	FRotator UnusedTilt;
	SampleSeaSurface(WorldXY, HeightCm, UnusedTilt);
	return static_cast<float>(SeaWorldFrame::Origin.Z) + HeightCm;
}

// ============================ FRocketTrailSystem ============================

void FRocketTrailSystem::Tick(const FSeaVfxContext& Ctx)
{
	RebuildTrails(Ctx.Now, Ctx.WindCms, Ctx.CamLoc);
	UpdatePuffs(Ctx.Now, Ctx.WindCms, Ctx.CamLoc);
}

void FRocketTrailSystem::MarkRocketDead(int32 Key)
{
	if (FRocketTrail* Trail = Trails.Find(Key))
	{
		Trail->bRocketAlive = false;
	}
}

void FRocketTrailSystem::SeedRocketPosition(int32 Key, const FVector& StagePosition)
{
	Trails.FindOrAdd(Key).LastRocketPosition = StagePosition;
}

bool FRocketTrailSystem::TryGetRocketPosition(int32 Key, FVector& OutStagePosition) const
{
	if (const FRocketTrail* Trail = Trails.Find(Key))
	{
		OutStagePosition = Trail->LastRocketPosition;
		return true;
	}
	return false;
}

void FRocketTrailSystem::Clear()
{
	for (auto& Pair : Trails)
	{
		if (Pair.Value.Ribbon.IsValid())
		{
			Pair.Value.Ribbon->DestroyComponent();
		}
	}
	Trails.Reset();
	// Particle family is instanced: drop every instance and clear the index-locked array.
	if (SmokeISM != nullptr)
	{
		SmokeISM->ClearInstances();
	}
	Puffs.Reset();
}

void FRocketTrailSystem::SampleTrail(int32 Key, const FVector& StagePosition, double Now)
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

void FRocketTrailSystem::RebuildTrails(double Now, const FVector& WindCms, const FVector& CamLoc)
{
	const float TrailLifetime = *TrailLifetimeS;
	const float WidthYoung = *TrailWidthYoungCm;
	const float WidthOld = *TrailWidthOldCm;
	for (auto It = Trails.CreateIterator(); It; ++It)
	{
		FRocketTrail& Trail = It.Value();

		// Age out spent smoke; drop the whole column once the rocket is gone
		// and the youngest point has faded.
		while (!Trail.Points.IsEmpty() && Now - Trail.Points[0].Time > TrailLifetime)
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
			UProceduralMeshComponent* Ribbon = NewObject<UProceduralMeshComponent>(Owner);
			Ribbon->RegisterComponent();
			Ribbon->AttachToComponent(Owner->GetRootComponent(),
			                          FAttachmentTransformRules::KeepWorldTransform);
			Ribbon->SetWorldTransform(FTransform::Identity);
			Ribbon->SetCastShadow(false);
			if (TrailMaterial != nullptr)
			{
				Ribbon->SetMaterial(0, TrailMaterial);
			}
			Trail.Ribbon = Ribbon;
		}

		const FVector CameraLocation = CamLoc;

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

			const float AgeAlpha = FMath::Clamp(Age / TrailLifetime, 0.0, 1.0);
			// Billow fast then settle: sqrt grows the column most in its first second
			// (where exhaust expands hardest) so it reads as a fat plume, not a wedge.
			const float WidthAlpha = FMath::Sqrt(AgeAlpha);
			const float HalfWidth = 0.5f * FMath::Lerp(WidthYoung, WidthOld, WidthAlpha);
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

void FRocketTrailSystem::EmitPuff(const FVector& StagePosition, double Now)
{
	if (SmokeISM == nullptr)
	{
		return;
	}
	// Hard cap on live puffs (translucent overdraw budget): drop this one.
	if (Puffs.Num() >= kPuffMaxAlive)
	{
		return;
	}
	const FVector Off = FMath::VRand() * FMath::FRandRange(0.0f, 240.0f);
	const FVector Pos = StagePosition + FVector(Off.X, Off.Y, FMath::Abs(Off.Z) * 0.4f);
	FPuff P;
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
	const int32 Idx = SmokeISM->AddInstance(
	    FTransform(FQuat::Identity, Pos, FVector(2.0f * P.BaseHalfM)), /*bWorldSpace=*/true);
	Puffs.Add(P);
	SmokeISM->SetCustomDataValue(Idx, 0, 0.0f, false);
}

void FRocketTrailSystem::UpdatePuffs(double Now, const FVector& WindCms, const FVector& CamLoc)
{
	if (SmokeISM == nullptr)
	{
		return;
	}
	// First pass: swap-remove faded puffs (O(1)), mirroring by popping the LAST instance
	// so Puffs[i] <-> instance i stays index-locked.
	for (int32 i = Puffs.Num() - 1; i >= 0; --i)
	{
		const double Age = Now - Puffs[i].SpawnTime;
		const float AgeFrac = FMath::Clamp(static_cast<float>(Age / kPuffLifeS), 0.0f, 1.0f);
		if (AgeFrac >= 1.0f)
		{
			Puffs.RemoveAtSwap(i, 1, EAllowShrinking::No);
			SmokeISM->RemoveInstance(SmokeISM->GetInstanceCount() - 1);
		}
	}
	// Second pass: drift + billboard every survivor, then batch-push the transforms.
	TArray<FTransform> Xforms;
	Xforms.Reserve(Puffs.Num());
	for (int32 i = 0; i < Puffs.Num(); ++i)
	{
		const FPuff& P = Puffs[i];
		const double Age = Now - P.SpawnTime;
		const float AgeFrac = FMath::Clamp(static_cast<float>(Age / kPuffLifeS), 0.0f, 1.0f);
		// Drift with the sim wind + a small per-puff lateral jitter (the bend IS weather).
		const FVector Pos = P.SpawnPos + (WindCms + P.Jitter) * Age;
		// Billboard: the plane (+Z normal) faces the camera, with a per-puff roll.
		FVector ToCam = CamLoc - Pos;
		if (!ToCam.Normalize())
		{
			ToCam = FVector::UpVector;
		}
		const FQuat Face = FRotationMatrix::MakeFromZ(ToCam).ToQuat();
		const FQuat Roll(ToCam, FMath::DegreesToRadians(P.RollDeg));
		// Balloon fast then settle (sqrt) so each puff visibly expands as it drifts off
		// the axis — the column reads as diffusing, not translating.
		const float HalfM = FMath::Lerp(P.BaseHalfM, kPuffOldHalfM, FMath::Sqrt(AgeFrac));
		Xforms.Add(FTransform(Roll * Face, Pos,
		                      FVector(2.0f * HalfM, 2.0f * HalfM, 1.0f)));  // Plane is 1 m
		SmokeISM->SetCustomDataValue(i, 0, AgeFrac, false);
	}
	SmokeISM->BatchUpdateInstancesTransforms(0, Xforms, /*bWorldSpace=*/true,
	                                         /*bMarkRenderStateDirty=*/true, /*bTeleport=*/true);
}

// ============================ FSplashSystem ============================

void FSplashSystem::Tick(const FSeaVfxContext& Ctx)
{
	UpdateSplashes(Ctx.Now);
}

void FSplashSystem::Clear()
{
	for (FSplash& Sp : Splashes)
	{
		if (Sp.Column.IsValid())
		{
			Sp.Column->Destroy();
		}
	}
	Splashes.Reset();
}

void FSplashSystem::SpawnSplash(const FVector& StagePosition, double Now, bool bAirburst)
{
	UStaticMesh* Shape = LoadObject<UStaticMesh>(
	    nullptr, bAirburst ? TEXT("/Engine/BasicShapes/Sphere") : TEXT("/Engine/BasicShapes/Cylinder"));
	if (Shape == nullptr)
	{
		return;
	}
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AStaticMeshActor* Column = Owner->GetWorld()->SpawnActor<AStaticMeshActor>(
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
	UE_LOG(LogSeaShieldVfx, Display, TEXT("%hs at (%.0f, %.0f, %.0f) cm t=%.1f"),
	       bAirburst ? "Airburst" : "Sea splash", StagePosition.X, StagePosition.Y,
	       StagePosition.Z, Now);
}

void FSplashSystem::UpdateSplashes(double Now)
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

// ============================ FExplosionSystem ============================

void FExplosionSystem::Tick(const FSeaVfxContext& Ctx)
{
	UpdateMuzzleFlashes(Ctx.Now, Ctx.CamLoc);
	UpdateDebris(Ctx.Now, Ctx.Dt, Ctx.CamLoc);
}

void FExplosionSystem::Clear()
{
	if (FlashISM != nullptr)
	{
		FlashISM->ClearInstances();
	}
	MuzzleFlashes.Reset();
	if (DebrisISM != nullptr)
	{
		DebrisISM->ClearInstances();
	}
	Debris.Reset();
}

void FExplosionSystem::SpawnExplosion(const FVector& StagePosition, const FVector& InheritedVelCms,
                                      double Now)
{
	// Layered airburst: a hot flash punch, the expanding fireball -> smoke sphere, a
	// radial spray of glowing fragments, and a few smoke puffs that linger and drift.
	SpawnFlash(StagePosition, Now, 18.0f);                       // bright detonation flash (blooms hard)
	Splash->SpawnSplash(StagePosition, Now, /*bAirburst=*/true);  // fireball core -> dark powder smoke
	SpawnDebris(StagePosition, InheritedVelCms, Now);
	for (int32 i = 0; i < 8; ++i)  // smoke aftermath: a thicker bank of puffs that drift + disperse
	{
		Trail->EmitPuff(StagePosition + FMath::VRand() * FMath::FRandRange(100.0f, 900.0f), Now);
	}
}

void FExplosionSystem::SpawnDebris(const FVector& StagePosition, const FVector& InheritedVelCms,
                                   double Now)
{
	if (DebrisISM == nullptr)
	{
		return;
	}
	for (int32 i = 0; i < kDebrisPerBurst; ++i)
	{
		if (Debris.Num() >= kDebrisMaxAlive)  // global cap (multi-kill): drop this fragment.
		{
			return;
		}
		FDebris D;
		D.Pos = StagePosition;
		// Radial ejection biased upward + a fraction of the target's inbound momentum.
		FVector Dir = FMath::VRand();
		Dir.Z = FMath::Abs(Dir.Z) * 0.6f + 0.25f;
		Dir.Normalize();
		D.VelCms = Dir * FMath::FRandRange(kDebrisSpeedMinCms, kDebrisSpeedMaxCms) +
		           InheritedVelCms * 0.25f;
		D.SpawnTime = Now;
		D.SizeM = FMath::FRandRange(0.9f, 2.2f);
		const int32 Idx = DebrisISM->AddInstance(
		    FTransform(FQuat::Identity, StagePosition, FVector(D.SizeM)), /*bWorldSpace=*/true);
		Debris.Add(D);
		DebrisISM->SetCustomDataValue(Idx, 0, 0.0f, false);
	}
}

void FExplosionSystem::UpdateDebris(double Now, float DeltaTime, const FVector& CamLoc)
{
	if (DebrisISM == nullptr)
	{
		return;
	}
	// First pass: swap-remove expired fragments (O(1)), mirroring by popping the LAST
	// instance so Debris[i] <-> instance i stays index-locked.
	for (int32 i = Debris.Num() - 1; i >= 0; --i)
	{
		const double Age = Now - Debris[i].SpawnTime;
		const float T = FMath::Clamp(static_cast<float>(Age / kDebrisLifeS), 0.0f, 1.0f);
		if (T >= 1.0f)
		{
			Debris.RemoveAtSwap(i, 1, EAllowShrinking::No);
			DebrisISM->RemoveInstance(DebrisISM->GetInstanceCount() - 1);
		}
	}
	// Second pass: integrate + billboard every survivor, then batch-push the transforms.
	TArray<FTransform> Xforms;
	Xforms.Reserve(Debris.Num());
	for (int32 i = 0; i < Debris.Num(); ++i)
	{
		FDebris& D = Debris[i];
		const double Age = Now - D.SpawnTime;
		const float T = FMath::Clamp(static_cast<float>(Age / kDebrisLifeS), 0.0f, 1.0f);
		D.VelCms.Z -= kDebrisGravityCms2 * DeltaTime;  // ballistic arc
		D.Pos += D.VelCms * DeltaTime;
		FVector ToCam = CamLoc - D.Pos;
		if (!ToCam.Normalize())
		{
			ToCam = FVector::UpVector;
		}
		const float S = D.SizeM * (1.0f - 0.5f * T);  // burn down as it cools
		// Velocity-stretched spark: orient the camera-facing quad so its long axis
		// runs along the fragment's SCREEN-PROJECTED velocity and stretch it by speed
		// — round sparks become tracer STREAKS (a motion-blur read) at zero extra
		// overdraw (same instance, just non-uniform scale). The streak relaxes back to
		// a point as the fragment slows and cools.
		const FVector Vel = D.VelCms;
		FVector VelInPlane = Vel - FVector::DotProduct(Vel, ToCam) * ToCam;  // drop the toward-cam part
		const float Speed = Vel.Size();
		FQuat Rot;
		float LengthScale = S;
		if (Speed > 1.0f && VelInPlane.Normalize())
		{
			Rot = FRotationMatrix::MakeFromZX(ToCam, VelInPlane).ToQuat();  // X = streak direction
			const float Stretch = FMath::Clamp(1.0f + Speed / 2200.0f, 1.0f, 3.8f) * (1.0f - 0.55f * T);
			LengthScale = S * FMath::Max(Stretch, 1.0f);
		}
		else
		{
			Rot = FRotationMatrix::MakeFromZ(ToCam).ToQuat();
		}
		const float WidthScale = S * 0.42f;  // thin across the streak
		Xforms.Add(FTransform(Rot, D.Pos, FVector(LengthScale, WidthScale, 1.0f)));
		DebrisISM->SetCustomDataValue(i, 0, T, false);
	}
	DebrisISM->BatchUpdateInstancesTransforms(0, Xforms, /*bWorldSpace=*/true,
	                                          /*bMarkRenderStateDirty=*/true, /*bTeleport=*/true);
}

void FExplosionSystem::SpawnFlash(const FVector& StagePosition, double Now, float SizeM)
{
	if (FlashISM == nullptr)
	{
		return;
	}
	FMuzzleFlash F;
	F.Pos = StagePosition;
	F.SpawnTime = Now;
	F.BaseScaleM = SizeM;
	const int32 Idx = FlashISM->AddInstance(
	    FTransform(FQuat::Identity, StagePosition, FVector(SizeM)), /*bWorldSpace=*/true);
	MuzzleFlashes.Add(F);
	FlashISM->SetCustomDataValue(Idx, 0, 0.0f, false);
}

void FExplosionSystem::SpawnMuzzleFlash(const FVector& StagePosition, double Now)
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
		Trail->EmitPuff(StagePosition + FMath::VRand() * FMath::FRandRange(60.0f, 320.0f), Now);
	}
}

void FExplosionSystem::UpdateMuzzleFlashes(double Now, const FVector& CamLoc)
{
	if (FlashISM == nullptr)
	{
		return;
	}
	// First pass: swap-remove spent flashes (O(1)), mirroring by popping the LAST instance
	// so MuzzleFlashes[i] <-> instance i stays index-locked.
	for (int32 i = MuzzleFlashes.Num() - 1; i >= 0; --i)
	{
		const double Age = Now - MuzzleFlashes[i].SpawnTime;
		if (Age > kMuzzleLifetimeS)
		{
			MuzzleFlashes.RemoveAtSwap(i, 1, EAllowShrinking::No);
			FlashISM->RemoveInstance(FlashISM->GetInstanceCount() - 1);
		}
	}
	// Second pass: billboard + expand every survivor (Pos is fixed), then batch-push.
	TArray<FTransform> Xforms;
	Xforms.Reserve(MuzzleFlashes.Num());
	for (int32 i = 0; i < MuzzleFlashes.Num(); ++i)
	{
		const FMuzzleFlash& F = MuzzleFlashes[i];
		const double Age = Now - F.SpawnTime;
		// Billboard the flash plane at the camera, expand + fade (Age drives the
		// emissive/opacity falloff so the star sprite punches then snaps off).
		const double T = Age / kMuzzleLifetimeS;
		FVector ToCam = CamLoc - F.Pos;
		if (!ToCam.Normalize())
		{
			ToCam = FVector::UpVector;
		}
		// Expand to ~2.4x base over its life as it fades (launch ~7->17 m; bigger for a burst).
		const float S = F.BaseScaleM * (1.0f + 1.4f * static_cast<float>(T));
		Xforms.Add(FTransform(FRotationMatrix::MakeFromZ(ToCam).ToQuat(), F.Pos,
		                      FVector(S, S, 1.0f)));
		FlashISM->SetCustomDataValue(i, 0, static_cast<float>(T), false);
	}
	FlashISM->BatchUpdateInstancesTransforms(0, Xforms, /*bWorldSpace=*/true,
	                                         /*bMarkRenderStateDirty=*/true, /*bTeleport=*/true);
}

// ============================ FWakeSystem ============================

void FWakeSystem::Tick(const FSeaVfxContext& Ctx)
{
	RebuildWake(Ctx.Now);
	RebuildBowWave(Ctx.Now);
	RebuildHullFoam(Ctx.Now);
}

void FWakeSystem::SampleWake(const FVector& ShipStage, const FVector& VelCms, double Now)
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

void FWakeSystem::RebuildWake(double Now)
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
		UProceduralMeshComponent* Ribbon = NewObject<UProceduralMeshComponent>(Owner);
		Ribbon->RegisterComponent();
		Ribbon->AttachToComponent(Owner->GetRootComponent(),
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
		L.Z = Surface->SeaSurfaceWorldZ(L) + static_cast<float>(kWakeSurfaceLiftCm);
		R.Z = Surface->SeaSurfaceWorldZ(R) + static_cast<float>(kWakeSurfaceLiftCm);
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

void FWakeSystem::RebuildBowWave(double Now)
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
		UProceduralMeshComponent* Ribbon = NewObject<UProceduralMeshComponent>(Owner);
		Ribbon->RegisterComponent();
		Ribbon->AttachToComponent(Owner->GetRootComponent(), FAttachmentTransformRules::KeepWorldTransform);
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
			Vi.Z = Surface->SeaSurfaceWorldZ(Vi) + static_cast<float>(kWakeSurfaceLiftCm);
			Vo.Z = Surface->SeaSurfaceWorldZ(Vo) + static_cast<float>(kWakeSurfaceLiftCm);
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

void FWakeSystem::RebuildHullFoam(double Now)
{
	if (WakeMaterial == nullptr || LastShipSea.IsNearlyZero())
	{
		if (HullFoamRibbon.IsValid())
		{
			HullFoamRibbon->ClearMeshSection(0);
		}
		return;
	}
	if (!HullFoamRibbon.IsValid())
	{
		UProceduralMeshComponent* Ribbon = NewObject<UProceduralMeshComponent>(Owner);
		Ribbon->RegisterComponent();
		Ribbon->AttachToComponent(Owner->GetRootComponent(), FAttachmentTransformRules::KeepWorldTransform);
		Ribbon->SetWorldTransform(FTransform::Identity);
		Ribbon->SetCastShadow(false);
		Ribbon->SetMaterial(0, WakeMaterial);
		HullFoamRibbon = Ribbon;
	}

	const FVector Up = FVector::UpVector;
	const FVector Fwd = LastShipFwd;
	const FVector Side = FVector::CrossProduct(Fwd, Up).GetSafeNormal();  // starboard
	const float LenHalf = ShipHalfLenCm;
	const float BeamHalf = ShipHalfLenCm * kHullFoamBeamRatio;
	// Faint always-on foam (the hull displaces the water it floats in) + more making way.
	const float Op = kHullFoamBaseOp + kHullFoamSpeedOp * LastShipSpeedFrac;

	TArray<FVector> Vertices;
	TArray<int32> Triangles;
	TArray<FLinearColor> Colors;
	TArray<FVector2D> UVs;
	Vertices.Reserve((kHullFoamSegments + 1) * 2);
	Colors.Reserve((kHullFoamSegments + 1) * 2);
	UVs.Reserve((kHullFoamSegments + 1) * 2);
	Triangles.Reserve(kHullFoamSegments * 6);

	// One closed ring: an ellipse hugging the hull waterline outline (fore-aft = LenHalf,
	// beam = BeamHalf), the band straddling that edge from just inside to kHullFoamBandCm
	// outside. Each edge is pinned per-vertex to the LIVE Gerstner surface so the collar
	// rides the swell at the real waterline as the hull bobs.
	for (int32 i = 0; i <= kHullFoamSegments; ++i)
	{
		const float Ang = (2.0f * PI) * (static_cast<float>(i % kHullFoamSegments) / kHullFoamSegments);
		const FVector EdgeOffset = Fwd * (FMath::Cos(Ang) * LenHalf) + Side * (FMath::Sin(Ang) * BeamHalf);
		const FVector Edge = LastShipSea + EdgeOffset;
		FVector Radial = EdgeOffset;  // outward (horizontal) — good enough for a moderate ellipse.
		Radial.Z = 0.0;
		if (!Radial.Normalize())
		{
			Radial = Side;
		}
		FVector Inner = Edge - Radial * kHullFoamInsetCm;
		FVector Outer = Edge + Radial * kHullFoamBandCm;
		Inner.Z = Surface->SeaSurfaceWorldZ(Inner) + static_cast<float>(kWakeSurfaceLiftCm);
		Outer.Z = Surface->SeaSurfaceWorldZ(Outer) + static_cast<float>(kWakeSurfaceLiftCm);
		Vertices.Add(Inner);
		Vertices.Add(Outer);
		Colors.Add(FLinearColor(1.0f, 1.0f, 1.0f, Op));
		Colors.Add(FLinearColor(1.0f, 1.0f, 1.0f, Op));
		UVs.Add(FVector2D(0.0, static_cast<float>(i)));  // U across the band (M_Wake edge falloff).
		UVs.Add(FVector2D(1.0, static_cast<float>(i)));
		if (i > 0)
		{
			const int32 B = (i - 1) * 2;
			Triangles.Append({B, B + 2, B + 1, B + 1, B + 2, B + 3});
		}
	}

	HullFoamRibbon->CreateMeshSection_LinearColor(0, Vertices, Triangles, /*Normals=*/{}, UVs, Colors,
	                                              /*Tangents=*/{}, /*bCreateCollision=*/false);
}

// ============================ FWreckageSystem ============================

void FWreckageSystem::Tick(const FSeaVfxContext& Ctx)
{
	UpdateWreckage(Ctx.Now, Ctx.Dt);
}

void FWreckageSystem::Clear()
{
	for (FWreckage& W : Wreckage)
	{
		if (W.Mesh.IsValid())
		{
			W.Mesh->Destroy();
		}
	}
	Wreckage.Reset();
}

void FWreckageSystem::SpawnWreckage(const FVector& StagePosition, const FVector& InheritedVelCms,
                                    double Now)
{
	if (TargetMesh == nullptr)
	{
		return;  // No hull mesh to throw; the airburst already sold the kill.
	}
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AStaticMeshActor* Hull =
	    Owner->GetWorld()->SpawnActor<AStaticMeshActor>(StagePosition, FRotator::ZeroRotator, Params);
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

void FWreckageSystem::UpdateWreckage(double Now, float DeltaTime)
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
