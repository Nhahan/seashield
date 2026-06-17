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
constexpr double kWakeMinSpeedCms = 180.0;       // DEADZONE: below this the hull isn't "making way" —
                                                 // suppress the astern wake + bow wave so buoyancy
                                                 // bob/drift doesn't leave a spurious dashed trail on
                                                 // a near-stationary ship (the collar still shows).
constexpr double kWakeSurfaceLiftCm = 60.0;      // ride just above the sea plane.
constexpr float kWakeHalfWidthYoungCm = 430.0f;  // ~ship beam at the stern (was 220 = too narrow/thin).
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
constexpr float kHullFoamBandCm = 560.0f;        // foam band width OUTSIDE the hull edge (was 320 — too thin to read at distance).
constexpr float kHullFoamInsetCm = 130.0f;       // band starts inside the edge so it overlaps the hull (no waterline gap).
constexpr float kHullFoamBaseOp = 0.0f;          // OFF: the always-on elliptical ring read as an artificial
                                                 // white halo around the hull. The M_Ocean intersection foam
                                                 // (SceneDepth proximity) now does the waterline wash organically.
constexpr float kHullFoamSpeedOp = 0.22f;        // only a faint band when actually making way.
// Buoyancy: the visual hull rides the live Gerstner surface (server owns XY/heading).
constexpr float kBuoyancyTiltDamp = 0.26f;       // a big hull rides the AVERAGE slope, not corks.
// Hull-waterline spray puffs: small billboard ISM particles emitted where waves slap the hull.
constexpr double kSprayLifeS = 0.7;              // puff lifetime (seconds).
constexpr int32 kSprayMaxAlive = 130;            // global cap (translucent overdraw budget).
constexpr float kSprayYoungHalfM = 0.9f;         // billboard half-size at spawn (metres).
constexpr float kSprayWaveThreshCm = 25.0f;      // wave elevation above mean needed to emit (chop, not just big crests).
constexpr int32 kSprayEmitPoints = 20;           // points sampled around the hull ellipse per tick.
constexpr double kSprayGravityCms2 = 981.0;      // downward acceleration (cm/s^2).
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

