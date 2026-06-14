#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"

#include "SeaShieldPlayerController.generated.h"

class USeaNetSubsystem;
class ASeaGunnerPawn;

// The gun director's input surface. Mouse aims the launcher (azimuth/elevation)
// via the possessed ASeaGunnerPawn; FIRE sends a manual FireRequest down the
// current bore so the unguided salvo flies where the player is looking. Keys
// trim the fire order (salvo count / dispersion). All commands route through
// USeaNetSubsystem (the command channel).
//
// Form-style UMG sliders are unreliable on UE 5.7 (the NativePaint issue the
// HUD widgets work around), so input is mouse-look + keys with a live read-out
// on the fire-control console.
UCLASS()
class SEASHIELD_API ASeaShieldPlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	ASeaShieldPlayerController();

	virtual void BeginPlay() override;
	virtual void SetupInputComponent() override;
	virtual void Tick(float DeltaSeconds) override;

private:
	USeaNetSubsystem* Net() const;

	void SalvoUp();
	void SalvoDown();
	void DispersionUp();
	void DispersionDown();
	void TrimAzLeft();
	void TrimAzRight();
	void TrimElUp();
	void TrimElDown();
	void Fire();

	// Mouse look -> launcher aim (degrees per mouse unit via MouseSensitivity).
	void Turn(float AxisValue);
	void LookUp(float AxisValue);
	ASeaGunnerPawn* Gunner() const;

	UPROPERTY(EditAnywhere, Category = "SeaShield")
	float MouseSensitivity = 0.35f;

	// -SeaOrderDemo: scripted track-designated fire (designate -> adjust order
	// -> fire on settle) — verifies the command path from a capture.
	bool bOrderDemo = false;
	float DemoElapsed = 0.0f;
	int32 DemoStep = 0;
	int32 SettleStreak = 0;
	// -SeaGunFire: fire one manual salvo down the current bore after a delay,
	// so the aim->fire path is verifiable from a capture without a live mouse.
	bool bGunFireDemo = false;
	bool bGunFired = false;

	// -SeaGamePlay: a scripted auto-gunner for the survival game — it solves the
	// intercept aim (SeaBallistics::SolveAim), slews the bore onto it, and fires
	// when laid on, wave after wave. Lets the whole playable loop (track -> lead
	// -> fire -> splash -> next wave) be verified from a capture without a mouse.
	bool bGamePlayDemo = false;
	bool bGameOrderSet = false;
	float ReaimTimer = 0.0f;
	float FireCooldown = 0.0f;
	float SolvedAzDeg = 0.0f;
	float SolvedElDeg = 0.0f;
	bool bHaveSolution = false;
	void TickGamePlayDemo(float DeltaSeconds);

	// -SeaManualPlay: a human-path play test. Unlike -SeaGamePlay (which teleports
	// the bore via SetAim and uses an omniscient solver), this drives the SAME
	// mouse-look handlers (Turn/LookUp) and the Fire() key handler a player uses,
	// and aims purely by nulling the on-screen gap between the impact pipper and
	// the lead ghost — proving the HUD aids alone are enough to find, lead, and
	// hit. It never reads ground-truth aim, only what the screen shows.
	bool bManualPlayDemo = false;
	// -SeaLeadDemo: deliberately aim with NO lead (straight at the current target)
	// so every salvo lags — a verification harness for the directional miss/lead
	// read-out (it never triggers in normal play because a good shot has ~0 error).
	bool bNoLeadAim = false;
	float ManualFireCooldown = 0.0f;
	void TickManualPlayDemo(float DeltaSeconds);
};
