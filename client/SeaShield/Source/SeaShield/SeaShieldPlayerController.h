#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"

#include "SeaShieldPlayerController.generated.h"

class USeaNetSubsystem;

// The weapons operator's input surface: the fire order (salvo / dispersion /
// trim) is adjusted from the keyboard and the designated track is committed to
// fire, all routed through USeaNetSubsystem (the command channel). Mouse input
// stays enabled so the PPI scope's click-to-designate keeps working.
//
// Drawing the controls as interactive UMG sliders/buttons is unreliable on UE
// 5.7 (the same NativePaint issue the HUD widgets work around), so the input is
// keys + a live read-out on the fire-control console — robust and demoable.
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

	// -SeaOrderDemo: scripted order (designate -> adjust salvo/dispersion/trim
	// -> fire on settle) so the interactive path is verifiable from a capture
	// without live keypresses — it drives the same handlers the keys call.
	bool bOrderDemo = false;
	float DemoElapsed = 0.0f;
	int32 DemoStep = 0;
	int32 SettleStreak = 0;
};
