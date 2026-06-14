#include "SeaNetSubsystem.h"

#include "Containers/Queue.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

#include "client/core/client_session.h"
#include "client/core/coords.h"
#include "protocol/messages.h"

#include "SeaBallistics.h"

DEFINE_LOG_CATEGORY_STATIC(LogSeaShield, Log, All);

namespace protocol = seashield::protocol;

namespace {

protocol::Role to_protocol_role(ESeaRole role)
{
	switch (role)
	{
	case ESeaRole::Commander: return protocol::Role::kCommander;
	case ESeaRole::Weapons: return protocol::Role::kWeapons;
	case ESeaRole::Solo: return protocol::Role::kSolo;
	case ESeaRole::Observer: break;
	}
	return protocol::Role::kObserver;
}

ESeaRole from_protocol_role(protocol::Role role)
{
	switch (role)
	{
	case protocol::Role::kCommander: return ESeaRole::Commander;
	case protocol::Role::kWeapons: return ESeaRole::Weapons;
	case protocol::Role::kSolo: return ESeaRole::Solo;
	default: break;
	}
	return ESeaRole::Observer;
}

ESeaEntityKind from_protocol_kind(protocol::EntityKind kind)
{
	switch (kind)
	{
	case protocol::EntityKind::kRocket: return ESeaEntityKind::Rocket;
	case protocol::EntityKind::kTrack: return ESeaEntityKind::Track;
	case protocol::EntityKind::kOwnShip: return ESeaEntityKind::OwnShip;
	default: break;
	}
	return ESeaEntityKind::Target;
}

FVector enu_to_ue(double east_m, double north_m, double up_m)
{
	const seashield::client::UeVector ue = seashield::client::to_ue_cm(east_m, north_m, up_m);
	return FVector(ue.x, ue.y, ue.z);
}

}  // namespace

// Network thread: the headlessly-tested ClientSession, with callbacks that
// enqueue raw protocol structs for the game thread (SPSC: one producer here,
// one consumer in USeaNetSubsystem::Tick).
class FSeaNetRunnable final : public FRunnable
{
public:
	FSeaNetRunnable(seashield::client::ClientSessionConfig config)
		: Session(MoveTemp(config), MakeCallbacks())
	{
	}

	virtual uint32 Run() override
	{
		Session.run();
		return 0;
	}

	virtual void Stop() override { Session.stop(); }

	seashield::client::ClientSession Session;
	TQueue<protocol::ServerWelcome, EQueueMode::Spsc> Welcomes;
	TQueue<protocol::Snapshot, EQueueMode::Spsc> Snapshots;
	TQueue<protocol::SnapshotDelta, EQueueMode::Spsc> Deltas;
	TQueue<protocol::EngagementEvent, EQueueMode::Spsc> Events;
	TQueue<protocol::FireSolution, EQueueMode::Spsc> Solutions;

private:
	seashield::client::ClientSessionCallbacks MakeCallbacks()
	{
		seashield::client::ClientSessionCallbacks callbacks;
		callbacks.on_welcome = [this](const protocol::ServerWelcome& welcome) { Welcomes.Enqueue(welcome); };
		callbacks.on_snapshot = [this](const protocol::Snapshot& batch) { Snapshots.Enqueue(batch); };
		callbacks.on_snapshot_delta = [this](const protocol::SnapshotDelta& batch) { Deltas.Enqueue(batch); };
		callbacks.on_event = [this](const protocol::EngagementEvent& event) { Events.Enqueue(event); };
		callbacks.on_fire_solution = [this](const protocol::FireSolution& solution) { Solutions.Enqueue(solution); };
		callbacks.on_error = [](const std::string& what)
		{ UE_LOG(LogSeaShield, Warning, TEXT("net: %hs"), what.c_str()); };
		return callbacks;
	}
};

void USeaNetSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	// Development path (headless smoke, quick PIE iteration): connect straight
	// from the command line, no Blueprint wiring needed.
	//   -SeaServer=host[:port] [-SeaRole=observer|commander|weapons|solo]
	FString Server;
	if (!FParse::Value(FCommandLine::Get(), TEXT("SeaServer="), Server))
	{
		return;
	}
	FString Host = Server;
	FString PortStr;
	int32 Port = 7777;  // Server default (server/main.cpp).
	if (Server.Split(TEXT(":"), &Host, &PortStr))
	{
		Port = FCString::Atoi(*PortStr);
	}
	FString RoleStr;
	FParse::Value(FCommandLine::Get(), TEXT("SeaRole="), RoleStr);
	ESeaRole Role = ESeaRole::Solo;
	if (RoleStr.Equals(TEXT("observer"), ESearchCase::IgnoreCase)) { Role = ESeaRole::Observer; }
	else if (RoleStr.Equals(TEXT("commander"), ESearchCase::IgnoreCase)) { Role = ESeaRole::Commander; }
	else if (RoleStr.Equals(TEXT("weapons"), ESearchCase::IgnoreCase)) { Role = ESeaRole::Weapons; }
	const bool bStarted = Connect(Host, Port, Role);
	UE_LOG(LogSeaShield, Display, TEXT("Auto-connect to %s:%d -> %s"), *Host, Port,
	       bStarted ? TEXT("session thread started") : TEXT("FAILED to start"));
	// Dev/demo: -SeaFire=N fires an N-rocket salvo at the first track that
	// produces a valid fire solution (verification runs, capture sessions).
	FParse::Value(FCommandLine::Get(), TEXT("SeaFire="), PendingDevSalvo);
}

bool USeaNetSubsystem::Connect(const FString& Host, int32 TcpPort, ESeaRole Role)
{
	if (Runnable != nullptr)
	{
		return false;  // One session per game instance; Disconnect() first.
	}
	seashield::client::ClientSessionConfig config;
	config.host = TCHAR_TO_UTF8(*Host);
	config.tcp_port = static_cast<uint16>(TcpPort);
	config.role = to_protocol_role(Role);
	Runnable = new FSeaNetRunnable(MoveTemp(config));
	Thread = FRunnableThread::Create(Runnable, TEXT("SeaShieldNet"));
	return Thread != nullptr;
}

void USeaNetSubsystem::Disconnect()
{
	if (Thread != nullptr)
	{
		Runnable->Stop();
		Thread->WaitForCompletion();
		delete Thread;
		Thread = nullptr;
	}
	delete Runnable;
	Runnable = nullptr;
	bWelcomed = false;
	LatestSolutions.Empty();
}

void USeaNetSubsystem::Deinitialize()
{
	Disconnect();
	Super::Deinitialize();
}

