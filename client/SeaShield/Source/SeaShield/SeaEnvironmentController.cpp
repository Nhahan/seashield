#include "SeaEnvironmentController.h"

#include "Components/ExponentialHeightFogComponent.h"
#include "Components/LocalFogVolumeComponent.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/LocalFogVolume.h"
#include "EngineUtils.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetMaterialLibrary.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialParameterCollection.h"
#include "ProceduralMeshComponent.h"
#include "UObject/ConstructorHelpers.h"
#include "WaterBodyActor.h"

#include "SeaLevelSetupLibrary.h"
#include "SeaNetSubsystem.h"

DEFINE_LOG_CATEGORY_STATIC(LogSeaEnvironment, Log, All);

namespace {
// Camera-relative rain volume (cm). Tall above the camera so streaks never
// visibly "begin", shallow below since the sea swallows them anyway.
constexpr float kRainHalfXY = 700.0f;
constexpr float kRainZMin = -400.0f;
constexpr float kRainZMax = 650.0f;
}  // namespace

void ASeaEnvironmentController::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (!bApplied)
	{
		const UGameInstance* GameInstance = GetGameInstance();
		if (GameInstance == nullptr)
		{
			return;
		}
		const USeaNetSubsystem* Net = GameInstance->GetSubsystem<USeaNetSubsystem>();
		if (Net == nullptr || !Net->IsWelcomed())
		{
			return;
		}
		CachedWeather = Net->GetWeather();
		bApplied = true;

		ApplySeaState(CachedWeather);
		ApplyFog(CachedWeather);
		ApplySeaMist(CachedWeather);

		if (WeatherParameters != nullptr)
		{
			UWorld* World = GetWorld();
			const FVector WindDir = CachedWeather.WindCms.GetSafeNormal();
			UKismetMaterialLibrary::SetVectorParameterValue(
			    World, WeatherParameters, TEXT("WindDir"),
			    FLinearColor(WindDir.X, WindDir.Y, 0.0f, 0.0f));
			UKismetMaterialLibrary::SetScalarParameterValue(World, WeatherParameters,
			                                                TEXT("WindSpeed"),
			                                                CachedWeather.WindCms.Size() / 100.0f);
			UKismetMaterialLibrary::SetScalarParameterValue(World, WeatherParameters,
			                                                TEXT("RainIntensity"),
			                                                CachedWeather.RainIntensity);
			UKismetMaterialLibrary::SetScalarParameterValue(World, WeatherParameters,
			                                                TEXT("GustSigma"),
			                                                CachedWeather.GustSigmaMps);
		}
	}
	UpdateRain(DeltaTime);
}

