#include "SeaShieldGameModeBase.h"

#include "Camera/CameraActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/SpectatorPawn.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "TimerManager.h"
#include "UnrealClient.h"

#include "Blueprint/UserWidget.h"

#include "SeaAfterActionWidget.h"
#include "SeaEnvironmentController.h"
#include "SeaFireControlPanel.h"
#include "SeaGameHudWidget.h"
#include "SeaGunnerPawn.h"
#include "SeaGunsightWidget.h"
#include "SeaPpiWidget.h"
#include "SeaShieldPlayerController.h"
#include "SeaWorldFrame.h"
#include "SeaWorldManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogSeaShieldGame, Log, All);

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
	// The player IS the gun director: a first-person sight on the launcher that
	// the mouse aims and FIRE shoots down the bore.
	DefaultPawnClass = ASeaGunnerPawn::StaticClass();
	PlayerControllerClass = ASeaShieldPlayerController::StaticClass();
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

	// -SeaCinematic: the trailer / screenshot quality tier. The cvar PROFILE
	// (Tools/cinematic.cvars — full water tessellation, full-res reflections /
	// clouds, Lumen reflections, supersample) is applied earlier by -dpcvars via
	// the --cinematic harness flag, because several biases must land before the
	// renderer / water-mesh build. Here we CONFIRM the tier engaged by reading the
	// live cvar values into the log, so a cinematic capture run is self-verifying
	// without needing a ProfileGPU pass dump. (Phase-4 cinematic post-process —
	// DOF / motion-blur framing the action — will hook in here.)
	if (FParse::Param(FCommandLine::Get(), TEXT("SeaCinematic")))
	{
		const auto CVarFloat = [](const TCHAR* Name) -> float
		{
			const IConsoleVariable* V = IConsoleManager::Get().FindConsoleVariable(Name);
			return V != nullptr ? V->GetFloat() : -1.0f;
		};
		UE_LOG(LogSeaShieldGame, Display,
		       TEXT("SeaCinematic tier ACTIVE — ScreenPercentage=%.0f WaterTessBias=%.0f "
		            "WaterReflDownsample=%.0f CloudRTMode=%.0f ReflectionMethod=%.0f "
		            "LumenReflDownsample=%.0f"),
		       CVarFloat(TEXT("r.ScreenPercentage")),
		       CVarFloat(TEXT("r.Water.WaterMesh.TessFactorBias")),
		       CVarFloat(TEXT("r.Water.SingleLayer.Reflection.DownsampleFactor")),
		       CVarFloat(TEXT("r.VolumetricRenderTarget.Mode")),
		       CVarFloat(TEXT("r.ReflectionMethod")),
		       CVarFloat(TEXT("r.Lumen.Reflections.DownsampleFactor")));

		// Trailer / screenshot hygiene: suppress the engine's on-screen debug
		// messages (the persistent red text from dev-time warnings, e.g. a
		// material's "missing bUsedWith..." note) so cinematic stills are clean.
		// This is tier-scoped — gameplay / dev captures keep screen messages ON so
		// real warnings stay visible. It hides only GEngine debug TEXT, not the
		// UMG tactical HUD.
		if (GEngine != nullptr)
		{
			GEngine->Exec(GetWorld(), TEXT("DisableAllScreenMessages"));
		}
	}

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
	// Gun sight: center reticle + target designators over the 3D view.
	if (USeaGunsightWidget* Sight =
	        CreateWidget<USeaGunsightWidget>(GetWorld(), USeaGunsightWidget::StaticClass()))
	{
		Sight->AddToViewport(5);
	}
	// Survival-game HUD: wave / score / lives + transient banners (self-hides
	// outside game-mode scenarios).
	if (USeaGameHudWidget* GameHud =
	        CreateWidget<USeaGameHudWidget>(GetWorld(), USeaGameHudWidget::StaticClass()))
	{
		GameHud->AddToViewport(15);
	}
	// After-action review — hidden until the engagement ends (kEngagementEnd),
	// then centered over the scene. Above the HUD layer so it reads as a card.
	if (USeaAfterActionWidget* Aar =
	        CreateWidget<USeaAfterActionWidget>(GetWorld(), USeaAfterActionWidget::StaticClass()))
	{
		Aar->AddToViewport(20);
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

	// -SeaProfileGPU=<seconds>: after warmup, dump a hierarchical per-pass GPU
	// breakdown to the log (ProfileGPU with the UI off) so perf_summary.sh can
	// attribute frame cost per pass WITHOUT reading an on-screen stat overlay.
	// Pair with -SeaQuit to bound the run. -ExecCmds is inert in -game on 5.7,
	// so this fires from a timer via GEngine->Exec like -SeaStat.
	float ProfileGpuDelay = 0.0f;
	if (FParse::Value(FCommandLine::Get(), TEXT("SeaProfileGPU="), ProfileGpuDelay))
	{
		FTimerHandle ProfileTimer;
		GetWorld()->GetTimerManager().SetTimer(
		    ProfileTimer,
		    [WeakWorld = TWeakObjectPtr<UWorld>(GetWorld())]()
		    {
			    if (WeakWorld.IsValid() && GEngine != nullptr)
			    {
				    GEngine->Exec(WeakWorld.Get(), TEXT("r.ProfileGPU.ShowUI 0"));
				    GEngine->Exec(WeakWorld.Get(), TEXT("ProfileGPU"));
			    }
		    },
		    FMath::Max(ProfileGpuDelay, 1.0f), false);
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

	// -SeaShotSeq=<interval>: take a screenshot every <interval> seconds from the
	// player's own view (a filmstrip of the whole survival loop in one run) and
	// quit at -SeaShotSeqQuit (default 42 s). For verifying the playable game.
	float SeqInterval = 0.0f;
	if (FParse::Value(FCommandLine::Get(), TEXT("SeaShotSeq="), SeqInterval) && SeqInterval > 0.1f)
	{
		TSharedRef<int32> Counter = MakeShared<int32>(0);
		FTimerHandle SeqTimer;
		GetWorld()->GetTimerManager().SetTimer(
		    SeqTimer,
		    [Counter]()
		    {
			    const FString Name = FString::Printf(TEXT("SeaSeq%02d.png"), (*Counter)++);
			    FScreenshotRequest::RequestScreenshot(Name, /*bShowUI=*/true, false);
		    },
		    SeqInterval, true);
		float SeqQuit = 42.0f;
		FParse::Value(FCommandLine::Get(), TEXT("SeaShotSeqQuit="), SeqQuit);
		FTimerHandle SeqQuitTimer;
		GetWorld()->GetTimerManager().SetTimer(
		    SeqQuitTimer, []() { FPlatformMisc::RequestExit(false); }, SeqQuit, false);
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
