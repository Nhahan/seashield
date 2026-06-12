#include "SeaWorldManager.h"

#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "UObject/ConstructorHelpers.h"

DEFINE_LOG_CATEGORY_STATIC(LogSeaShieldWorld, Log, All);

ASeaWorldManager::ASeaWorldManager()
{
	PrimaryActorTick.bCanEverTick = true;

	// Code-first defaults: the meshes Tools/import_assets.py lands in the
	// project. The hostile target is the procedural anti-ship missile.
	static ConstructorHelpers::FObjectFinder<UStaticMesh> MissileFinder(
		TEXT("/Game/SeaShield/Meshes/SM_Missile"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> RocketFinder(
		TEXT("/Game/SeaShield/Meshes/SM_Rocket"));
	if (MissileFinder.Succeeded())
	{
		TargetMesh = MissileFinder.Object;
	}
	if (RocketFinder.Succeeded())
	{
		RocketMesh = RocketFinder.Object;
	}
}

TSubclassOf<AActor> ASeaWorldManager::ClassFor(ESeaEntityKind Kind) const
{
	switch (Kind)
	{
	case ESeaEntityKind::Rocket: return RocketClass;
	case ESeaEntityKind::Track: return TrackClass;
	case ESeaEntityKind::Target: break;
	}
	return TargetClass;
}

UStaticMesh* ASeaWorldManager::MeshFor(ESeaEntityKind Kind) const
{
	switch (Kind)
	{
	case ESeaEntityKind::Rocket: return RocketMesh;
	case ESeaEntityKind::Track: return TrackMesh;
	case ESeaEntityKind::Target: break;
	}
	return TargetMesh;
}

AActor* ASeaWorldManager::SpawnFor(const FSeaEntityState& Entity)
{
	if (const TSubclassOf<AActor> Class = ClassFor(Entity.Kind); Class != nullptr)
	{
		return GetWorld()->SpawnActor<AActor>(Class, Entity.Position, FRotator::ZeroRotator);
	}
	UStaticMesh* Mesh = MeshFor(Entity.Kind);
	if (Mesh == nullptr)
	{
		return nullptr;  // Kind intentionally not visualized (e.g. tracks).
	}
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AStaticMeshActor* MeshActor = GetWorld()->SpawnActor<AStaticMeshActor>(
		Entity.Position, FRotator::ZeroRotator, Params);
	if (MeshActor != nullptr)
	{
		MeshActor->SetMobility(EComponentMobility::Movable);
		MeshActor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
		// The generators model +Y as the nose (asset_lib.py render notes);
		// the actor convention below points +X along the velocity.
		MeshActor->GetStaticMeshComponent()->SetRelativeRotation(FRotator(0.0, -90.0, 0.0));
	}
	return MeshActor;
}

void ASeaWorldManager::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	const UGameInstance* GameInstance = GetGameInstance();
	if (GameInstance == nullptr)
	{
		return;
	}
	USeaNetSubsystem* Net = GameInstance->GetSubsystem<USeaNetSubsystem>();
	if (Net == nullptr)
	{
		return;
	}

	TArray<FSeaEntityState> Entities;
	Net->SampleEntities(Entities);

	TSet<int32> Alive;
	for (const FSeaEntityState& Entity : Entities)
	{
		const int32 Key = (static_cast<int32>(Entity.Kind) << 16) | Entity.Id;
		Alive.Add(Key);

		TWeakObjectPtr<AActor>& Slot = Spawned.FindOrAdd(Key);
		if (!Slot.IsValid())
		{
			Slot = SpawnFor(Entity);
			if (!Slot.IsValid())
			{
				continue;
			}
			if (!bLoggedFirstSpawn)
			{
				bLoggedFirstSpawn = true;
				UE_LOG(LogSeaShieldWorld, Display,
				       TEXT("First entity actor spawned: kind=%d id=%d at (%.0f, %.0f, %.0f) cm"),
				       static_cast<int32>(Entity.Kind), Entity.Id, Entity.Position.X,
				       Entity.Position.Y, Entity.Position.Z);
			}
		}
		// Face the velocity when it is meaningful; tracks keep identity
		// rotation (their symbology lives on the PPI, not in 3D pose).
		FRotator Rotation = Slot->GetActorRotation();
		if (Entity.Kind != ESeaEntityKind::Track && !Entity.Velocity.IsNearlyZero(1.0))
		{
			Rotation = Entity.Velocity.Rotation();
		}
		Slot->SetActorLocationAndRotation(Entity.Position, Rotation);
	}

	// Entities gone from the sample (resolved rockets, dropped tracks,
	// destroyed target) leave the stage.
	for (auto It = Spawned.CreateIterator(); It; ++It)
	{
		if (!Alive.Contains(It.Key()))
		{
			if (It.Value().IsValid())
			{
				It.Value()->Destroy();
			}
			It.RemoveCurrent();
		}
	}
}
