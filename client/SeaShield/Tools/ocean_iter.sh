#!/bin/zsh
# ocean_iter.sh [tag] — one realistic-ocean iteration: real-RHI apply (builds M_Ocean + spawns
# SM_Ocean + hides plugin surface + saves throwaway L_RangeCustom) then diagnostic cinematic
# stills (hull grazing / hero / godray-to-sun / horizon). Real RHI so the translucent shader
# compiles. Each capture self-quits. NEVER writes L_Range. Mesh import is a separate one-time step
# (import_ocean.py).
set -u
TAG="${1:-ocn}"
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
UE="${UE:-/Users/Shared/Epic Games/UE_5.7}"
PROJ="$ROOT/client/SeaShield/SeaShield.uproject"
T="$ROOT/client/SeaShield/Tools"
SHOT="$ROOT/client/SeaShield/Saved/Screenshots/MacEditor"
ALOG="/tmp/ocean_apply.log"; : > "$ALOG"

echo "=== APPLY (real RHI: build M_Ocean + L_RangeCustom) ==="
"$UE/Engine/Binaries/Mac/UnrealEditor" "$PROJ" -windowed -ResX=256 -ResY=256 -unattended -NoSound -nosplash \
    -ExecCmds="py $T/apply_ocean.py" -abslog="$ALOG"
echo "apply editor exited rc=$?"
if ! grep -q "SeaShieldOcean: DONE\|L_RangeCustom saved" "$ALOG"; then
  echo "!!! APPLY did not reach DONE — tail:"; tail -40 "$ALOG"; exit 1
fi
grep -iE "M_Ocean built|Ocean_Cine spawned|plugin surface hidden|Failed to compile.*M_Ocean" "$ALOG" | tail -6

shoot() { # name camera
  local name="$1" cam="$2"
  echo "=== capture $name  cam=$cam ==="
  "$T/cinematic_shot.sh" --idle --shot 6 --dur 16 --map /Game/SeaShield/Maps/L_RangeCustom --cam "$cam"
  if [ -f "$SHOT/SeaShot.png" ]; then mv "$SHOT/SeaShot.png" "$SHOT/${TAG}_${name}.png"; echo "  -> ${TAG}_${name}.png"; else echo "  !!! no SeaShot.png"; fi
}
shoot hull    "295000,297000,820,-4,30"
shoot hero    "289000,293200,1600,-4,32"
shoot godray  "313000,309000,2800,7,216"
echo "=== DONE ==="
ls -la "$SHOT"/${TAG}_*.png
