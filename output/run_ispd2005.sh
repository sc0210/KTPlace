#!/bin/zsh
# Runs the ISPD 2005 suite; everything lands under this repo's output/.
#
# adaptec1 is vendored in benchmark/, so it always runs. The other seven designs
# are not in the tree and have no working download URL, so they run only where
# someone has already unpacked them, and are reported as SKIP rather than failing.
# The per-design summary goes to output/_logs/suite.log and a one-line verdict for
# each design goes to stdout.
set -u

# The repo root, from this script's own location, so the suite runs the same
# wherever the checkout is rather than only where it was written.
ROOT="${0:A:h:h}"
cd "$ROOT" || exit 1

LOGDIR="$ROOT/output/_logs"
mkdir -p "$LOGDIR"
SUITE_LOG="$LOGDIR/suite.log"

DESIGNS=(adaptec1 adaptec2 adaptec3 adaptec4 bigblue1 bigblue2 bigblue3 bigblue4)

# Animation on, and the frame budget raised well above the default: the suite is
# how the animation is looked at, and the default 300 frames on a 12-iteration
# run leaves the legalizer and the detailed placer unrepresented. The stills are
# removed once each GIF is written, so this costs no disk in the end.
run_one() {
  local name="$1" d="$2"
  rm -rf "output/$name"
  mkdir -p "output/$name"
  echo "=== START $name ($(date +%T))" | tee -a "$SUITE_LOG"

  KTPLACE_ANIM=1 KTPLACE_ANIM_MAX_FRAMES=1200 KTPLACE_ANIM_BLEND=2 \
  KTPLACE_SIMPL_TRACE_EVERY=1 KTPLACE_SIMPL_CG_EVERY=8 \
    ./build/bin/ktplace "$d" -a simpl -w "output/$name" > "$LOGDIR/$name.log" 2>&1
  local rc=$?

  # The stills are deleted once the GIF is written, so the frame count is read
  # from the run's own report rather than by counting files that are gone.
  local n
  n=$(grep -oE "animation: [0-9]+ frame" "$LOGDIR/$name.log" 2>/dev/null | grep -oE "[0-9]+")
  local g
  g=$(ls -la "output/$name/plots/anim/placement.gif" 2>/dev/null | awk '{print $5}')
  local v u p
  v=$(grep -oE "verdict *\| *[A-Z]+" "output/$name/ktplace.log" 2>/dev/null | grep -oE "[A-Z]+$")
  # Matched on the row's full name, not on the bare word "utilisation": that
  # word is now the start of two rows, "utilisation (movable / rows)" and
  # "utilisation (incl. fixed cells)", and grepping for the prefix silently
  # matched neither once the labels were disambiguated -- the suite printed
  # util=? and nobody could tell a naming change from a real regression.
  u=$(grep -oE "utilisation \(movable / rows\) *\| *[0-9.]+%" \
        "output/$name/ktplace.log" 2>/dev/null | grep -oE "[0-9.]+%" | head -1)
  # The high-resolution final still, when one was written.
  p=$(ls -la "output/$name/plots/final/final.png" 2>/dev/null | awk '{print $5}')

  echo "=== END $name rc=$rc frames=${n:-?} gif=${g:-none}B verdict=${v:-?} util=${u:-?} final=${p:-none}B ($(date +%T))" \
    | tee -a "$SUITE_LOG"
}

for name in "${DESIGNS[@]}"; do
  d="benchmark/ISPD_2005/$name"
  if [[ ! -d "$d" ]]; then
    echo "=== SKIP $name (absent)" | tee -a "$SUITE_LOG"
    continue
  fi
  run_one "$name" "$d"
done

echo "=== SUITE DONE ($(date +%T))" | tee -a "$SUITE_LOG"
