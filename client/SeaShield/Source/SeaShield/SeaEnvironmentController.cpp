#include "SeaEnvironmentController.h"

#include "Components/ExponentialHeightFogComponent.h"
#include "Engine/ExponentialHeightFog.h"
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
	for (TActorIterator<AWaterBody> It(GetWorld()); It; ++It)
	{
		Ocean = *It;
		break;
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
	const float MaxAmplitudeCm = FMath::Clamp(8.0 + FMath::Pow(EffectiveWind, 1.7), 8.0, 150.0);
	const float MinAmplitudeCm = MaxAmplitudeCm / 10.0f;
	const float MaxWavelengthCm = FMath::Clamp(4000.0 + EffectiveWind * 700.0, 6000.0, 18000.0);
	const float MinWavelengthCm = 700.0f;
	const float SpreadDeg = FMath::Clamp(110.0 - EffectiveWind * 3.5, 45.0, 110.0);

	USeaLevelSetupLibrary::AssignGeneratedOceanWaves(
	    Ocean, /*Seed=*/7, /*NumWaves=*/32, MinWavelengthCm, MaxWavelengthCm, MinAmplitudeCm,
	    MaxAmplitudeCm, static_cast<float>(WindBearingDeg), SpreadDeg,
	    /*SmallWaveSteepness=*/0.18f, /*LargeWaveSteepness=*/0.12f);
	UE_LOG(LogSeaEnvironment, Display,
	       TEXT("Sea state applied: wind %.1f m/s @ %.0fdeg gust_sigma %.2f -> amp<=%.0f cm wl<=%.0f cm"),
	       WindSpeedMps, WindBearingDeg, Weather.GustSigmaMps, MaxAmplitudeCm, MaxWavelengthCm);
}

void ASeaEnvironmentController::ApplyFog(const FSeaWeather& Weather) const
{
	if (Weather.RainIntensity <= 0.0f)
	{
		return;  // Keep the level's clear-weather fog as authored.
	}
	for (TActorIterator<AExponentialHeightFog> It(GetWorld()); It; ++It)
	{
		if (UExponentialHeightFogComponent* Fog = It->GetComponent())
		{
			// 0.02 is the engine default "clear" density; rain thickens the
			// air toward a ~0.1 squall murk that genuinely eats the horizon.
			const float Density = 0.02f + 0.08f * Weather.RainIntensity;
			Fog->SetFogDensity(Density);
			Fog->SetStartDistance(0.0f);
			UE_LOG(LogSeaEnvironment, Display, TEXT("Rain fog applied: density %.3f (rain %.2f)"),
			       Density, Weather.RainIntensity);
		}
		break;
	}
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
