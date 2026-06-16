#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"

#include "SeaGunnerPawn.generated.h"

class UCameraComponent;
class USpringArmComponent;

// Third-person fire director (World-of-Warships model). A spring-arm camera
// orbits the own-ship from astern/above so the player sees the hull, the sea,
// the outgoing rocket smoke and the incoming threats; the mouse still aims the
// unguided battery (azimuth/elevation kept as a SEPARATE "bore" state, NOT the
// camera), so the fire request and HUD reticle are unchanged. RMB/Shift snaps to
// the first-person launcher sight (narrow FOV) for precise aiming. Firing reads
// AimAzimuthDeg()/AimElevationDeg() and sends a manual FireRequest; the rockets
// then fly ballistically (gravity + wind), so leading the target is the game.
UCLASS()
class SEASHIELD_API ASeaGunnerPawn : public APawn
{
	GENERATED_BODY()

public:
	ASeaGunnerPawn();

	// Aim in world/compass terms for the fire request — the BORE, independent of
	// the camera. Azimuth: degrees CW from north (+X). Elevation: above horizon.
	float AimAzimuthDeg() const;
	float AimElevationDeg() const;

	// Mouse look (bound by the player controller) — moves the bore aim.
	void AddAzimuth(float DeltaDeg);
	void AddElevation(float DeltaDeg);
	// Absolute aim (used by the scripted demo / auto-gunner).
	void SetAim(float AzimuthDeg, float ElevationDeg);

	// Third-person camera controls.
	void AddOrbitDistance(float DeltaCm);     // wheel scroll — zoom (dolly the orbit in/out)
	void AddOrbitYaw(float DeltaDeg);         // MMB-drag — free-orbit the camera bearing
	void AddOrbitPitch(float DeltaDeg);       // MMB-drag — adjust the camera tilt angle
	void SetSightMode(bool bEnabled);         // RMB/Shift — blend to/from the first-person sight
	// Mouse-aim scale: finer in the zoomed sight (scales with the FOV ratio) so the
	// same mouse delta does not feel twitchy when magnified.
	float AimSensitivityScale() const;

	UPROPERTY(EditAnywhere, Category = "SeaShield")
	float MinElevationDeg = 0.0f;
	UPROPERTY(EditAnywhere, Category = "SeaShield")
	float MaxElevationDeg = 85.0f;
	// Default bore: a high lob toward the threat sector so the first frame already
	// frames incoming targets rather than the empty horizon.
	UPROPERTY(EditAnywhere, Category = "SeaShield")
	float InitialAzimuthDeg = 37.0f;
	UPROPERTY(EditAnywhere, Category = "SeaShield")
	float InitialElevationDeg = 10.0f;

	// Orbit rig tuning.
	UPROPERTY(EditAnywhere, Category = "SeaShield|Camera")
	float OrbitDistanceCm = 6500.0f;
	UPROPERTY(EditAnywhere, Category = "SeaShield|Camera")
	float MinOrbitDistanceCm = 2500.0f;
	UPROPERTY(EditAnywhere, Category = "SeaShield|Camera")
	float MaxOrbitDistanceCm = 16000.0f;
	UPROPERTY(EditAnywhere, Category = "SeaShield|Camera")
	float OrbitPitchDeg = -24.0f;  // negative = camera rides high, looking down-forward
	                               // (steeper trims expensive grazing-horizon water)
	UPROPERTY(EditAnywhere, Category = "SeaShield|Camera")
	float OrbitFovDeg = 88.0f;
	UPROPERTY(EditAnywhere, Category = "SeaShield|Camera")
	float SightFovDeg = 22.0f;  // narrow "sniper" sight

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

private:
	UPROPERTY(VisibleAnywhere, Category = "SeaShield")
	TObjectPtr<USpringArmComponent> CameraArm;
	UPROPERTY(VisibleAnywhere, Category = "SeaShield")
	TObjectPtr<UCameraComponent> Camera;

	UPROPERTY()
	TObjectPtr<class USeaNetSubsystem> Net;

	float AzimuthDeg = 45.0f;
	float ElevationDeg = 18.0f;
	bool bSightMode = false;
	float SightAlpha = 0.0f;  // 0 = orbit, 1 = first-person sight (blended)
	// Free-look (MMB-drag) camera-angle offsets, relative to the bore-following
	// orbit. Persist so the player can set a preferred viewing angle.
	float OrbitYawOffsetDeg = 0.0f;
	float OrbitPitchOffsetDeg = 0.0f;

	// Re-anchor the rig pivot to the live own-ship pose and drive the camera from
	// the bore + sight blend.
	void UpdateRig(float DeltaSeconds);
};
