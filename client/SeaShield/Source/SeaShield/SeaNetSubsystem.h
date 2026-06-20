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
enum class ESeaEntityKind : uint8 { Target, Rocket, Track, OwnShip };

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
	UPROPERTY(BlueprintReadOnly, Category = "SeaShield") float Humidity = 0.5f;                  // 0..1; drives seed-random fog/sea-mist amount
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

// Survival-game scoreboard, derived entirely from the engagement event stream
// (kRoundStart / kTargetDestroyed / kTargetHitShip / kEngagementEnd). The
// server holds the authoritative lives count; the console reconstructs it so no
// extra wire message is needed. MaxLives mirrors the scenario's game_lives.
USTRUCT(BlueprintType)
struct FSeaGameState
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "SeaShield") int32 Wave = 0;      // 0 = not started.
	UPROPERTY(BlueprintReadOnly, Category = "SeaShield") int32 Kills = 0;
	UPROPERTY(BlueprintReadOnly, Category = "SeaShield") int32 Leaks = 0;     // Targets that hit the ship.
	UPROPERTY(BlueprintReadOnly, Category = "SeaShield") int32 Lives = 3;
	UPROPERTY(BlueprintReadOnly, Category = "SeaShield") int32 MaxLives = 3;
	UPROPERTY(BlueprintReadOnly, Category = "SeaShield") bool bGameOver = false;
	// Points: each kill scores base × wave-multiplier × accuracy × streak. Streak
	// is consecutive kills without a leak; it multiplies and resets on a leak.
	UPROPERTY(BlueprintReadOnly, Category = "SeaShield") int32 Score = 0;
	UPROPERTY(BlueprintReadOnly, Category = "SeaShield") int32 Streak = 0;
	UPROPERTY(BlueprintReadOnly, Category = "SeaShield") int32 BestStreak = 0;
};

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

	// Own-ship helm: rudder [-1,1] (+ = starboard), throttle [0,1]. Held
	// set-points; send on change. No-op without a live session.
	UFUNCTION(BlueprintCallable, Category = "SeaShield")
	void SteerShip(float Rudder, float Throttle);

	// Latest own-ship pose (kOwnShip entity). False until one has arrived.
	UFUNCTION(BlueprintPure, Category = "SeaShield")
	bool GetOwnShip(FSeaEntityState& OutShip) const;

	// --- Weapons command state (operator fire order) ---
	// Designation is the single source of truth here: the PPI writes it on
	// click, the console reads it, the controller fires it. The first designation
	// of an engagement is also the operator's "approve" moment for the AAR
	// reaction-time timeline (detect -> approve -> fire): we stamp it with the
	// latest snapshot tick (same sim clock the engagement events carry).
	UFUNCTION(BlueprintCallable, Category = "SeaShield")
	void DesignateTrack(int32 TrackId)
	{
		DesignatedTrackId = TrackId;
		if (TrackId != 0 && FirstDesignateTick < 0)
		{
			FirstDesignateTick = LatestSnapshotTick;
		}
	}
	UFUNCTION(BlueprintPure, Category = "SeaShield")
	int32 GetDesignatedTrack() const { return DesignatedTrackId; }

	// Sim tick at which the operator first designated a track this engagement
	// (the AAR's "approve" beat); negative if no designation was captured.
	int64 GetFirstDesignateTick() const { return FirstDesignateTick; }

	// Full retained engagement-event log (cleared on kRoundStart / a new
	// engagement). Lets the AAR reconstruct an exact tick-ordered timeline and
	// keeps the door open for a future replay UI without a wire change.
	const TArray<FSeaEngagementEvent>& GetEngagementLog() const { return EngagementLog; }

	UFUNCTION(BlueprintCallable, Category = "SeaShield") void AdjustSalvo(int32 Delta);
	UFUNCTION(BlueprintCallable, Category = "SeaShield") void AdjustDispersion(float DeltaMrad);
	UFUNCTION(BlueprintCallable, Category = "SeaShield") void AdjustTrim(float DeltaAzDeg, float DeltaElDeg);
	// Fires the current order at the designated track (no-op if none / no link).
	UFUNCTION(BlueprintCallable, Category = "SeaShield") void CommitFire();

	UFUNCTION(BlueprintPure, Category = "SeaShield") int32 GetOrderSalvo() const { return OrderSalvo; }
	UFUNCTION(BlueprintPure, Category = "SeaShield") float GetOrderDispersionMrad() const { return OrderDispersionMrad; }
	UFUNCTION(BlueprintPure, Category = "SeaShield") float GetOrderAzTrimDeg() const { return OrderAzTrimDeg; }
	UFUNCTION(BlueprintPure, Category = "SeaShield") float GetOrderElTrimDeg() const { return OrderElTrimDeg; }

	UPROPERTY(BlueprintAssignable, Category = "SeaShield")
	FSeaEngagementEventSignature OnEngagementEvent;

	// Survival-game scoreboard (game_mode scenarios). Wave 0 means the run has
	// not begun or this is a plain single engagement.
	UFUNCTION(BlueprintPure, Category = "SeaShield")
	FSeaGameState GetGameState() const { return GameState; }

	// Lead-error of the most recent salvo: at fire time the client snapshots the
	// predicted intercept (the lead solution it committed to); one time-of-flight
	// later it compares the threat's ACTUAL position. The gap is decomposed in the
	// LOS-relative orthonormal frame (ship->intercept) so the axes are honest and
	// independent (a diving target no longer double-counts along/vertical):
	//   LateralM  cross-LOS, horizontal: + = right of the line of sight
	//   UpM       vertical:              + = high
	//   LongM     along-LOS:             + = long (past the target's range)
	// — how far a maneuvering threat slipped out of the committed unguided
	// solution, the project's thesis made measurable. Purely client-side.
	struct FSeaLeadError
	{
		bool bValid = false;
		float MissM = 0.0f;     // magnitude (m)
		float LateralM = 0.0f;  // + right / - left   (aim correction)
		float UpM = 0.0f;       // + high  / - low
		float LongM = 0.0f;     // + long  / - short  (range/timing)
		float AgeS = 999.0f;    // seconds since measured (for transient display)
	};
	FSeaLeadError GetLeadError() const { return LeadError; }

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
	int32 PendingDevSalvo = 0;   // -SeaFire=N: salvo once the solution settles.
	int32 ValidSolutionStreak = 0;
	ESeaRole AssignedRole = ESeaRole::Observer;
	FSeaWeather Weather;
	TMap<int32, FSeaFireSolution> LatestSolutions;
	int32 DesignatedTrackId = 0;
	int64 LatestSnapshotTick = 0;    // last completed snapshot/delta tick (sim clock)
	int64 FirstDesignateTick = -1;   // first operator designation this engagement (AAR)
	TArray<FSeaEngagementEvent> EngagementLog;  // retained, in arrival order (AAR timeline)
	int32 OrderSalvo = 8;            // rockets per salvo (1..16)
	float OrderDispersionMrad = 3.0f;  // pattern spread (0..20)
	float OrderAzTrimDeg = 0.0f;     // operator trim, server clamps to +-15
	float OrderElTrimDeg = 0.0f;
	FSeaGameState GameState;
	float LastKillMissM = 0.0f;  // miss of the most recent killing rocket (scoring)

	// Lead-error tracking (client-only). A shot pushes its predicted intercept;
	// after one time-of-flight the actual threat position is compared.
	struct FPendingShot
	{
		FVector PredictedUeCm = FVector::ZeroVector;  // committed intercept (rel. origin)
		double MatureAtS = 0.0;
	};
	TArray<FPendingShot> PendingShots;
	double ClockS = 0.0;
	FSeaLeadError LeadError;
	// (kind, subject, tick) dedup — the v4 bind-time TCP event backlog may
	// overlap the live UDP stream at the boundary (client_session.cpp leaves
	// dedup to the consumer by design).
	TSet<uint64> SeenEventKeys;
};
