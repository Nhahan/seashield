#include "SeaShieldPlayerController.h"

#include "Components/InputComponent.h"
#include "Engine/GameInstance.h"
#include "InputCoreTypes.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

#include "SeaBallistics.h"
#include "SeaGunnerPawn.h"
#include "SeaNetSubsystem.h"

DEFINE_LOG_CATEGORY_STATIC(LogSeaInput, Log, All);

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
	// Gunnery is mouse-look: capture the cursor so mouse motion aims the
	// launcher instead of moving a pointer.
	bShowMouseCursor = false;
	SetInputMode(FInputModeGameOnly());
	bOrderDemo = FParse::Param(FCommandLine::Get(), TEXT("SeaOrderDemo"));
	bGunFireDemo = FParse::Param(FCommandLine::Get(), TEXT("SeaGunFire"));
	bGamePlayDemo = FParse::Param(FCommandLine::Get(), TEXT("SeaGamePlay"));
	bManualPlayDemo = FParse::Param(FCommandLine::Get(), TEXT("SeaManualPlay"));
	if (FParse::Param(FCommandLine::Get(), TEXT("SeaLeadDemo")))
	{
		bManualPlayDemo = true;  // reuse the manual-play loop...
		bNoLeadAim = true;       // ...but aim with no lead to force a measurable miss.
	}
	UE_LOG(LogSeaInput, Display,
	       TEXT("PC BeginPlay: gunFire=%d orderDemo=%d gamePlay=%d manualPlay=%d pawn=%s"),
	       bGunFireDemo, bOrderDemo, bGamePlayDemo, bManualPlayDemo, *GetNameSafe(GetPawn()));
}

ASeaGunnerPawn* ASeaShieldPlayerController::Gunner() const
{
	return Cast<ASeaGunnerPawn>(GetPawn());
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
	InputComponent->BindKey(EKeys::SpaceBar, IE_Pressed, this, &ASeaShieldPlayerController::Fire);
	InputComponent->BindKey(EKeys::LeftMouseButton, IE_Pressed, this, &ASeaShieldPlayerController::Fire);
	InputComponent->BindAxisKey(EKeys::MouseX, this, &ASeaShieldPlayerController::Turn);
	InputComponent->BindAxisKey(EKeys::MouseY, this, &ASeaShieldPlayerController::LookUp);
}

void ASeaShieldPlayerController::Turn(float AxisValue)
{
	if (AxisValue != 0.0f) { if (ASeaGunnerPawn* G = Gunner()) { G->AddAzimuth(AxisValue * MouseSensitivity); } }
}

void ASeaShieldPlayerController::LookUp(float AxisValue)
{
	if (AxisValue != 0.0f) { if (ASeaGunnerPawn* G = Gunner()) { G->AddElevation(AxisValue * MouseSensitivity); } }
}

void ASeaShieldPlayerController::SalvoUp()       { if (auto* N = Net()) N->AdjustSalvo(+1); }
void ASeaShieldPlayerController::SalvoDown()     { if (auto* N = Net()) N->AdjustSalvo(-1); }
void ASeaShieldPlayerController::DispersionUp()  { if (auto* N = Net()) N->AdjustDispersion(+0.5f); }
void ASeaShieldPlayerController::DispersionDown(){ if (auto* N = Net()) N->AdjustDispersion(-0.5f); }
void ASeaShieldPlayerController::TrimAzLeft()    { if (auto* N = Net()) N->AdjustTrim(-1.0f, 0.0f); }
void ASeaShieldPlayerController::TrimAzRight()   { if (auto* N = Net()) N->AdjustTrim(+1.0f, 0.0f); }
void ASeaShieldPlayerController::TrimElUp()      { if (auto* N = Net()) N->AdjustTrim(0.0f, +1.0f); }
void ASeaShieldPlayerController::TrimElDown()    { if (auto* N = Net()) N->AdjustTrim(0.0f, -1.0f); }
void ASeaShieldPlayerController::Fire()
{
	USeaNetSubsystem* N = Net();
	if (N == nullptr)
	{
		return;
	}
	// Gunnery: fire the salvo down the current bore (manual az/el). Falls back
	// to a track-designated order if no gunner pawn is possessed.
	if (const ASeaGunnerPawn* G = Gunner())
	{
		UE_LOG(LogSeaInput, Display, TEXT("Fire: manual az=%.1f el=%.1f salvo=%d disp=%.1f"),
		       G->AimAzimuthDeg(), G->AimElevationDeg(), N->GetOrderSalvo(),
		       N->GetOrderDispersionMrad());
		N->FireManual(G->AimAzimuthDeg(), G->AimElevationDeg(), N->GetOrderSalvo(),
		              N->GetOrderDispersionMrad());
	}
	else
	{
		UE_LOG(LogSeaInput, Display, TEXT("Fire: no gunner pawn -> CommitFire"));
		N->CommitFire();
	}
}