void ASeaEnvironmentController::ApplySeaState(const FSeaWeather& Weather) const
{
	AWaterBody* Ocean = nullptr;
	TActorIterator<AWaterBody> OceanIt(GetWorld());  // first water body, if any (UE5.8 -Werror flags the old grab-then-break loop's unreachable ++It)
	if (OceanIt)
	{
		Ocean = *OceanIt;
	}
	if (Ocean == nullptr)
	{
		UE_LOG(LogSeaEnvironment, Warning, TEXT("ApplySeaState: no water body in level"));
		return;
	}

	// UE frame: X = north, Y = east; the generator's wind angle is degrees
	// from +X toward +Y, which matches a compass bearing directly.
	const double WindSpeedMps = Weather.WindCms.Size() / 100.0;
	const double WindBearingDeg =
	    FMath::RadiansToDegrees(FMath::Atan2(Weather.WindCms.Y, Weather.WindCms.X));

	// Sea-state mapping, gusts included as effective wind. Exponent 1.7 tracks
	// wave energy growth: 2 m/s -> ~11 cm chop, 7 m/s -> ~35 cm, 16 m/s ->
	// ~1.2 m seas. Wavelengths stretch with the wind; the directional spread
	// narrows as the wind organizes the sea.
	const double EffectiveWind = WindSpeedMps + 0.5 * Weather.GustSigmaMps;
	// The hull is buoyancy-coupled (SeaWorldManager rides the Gerstner surface), so the
	// sea can run as a real swell without the fixed-Z hull clipping. Scale the wind-driven
	// amplitude up into metre-scale crests that break + catch the foam.
	const float MaxAmplitudeCm = FMath::Clamp((16.0 + FMath::Pow(EffectiveWind, 1.7)) * 1.5, 35.0, 240.0);
	const float MinAmplitudeCm = MaxAmplitudeCm / 10.0f;
	const float MaxWavelengthCm = FMath::Clamp(4000.0 + EffectiveWind * 700.0, 6000.0, 18000.0);
	const float MinWavelengthCm = 700.0f;
	const float SpreadDeg = FMath::Clamp(110.0 - EffectiveWind * 3.5, 45.0, 110.0);

	// Sharper crests (was 0.18/0.12, gently rounded) so the sea reads as a real,
	// breaking swell and the Gerstner peaks rise enough to catch the MI_SeaOcean foam.
	USeaLevelSetupLibrary::AssignGeneratedOceanWaves(
	    Ocean, /*Seed=*/7, /*NumWaves=*/32, MinWavelengthCm, MaxWavelengthCm, MinAmplitudeCm,
	    MaxAmplitudeCm, static_cast<float>(WindBearingDeg), SpreadDeg,
	    /*SmallWaveSteepness=*/0.42f, /*LargeWaveSteepness=*/0.30f);
	UE_LOG(LogSeaEnvironment, Display,
	       TEXT("Sea state applied: wind %.1f m/s @ %.0fdeg gust_sigma %.2f -> amp<=%.0f cm wl<=%.0f cm"),
	       WindSpeedMps, WindBearingDeg, Weather.GustSigmaMps, MaxAmplitudeCm, MaxWavelengthCm);
}

void ASeaEnvironmentController::ApplyFog(const FSeaWeather& Weather) const
{
	TActorIterator<AExponentialHeightFog> FogIt(GetWorld());  // first height fog, if any (UE5.8 -Werror: unreachable ++It)
	if (!FogIt)
	{
		return;
	}
	UExponentialHeightFogComponent* Fog = FogIt->GetComponent();
	if (Fog == nullptr)
	{
		return;
	}
	// The sky's moisture IS the visibility the fire-control problem loses, and it is
	// seed-random per engagement (sim::Weather.humidity). Humid air hazes the distant
	// horizon even with NO rain; rain thickens it into a near squall murk. 0.020 = the
	// authored clear base. The Fog Screen-Space Scattering (enable_fsss, set in level
	// setup) makes this fog physically scatter the sun/sky through it.
	const float WindMps = Weather.WindCms.Size() / 100.0f;
	const float Calm = 1.0f - FMath::Clamp(WindMps / 6.0f, 0.0f, 1.0f);
	const float MistAmt = FMath::Clamp((Weather.Humidity - 0.40f) / 0.60f, 0.0f, 1.0f);  // 0 below 40% RH
	// Calm humid air -> low sea fog that hugs the ship; rain -> a near squall murk. SeaFog pulls the
	// veil in CLOSE (not just a distant horizon haze) so the weather is felt around the ownship.
	const float SeaFog = FMath::Max(MistAmt * Calm, Weather.RainIntensity);
	const float Density = 0.015f + 0.10f * MistAmt + 0.11f * Weather.RainIntensity;  // clear .015 -> mist ~.107 -> rain ~.16
	Fog->SetFogDensity(Density);
	Fog->SetStartDistance(FMath::Lerp(72000.0f, 6000.0f, SeaFog));  // clear 720 m horizon haze -> sea fog 60 m around the ship
	// UE5.8 volumetric fog (enabled statically on the component in apply_ocean/setup_level) renders the
	// near/mid mist as REAL 3D god-ray scattering — the low sun shafts THROUGH the 해무. The enable flag has
	// no runtime setter, but the punch + phase do: drive them from the seed weather so humid/rain seeds
	// scatter the sun hard (thick glowing mist) and clear seeds fade it to near-nothing. (These are no-ops
	// if volumetric fog is off, e.g. a perf fallback — harmless.)
	Fog->SetVolumetricFogExtinctionScale(FMath::Lerp(0.5f, 2.2f, SeaFog));        // clear ~0.5 -> heavy fog 2.2: denser scatter
	Fog->SetVolumetricFogScatteringDistribution(FMath::Lerp(0.15f, 0.55f, MistAmt));  // humid calm air -> tighter forward sun glow
	UE_LOG(LogSeaEnvironment, Display,
	       TEXT("Fog applied: density %.3f start %.0f vol_ext %.2f (humidity %.2f, rain %.2f, calm %.2f)"),
	       Density, FMath::Lerp(72000.0f, 6000.0f, SeaFog), FMath::Lerp(0.5f, 2.2f, SeaFog),
	       Weather.Humidity, Weather.RainIntensity, Calm);
}