void USeaNetSubsystem::Tick(float DeltaTime)
{
	if (Runnable == nullptr)
	{
		return;
	}
	ClockS += DeltaTime;
	LeadError.AgeS += DeltaTime;
	protocol::ServerWelcome welcome;
	while (Runnable->Welcomes.Dequeue(welcome))
	{
		bWelcomed = true;
		AssignedRole = from_protocol_role(welcome.role);
		Weather.WindCms = enu_to_ue(welcome.surface_wind_east_mps, welcome.surface_wind_north_mps, 0.0);
		Weather.RainIntensity = static_cast<float>(welcome.rain_intensity);
		Weather.GustSigmaMps = static_cast<float>(welcome.gust_sigma_mps);
		UE_LOG(LogSeaShield, Display,
		       TEXT("Welcomed: role=%d wind=(%.1f, %.1f) m/s rain=%.2f gust_sigma=%.2f"),
		       static_cast<int32>(AssignedRole), welcome.surface_wind_east_mps,
		       welcome.surface_wind_north_mps, Weather.RainIntensity, Weather.GustSigmaMps);
	}
	// Events are drained BEFORE snapshots: a kRoundStart resets the snapshot
	// pipeline (the previous wave's entities must not interpolate across the map
	// into the new target), and the fresh wave's snapshot — which may ride the
	// very same frame — then repopulates it.
	protocol::EngagementEvent event;
	while (Runnable->Events.Dequeue(event))
	{
		const uint64 EventKey = (static_cast<uint64>(event.kind) << 48) |
		                        (static_cast<uint64>(event.subject_id) << 32) |
		                        static_cast<uint64>(event.tick);
		bool bAlreadySeen = false;
		SeenEventKeys.Add(EventKey, &bAlreadySeen);
		if (bAlreadySeen)
		{
			continue;
		}
		// Survival-game scoreboard, reconstructed from the event stream.
		switch (static_cast<protocol::EventKind>(event.kind))
		{
		case protocol::EventKind::kRoundStart:
			GameState.Wave = event.subject_id;
			GameState.bGameOver = false;
			Assembler = seashield::client::SnapshotAssembler{};
			Interp = seashield::client::InterpolationBuffer{};
			PendingShots.Reset();  // stale shots would compare against the new wave's target
			LeadError = FSeaLeadError{};
			break;
		case protocol::EventKind::kRocketResolved:
			if (event.killed)
			{
				LastKillMissM = event.miss_distance_m;  // tightness of the killing pass
			}
			break;
		case protocol::EventKind::kTargetDestroyed:
		{
			++GameState.Kills;
			// Points: base × wave multiplier × accuracy × streak.
			const float WaveMult = 1.0f + 0.1f * static_cast<float>(FMath::Max(0, GameState.Wave - 1));
			const float AccBonus = LastKillMissM > 0.0f && LastKillMissM < 5.0f    ? 1.5f
			                       : LastKillMissM > 0.0f && LastKillMissM < 11.0f ? 1.2f
			                                                                       : 1.0f;
			const float StreakMult = 1.0f + 0.05f * static_cast<float>(GameState.Streak);
			GameState.Score += FMath::RoundToInt(100.0f * WaveMult * AccBonus * StreakMult);
			++GameState.Streak;
			GameState.BestStreak = FMath::Max(GameState.BestStreak, GameState.Streak);
			LastKillMissM = 0.0f;
			break;
		}
		case protocol::EventKind::kTargetHitShip:
			++GameState.Leaks;
			GameState.Lives = FMath::Max(0, GameState.MaxLives - GameState.Leaks);
			GameState.Streak = 0;  // a leak breaks the streak
			break;
		case protocol::EventKind::kEngagementEnd:
			GameState.bGameOver = true;
			break;
		default:
			break;
		}
		FSeaEngagementEvent out;
		out.Kind = static_cast<uint8>(event.kind);
		out.SubjectId = event.subject_id;
		out.Tick = static_cast<int64>(event.tick);
		out.MissDistanceM = event.miss_distance_m;
		out.bDetonated = event.detonated;
		out.bKilled = event.killed;
		OnEngagementEvent.Broadcast(out);
	}
	protocol::Snapshot batch;
	while (Runnable->Snapshots.Dequeue(batch))
	{
		if (auto done = Assembler.push(batch))
		{
			if (!bLoggedFirstSnapshot)
			{
				bLoggedFirstSnapshot = true;
				UE_LOG(LogSeaShield, Display, TEXT("Snapshot pipeline live: first completed tick=%u (%d entities)"),
				       done->tick, static_cast<int32>(done->entities.size()));
			}
			// Acking switches this client to the delta stream (protocol v4).
			Runnable->Session.ack_snapshot(done->tick);
			Interp.push(MoveTemp(*done));
		}
	}
	protocol::SnapshotDelta delta_batch;
	while (Runnable->Deltas.Dequeue(delta_batch))
	{
		if (auto done = Assembler.push_delta(delta_batch))
		{
			Runnable->Session.ack_snapshot(done->tick);
			Interp.push(MoveTemp(*done));
		}
	}
	protocol::FireSolution solution;
	while (Runnable->Solutions.Dequeue(solution))
	{
		FSeaFireSolution out;
		out.TrackId = solution.track_id;
		out.bValid = solution.valid;
		out.Pip = enu_to_ue(solution.pip_x, solution.pip_y, solution.pip_z);
		out.TimeOfFlightS = solution.time_of_flight_s;
		out.DispersionRadiusM = solution.dispersion_radius_m;
		LatestSolutions.Add(out.TrackId, out);

		if (PendingDevSalvo > 0 && out.bValid)
		{
			// Let the Kalman filter settle before committing the salvo —
			// firing on the very first solution is the worst case the
			// experiment report quantifies (settle-time effect, §2).
			if (++ValidSolutionStreak >= 16)
			{
				UE_LOG(LogSeaShield, Display,
				       TEXT("Dev salvo: firing %d rockets at track %d (ToF %.1f s)"),
				       PendingDevSalvo, out.TrackId, out.TimeOfFlightS);
				FireAtTrack(out.TrackId, 0.0f, 0.0f, PendingDevSalvo, 3.0f);
				PendingDevSalvo = 0;
			}
		}
	}

	// Mature lead-error shots: one time-of-flight after firing, compare the
	// threat's actual position with where we predicted it would be (the lead we
	// committed to). The split into along-track / vertical is the learnable
	// error — and the measure of how a maneuvering target beats an unguided shot.
	if (PendingShots.Num() > 0)
	{
		TArray<FSeaEntityState> Sample;
		SampleEntities(Sample);
		const FSeaEntityState* Target = nullptr;
		for (const FSeaEntityState& E : Sample)
		{
			if (E.Kind == ESeaEntityKind::Target && E.State == 0)
			{
				Target = &E;
				break;
			}
		}
		for (int32 i = 0; i < PendingShots.Num();)
		{
			if (ClockS < PendingShots[i].MatureAtS)
			{
				++i;
				continue;
			}
			if (Target != nullptr)
			{
				// Decompose the miss in the LOS-relative orthonormal frame
				// (ship -> committed intercept): lateral (right+) / up / along-LOS
				// (long+). Orthonormal, so a diving target no longer double-counts.
				const FVector Miss = Target->Position - PendingShots[i].PredictedUeCm;  // UE cm
				const FVector LosH =
				    FVector(PendingShots[i].PredictedUeCm.X, PendingShots[i].PredictedUeCm.Y, 0.0)
				        .GetSafeNormal();
				const FVector Cross = FVector::CrossProduct(FVector::UpVector, LosH).GetSafeNormal();
				LeadError.bValid = true;
				LeadError.MissM = Miss.Size() / 100.0f;
				LeadError.LateralM = FVector::DotProduct(Miss, Cross) / 100.0f;
				LeadError.UpM = Miss.Z / 100.0f;
				LeadError.LongM = FVector::DotProduct(Miss, LosH) / 100.0f;
				LeadError.AgeS = 0.0f;
			}
			PendingShots.RemoveAt(i);
		}
	}
}

