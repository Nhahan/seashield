#include "SeaNetSubsystem.h"

#include "Containers/Queue.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"

#include "client/core/client_session.h"
#include "client/core/coords.h"
#include "protocol/messages.h"

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
		{ UE_LOG(LogTemp, Warning, TEXT("SeaShield net: %hs"), what.c_str()); };
		return callbacks;
	}
};

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

void USeaNetSubsystem::Tick(float)
{
	if (Runnable == nullptr)
	{
		return;
	}
	protocol::ServerWelcome welcome;
	while (Runnable->Welcomes.Dequeue(welcome))
	{
		bWelcomed = true;
		AssignedRole = from_protocol_role(welcome.role);
		Weather.WindCms = enu_to_ue(welcome.surface_wind_east_mps, welcome.surface_wind_north_mps, 0.0);
		Weather.RainIntensity = static_cast<float>(welcome.rain_intensity);
		Weather.GustSigmaMps = static_cast<float>(welcome.gust_sigma_mps);
	}
	protocol::Snapshot batch;
	while (Runnable->Snapshots.Dequeue(batch))
	{
		if (auto done = Assembler.push(batch))
		{
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
	protocol::EngagementEvent event;
	while (Runnable->Events.Dequeue(event))
	{
		FSeaEngagementEvent out;
		out.Kind = static_cast<uint8>(event.kind);
		out.SubjectId = event.subject_id;
		out.Tick = static_cast<int64>(event.tick);
		out.MissDistanceM = event.miss_distance_m;
		out.bDetonated = event.detonated;
		out.bKilled = event.killed;
		OnEngagementEvent.Broadcast(out);
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
}