void ASeaEnvironmentController::ApplySeaMist(const FSeaWeather& Weather)
{
	// Sea mist / fog banks form in CALM, HUMID air over water (advection/radiation fog)
	// and are torn apart by wind. A low Local Fog Volume (UE5.8) hugging the sea; its
	// density is seed-random (humidity × calm), so each engagement draws a different
	// morning. FogPhaseG forward-scatters the sun -> the mist glows toward the light.
	const float WindMps = Weather.WindCms.Size() / 100.0f;
	const float Calm = 1.0f - FMath::Clamp(WindMps / 6.0f, 0.0f, 1.0f);              // >6 m/s tears it away
	const float Humid = FMath::Clamp((Weather.Humidity - 0.5f) * 2.5f, 0.0f, 1.0f);  // needs >0.5 RH
	const float Mist = Humid * Calm;  // 0 = clear, 1 = thick sea fog

	if (SeaMist == nullptr)
	{
		// Centre the low dome on the ocean surface (works for both the gameplay L_Range
		// and the cinematic L_RangeCustom, which share the water body's stage position).
		FVector Centre(0.0f, 0.0f, 0.0f);
		TActorIterator<AWaterBody> WaterIt(GetWorld());
		if (WaterIt)
		{
			Centre = WaterIt->GetActorLocation();
		}
		SeaMist = GetWorld()->SpawnActor<ALocalFogVolume>(Centre, FRotator::ZeroRotator);
	}
	if (SeaMist == nullptr)
	{
		return;
	}
	ULocalFogVolumeComponent* Vol = SeaMist->GetComponent();
	if (Vol == nullptr)
	{
		return;
	}
	// Scale the COMPONENT (the AInfo root may not carry the actor scale into the fog) -> a wide, low mist
	// SHEET over the sea (~4 km across, ~150 m tall), not a 5 m ball at the origin.
	Vol->SetWorldScale3D(FVector(800.0f, 800.0f, 30.0f));
	Vol->SetVisibility(Mist > 0.01f, true);
	Vol->SetRadialFogExtinction(Mist * 1.2f);   // extinction >1 reads clearly
	Vol->SetHeightFogExtinction(Mist * 2.0f);   // height-distributed -> hugs the surface
	Vol->SetHeightFogFalloff(120.0f);           // a thick, visible mist layer (lower = larger transition)
	Vol->SetFogPhaseG(0.6f);                    // forward scatter -> bright toward the sun
	UE_LOG(LogSeaEnvironment, Display,
	       TEXT("Sea mist applied: mist %.2f (humidity %.2f, wind %.1f m/s, calm %.2f)"),
	       Mist, Weather.Humidity, WindMps, Calm);
}

