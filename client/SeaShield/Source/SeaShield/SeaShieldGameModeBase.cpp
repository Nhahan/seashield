#include "SeaShieldGameModeBase.h"

#include "Camera/CameraActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformMisc.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "TimerManager.h"
#include "UnrealClient.h"

#include "SeaEnvironmentController.h"
#include "SeaWorldManager.h"

namespace {

template <typename ActorType>
void EnsureSingleton(UWorld* World)
{
	if (TActorIterator<ActorType>(World))
	{
		return;  // Placed in the level (e.g. with tuned properties) — keep it.
	}
	World->SpawnActor<ActorType>();
}

}  // namespace

void ASeaShieldGameModeBase::BeginPlay()
{
	Super::BeginPlay();
	EnsureSingleton<ASeaWorldManager>(GetWorld());
	EnsureSingleton<ASeaEnvironmentController>(GetWorld());

	// Dev capture path: -SeaShot=<seconds> frames the standard quarter view,
	// writes Saved/Screenshots/<platform>/SeaShot.png after the delay, then
	// exits. In-game rendering exercises the real pipeline (water info
	// texture etc.) that editor offscreen captures miss.
	float ShotDelay = 0.0f;
	if (FParse::Value(FCommandLine::Get(), TEXT("SeaShot="), ShotDelay))
	{
		ACameraActor* Camera = GetWorld()->SpawnActor<ACameraActor>(
			FVector(-15000.0, -9000.0, 4000.0), FRotator(-13.0, 31.0, 0.0));
		if (APlayerController* Controller = GetWorld()->GetFirstPlayerController())
		{
			Controller->SetViewTarget(Camera);
		}
		ShotDelay = FMath::Max(ShotDelay, 0.5f);
		FTimerHandle ShotTimer;
		GetWorld()->GetTimerManager().SetTimer(
			ShotTimer,
			[]() { FScreenshotRequest::RequestScreenshot(TEXT("SeaShot.png"), false, false); },
			ShotDelay, false);
		FTimerHandle QuitTimer;
		GetWorld()->GetTimerManager().SetTimer(
			QuitTimer, []() { FPlatformMisc::RequestExit(false); }, ShotDelay + 3.0f, false);
	}
}