// The VISIBLE ocean is M_Ocean's 32-wave Gerstner sum (setup_materials._ocean_waves). Mirror that
// EXACT spectrum here so the hull, wake, bow and hull-foam ride the surface the camera sees (the
// Water-plugin waves are smaller and a different phase). Render-only — see SampleSeaSurface.
namespace SeaOceanWaves
{
const float kWL[32]={18000.00f,15615.92f,13547.61f,11753.25f,10196.55f,8846.03f,7674.38f,6657.92f,5776.09f,5011.05f,4347.35f,3771.55f,3272.01f,2838.64f,2462.66f,2136.49f,1853.51f,1608.02f,1395.04f,1210.27f,1049.97f,910.90f,790.25f,685.59f,594.78f,516.00f,447.66f,388.37f,336.93f,292.30f,253.59f,220.00f};
const float kAM[32]={12.232f,11.393f,10.612f,9.884f,9.206f,8.575f,7.987f,7.439f,6.929f,6.454f,6.011f,5.599f,5.215f,4.857f,4.524f,4.214f,3.925f,3.656f,3.405f,3.172f,2.954f,2.752f,2.563f,2.387f,2.223f,2.071f,1.929f,1.797f,1.673f,1.559f,1.452f,1.352f};
const float kDX[32]={0.80572f,0.71226f,0.73611f,0.88748f,0.91514f,0.53321f,0.90633f,0.55548f,0.96706f,0.57566f,0.62237f,0.87118f,0.29534f,0.43499f,0.81383f,0.99956f,0.97439f,0.90046f,0.57258f,0.95842f,0.89776f,0.97294f,0.45435f,0.16039f,0.99897f,0.98803f,0.40538f,0.94757f,0.66676f,0.80935f,0.48629f,0.52019f};
const float kDY[32]={0.59230f,0.70192f,0.67686f,0.46085f,0.40315f,0.84598f,0.42258f,0.83153f,0.25455f,0.81769f,0.78272f,0.49096f,0.95539f,0.90044f,0.58110f,0.02983f,0.22488f,0.43494f,0.81985f,0.28535f,0.44047f,0.23108f,0.89082f,0.98705f,-0.04534f,-0.15426f,0.91415f,0.31956f,0.74527f,0.58733f,0.87380f,0.85405f};
const float kQF[32]={6.7335814f,6.2718216f,5.8417273f,5.4411270f,5.0679981f,4.7204568f,4.3967484f,4.0952385f,3.8144048f,3.5528295f,3.3091919f,3.0822618f,2.8708937f,2.6740203f,2.4906476f,2.3198498f,2.1607646f,2.0125887f,1.8745741f,1.7460240f,1.6262892f,1.5147654f,1.4108893f,1.3141367f,1.2240189f,1.1400810f,1.0618992f,0.9890787f,0.9212520f,0.8580765f,0.7992334f,0.7444254f};
const float kSP[32]={0.58518f,0.62826f,0.67452f,0.72418f,0.77750f,0.83474f,0.89620f,0.96218f,1.03302f,1.10907f,1.19073f,1.27839f,1.37252f,1.47357f,1.58206f,1.69853f,1.82359f,1.95785f,2.10200f,2.25675f,2.42291f,2.60129f,2.79281f,2.99843f,3.21919f,3.45620f,3.71066f,3.98385f,4.27716f,4.59207f,4.93016f,5.29314f};
}  // namespace SeaOceanWaves

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
	// Sample the M_Ocean Gerstner spectrum (SeaOceanWaves) on the SAME game-time clock the material's
	// WPO uses, so the hull, wake, bow and hull-foam ride the surface the camera actually sees (the
	// plugin's own waves are smaller and a different phase). Render-only: buoyancy/wake are purely
	// visual and the server /sim/ never reads the water -> determinism/golden are unaffected.
	UWorld* World = Owner != nullptr ? Owner->GetWorld() : nullptr;
	const double T = World != nullptr ? World->GetTimeSeconds() : 0.0;
	const double X = WorldXY.X, Y = WorldXY.Y;
	double H = 0.0, Nx = 0.0, Ny = 0.0, Jz = 0.0;
	for (int i = 0; i < 32; ++i)
	{
		const double k = 6.28318530718 / SeaOceanWaves::kWL[i];
		const double ph = k * (SeaOceanWaves::kDX[i] * X + SeaOceanWaves::kDY[i] * Y) + SeaOceanWaves::kSP[i] * T;
		const double c = FMath::Cos(ph), s = FMath::Sin(ph);
		const double wa = k * SeaOceanWaves::kAM[i];
		H  += SeaOceanWaves::kAM[i] * s;
		Nx += -SeaOceanWaves::kDX[i] * wa * c;
		Ny += -SeaOceanWaves::kDY[i] * wa * c;
		Jz += SeaOceanWaves::kQF[i] * wa * s;
	}
	OutHeightCm = static_cast<float>(H);
	FVector Normal(Nx, Ny, 1.0 - Jz);
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
	EmitSpray(Ctx.Now);
	UpdateSpray(Ctx.Now, Ctx.CamLoc);
}