void USeaNetSubsystem::SampleEntities(TArray<FSeaEntityState>& OutEntities) const
{
	OutEntities.Reset();
	const auto render_tick = Interp.render_tick(/*delay_ticks=*/6.0);
	if (!render_tick.has_value())
	{
		return;
	}
	for (const seashield::client::SampledEntity& sampled : Interp.sample(*render_tick))
	{
		FSeaEntityState state;
		state.Id = sampled.id;
		state.Kind = from_protocol_kind(sampled.kind);
		state.State = sampled.state;
		if (sampled.kind == protocol::EntityKind::kTrack)
		{
			state.TrackSigmaM = static_cast<float>(protocol::dequantize_track_sigma(sampled.flags));
		}
		state.Position = enu_to_ue(sampled.pos_x, sampled.pos_y, sampled.pos_z);
		state.Velocity = enu_to_ue(sampled.vel_x, sampled.vel_y, sampled.vel_z);
		state.bExtrapolated = sampled.extrapolated;
		OutEntities.Add(state);
	}
}

bool USeaNetSubsystem::GetFireSolution(int32 TrackId, FSeaFireSolution& OutSolution) const
{
	if (const FSeaFireSolution* found = LatestSolutions.Find(TrackId))
	{
		OutSolution = *found;
		return true;
	}
	return false;
}

void USeaNetSubsystem::AdjustSalvo(int32 Delta)
{
	OrderSalvo = FMath::Clamp(OrderSalvo + Delta, 1, 16);
}

void USeaNetSubsystem::AdjustDispersion(float DeltaMrad)
{
	OrderDispersionMrad = FMath::Clamp(OrderDispersionMrad + DeltaMrad, 0.0f, 20.0f);
}

void USeaNetSubsystem::AdjustTrim(float DeltaAzDeg, float DeltaElDeg)
{
	// Mirror the server's operator-trim clamp (+-15 deg) so the console never
	// shows an order the server would silently reduce.
	OrderAzTrimDeg = FMath::Clamp(OrderAzTrimDeg + DeltaAzDeg, -15.0f, 15.0f);
	OrderElTrimDeg = FMath::Clamp(OrderElTrimDeg + DeltaElDeg, -15.0f, 15.0f);
}

void USeaNetSubsystem::CommitFire()
{
	if (DesignatedTrackId == 0)
	{
		UE_LOG(LogSeaShield, Warning, TEXT("Fire order rejected: no track designated"));
		return;
	}
	UE_LOG(LogSeaShield, Display,
	       TEXT("Fire order: track %d salvo %d disp %.1f mrad trim (%.1f, %.1f) deg"),
	       DesignatedTrackId, OrderSalvo, OrderDispersionMrad, OrderAzTrimDeg, OrderElTrimDeg);
	FireAtTrack(DesignatedTrackId, OrderAzTrimDeg, OrderElTrimDeg, OrderSalvo, OrderDispersionMrad);
}

