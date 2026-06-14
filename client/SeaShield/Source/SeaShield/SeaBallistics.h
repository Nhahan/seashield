#pragma once

#include "CoreMinimal.h"

#include <functional>

// Client-side exterior-ballistics predictor — the gun director's "fire-control
// computer". It mirrors the server's point-mass RK4 model (sim/ballistics.cpp:
// boost-glide thrust + quadratic drag against the air-relative velocity +
// gravity) so the HUD can show where the current bore's salvo will go and where
// it will pass closest to the inbound threat. It is an AID, not the authority:
// the server owns the real trajectory, and the gusts (OU turbulence, by design
// unpredictable) plus the launcher dispersion are exactly what the predictor
// cannot know — the gap between the computed intercept and where the rockets
// really fall IS the difficulty of unguided gunnery the whole project quantifies.
//
// All vectors are SIM ENU METERS (x=east, y=north, z=up) relative to the ship at
// the logical origin; the caller maps results to UE centimetres at the boundary.
namespace SeaBallistics
{
// Default ordnance — mirrors sim::RocketParams (sim/ballistics.h). The game
// scenario does not override these, so the predictor matches the live rockets
// up to the unmodelled gust/dispersion.
inline constexpr double kMassKg = 60.0;
inline constexpr double kCdA = 0.012;
inline constexpr double kThrustN = 18000.0;
inline constexpr double kBurnTimeS = 1.5;
inline constexpr double kRailExitMps = 30.0;
inline constexpr double kGravityMps2 = 9.80665;
inline constexpr double kLaunchUpM = 2.0;       // sim::kLaunchPosition.z (deck).
inline constexpr double kMaxFlightS = 30.0;     // integration ceiling.
inline constexpr double kStepS = 0.05;          // 20 Hz — an aid, not a render path.

inline FVector BoreDirEnu(double AzimuthDeg, double ElevationDeg)
{
	const double Az = FMath::DegreesToRadians(AzimuthDeg);
	const double El = FMath::DegreesToRadians(ElevationDeg);
	return FVector(FMath::Sin(Az) * FMath::Cos(El), FMath::Cos(Az) * FMath::Cos(El),
	               FMath::Sin(El));
}

inline double AirDensity(double AltitudeM)
{
	// Exponential atmosphere — a faithful-enough stand-in for the server's
	// temperature/humidity model at the altitudes rockets fly (scale ~8.5 km).
	return 1.225 * FMath::Exp(-FMath::Max(0.0, AltitudeM) / 8500.0);
}

inline FVector RocketAccel(const FVector& Pos, const FVector& Vel, double AgeS,
                           const FVector& WindEnu, const FVector& LaunchDir)
{
	FVector Accel(0.0, 0.0, -kGravityMps2);
	if (AgeS < kBurnTimeS && kThrustN > 0.0)
	{
		const double Speed = Vel.Size();
		const FVector ThrustDir = Speed > 1.0 ? Vel / Speed : LaunchDir;
		Accel += ThrustDir * (kThrustN / kMassKg);
	}
	const FVector VRel = Vel - WindEnu;
	const double RelSpeed = VRel.Size();
	if (RelSpeed > 0.0)
	{
		const double K = 0.5 * AirDensity(FMath::Max(0.0, Pos.Z)) * kCdA / kMassKg;
		Accel -= VRel * (K * RelSpeed);
	}
	return Accel;
}

// Integrate the bore trajectory (RK4), calling Visit(pos, age) each step until
// it returns to the sea or hits the flight ceiling. Returns true if it splashed.
inline bool IntegrateBore(double AzimuthDeg, double ElevationDeg, const FVector& WindEnu,
                          const TFunctionRef<void(const FVector&, double)> Visit)
{
	const FVector LaunchDir = BoreDirEnu(AzimuthDeg, ElevationDeg);
	FVector Pos(0.0, 0.0, kLaunchUpM);
	FVector Vel = LaunchDir * kRailExitMps;
	double Age = 0.0;
	auto A = [&](const FVector& P, const FVector& V, double T)
	{ return RocketAccel(P, V, T, WindEnu, LaunchDir); };
	Visit(Pos, Age);
	const int MaxSteps = static_cast<int>(kMaxFlightS / kStepS);
	for (int Step = 0; Step < MaxSteps; ++Step)
	{
		const FVector k1p = Vel;
		const FVector k1v = A(Pos, Vel, Age);
		const FVector k2p = Vel + k1v * (kStepS * 0.5);
		const FVector k2v = A(Pos + k1p * (kStepS * 0.5), Vel + k1v * (kStepS * 0.5), Age + kStepS * 0.5);
		const FVector k3p = Vel + k2v * (kStepS * 0.5);
		const FVector k3v = A(Pos + k2p * (kStepS * 0.5), Vel + k2v * (kStepS * 0.5), Age + kStepS * 0.5);
		const FVector k4p = Vel + k3v * kStepS;
		const FVector k4v = A(Pos + k3p * kStepS, Vel + k3v * kStepS, Age + kStepS);
		Pos += (k1p + (k2p + k3p) * 2.0 + k4p) * (kStepS / 6.0);
		Vel += (k1v + (k2v + k3v) * 2.0 + k4v) * (kStepS / 6.0);
		Age += kStepS;
		Visit(Pos, Age);
		if (Pos.Z <= 0.0 && Age > 0.5)
		{
			return true;
		}
	}
	return false;
}

// Where the current bore's salvo crosses the sea (used when there is no target
// to intercept — still shows the player where shots are going).
struct FImpact
{
	FVector EnuMeters = FVector::ZeroVector;
	float TimeOfFlightS = 0.0f;
	bool bValid = false;
};

inline FImpact PredictImpact(double AzimuthDeg, double ElevationDeg, const FVector& WindEnu)
{
	FImpact Out;
	FVector Prev(0, 0, kLaunchUpM);
	double PrevAge = 0.0;
	bool bDone = false;
	IntegrateBore(AzimuthDeg, ElevationDeg, WindEnu,
	              [&](const FVector& P, double Age)
	              {
		              if (!bDone && P.Z <= 0.0 && Age > 0.5)
		              {
			              const double Span = Prev.Z - P.Z;
			              const double Alpha = Span > 1e-6 ? Prev.Z / Span : 0.0;
			              Out.EnuMeters = FMath::Lerp(Prev, P, static_cast<float>(Alpha));
			              Out.EnuMeters.Z = 0.0;
			              Out.TimeOfFlightS = static_cast<float>(Age - kStepS * (1.0 - Alpha));
			              Out.bValid = true;
			              bDone = true;
		              }
		              Prev = P;
		              PrevAge = Age;
	              });
	return Out;
}

// Fire-control intercept solution against a moving target (CV extrapolation).
// Mirrors the sim's per-tick closest-approach adjudication: find the instant the
// bore trajectory passes nearest the target's future position, in 3D. The
// pipper sits where the salvo will be then; the lead ghost where the target will
// be. Align them (MissM small) to score a hit.
struct FFireAid
{
	FVector PipperEnu = FVector::ZeroVector;  // bore position at intercept time
	FVector LeadEnu = FVector::ZeroVector;    // target position at intercept time
	float TimeOfFlightS = 0.0f;
	float MissM = 0.0f;  // 3D gap at closest approach (aim quality)
	bool bValid = false;
};

inline FFireAid ComputeFireAid(double AzimuthDeg, double ElevationDeg, const FVector& WindEnu,
                               const FVector& TargetPosEnu, const FVector& TargetVelEnu)
{
	FFireAid Out;
	double BestMiss = 1e30;
	IntegrateBore(AzimuthDeg, ElevationDeg, WindEnu,
	              [&](const FVector& P, double Age)
	              {
		              if (Age < 0.05)
		              {
			              return;  // skip the muzzle.
		              }
		              const FVector TargetAt = TargetPosEnu + TargetVelEnu * Age;
		              const double D = FVector::Distance(P, TargetAt);
		              if (D < BestMiss)
		              {
			              BestMiss = D;
			              Out.PipperEnu = P;
			              Out.LeadEnu = TargetAt;
			              Out.TimeOfFlightS = static_cast<float>(Age);
			              Out.MissM = static_cast<float>(D);
			              Out.bValid = true;
		              }
	              });
	return Out;
}

// Coarse search for the (azimuth, elevation) that best intercepts the target —
// the scripted auto-gunner uses this to play a wave for capture verification.
// Returns the best aim and its miss; searches around the straight-line bearing.
struct FAimSolution
{
	float AzimuthDeg = 0.0f;
	float ElevationDeg = 0.0f;
	float MissM = 1e30f;
	bool bValid = false;
};

inline FAimSolution SolveAim(const FVector& WindEnu, const FVector& TargetPosEnu,
                             const FVector& TargetVelEnu)
{
	FAimSolution Best;
	const double CenterAz = FMath::RadiansToDegrees(FMath::Atan2(TargetPosEnu.X, TargetPosEnu.Y));
	for (double Az = CenterAz - 25.0; Az <= CenterAz + 25.0; Az += 1.5)
	{
		for (double El = 4.0; El <= 70.0; El += 1.5)
		{
			const FFireAid Aid = ComputeFireAid(Az, El, WindEnu, TargetPosEnu, TargetVelEnu);
			if (Aid.bValid && Aid.MissM < Best.MissM)
			{
				Best.MissM = Aid.MissM;
				Best.AzimuthDeg = static_cast<float>(Az);
				Best.ElevationDeg = static_cast<float>(El);
				Best.bValid = true;
			}
		}
	}
	return Best;
}
}  // namespace SeaBallistics