void ASeaEnvironmentController::UpdateRain(float DeltaTime)
{
	const float Intensity = bApplied ? CachedWeather.RainIntensity : 0.0f;
	if (Intensity <= 0.0f)
	{
		return;
	}
	APlayerCameraManager* Camera = UGameplayStatics::GetPlayerCameraManager(GetWorld(), 0);
	if (Camera == nullptr)
	{
		return;
	}

	if (RainMesh == nullptr)
	{
		RainMesh = NewObject<UProceduralMeshComponent>(this);
		RainMesh->RegisterComponent();
		RainMesh->AttachToComponent(GetRootComponent(),
		                            FAttachmentTransformRules::KeepWorldTransform);
		RainMesh->SetWorldTransform(FTransform::Identity);
		RainMesh->SetCastShadow(false);
		RainMaterial = LoadObject<UMaterialInterface>(
		    nullptr, TEXT("/Game/SeaShield/Materials/M_Rain"));
		if (RainMaterial != nullptr)
		{
			RainMesh->SetMaterial(0, RainMaterial);
		}
	}

	const int32 Want = FMath::RoundToInt(RainDropsAtFullIntensity * Intensity);
	while (Drops.Num() < Want)
	{
		Drops.Add(FVector(FMath::FRandRange(-kRainHalfXY, kRainHalfXY),
		                  FMath::FRandRange(-kRainHalfXY, kRainHalfXY),
		                  FMath::FRandRange(kRainZMin, kRainZMax)));
	}
	if (Drops.Num() > Want)
	{
		Drops.SetNum(Want);
	}

	// The fall vector IS the weather: terminal velocity down, simulation wind
	// across — every streak leans the way the trail ribbons drift.
	const FVector Fall = CachedWeather.WindCms + FVector(0.0, 0.0, -RainFallSpeedCms);
	const FVector Step = Fall * DeltaTime;
	for (FVector& Drop : Drops)
	{
		Drop += Step;
		Drop.X = FMath::Wrap(Drop.X, double(-kRainHalfXY), double(kRainHalfXY));
		Drop.Y = FMath::Wrap(Drop.Y, double(-kRainHalfXY), double(kRainHalfXY));
		if (Drop.Z < kRainZMin)
		{
			Drop.Z += (kRainZMax - kRainZMin);
			Drop.X = FMath::FRandRange(-kRainHalfXY, kRainHalfXY);
			Drop.Y = FMath::FRandRange(-kRainHalfXY, kRainHalfXY);
		}
	}

	const FVector CameraLocation = Camera->GetCameraLocation();
	const FVector StreakDir = Fall.GetSafeNormal();
	const float StreakLength = FMath::Clamp(Fall.Size() * 0.035f, 25.0f, 70.0f);
	const float HalfWidth = 0.8f;

	const int32 Count = Drops.Num();
	TArray<FVector> Vertices;
	TArray<int32> Triangles;
	TArray<FLinearColor> Colors;
	Vertices.Reserve(Count * 4);
	Colors.Reserve(Count * 4);
	Triangles.Reserve(Count * 6);
	for (const FVector& Drop : Drops)
	{
		const FVector Head = CameraLocation + Drop;
		const FVector Tail = Head - StreakDir * StreakLength;
		FVector Side = FVector::CrossProduct(StreakDir, Drop.GetSafeNormal());
		if (!Side.Normalize())
		{
			Side = FVector::RightVector;
		}
		// Nearer streaks read stronger; the far half of the volume dissolves
		// into the fog instead of popping at the wrap boundary. Drops almost
		// AT the lens would smear into giant white dashes — fade those out
		// the way a real lens defocuses them.
		const float Near = 1.0f - FMath::Clamp(Drop.Size2D() / kRainHalfXY, 0.0f, 1.0f);
		const float LensFade = FMath::Clamp((Drop.Size() - 60.0f) / 150.0f, 0.0f, 1.0f);
		const FLinearColor Color(1.0f, 1.0f, 1.0f, (0.16f + 0.55f * Near) * LensFade);

		const int32 Base = Vertices.Num();
		Vertices.Add(Head - Side * HalfWidth);
		Vertices.Add(Head + Side * HalfWidth);
		Vertices.Add(Tail - Side * HalfWidth);
		Vertices.Add(Tail + Side * HalfWidth);
		for (int32 i = 0; i < 4; ++i)
		{
			Colors.Add(Color);
		}
		Triangles.Append({Base, Base + 2, Base + 1, Base + 1, Base + 2, Base + 3});
	}
	RainMesh->CreateMeshSection_LinearColor(0, Vertices, Triangles, /*Normals=*/{}, /*UVs=*/{},
	                                        Colors, /*Tangents=*/{}, /*bCreateCollision=*/false);
}
