#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Tickable.h"

#include "client/core/interp_buffer.h"

#include "SeaNetSubsystem.generated.h"

class FSeaNetRunnable;
class FRunnableThread;

// Presentation-side mirror of the wire enums. Values are NOT pinned to the
// protocol's — conversion goes through explicit switches in the .cpp, so a
// protocol renumbering fails loudly at compile time instead of silently.
UENUM(BlueprintType)
enum class ESeaEntityKind : uint8 { Target, Rocket, Track };

UENUM(BlueprintType)
enum class ESeaRole : uint8 { Observer, Commander, Weapons, Solo };

USTRUCT(BlueprintType)
struct FSeaEntityState
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "SeaShield") int32 Id = 0;
	UPROPERTY(BlueprintReadOnly, Category = "SeaShield") ESeaEntityKind Kind = ESeaEntityKind::Target;
	// Target: 0 alive / 1 destroyed. Rocket: 0 boost / 1 glide.
	// Track: 0 tentative / 1 confirmed / 2 coasting.
	UPROPERTY(BlueprintReadOnly, Category = "SeaShield") uint8 State = 0;
	UPROPERTY(BlueprintReadOnly, Category = "SeaShield") float TrackSigmaM = 0.0f;
	UPROPERTY(BlueprintReadOnly, Category = "SeaShield") FVector Position = FVector::ZeroVector;  // UE cm.
	UPROPERTY(BlueprintReadOnly, Category = "SeaShield") FVector Velocity = FVector::ZeroVector;  // UE cm/s.
	UPROPERTY(BlueprintReadOnly, Category = "SeaShield") bool bExtrapolated = false;
};

USTRUCT(BlueprintType)
struct FSeaFireSolution
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "SeaShield") int32 TrackId = 0;
	UPROPERTY(BlueprintReadOnly, Category = "SeaShield") bool bValid = false;
	UPROPERTY(BlueprintReadOnly, Category = "SeaShield") FVector Pip = FVector::ZeroVector;  // UE cm.
	UPROPERTY(BlueprintReadOnly, Category = "SeaShield") float TimeOfFlightS = 0.0f;
	UPROPERTY(BlueprintReadOnly, Category = "SeaShield") float DispersionRadiusM = 0.0f;
};

USTRUCT(BlueprintType)
struct FSeaWeather
{
	GENERATED_BODY()

	// Visual drivers straight from the simulation weather (ServerWelcome v3).
	UPROPERTY(BlueprintReadOnly, Category = "SeaShield") FVector WindCms = FVector::ZeroVector;  // UE frame.
	UPROPERTY(BlueprintReadOnly, Category = "SeaShield") float RainIntensity = 0.0f;             // 0..1
	UPROPERTY(BlueprintReadOnly, Category = "SeaShield") float GustSigmaMps = 0.0f;
};

USTRUCT(BlueprintType)
struct FSeaEngagementEvent
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "SeaShield") uint8 Kind = 0;  // protocol::EventKind value.
	UPROPERTY(BlueprintReadOnly, Category = "SeaShield") int32 SubjectId = 0;
	UPROPERTY(BlueprintReadOnly, Category = "SeaShield") int64 Tick = 0;
	UPROPERTY(BlueprintReadOnly, Category = "SeaShield") float MissDistanceM = 0.0f;
	UPROPERTY(BlueprintReadOnly, Category = "SeaShield") bool bDetonated = false;
	UPROPERTY(BlueprintReadOnly, Category = "SeaShield") bool bKilled = false;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FSeaEngagementEventSignature, const FSeaEngagementEvent&, Event);

// Owns the network thread (FSeaNetRunnable wrapping the headlessly-tested
// seashield::client::ClientSession) and the game-thread snapshot pipeline
// (assembler -> interpolation buffer). All UFUNCTION surface runs on the
// game thread; the wire stays on the network thread (charter §4.6 응용).
UCLASS()
class SEASHIELD_API USeaNetSubsystem : public UGameInstanceSubsystem, public FTickableGameObject
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, Category = "SeaShield")
	bool Connect(const FString& Host, int32 TcpPort, ESeaRole Role);

	UFUNCTION(BlueprintCallable, Category = "SeaShield")
	void Disconnect();

	UFUNCTION(BlueprintPure, Category = "SeaShield")
	bool IsWelcomed() const { return bWelcomed; }

	UFUNCTION(BlueprintPure, Category = "SeaShield")
	ESeaRole GetRole() const { return AssignedRole; }

	UFUNCTION(BlueprintPure, Category = "SeaShield")
	FSeaWeather GetWeather() const { return Weather; }

	// Render-time entity sample at the delayed interpolation clock (~100 ms).
	UFUNCTION(BlueprintCallable, Category = "SeaShield")
	void SampleEntities(TArray<FSeaEntityState>& OutEntities) const;

	// Latest streamed solution for a track (2 Hz feed); false if none yet.
	UFUNCTION(BlueprintPure, Category = "SeaShield")
	bool GetFireSolution(int32 TrackId, FSeaFireSolution& OutSolution) const;

	// Track-designated fire: azimuth/elevation ride along as operator trim
	// (server clamps to ±15°). Salvo/dispersion as on the wire.
	UFUNCTION(BlueprintCallable, Category = "SeaShield")
	void FireAtTrack(int32 TrackId, float AzimuthOffsetDeg, float ElevationOffsetDeg,
	                 int32 SalvoCount, float DispersionMrad);

	UFUNCTION(BlueprintCallable, Category = "SeaShield")
	void FireManual(float AzimuthDeg, float ElevationDeg, int32 SalvoCount, float DispersionMrad);

	UPROPERTY(BlueprintAssignable, Category = "SeaShield")
	FSeaEngagementEventSignature OnEngagementEvent;

	// --- UGameInstanceSubsystem ---
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// --- FTickableGameObject: drain the network-thread queues every frame ---
	virtual void Tick(float DeltaTime) override;
	virtual bool IsTickable() const override { return Runnable != nullptr; }
	virtual TStatId GetStatId() const override
	{
		RETURN_QUICK_DECLARE_CYCLE_STAT(USeaNetSubsystem, STATGROUP_Tickables);
	}

private:
	FSeaNetRunnable* Runnable = nullptr;
	FRunnableThread* Thread = nullptr;

	// Game-thread state (filled by Tick from the queues).
	seashield::client::SnapshotAssembler Assembler;
	seashield::client::InterpolationBuffer Interp;
	bool bWelcomed = false;
	bool bLoggedFirstSnapshot = false;
	ESeaRole AssignedRole = ESeaRole::Observer;
	FSeaWeather Weather;
	TMap<int32, FSeaFireSolution> LatestSolutions;
};
