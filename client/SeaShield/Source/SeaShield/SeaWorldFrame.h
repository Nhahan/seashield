#pragma once

#include "CoreMinimal.h"

// Where the simulation's ENU origin (the ownship) sits on the UE stage.
//
// Deliberately NOT the world origin: UE 5.7's Water plugin renders a corrupted
// ~512 m patch of ocean anchored at world zero on Metal (the zone-view data
// window falls back to its defaults there — isolated by probe captures: the
// patch tracks neither the camera, the zone actor, the ocean spline, nor any
// mesh, and the sea is flawless everywhere else). Keeping the play area on a
// far bearing hides it without touching engine code.
//
// Only actor placement uses this offset. USeaNetSubsystem's coordinate space
// (SampleEntities, fire solutions, the PPI) keeps the ownship at its logical
// origin — subtract/add SeaWorldFrame::Origin exactly once when crossing
// between logic space and the stage.
namespace SeaWorldFrame
{
inline const FVector Origin(300000.0, 300000.0, 0.0);  // 3 km north-east of world zero.
}
