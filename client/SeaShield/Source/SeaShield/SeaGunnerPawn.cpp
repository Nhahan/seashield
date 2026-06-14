#include "SeaGunnerPawn.h"

#include "Camera/CameraComponent.h"
#include "Engine/GameInstance.h"

#include "SeaNetSubsystem.h"
#include "SeaWorldFrame.h"

namespace
{
// Director's eye height above the deck so the bow does not block the threat sector.
constexpr double kSightHeightCm = 2200.0;
}  // namespace

ASeaGunnerPawn::ASeaGunnerPawn()
{
	PrimaryActorTick.bCanEverTick = true;  // The sight rides the moving ship.
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
	if (const UGameInstance* GI = GetGameInstance())
	{
		Net = GI->GetSubsystem<USeaNetSubsystem>();
	}
	// The launcher/ownship starts at the stage origin; Tick re-anchors it to the
	// live own-ship pose once snapshots arrive.
	SetActorLocation(SeaWorldFrame::Origin + FVector(0.0, 0.0, kSightHeightCm));
	ApplyAim();
}

void ASeaGunnerPawn::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	// Position follows the own ship; rotation (the gun aim) is left untouched —
	// it is world/compass-referenced, set only by mouse-look via ApplyAim().
	FSeaEntityState Ship;
	if (Net != nullptr && Net->GetOwnShip(Ship))
	{
		SetActorLocation(SeaWorldFrame::Origin + Ship.Position + FVector(0.0, 0.0, kSightHeightCm));
	}
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
