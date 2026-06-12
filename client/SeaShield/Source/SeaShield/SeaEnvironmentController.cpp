#include "SeaEnvironmentController.h"

#include "Kismet/KismetMaterialLibrary.h"
#include "Materials/MaterialParameterCollection.h"
#include "NiagaraComponent.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraSystem.h"

#include "SeaNetSubsystem.h"

void ASeaEnvironmentController::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (bApplied)
	{
		return;  // The scenario weather is fixed for the engagement; apply once.
	}
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
	const FSeaWeather Weather = Net->GetWeather();
	bApplied = true;

	if (WeatherParameters != nullptr)
	{
		UWorld* World = GetWorld();
		const FVector WindDir = Weather.WindCms.GetSafeNormal();
		UKismetMaterialLibrary::SetVectorParameterValue(
		    World, WeatherParameters, TEXT("WindDir"),
		    FLinearColor(WindDir.X, WindDir.Y, 0.0f, 0.0f));
		UKismetMaterialLibrary::SetScalarParameterValue(World, WeatherParameters,
		                                                TEXT("WindSpeed"),
		                                                Weather.WindCms.Size() / 100.0f);  // m/s
		UKismetMaterialLibrary::SetScalarParameterValue(World, WeatherParameters,
		                                                TEXT("RainIntensity"),
		                                                Weather.RainIntensity);
		UKismetMaterialLibrary::SetScalarParameterValue(World, WeatherParameters,
		                                                TEXT("GustSigma"), Weather.GustSigmaMps);
	}

	if (RainSystem != nullptr && Weather.RainIntensity > 0.0f && RainComponent == nullptr)
	{
		RainComponent = UNiagaraFunctionLibrary::SpawnSystemAttached(
		    RainSystem, GetRootComponent(), NAME_None, FVector::ZeroVector, FRotator::ZeroRotator,
		    EAttachLocation::KeepRelativeOffset, /*bAutoDestroy=*/false);
		if (RainComponent != nullptr)
		{
			RainComponent->SetFloatParameter(TEXT("SpawnRate"),
			                                 RainRateAtFullIntensity * Weather.RainIntensity);
			RainComponent->SetVectorParameter(TEXT("Wind"), Weather.WindCms);
		}
	}
}
