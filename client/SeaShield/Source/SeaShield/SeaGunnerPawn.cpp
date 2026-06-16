#include "SeaGunnerPawn.h"

#include "Camera/CameraComponent.h"
#include "Engine/GameInstance.h"
#include "GameFramework/SpringArmComponent.h"

#include "SeaNetSubsystem.h"
#include "SeaWorldFrame.h"

namespace
{
// Orbit/sight pivot above the deck so the bow does not block the threat sector
// and the sight eye-height matches the old first-person bridge view.
constexpr double kPivotHeightCm = 2200.0;
constexpr float kSightBlendSpeed = 7.0f;  // per second toward the sight/orbit target
}  // namespace

ASeaGunnerPawn::ASeaGunnerPawn()
{
	PrimaryActorTick.bCanEverTick = true;  // The rig rides the moving ship.

	CameraArm = CreateDefaultSubobject<USpringArmComponent>(TEXT("CameraArm"));
	SetRootComponent(CameraArm);
	CameraArm->TargetArmLength = OrbitDistanceCm;
	CameraArm->bDoCollisionTest = false;        // open sea — never pull the camera in
	CameraArm->bUsePawnControlRotation = false;  // we drive the arm from the bore, not control rotation
	CameraArm->bEnableCameraLag = true;          // damped dolly/orbit (WoWS feel)
	CameraArm->bEnableCameraRotationLag = true;
	CameraArm->CameraLagSpeed = 6.0f;
	CameraArm->CameraRotationLagSpeed = 9.0f;

	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->SetupAttachment(CameraArm, USpringArmComponent::SocketName);
	Camera->bUsePawnControlRotation = false;

	AzimuthDeg = InitialAzimuthDeg;
	ElevationDeg = InitialElevationDeg;
}

void ASeaGunnerPawn::BeginPlay()
{
	Super::BeginPlay();
	AzimuthDeg = InitialAzimuthDeg;
	ElevationDeg = InitialElevationDeg;
	if (const UGameInstance* GI = GetGameInstance())
	{
		Net = GI->GetSubsystem<USeaNetSubsystem>();
	}
	// Start at the stage origin; Tick re-anchors to the live own-ship pose.
	SetActorLocation(SeaWorldFrame::Origin + FVector(0.0, 0.0, kPivotHeightCm));
	UpdateRig(0.0f);
}

void ASeaGunnerPawn::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	UpdateRig(DeltaSeconds);
}

void ASeaGunnerPawn::UpdateRig(float DeltaSeconds)
{
	// Pivot rides the own ship (camera frames hull + sea + engagement).
	FSeaEntityState Ship;
	const FVector Pivot = (Net != nullptr && Net->GetOwnShip(Ship))
	                          ? SeaWorldFrame::Origin + Ship.Position + FVector(0.0, 0.0, kPivotHeightCm)
	                          : SeaWorldFrame::Origin + FVector(0.0, 0.0, kPivotHeightCm);
	SetActorLocation(Pivot);

	// Blend orbit <-> first-person sight.
	const float Target = bSightMode ? 1.0f : 0.0f;
	SightAlpha = FMath::FInterpConstantTo(SightAlpha, Target, DeltaSeconds, kSightBlendSpeed);

	// Camera looks DOWN-forward along the aim bearing in orbit (with the MMB
	// free-look offset), or along the bore in the sight. Yaw tracks the bore
	// azimuth so the player looks where they aim; the free-look offset fades out
	// in the sight so the sniper view stays bore-true.
	const float OrbitPitch = FMath::Clamp(OrbitPitchDeg + OrbitPitchOffsetDeg, -85.0f, 8.0f);
	const float ArmPitch = FMath::Lerp(OrbitPitch, ElevationDeg, SightAlpha);
	const float Yaw = AzimuthDeg + OrbitYawOffsetDeg * (1.0f - SightAlpha);
	const float ArmLength = FMath::Lerp(OrbitDistanceCm, 0.0f, SightAlpha);
	const float Fov = FMath::Lerp(OrbitFovDeg, SightFovDeg, SightAlpha);
	CameraArm->TargetArmLength = ArmLength;
	SetActorRotation(FRotator(ArmPitch, Yaw, 0.0));
	if (Camera != nullptr)
	{
		Camera->SetFieldOfView(Fov);
	}
}

void ASeaGunnerPawn::AddAzimuth(float DeltaDeg)
{
	AzimuthDeg = FMath::UnwindDegrees(AzimuthDeg + DeltaDeg);
}

void ASeaGunnerPawn::AddElevation(float DeltaDeg)
{
	ElevationDeg = FMath::Clamp(ElevationDeg + DeltaDeg, MinElevationDeg, MaxElevationDeg);
}

void ASeaGunnerPawn::SetAim(float NewAzimuthDeg, float NewElevationDeg)
{
	AzimuthDeg = FMath::UnwindDegrees(NewAzimuthDeg);
	ElevationDeg = FMath::Clamp(NewElevationDeg, MinElevationDeg, MaxElevationDeg);
}

void ASeaGunnerPawn::AddOrbitDistance(float DeltaCm)
{
	OrbitDistanceCm = FMath::Clamp(OrbitDistanceCm + DeltaCm, MinOrbitDistanceCm, MaxOrbitDistanceCm);
}

void ASeaGunnerPawn::AddOrbitYaw(float DeltaDeg)
{
	OrbitYawOffsetDeg = FMath::ClampAngle(OrbitYawOffsetDeg + DeltaDeg, -150.0f, 150.0f);
}

void ASeaGunnerPawn::AddOrbitPitch(float DeltaDeg)
{
	// Clamp so the total orbit pitch (base + offset) stays between looking nearly
	// straight down and a touch above the horizon (matches the UpdateRig clamp).
	OrbitPitchOffsetDeg = FMath::Clamp(OrbitPitchOffsetDeg + DeltaDeg, -85.0f - OrbitPitchDeg, 8.0f - OrbitPitchDeg);
}

void ASeaGunnerPawn::SetSightMode(bool bEnabled) { bSightMode = bEnabled; }

float ASeaGunnerPawn::AimSensitivityScale() const
{
	return FMath::Lerp(1.0f, SightFovDeg / FMath::Max(OrbitFovDeg, 1.0f), SightAlpha);
}

float ASeaGunnerPawn::AimAzimuthDeg() const { return AzimuthDeg; }
float ASeaGunnerPawn::AimElevationDeg() const { return ElevationDeg; }
