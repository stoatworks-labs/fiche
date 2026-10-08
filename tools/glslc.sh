#!/usr/bin/env bash
#
# Every shader through glslc, the one shader check that needs no GL driver.
# Called by tools/verify.sh AND by CI, so the two cannot drift: a runner with
# no accelerated GL cannot compile a shader through a driver, and this is how
# CI covers the shaders instead (pwtest --offline covers the CPU half).
#
#     tools/glslc.sh          exit 0 when every shader compiles, or glslc is absent
#     GLSLC_REQUIRED=1 tools/glslc.sh   ...and a missing glslc is a failure (CI)
#
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

#---------------------------------------------------------------------------
# Every shader, through a real GLSL compiler, before a host has to find out.
#
# A shader that will not compile presents to an operator as "the effect does
# nothing", with the real message buried in the diagnostics log -- so without
# this it is caught at run time, in a host, or not at all.
#
# --target-env=opengl4.5 with -fauto-map-locations: glslc targets SPIR-V, which
# demands an explicit layout( location ) on every uniform and varying. Those are
# Vulkan rules and not GLSL ones, and without the flag every shader "fails" for
# reasons that have nothing to do with the code.
#
# glslc is optional -- `brew install shaderc` -- so a machine without it skips
# rather than fails.
#---------------------------------------------------------------------------
shaders_compile() {
	local dir bad=0 n=0 shader

	if ! command -v glslc >/dev/null 2>&1; then
		printf '   skipped: glslc not installed (brew install shaderc)\n'
		return 0
	fi

	dir="$( mktemp -d )"

	python3 - "$dir" <<'SHADERS_PY'
import re, sys, pathlib
out = pathlib.Path( sys.argv[ 1 ] )

# Where this repo keeps its GLSL.
FILES = [
	"source/Shaders.cpp",
]

# Shaders the plugin assembles at run time: kVersion + kCommon + the pass, the
# order Shaders.cpp's Assemble() uses. Every pass is assembled, so every one is
# listed; a name that has moved is a KeyError.
ASSEMBLED = {
	"quad":      [ "kVersion", "kQuadVertex" ],
	"copy":      [ "kVersion", "kCommon", "kCopyFragment" ],
	"mip":       [ "kVersion", "kCommon", "kMipFragment" ],
	"film":      [ "kVersion", "kCommon", "kFilmFragment" ],
	"mip_layer": [ "kVersion", "kCommon", "kMipLayerFragment" ],
	"screen":    [ "kVersion", "kCommon", "kScreenFragment" ],
}

named, unnamed = {}, []
for f in FILES:
	text = pathlib.Path( f ).read_text()
	for m in re.finditer( r'(?:(\w+)\s*(?:\[\s*\])?\s*=\s*)?R"\((.*?)\)"', text, re.S ):
		if m.group( 1 ): named[ m.group( 1 ) ] = m.group( 2 )
		else:            unnamed.append( m.group( 2 ) )
	# Adjacent string literals, joined: MSVC C2026 caps one literal at about
	# 16 KB, so a shader that outgrows it is split and has to be rejoined here.
	for m in re.finditer( r'(\w+)\s*=\s*((?:"(?:[^"\\\n]|\\.)*"\s*)+);', text ):
		named.setdefault( m.group( 1 ), "".join(
			s.encode().decode( "unicode_escape" )
			for s in re.findall( r'"((?:[^"\\\n]|\\.)*)"', m.group( 2 ) ) ) )

def emit( name, body ):
	# The vertex shader is the one that writes gl_Position; everything else is a
	# fragment shader. glslc takes the stage from the extension.
	ext = ".vert" if re.search( r"\bgl_Position\s*=", body ) else ".frag"
	( out / ( name + ext ) ).write_text( body )

def piece( p ):
	# An int indexes the raw strings that are not assigned to a name, in source
	# order. A literal starts with #version. Anything else names a constant
	# above -- and a name that has moved is a KeyError here, not a silent skip.
	if isinstance( p, int ):       return unnamed[ p ]
	if p.startswith( "#version" ): return p
	return named[ p ]

for name, body in named.items():
	if body.lstrip().startswith( "#version" ) and "void main" in body:
		emit( name, body )

# A piece that no pass uses is a pass that is not being checked.
used = { p for parts in ASSEMBLED.values() for p in parts }
for name in named:
	if name.startswith( "k" ) and name not in used:
		sys.exit( f"{name} is a shader piece no ASSEMBLED entry uses" )

for name, parts in ASSEMBLED.items():
	emit( name, "".join( piece( p ) for p in parts ) )
SHADERS_PY

	for shader in "$dir"/*.vert "$dir"/*.frag; do
		[ -e "$shader" ] || continue
		n=$(( n + 1 ))
		if ! glslc --target-env=opengl4.5 -fauto-map-locations \
			   "$shader" -o /dev/null 2>"$dir/err"; then
			printf '   %s does not compile\n' "$( basename "$shader" )"
			sed "s|$dir/||; s|^|      |" "$dir/err"
			bad=$(( bad + 1 ))
		fi
	done

	if [ "$n" -eq 0 ]; then
		# No shaders at all is a FAILURE, not a pass. It means the extraction
		# above has lost track of where this repo keeps its GLSL, and a check
		# that silently looks at nothing is worse than no check.
		printf '   no shaders were extracted -- the extraction has gone stale\n'
		rm -rf "$dir"
		return 1
	fi

	if [ "$bad" -eq 0 ]; then
		printf '   %d shaders, all compile\n' "$n"
	fi
	rm -rf "$dir"
	return "$bad"
}

if ! command -v glslc >/dev/null 2>&1 && [ "${GLSLC_REQUIRED:-0}" = 1 ]; then
	printf '   glslc is required here and not installed\n'
	exit 1
fi
shaders_compile
