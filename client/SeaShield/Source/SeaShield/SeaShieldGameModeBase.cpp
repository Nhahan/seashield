#include "SeaShieldGameModeBase.h"

#include "Camera/CameraActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/SpectatorPawn.h"
#include "HAL/PlatformMisc.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "TimerManager.h"
#include "UnrealClient.h"

#include "Blueprint/UserWidget.h"

#include "SeaEnvironmentController.h"
#include "SeaFireControlPanel.h"
#include "SeaPpiWidget.h"
#include "SeaWorldFrame.h"
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

ASeaShieldGameModeBase::ASeaShieldGameModeBase()
{
	// The flying DefaultPawn carries a visible sphere mesh that photobombs
	// captures; the spectator pawn flies the same but draws nothing.
	DefaultPawnClass = ASpectatorPawn::StaticClass();
}

AActor* ASeaShieldGameModeBase::ChoosePlayerStart_Implementation(AController* Player)
{
	if (!LazyPlayerStart.IsValid())
	{
		// Bridge-wing vantage: abaft the port quarter, conning-height, looking
		// over the bow toward the threat axis.
		const FVector Spot = SeaWorldFrame::Origin + FVector(-16000.0, -9000.0, 2500.0);
		const FRotator Facing(0.0, FMath::RadiansToDegrees(FMath::Atan2(9000.0, 16000.0)), 0.0);
		LazyPlayerStart = GetWorld()->SpawnActor<APlayerStart>(Spot, Facing);
	}
	return LazyPlayerStart.IsValid() ? LazyPlayerStart.Get()
	                                 : Super::ChoosePlayerStart_Implementation(Player);
}

void ASeaShieldGameModeBase::BeginPlay()
{
	Super::BeginPlay();
	EnsureSingleton<ASeaWorldManager>(GetWorld());
	EnsureSingleton<ASeaEnvironmentController>(GetWorld());

	// The tactical HUD: PPI scope (bottom-left) + fire-control console
	// (bottom-right). Pure C++ widgets — no UMG assets to wire. Both gate
	// themselves on welcome/role (Observer sees the bare 3D world).
	// AddToViewport's default slot is already a fullscreen stretch; each
	// widget places itself via its own internal canvas slot. Do NOT touch the
	// viewport slot (SetPositionInViewport et al.) — on 5.7 that perturbs the
	// stretch into a desired-size box pinned top-left.
	USeaPpiWidget* Ppi = CreateWidget<USeaPpiWidget>(GetWorld(), USeaPpiWidget::StaticClass());
	if (Ppi != nullptr)
	{
		Ppi->AddToViewport(10);
		Ppi->SelectTrack(1);  // Demo default; scope clicks re-designate.
	}
	if (USeaFireControlPanel* Panel =
	        CreateWidget<USeaFireControlPanel>(GetWorld(), USeaFireControlPanel::StaticClass()))
	{
		Panel->PpiRef = Ppi;
		Panel->AddToViewport(10);
	}

	// Dev capture path: -SeaShot=<seconds> frames the standard quarter view,
	// writes Saved/Screenshots/<platform>/SeaShot.png after the delay, then
	// exits. In-game rendering exercises the real pipeline (water info
	// texture etc.) that editor offscreen captures miss. -SeaShotFromPawn
	// keeps the player's own (PlayerStart) view instead of a spawned camera.
	// Event-triggered captures (-SeaShotOnBurst/-SeaShotOnSplash) need a
	// fallback exit for runs where the event never happens (all-miss salvos).
	if (FParse::Param(FCommandLine::Get(), TEXT("SeaShotOnBurst")) ||
	    FParse::Param(FCommandLine::Get(), TEXT("SeaShotOnSplash")))
	{
		FTimerHandle FallbackQuit;
		GetWorld()->GetTimerManager().SetTimer(
		    FallbackQuit, []() { FPlatformMisc::RequestExit(false); }, 100.0f, false);
	}

	// -SeaStat=<name>: toggle a stat display (e.g. gpu, unit) once the world
	// is live — -ExecCmds proved unreliable for stats in -game on 5.7.
	FString StatName;
	if (FParse::Value(FCommandLine::Get(), TEXT("SeaStat="), StatName))
	{
		FTimerHandle StatTimer;
		GetWorld()->GetTimerManager().SetTimer(
		    StatTimer,
		    [WeakWorld = TWeakObjectPtr<UWorld>(GetWorld()), StatName]()
		    {
			    if (WeakWorld.IsValid() && GEngine != nullptr)
			    {
				    GEngine->Exec(WeakWorld.Get(),
				                  *FString::Printf(TEXT("stat %s"), *StatName));
			    }
		    },
		    1.5f, false);
	}

	// -SeaQuit=<seconds>: exit with NO screenshot — the screenshot's GPU
	// readback is itself a ~400 ms frame (hitch forensics), so performance
	// measurement runs must not take one.
	float QuitDelay = 0.0f;
	if (FParse::Value(FCommandLine::Get(), TEXT("SeaQuit="), QuitDelay))
	{
		FTimerHandle QuitOnly;
		GetWorld()->GetTimerManager().SetTimer(
		    QuitOnly, []() { FPlatformMisc::RequestExit(false); }, FMath::Max(QuitDelay, 1.0f),
		    false);
	}

	float ShotDelay = 0.0f;
	if (FParse::Value(FCommandLine::Get(), TEXT("SeaShot="), ShotDelay))
	{
		if (!FParse::Param(FCommandLine::Get(), TEXT("SeaShotFromPawn")))
		{
			// SeaShotX/Y/Z are stage-frame overrides; the default quarter view
			// frames the ownship at SeaWorldFrame::Origin.
			FVector CameraLocation = SeaWorldFrame::Origin + FVector(-15000.0, -9000.0, 4000.0);
			FParse::Value(FCommandLine::Get(), TEXT("SeaShotX="), CameraLocation.X);
			FParse::Value(FCommandLine::Get(), TEXT("SeaShotY="), CameraLocation.Y);
			FParse::Value(FCommandLine::Get(), TEXT("SeaShotZ="), CameraLocation.Z);
			FRotator CameraRotation(-13.0, 31.0, 0.0);
			FParse::Value(FCommandLine::Get(), TEXT("SeaShotPitch="), CameraRotation.Pitch);
			FParse::Value(FCommandLine::Get(), TEXT("SeaShotYaw="), CameraRotation.Yaw);
			ACameraActor* Camera = GetWorld()->SpawnActor<ACameraActor>(CameraLocation, CameraRotation);
			if (APlayerController* Controller = GetWorld()->GetFirstPlayerController())
			{
				Controller->SetViewTarget(Camera);
			}
		}
		ShotDelay = FMath::Max(ShotDelay, 0.5f);
		FTimerHandle ShotTimer;
		GetWorld()->GetTimerManager().SetTimer(
			ShotTimer,
			[]() { FScreenshotRequest::RequestScreenshot(TEXT("SeaShot.png"), /*bShowUI=*/true, false); },
			ShotDelay, false);
		FTimerHandle QuitTimer;
		GetWorld()->GetTimerManager().SetTimer(
			QuitTimer, []() { FPlatformMisc::RequestExit(false); }, ShotDelay + 3.0f, false);
	}
}
