#include "SeaShieldPlayerController.h"

#include "Components/InputComponent.h"
#include "Engine/GameInstance.h"
#include "InputCoreTypes.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

#include "SeaNetSubsystem.h"

ASeaShieldPlayerController::ASeaShieldPlayerController()
{
	PrimaryActorTick.bCanEverTick = true;
}

USeaNetSubsystem* ASeaShieldPlayerController::Net() const
{
	const UGameInstance* GameInstance = GetGameInstance();
	return GameInstance != nullptr ? GameInstance->GetSubsystem<USeaNetSubsystem>() : nullptr;
}

void ASeaShieldPlayerController::BeginPlay()
{
	Super::BeginPlay();
	// Keep the cursor and let both the game (keys) and UI (PPI click) receive
	// input — the scope's click-to-designate lives in a Slate widget.
	bShowMouseCursor = true;
	SetInputMode(FInputModeGameAndUI());
	bOrderDemo = FParse::Param(FCommandLine::Get(), TEXT("SeaOrderDemo"));
}

void ASeaShieldPlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();
	if (InputComponent == nullptr)
	{
		return;
	}
	InputComponent->BindKey(EKeys::RightBracket, IE_Pressed, this, &ASeaShieldPlayerController::SalvoUp);
	InputComponent->BindKey(EKeys::LeftBracket, IE_Pressed, this, &ASeaShieldPlayerController::SalvoDown);
	InputComponent->BindKey(EKeys::Quote, IE_Pressed, this, &ASeaShieldPlayerController::DispersionUp);
	InputComponent->BindKey(EKeys::Semicolon, IE_Pressed, this, &ASeaShieldPlayerController::DispersionDown);
	InputComponent->BindKey(EKeys::Left, IE_Pressed, this, &ASeaShieldPlayerController::TrimAzLeft);
	InputComponent->BindKey(EKeys::Right, IE_Pressed, this, &ASeaShieldPlayerController::TrimAzRight);
	InputComponent->BindKey(EKeys::Up, IE_Pressed, this, &ASeaShieldPlayerController::TrimElUp);
	InputComponent->BindKey(EKeys::Down, IE_Pressed, this, &ASeaShieldPlayerController::TrimElDown);
	InputComponent->BindKey(EKeys::F, IE_Pressed, this, &ASeaShieldPlayerController::Fire);
}

void ASeaShieldPlayerController::SalvoUp()       { if (auto* N = Net()) N->AdjustSalvo(+1); }
void ASeaShieldPlayerController::SalvoDown()     { if (auto* N = Net()) N->AdjustSalvo(-1); }
void ASeaShieldPlayerController::DispersionUp()  { if (auto* N = Net()) N->AdjustDispersion(+0.5f); }
void ASeaShieldPlayerController::DispersionDown(){ if (auto* N = Net()) N->AdjustDispersion(-0.5f); }
void ASeaShieldPlayerController::TrimAzLeft()    { if (auto* N = Net()) N->AdjustTrim(-1.0f, 0.0f); }
void ASeaShieldPlayerController::TrimAzRight()   { if (auto* N = Net()) N->AdjustTrim(+1.0f, 0.0f); }
void ASeaShieldPlayerController::TrimElUp()      { if (auto* N = Net()) N->AdjustTrim(0.0f, +1.0f); }
void ASeaShieldPlayerController::TrimElDown()    { if (auto* N = Net()) N->AdjustTrim(0.0f, -1.0f); }
void ASeaShieldPlayerController::Fire()          { if (auto* N = Net()) N->CommitFire(); }

void ASeaShieldPlayerController::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!bOrderDemo)
	{
		return;
	}
	USeaNetSubsystem* N = Net();
	if (N == nullptr || !N->IsWelcomed())
	{
		return;
	}
	DemoElapsed += DeltaSeconds;
	switch (DemoStep)
	{
	case 0:  // Designate the demo target track.
		if (DemoElapsed > 2.0f) { N->DesignateTrack(1); ++DemoStep; }
		break;
	case 1:  // Build up the salvo (8 -> 12), one step at a time for the read-out.
		if (DemoElapsed > 4.0f) { N->AdjustSalvo(+4); ++DemoStep; }
		break;
	case 2:  // Widen the pattern (3.0 -> 5.0 mrad).
		if (DemoElapsed > 5.0f) { N->AdjustDispersion(+2.0f); ++DemoStep; }
		break;
	case 3:  // Nudge the trim.
		if (DemoElapsed > 6.0f) { N->AdjustTrim(+2.0f, +1.0f); ++DemoStep; }
		break;
	case 4:  // Fire once the solution has settled (worst case is the first one).
	{
		FSeaFireSolution Solution;
		if (N->GetFireSolution(1, Solution) && Solution.bValid && ++SettleStreak >= 16)
		{
			N->CommitFire();
			++DemoStep;
		}
		break;
	}
	default:
		break;
	}
}
