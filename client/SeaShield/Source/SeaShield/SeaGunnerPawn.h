#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"

#include "SeaGunnerPawn.generated.h"

class UCameraComponent;

// First-person gun director: the player stands at the ship's launcher and aims
// the unguided rocket battery by eye. Mouse drives azimuth/elevation; the
// camera looks down the bore. Firing reads this pawn's aim and sends a manual
// FireRequest (azimuth/elevation) to the server, which launches the salvo on
// that bearing — the rockets then fly ballistically (gravity + wind), so
// leading the target and reading the conditions is the whole game.
UCLASS()
class SEASHIELD_API ASeaGunnerPawn : public APawn
{
	GENERATED_BODY()

public:
	ASeaGunnerPawn();

	// Aim in world/compass terms for the fire request.
	// Azimuth: degrees clockwise from north (+X). Elevation: degrees above horizon.
	float AimAzimuthDeg() const;
	float AimElevationDeg() const;

	// Mouse look (bound by the player controller).
	void AddAzimuth(float DeltaDeg);
	void AddElevation(float DeltaDeg);
	// Absolute aim (used by the scripted demo / future auto-slew).
	void SetAim(float AzimuthDeg, float ElevationDeg);

	UPROPERTY(EditAnywhere, Category = "SeaShield")
	float MinElevationDeg = 0.0f;
	UPROPERTY(EditAnywhere, Category = "SeaShield")
	float MaxElevationDeg = 85.0f;
	// Default bore: a high lob toward the threat sector so the first frame
	// already frames incoming targets rather than the empty horizon.
	UPROPERTY(EditAnywhere, Category = "SeaShield")
	float InitialAzimuthDeg = 37.0f;
	UPROPERTY(EditAnywhere, Category = "SeaShield")
	float InitialElevationDeg = 10.0f;

	virtual void BeginPlay() override;

private:
	UPROPERTY(VisibleAnywhere, Category = "SeaShield")
	TObjectPtr<UCameraComponent> Camera;

	float AzimuthDeg = 45.0f;
	float ElevationDeg = 18.0f;

	void ApplyAim();
};
