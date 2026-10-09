#!/usr/bin/env bash
#
# Mutation testing: change ONE character of the shipped code and require that a
# check fails. A check that still passes against a mutant was not looking at
# the code it claims to cover -- and a harness that compiled its own copy of
# the shaders would pass every GLSL mutant here.
#
# Each mutant is a copy of the tree in a temporary directory (the FFGL SDK is
# symlinked, not copied), built arm64-only, with only the named check run at
# CI's raster, under a time limit (a mutant can hang: honeydew's lesson; a
# timeout counts as caught).
#
#     tools/mutate.sh
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# file | the exact text | its one-character mutant | the check that must fail | what it is
# The fields are split on '|', so neither a target nor a mutant may contain one
# (conway's trap).
MUTANTS=(
	"source/Shaders.cpp|float lens = ( Hooks & 1 ) != 0 ? M : M + 1.0;|float lens = ( Hooks & 1 ) != 0 ? M : M - 1.0;|--defocus|GLSL: the blur circle's ( M + 1 ) as ( M - 1 ), + -> -"
	"source/Shaders.cpp|return st.w - bowAt( p ) - Parfocal * log2( st.z / DesignZoom );|return st.w + bowAt( p ) - Parfocal * log2( st.z / DesignZoom );|--field|GLSL: the bow added to the knob instead of taken from it, - -> +"
	"source/Shaders.cpp|fall    = ( Hooks & 8 ) != 0 ? q * sqrt( q ) : q * q;|fall    = ( Hooks & 8 ) != 0 ? q * sqrt( q ) : q + q;|--screen|GLSL: the falloff 2 q instead of q squared, * -> +"
	"source/Shaders.cpp|fragColor = sum / ( scale.x * scale.y );|fragColor = sum / ( scale.x + scale.y );|--mips|GLSL: a mip texel's area as the sum of its sides, * -> +"
	"source/Shaders.cpp|f = 1.0 - f;|f = 1.0 + f;|--stock|GLSL: a negative adds the exposure instead of reversing it, - -> +"
	"source/Shaders.cpp|cell.y * Grid.x + cell.x;|cell.y * Grid.y + cell.x;|--filmed|GLSL: the filmed frame's index counts rows by the wrong side, x -> y"
	"source/Hand.cpp|return x * x * x * ( 10.0 - 15.0 * x + 6.0 * x * x );|return x * x * x * ( 10.0 - 16.0 * x + 6.0 * x * x );|--fitts|C++: minimum jerk's 15 as 16, 5 -> 6"
	"source/Hand.cpp|const double ep = -2.0 * zeta * vh / w;|const double ep = -3.0 * zeta * vh / w;|--carriage|C++: the grip's particular solution 3 zeta, not 2, 2 -> 3"
	"source/Hand.cpp|std::log2( distance / width + 1.0 );|std::log2( distance / width + 2.0 );|--fitts|C++: Shannon's + 1 as + 2, 1 -> 2"
	"source/Reader.cpp|return std::fabs( defocusMm ) * magnification / ( 2.0 * fNumber * ( magnification + 1.0 ) );|return std::fabs( defocusMm ) * magnification / ( 3.0 * fNumber * ( magnification + 1.0 ) );|--defocus|C++: the blur circle's radius over 3 N, not 2 N, 2 -> 3"
	"source/Fiche.cpp|filmClock -= interval;|filmClock += interval;|--filmed|C++: the camera's clock gains an interval instead of spending one, - -> +"
	"source/Shaders.cpp|return p - CardSize * floor( p / CardSize );|return p + CardSize * floor( p / CardSize );|--endless|GLSL: a point before the tile moved a tile further away, not onto it, - -> +"
	"source/Hand.cpp|targetX += card.width * std::round( ( planned.x - targetX ) / card.width );|targetX += card.width * std::round( ( planned.x + targetX ) / card.width );|--operator-law|C++: the nearest copy of a view found from the sum of positions, not the difference, - -> +"
)

limit() {
	# macOS has no timeout(1): perl's alarm is enough.
	perl -e 'alarm shift; exec @ARGV' "$@"
}

caught=0
for entry in "${MUTANTS[@]}"; do
	IFS='|' read -r file original mutant check what <<<"$entry"
	tree="$WORK/tree"
	rm -rf "$tree"
	mkdir -p "$tree/external"
	cp -R "$REPO/source" "$REPO/tools" "$REPO/cmake" "$REPO/CMakeLists.txt" "$tree/"
	ln -s "$REPO/external/ffgl" "$tree/external/ffgl"

	python3 - "$tree/$file" "$original" "$mutant" <<'PY'
import sys, pathlib
path, original, mutant = pathlib.Path(sys.argv[1]), sys.argv[2], sys.argv[3]
text = path.read_text()
if text.count(original) != 1:
    sys.exit(f"mutation target found {text.count(original)} times in {path}: '{original}'")
if sum(a != b for a, b in zip(original, mutant)) != 1 or len(original) != len(mutant):
    sys.exit("a mutant must differ by exactly one character")
path.write_text(text.replace(original, mutant))
PY

	printf '\n== mutant: %s\n' "$what"
	cmake -S "$tree" -B "$tree/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 >/dev/null
	cmake --build "$tree/build" --target mftest -j"$(sysctl -n hw.ncpu)" >/dev/null 2>&1
	status=0
	extra=()
	[[ "$check" == "--fitts" || "$check" == "--operator-law" ]] || extra=( --size 320x180 )
	limit 300 "$tree/build/mftest" "$check" "${extra[@]}" >"$WORK/log" 2>&1 || status=$?
	if [[ "$status" -eq 0 ]]; then
		printf '   FAIL  %s still PASSES -- the check does not cover this code\n' "$check"
	else
		printf '   ok    %s fails against the mutant (exit %d):\n' "$check" "$status"
		grep -E '^  FAIL' "$WORK/log" | head -2 | cut -c1-160 | sed 's/^/        /'
		caught=$(( caught + 1 ))
	fi
done

printf '\nmutants: %d, caught: %d\n' "${#MUTANTS[@]}" "$caught"
[[ "$caught" -eq "${#MUTANTS[@]}" ]]