void FWakeSystem::SampleWake(const FVector& ShipStage, const FVector& VelCms, double Now)
{
	const float Speed2D = static_cast<float>(VelCms.Size2D());
	// Deadzone: a near-stationary hull (buoyancy bob/drift only) makes no wake or bow wave; remap
	// so foam starts at kWakeMinSpeedCms, not at the first cm/s of wave-induced jitter.
	const float SpeedFrac =
	    Speed2D < static_cast<float>(kWakeMinSpeedCms)
	        ? 0.0f
	        : FMath::Clamp(static_cast<float>((Speed2D - kWakeMinSpeedCms) /
	                                          (kWakeFullSpeedCms - kWakeMinSpeedCms)),
	                       0.0f, 1.0f);
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
		// Foam opacity: ZERO when stopped (deadzone -> SpeedFrac 0), but once making way it has a
		// FLOOR so even a slow hull reads as a CLEAN CONTINUOUS wake, not faint dashes catching only
		// the wave crests (SpeedFrac was ~0.1 at cruise -> near-invisible broken foam). Fades astern.
		const float Opacity = (P.SpeedFrac <= 0.0f)
		                          ? 0.0f
		                          : FMath::Lerp(0.55f, 1.0f, P.SpeedFrac) *
		                                FMath::Pow(1.0f - AgeAlpha, 1.5f);

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

void FWakeSystem::EmitSpray(double Now)
{
	if (SprayISM == nullptr || LastShipSea.IsNearlyZero())
	{
		return;
	}
	const FVector Up = FVector::UpVector;
	const FVector Fwd = LastShipFwd;
	const FVector Side = FVector::CrossProduct(Fwd, Up).GetSafeNormal();
	const float LenHalf = ShipHalfLenCm;
	const float BeamHalf = ShipHalfLenCm * kHullFoamBeamRatio;

	for (int32 j = 0; j < kSprayEmitPoints; ++j)
	{
		const float Ang = (2.0f * PI) * (static_cast<float>(j) / kSprayEmitPoints);
		const FVector EdgeOffset = Fwd * (FMath::Cos(Ang) * LenHalf) + Side * (FMath::Sin(Ang) * BeamHalf);
		const FVector Edge = LastShipSea + EdgeOffset;

		// Wave elevation above the mean sea surface at this hull point.
		const float Elev = Surface->SeaSurfaceWorldZ(Edge) - static_cast<float>(SeaWorldFrame::Origin.Z);

		// Bow bias: emit more spray where the hull is cutting through water.
		const float Bow = FMath::Cos(Ang) > 0.5f ? 2.0f : 1.0f;

		if (Elev > kSprayWaveThreshCm && FMath::FRand() < 0.30f * Bow &&
		    Sprays.Num() < kSprayMaxAlive)
		{
			FSpray S;
			S.SpawnPos = FVector(Edge.X, Edge.Y, Surface->SeaSurfaceWorldZ(Edge) + 30.0f);
			// Outward radial direction in the horizontal plane.
			FVector R = EdgeOffset;
			R.Z = 0.0f;
			if (!R.Normalize())
			{
				R = Side;
			}
			S.Vel = R * FMath::FRandRange(120.0f, 260.0f) +
			        FVector::UpVector * FMath::FRandRange(280.0f, 560.0f);
			S.BaseHalfM = FMath::FRandRange(kSprayYoungHalfM * 0.7f, kSprayYoungHalfM * 1.3f);
			S.SpawnTime = Now;
			const int32 Idx = SprayISM->AddInstance(
			    FTransform(FQuat::Identity, S.SpawnPos, FVector(2.0f * S.BaseHalfM)), /*bWorldSpace=*/true);
			Sprays.Add(S);
			SprayISM->SetCustomDataValue(Idx, 0, 0.0f, false);
		}
	}
}

void FWakeSystem::UpdateSpray(double Now, const FVector& CamLoc)
{
	if (SprayISM == nullptr)
	{
		return;
	}
	// First pass: swap-remove expired sprays, mirroring by popping the LAST instance
	// so Sprays[i] <-> instance i stays index-locked (same pattern as UpdatePuffs).
	for (int32 i = Sprays.Num() - 1; i >= 0; --i)
	{
		const double Age = Now - Sprays[i].SpawnTime;
		if (Age >= kSprayLifeS)
		{
			Sprays.RemoveAtSwap(i, 1, EAllowShrinking::No);
			SprayISM->RemoveInstance(SprayISM->GetInstanceCount() - 1);
		}
	}
	// Second pass: closed-form ballistic position + camera-facing billboard, then batch-push.
	if (Sprays.IsEmpty())
	{
		return;
	}
	TArray<FTransform> Xforms;
	Xforms.Reserve(Sprays.Num());
	for (int32 i = 0; i < Sprays.Num(); ++i)
	{
		const FSpray& Sp = Sprays[i];
		const double Age = Now - Sp.SpawnTime;
		const float AgeFrac = FMath::Clamp(static_cast<float>(Age / kSprayLifeS), 0.0f, 1.0f);
		// Closed-form ballistic: pos = spawnPos + vel*t + 0.5*g*t^2 (no per-frame dt needed).
		const FVector Pos = Sp.SpawnPos + Sp.Vel * Age +
		                    FVector(0.0f, 0.0f, -0.5f * static_cast<float>(kSprayGravityCms2) *
		                                             static_cast<float>(Age * Age));
		// Expand as it ages (same balloon pattern as smoke puffs).
		const float S = 2.0f * Sp.BaseHalfM * (1.0f + 0.8f * AgeFrac);
		// Camera-facing billboard: plane's +Z faces the camera (same as smoke puffs).
		FVector ToCam = CamLoc - Pos;
		if (!ToCam.Normalize())
		{
			ToCam = FVector::UpVector;
		}
		const FQuat Face = FRotationMatrix::MakeFromZ(ToCam).ToQuat();
		Xforms.Add(FTransform(Face, Pos, FVector(S, S, 1.0f)));
		SprayISM->SetCustomDataValue(i, 0, AgeFrac, false);
	}
	SprayISM->BatchUpdateInstancesTransforms(0, Xforms, /*bWorldSpace=*/true,
	                                         /*bMarkRenderStateDirty=*/true, /*bTeleport=*/true);
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