void USeaNetSubsystem::FireAtTrack(int32 TrackId, float AzimuthOffsetDeg, float ElevationOffsetDeg,
                                   int32 SalvoCount, float DispersionMrad)
{
	if (Runnable == nullptr)
	{
		return;
	}
	protocol::FireRequest fire;
	fire.track_id = static_cast<uint16>(TrackId);
	fire.azimuth_rad = FMath::DegreesToRadians(AzimuthOffsetDeg);
	fire.elevation_rad = FMath::DegreesToRadians(ElevationOffsetDeg);
	fire.salvo_count = static_cast<uint8>(SalvoCount);
	fire.dispersion_mrad = DispersionMrad;
	Runnable->Session.request_fire(fire);
}

void USeaNetSubsystem::FireManual(float AzimuthDeg, float ElevationDeg, int32 SalvoCount,
                                  float DispersionMrad)
{
	if (Runnable == nullptr)
	{
		return;
	}
	protocol::FireRequest fire;
	fire.azimuth_rad = FMath::DegreesToRadians(AzimuthDeg);
	fire.elevation_rad = FMath::DegreesToRadians(ElevationDeg);
	fire.salvo_count = static_cast<uint8>(SalvoCount);
	fire.dispersion_mrad = DispersionMrad;
	Runnable->Session.request_fire(fire);

	// Snapshot the committed lead solution so we can measure, one time-of-flight
	// later, how far the threat slipped out of it (GetLeadError / the thesis).
	TArray<FSeaEntityState> Entities;
	SampleEntities(Entities);
	for (const FSeaEntityState& E : Entities)
	{
		if (E.Kind != ESeaEntityKind::Target || E.State != 0)
		{
			continue;
		}
		const FVector WindCms = Weather.WindCms;
		const FVector WindEnu(WindCms.Y / 100.0, WindCms.X / 100.0, WindCms.Z / 100.0);
		const FVector TPosEnu(E.Position.Y / 100.0, E.Position.X / 100.0, E.Position.Z / 100.0);
		const FVector TVelEnu(E.Velocity.Y / 100.0, E.Velocity.X / 100.0, E.Velocity.Z / 100.0);
		// Launch from the own ship's pose, inheriting its velocity (matches the
		// server's moving-platform trajectory). Zero when the ship is at rest.
		FVector ShipPosEnu = FVector::ZeroVector;
		FVector ShipVelEnu = FVector::ZeroVector;
		FSeaEntityState ShipState;
		if (GetOwnShip(ShipState))
		{
			ShipPosEnu = FVector(ShipState.Position.Y / 100.0, ShipState.Position.X / 100.0,
			                     ShipState.Position.Z / 100.0);
			ShipVelEnu = FVector(ShipState.Velocity.Y / 100.0, ShipState.Velocity.X / 100.0,
			                     ShipState.Velocity.Z / 100.0);
		}
		const SeaBallistics::FFireAid Aid = SeaBallistics::ComputeFireAid(
		    AzimuthDeg, ElevationDeg, WindEnu, TPosEnu, TVelEnu, ShipPosEnu, ShipVelEnu);
		if (Aid.bValid && Aid.TimeOfFlightS > 0.1f)
		{
			FPendingShot Shot;
			// Predicted intercept (the lead solution) back into UE cm.
			Shot.PredictedUeCm = FVector(Aid.LeadEnu.Y * 100.0, Aid.LeadEnu.X * 100.0,
			                             Aid.LeadEnu.Z * 100.0);
			Shot.MatureAtS = ClockS + Aid.TimeOfFlightS;
			PendingShots.Add(Shot);
		}
		break;
	}
}

void USeaNetSubsystem::SteerShip(float Rudder, float Throttle)
{
	if (Runnable == nullptr)
	{
		return;
	}
	protocol::ShipCommand steer;
	steer.rudder = FMath::Clamp(Rudder, -1.0f, 1.0f);
	steer.throttle = FMath::Clamp(Throttle, 0.0f, 1.0f);
	Runnable->Session.request_steer(steer);
}

bool USeaNetSubsystem::GetOwnShip(FSeaEntityState& OutShip) const
{
	TArray<FSeaEntityState> Entities;
	SampleEntities(Entities);
	for (const FSeaEntityState& E : Entities)
	{
		if (E.Kind == ESeaEntityKind::OwnShip)
		{
			OutShip = E;
			return true;
		}
	}
	return false;
}