void ASeaShieldPlayerController::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!bOrderDemo && !bGunFireDemo && !bGamePlayDemo && !bManualPlayDemo)
	{
		return;  // Live play needs no per-tick work; only the scripted demos do.
	}
	USeaNetSubsystem* N = Net();
	if (N == nullptr || !N->IsWelcomed())
	{
		return;
	}
	if (bGamePlayDemo)
	{
		TickGamePlayDemo(DeltaSeconds);
		return;
	}
	if (bManualPlayDemo)
	{
		TickManualPlayDemo(DeltaSeconds);
		return;
	}
	// -SeaGunFire: one scripted manual salvo down the current bore (~6 s in),
	// so the aim->FireManual->ballistic-flight path is capture-verifiable.
	if (bGunFireDemo)
	{
		DemoElapsed += DeltaSeconds;
		if (!bGunFired && DemoElapsed > 6.0f)
		{
			bGunFired = true;
			// Auto-slew onto the first target so the capture shows the reticle
			// and designator on it and the salvo flying its way (a human does
			// this by mouse). Aim straight at it plus a little loft for the arc.
			if (ASeaGunnerPawn* G = Gunner())
			{
				TArray<FSeaEntityState> Entities;
				N->SampleEntities(Entities);
				for (const FSeaEntityState& E : Entities)
				{
					if (E.Kind != ESeaEntityKind::Target) { continue; }
					const float Az = FMath::RadiansToDegrees(FMath::Atan2(E.Position.Y, E.Position.X));
					const float Horiz = FMath::Sqrt(E.Position.X * E.Position.X + E.Position.Y * E.Position.Y);
					const float El = FMath::RadiansToDegrees(FMath::Atan2(E.Position.Z, Horiz));
					G->SetAim(Az, El + 8.0f);
					UE_LOG(LogSeaInput, Display,
					       TEXT("GunFire demo: target az=%.1f el=%.1f -> aim %.1f/%.1f, fire"), Az,
					       El, G->AimAzimuthDeg(), G->AimElevationDeg());
					break;
				}
			}
			Fire();
		}
		return;
	}
	if (!bOrderDemo)
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

void ASeaShieldPlayerController::TickGamePlayDemo(float DeltaSeconds)
{
	USeaNetSubsystem* N = Net();
	ASeaGunnerPawn* G = Gunner();
	if (N == nullptr || G == nullptr)
	{
		return;
	}
	// One-time: widen the fire order to a satisfying area salvo for the demo.
	if (!bGameOrderSet)
	{
		bGameOrderSet = true;
		N->AdjustSalvo(+6);          // 8 -> 14 rockets.
		N->AdjustDispersion(+2.0f);  // 3 -> 5 mrad pattern.
	}

	TArray<FSeaEntityState> Entities;
	N->SampleEntities(Entities);
	const FSeaEntityState* Target = nullptr;
	for (const FSeaEntityState& E : Entities)
	{
		if (E.Kind == ESeaEntityKind::Target && E.State == 0)
		{
			Target = &E;
			break;
		}
	}

	FireCooldown -= DeltaSeconds;
	ReaimTimer -= DeltaSeconds;
	if (Target == nullptr)
	{
		bHaveSolution = false;
		return;  // Between waves — hold the bore.
	}

	const FVector WindCms = N->GetWeather().WindCms;
	const FVector WindEnu(WindCms.Y / 100.0, WindCms.X / 100.0, WindCms.Z / 100.0);
	const FVector TPosEnu(Target->Position.Y / 100.0, Target->Position.X / 100.0,
	                      Target->Position.Z / 100.0);
	const FVector TVelEnu(Target->Velocity.Y / 100.0, Target->Velocity.X / 100.0,
	                      Target->Velocity.Z / 100.0);

	if (ReaimTimer <= 0.0f)
	{
		ReaimTimer = 0.35f;
		const SeaBallistics::FAimSolution Sol = SeaBallistics::SolveAim(WindEnu, TPosEnu, TVelEnu);
		bHaveSolution = Sol.bValid;
		if (Sol.bValid)
		{
			SolvedAzDeg = Sol.AzimuthDeg;
			SolvedElDeg = Sol.ElevationDeg;
		}
	}
	if (!bHaveSolution)
	{
		return;
	}

	// Slew the bore smoothly onto the solution so the capture shows tracking.
	const float CurAz = G->AimAzimuthDeg();
	const float CurEl = G->AimElevationDeg();
	const float DAz = FMath::FindDeltaAngleDegrees(CurAz, SolvedAzDeg);
	const float DEl = SolvedElDeg - CurEl;
	const float Rate = 80.0f * DeltaSeconds;  // deg/s slew limit.
	G->SetAim(CurAz + FMath::Clamp(DAz, -Rate, Rate), CurEl + FMath::Clamp(DEl, -Rate, Rate));

	// Engage at close range: short time-of-flight keeps the unguided salvo's CV
	// lead error small, so the pattern actually catches the threat.
	const float RangeM = TPosEnu.Size();
	if (RangeM < 6000.0f && FMath::Abs(DAz) < 1.5f && FMath::Abs(DEl) < 1.5f &&
	    FireCooldown <= 0.0f)
	{
		FireCooldown = 1.6f;
		N->FireManual(G->AimAzimuthDeg(), G->AimElevationDeg(), N->GetOrderSalvo(),
		              N->GetOrderDispersionMrad());
		UE_LOG(LogSeaInput, Display, TEXT("GamePlay: fire az=%.1f el=%.1f salvo=%d rng=%.0fm"),
		       G->AimAzimuthDeg(), G->AimElevationDeg(), N->GetOrderSalvo(), RangeM);
	}
}

