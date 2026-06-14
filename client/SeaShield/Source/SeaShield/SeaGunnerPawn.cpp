#include "SeaGunnerPawn.h"

#include "Camera/CameraComponent.h"

#include "SeaWorldFrame.h"

ASeaGunnerPawn::ASeaGunnerPawn()
{
	PrimaryActorTick.bCanEverTick = false;
	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("GunSight"));
	SetRootComponent(Camera);
	AzimuthDeg = InitialAzimuthDeg;
	ElevationDeg = InitialElevationDeg;
}

void ASeaGunnerPawn::BeginPlay()
{
	Super::BeginPlay();
	AzimuthDeg = InitialAzimuthDeg;
	ElevationDeg = InitialElevationDeg;
	// Director's sight on the ship — above the deck so the bow does not block
	// the threat sector. The launcher/ownship sits at the stage origin.
	SetActorLocation(SeaWorldFrame::Origin + FVector(0.0, 0.0, 2200.0));
	ApplyAim();
}

void ASeaGunnerPawn::AddAzimuth(float DeltaDeg)
{
	AzimuthDeg = FMath::UnwindDegrees(AzimuthDeg + DeltaDeg);
	ApplyAim();
}

void ASeaGunnerPawn::AddElevation(float DeltaDeg)
{
	ElevationDeg = FMath::Clamp(ElevationDeg + DeltaDeg, MinElevationDeg, MaxElevationDeg);
	ApplyAim();
}

void ASeaGunnerPawn::SetAim(float NewAzimuthDeg, float NewElevationDeg)
{
	AzimuthDeg = FMath::UnwindDegrees(NewAzimuthDeg);
	ElevationDeg = FMath::Clamp(NewElevationDeg, MinElevationDeg, MaxElevationDeg);
	ApplyAim();
}

void ASeaGunnerPawn::ApplyAim()
{
	// FRotator(Pitch=elevation up, Yaw=azimuth from north, Roll=0). With the
	// project frame (UE +X=north, +Y=east), this yaw equals the sim's compass
	// azimuth and the forward vector matches direction_from_az_el(az, el) — so
	// what the player sees down the sight is exactly where the salvo flies.
	SetActorRotation(FRotator(ElevationDeg, AzimuthDeg, 0.0));
}

float ASeaGunnerPawn::AimAzimuthDeg() const { return AzimuthDeg; }
float ASeaGunnerPawn::AimElevationDeg() const { return ElevationDeg; }