void ASeaShieldPlayerController::TickManualPlayDemo(float DeltaSeconds)
{
	// A HUMAN-PATH play test: drive the real mouse-look handlers (Turn/LookUp)
	// and the real Fire() key handler, and pull the trigger ONLY when the live
	// on-screen pipper overlaps the lead ghost (the green SOLUTION) — fire purely
	// on what the HUD shows. No SetAim teleport, no firing on hidden truth. This
	// verifies the input bindings AND that the aids alone suffice to hit.
	USeaNetSubsystem* N = Net();
	ASeaGunnerPawn* G = Gunner();
	if (N == nullptr || G == nullptr)
	{
		return;
	}
	if (!bGameOrderSet)
	{
		bGameOrderSet = true;
		N->AdjustSalvo(+6);
		N->AdjustDispersion(+2.0f);
	}

	TArray<FSeaEntityState> Entities;
	N->SampleEntities(Entities);
	const FSeaEntityState* Target = nullptr;
	for (const FSeaEntityState& E : Entities)
	{
		if (E.Kind == ESeaEntityKind::Target && E.State == 0)
		{
			Target = &E;
			break;
		}
	}
	ManualFireCooldown -= DeltaSeconds;
	ReaimTimer -= DeltaSeconds;
	if (Target == nullptr)
	{
		return;
	}

	const FVector WindCms = N->GetWeather().WindCms;
	const FVector WindEnu(WindCms.Y / 100.0, WindCms.X / 100.0, WindCms.Z / 100.0);
	const FVector TPosEnu(Target->Position.Y / 100.0, Target->Position.X / 100.0,
	                      Target->Position.Z / 100.0);
	const FVector TVelEnu(Target->Velocity.Y / 100.0, Target->Velocity.X / 100.0,
	                      Target->Velocity.Z / 100.0);

	// The lead the player reads off the pipper/ghost, refreshed periodically.
	if (ReaimTimer <= 0.0f)
	{
		ReaimTimer = 0.4f;
		// -SeaLeadDemo aims at a STATIONARY target (correct drop, zero lead) so the
		// real target's motion guarantees a lag miss — exercising the read-out.
		const FVector SolveVel = bNoLeadAim ? FVector::ZeroVector : TVelEnu;
		const SeaBallistics::FAimSolution Sol = SeaBallistics::SolveAim(WindEnu, TPosEnu, SolveVel);
		bHaveSolution = Sol.bValid;
		if (Sol.bValid)
		{
			SolvedAzDeg = Sol.AzimuthDeg;
			SolvedElDeg = Sol.ElevationDeg;
		}
	}
	if (!bHaveSolution)
	{
		return;
	}

	// Slew onto it THROUGH the mouse-look handlers — the exact path a mouse drives
	// (Turn -> AddAzimuth, LookUp -> AddElevation), rate-limited like a hand.
	const float CurAz = G->AimAzimuthDeg();
	const float CurEl = G->AimElevationDeg();
	const float DAz = FMath::FindDeltaAngleDegrees(CurAz, SolvedAzDeg);
	const float DEl = SolvedElDeg - CurEl;
	const float MaxStepDeg = 90.0f * DeltaSeconds;
	const float Sens = FMath::Max(MouseSensitivity, 1e-3f);
	Turn(FMath::Clamp(DAz, -MaxStepDeg, MaxStepDeg) / Sens);
	LookUp(FMath::Clamp(DEl, -MaxStepDeg, MaxStepDeg) / Sens);

	// Fire ONLY on the live on-screen solution (pipper within fuze of the ghost
	// at the current bore) — i.e. on the green SOLUTION the HUD shows.
	const SeaBallistics::FFireAid Aid = SeaBallistics::ComputeFireAid(
	    G->AimAzimuthDeg(), G->AimElevationDeg(), WindEnu, TPosEnu, TVelEnu);
	const float RangeM = TPosEnu.Size();
	if (Aid.bValid && Aid.MissM < 16.0f && RangeM < 6000.0f && ManualFireCooldown <= 0.0f)
	{
		ManualFireCooldown = 1.6f;
		Fire();  // the real F/Space/LMB handler.
		UE_LOG(LogSeaInput, Display,
		       TEXT("ManualPlay: FIRE on SOLUTION miss=%.1fm rng=%.0fm az=%.1f el=%.1f"), Aid.MissM,
		       RangeM, G->AimAzimuthDeg(), G->AimElevationDeg());
	}
}
