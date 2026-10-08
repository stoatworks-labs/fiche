#include "Shaders.h"

namespace fiche::shaders
{

const char* const kVersion = "#version 410 core\n";

const char* const kQuadVertex = R"(
layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv          = vUV;
}
)";

//---------------------------------------------------------------------------
// What every fragment pass shares: the hash and the sRGB transfer.
//---------------------------------------------------------------------------
const char* const kCommon = R"(
uniform uint Seed;
uniform int Hooks;// negative-control hooks; 0 in every shipped frame

//The PCG output mix: exact 32-bit integer arithmetic, the same on every GPU
//and in Hash.h. Never fract( sin( x ) ).
uint pcg( uint v )
{
	uint state = v * 747796405u + 2891336453u;
	uint word  = ( ( state >> ( ( state >> 28u ) + 4u ) ) ^ state ) * 277803737u;
	return ( word >> 22u ) ^ word;
}
uint hash3( uint a, uint b, uint c )
{
	return pcg( a ^ pcg( b ^ pcg( c ) ) );
}
//24 bits of a hash as a float in [0, 1), exactly.
float unit( uint h )
{
	return float( h >> 8u ) * ( 1.0 / 16777216.0 );
}
uint pack2( ivec2 v )
{
	return uint( v.x ) | ( uint( v.y ) << 16u );
}

//The sRGB transfer, both ways, on 0..1.
vec3 toLinear( vec3 c )
{
	vec3 lo = c / 12.92;
	vec3 hi = pow( ( c + 0.055 ) / 1.055, vec3( 2.4 ) );
	return mix( hi, lo, lessThanEqual( c, vec3( 0.04045 ) ) );
}
vec3 toCode( vec3 l )
{
	vec3 lo = 12.92 * l;
	vec3 hi = 1.055 * pow( l, vec3( 1.0 / 2.4 ) ) - 0.055;
	return mix( hi, lo, lessThanEqual( l, vec3( 0.0031308 ) ) );
}
)";

//---------------------------------------------------------------------------
// 1. The clip, in linear light.
//---------------------------------------------------------------------------
const char* const kCopyFragment = R"(
uniform sampler2D InputTexture;

out vec4 fragColor;

void main()
{
	vec4 c    = texelFetch( InputTexture, ivec2( gl_FragCoord.xy ), 0 );
	fragColor = vec4( toLinear( clamp( c.rgb, 0.0, 1.0 ) ), c.a );
}
)";

//---------------------------------------------------------------------------
// 2. One box mip level of the live texture. The texture's BASE and MAX level
// are both the level below while this runs, so the level being written is not
// one being read (GL 4.1, 9.3.1), and texelFetch's level 0 is that level.
//---------------------------------------------------------------------------
const char* const kMipFragment = R"(
uniform sampler2D Source;
uniform ivec2 SourceSize;

out vec4 fragColor;

void main()
{
	ivec2 p  = 2 * ivec2( gl_FragCoord.xy );
	ivec2 hi = SourceSize - 1;
	vec4 a   = texelFetch( Source, min( p, hi ), 0 );
	vec4 b   = texelFetch( Source, min( p + ivec2( 1, 0 ), hi ), 0 );
	vec4 c   = texelFetch( Source, min( p + ivec2( 0, 1 ), hi ), 0 );
	vec4 d   = texelFetch( Source, min( p + ivec2( 1, 1 ), hi ), 0 );
	fragColor = 0.25 * ( a + b + c + d );
}
)";

//---------------------------------------------------------------------------
// 3. A step-and-repeat camera's exposure: the clip into one frame of the
// store, sRGB-coded so eight bits hold it.
//---------------------------------------------------------------------------
const char* const kFilmFragment = R"(
uniform sampler2D LiveTex;
uniform vec2 LiveSize;
uniform vec2 StoreSize;
uniform float LiveLevels;

out vec4 fragColor;

void main()
{
	vec2 st   = gl_FragCoord.xy / StoreSize;
	float lod = log2( max( LiveSize.x / StoreSize.x, 1.0 ) );
	vec3 lin  = textureLod( LiveTex, st, min( lod, LiveLevels ) ).rgb;
	fragColor = vec4( toCode( clamp( lin, 0.0, 1.0 ) ), 1.0 );
}
)";

//---------------------------------------------------------------------------
// 4. One box mip level of one store layer, averaged in linear light.
//---------------------------------------------------------------------------
const char* const kMipLayerFragment = R"(
uniform sampler2DArray Source;
uniform int Layer;
uniform ivec2 SourceSize;

out vec4 fragColor;

vec3 fetch( ivec2 p )
{
	return toLinear( texelFetch( Source, ivec3( min( p, SourceSize - 1 ), Layer ), 0 ).rgb );
}

void main()
{
	ivec2 p   = 2 * ivec2( gl_FragCoord.xy );
	vec3 lin  = 0.25 * ( fetch( p ) + fetch( p + ivec2( 1, 0 ) ) + fetch( p + ivec2( 0, 1 ) ) + fetch( p + ivec2( 1, 1 ) ) );
	fragColor = vec4( toCode( lin ), 1.0 );
}
)";

//---------------------------------------------------------------------------
// 5. The reader, to the host.
//
// Units: millimetres. A screen point s is measured from the screen's centre,
// y up; a film point f from the card's top-left, y down; the carriage
// position P is the film point at the screen's centre, so f = P + ( s.x,
// -s.y ) / M. Each of the exposure's states is ( P.x, P.y, M, knob ).
//---------------------------------------------------------------------------
const char* const kScreenFragment = R"(
uniform sampler2D LiveTex;      // the clip, linear, box mips
uniform sampler2DArray StoreTex;// the filmed frames, sRGB-coded, box mips
uniform sampler2D BowTex;       // the card's height every millimetre
uniform sampler2D FontTex;      // the title's glyphs, 8 x 8 texels each
uniform sampler2D InputTexture;

uniform ivec2 Size;      // the output, pixels
uniform float PxPerMm;   // pixels per screen millimetre
uniform vec2 LiveSize;
uniform float LiveLevels;
uniform vec2 StoreSize;
uniform float StoreLevels;
uniform int Filmed;      // 0: every frame is the live clip; 1: the store
uniform int Written;     // frames the camera has exposed so far

uniform ivec2 Grid;      // columns, rows
uniform vec2 FrameSize;  // mm
uniform vec2 GridOrigin; // frame ( 0, 0 )'s top-left, mm
uniform float Gutter;    // mm
uniform vec2 CardSize;   // mm
uniform vec2 BowSize;    // samples
uniform float HeaderHeight;

uniform vec4 States[ 17 ];// the exposure: ( P.x, P.y, M, knob ), oldest first
uniform int StateCount;
uniform float FNumber;
uniform float Parfocal;  // mm of focus per doubling of magnification
uniform float DesignZoom;
uniform vec2 Disc[ 32 ]; // golden-angle directions, from the CPU

uniform int Negative;
uniform int Colour;
uniform vec3 Base;       // the film's transmittance where clear
uniform vec3 Dense;      // ...and where densest
uniform uint TDust;      // a dust cell holds a particle
uniform uint TScratch;   // a 2 mm band holds a scratch

uniform int Title[ 40 ];
uniform int TitleLength;
uniform vec2 FontSize;

uniform vec3 Lamp;
uniform float Throw;     // mm; 0 is no falloff
uniform float Grain;
uniform float Room;
uniform float MixAmount;

in vec2 uv;
out vec4 fragColor;

const int MAX_TAPS        = 32;
const int MAX_STATES      = 17;
const float DUST_CELL     = 0.5;  // mm
const float SCRATCH_BAND  = 2.0;  // mm
const float GLYPH_PX      = 0.8;  // mm per font pixel
const float GLYPH_ADVANCE = 6.0;  // font pixels per character
const float TITLE_LEFT    = 6.0;  // mm
const float TITLE_TOP     = 2.2;  // mm
const float GRAIN_CELL    = 0.25; // mm of screen

const uint SALT_DUST      = 0x64757374u;
const uint SALT_DUST_R    = 0x72616469u;
const uint SALT_DUST_X    = 0x6475785fu;
const uint SALT_DUST_Y    = 0x6475795fu;
const uint SALT_SCRATCH   = 0x73637261u;
const uint SALT_SCRATCH_Y = 0x7363795fu;
const uint SALT_SCRATCH_X = 0x7363785fu;
const uint SALT_SCRATCH_L = 0x73636c5fu;
const uint SALT_SCRATCH_W = 0x7363775fu;
const uint SALT_GRAIN     = 0x67726169u;

//--- the reader's geometry ----------------------------------------------------
float bowAt( vec2 p )
{
	vec2 st = ( clamp( p, vec2( 0.0 ), CardSize ) + 0.5 ) / BowSize;
	float h = textureLod( BowTex, st, 0.0 ).r;
	return ( Hooks & 64 ) != 0 ? -h : h;
}

//The state at u in [0, 1] across the exposure, linear between the CPU's.
vec4 stateAt( float u )
{
	if( StateCount <= 1 )
		return States[ 0 ];
	float x = u * float( StateCount - 1 );
	int k   = min( int( floor( x ) ), StateCount - 2 );
	return mix( States[ k ], States[ k + 1 ], x - float( k ) );
}

vec2 filmPoint( vec2 s, vec4 st )
{
	float M = ( Hooks & 4 ) != 0 ? st.z * 1.02 : st.z;
	return st.xy + vec2( s.x, -s.y ) / M;
}

//The defocus at film point p: the knob, less the card's height there, less
//the zoom's focus shift.
float defocusAt( vec2 p, vec4 st )
{
	return st.w - bowAt( p ) - Parfocal * log2( st.z / DesignZoom );
}

//The blur circle's radius on the film: |delta| M / ( 2 N ( M + 1 ) ).
float filmBlur( float delta, float M )
{
	float lens = ( Hooks & 1 ) != 0 ? M : M + 1.0;
	return abs( delta ) * M / ( 2.0 * FNumber * lens );
}

//--- the card -----------------------------------------------------------------
//The title, in square font pixels, box-filtered exactly by the footprint up
//to a pixel and by the atlas's box mips beyond.
float titleCover( vec2 p, float fp )
{
	if( TitleLength <= 0 )
		return 0.0;
	vec2 f = ( p - vec2( TITLE_LEFT, TITLE_TOP ) ) / GLYPH_PX;
	if( f.x < -1.0 || f.y < -1.0 || f.y > 8.0 )
		return 0.0;
	int ch = int( floor( f.x / GLYPH_ADVANCE ) );
	if( ch < 0 || ch >= TitleLength )
		return 0.0;
	vec2 texel   = vec2( float( Title[ ch ] * 8 + 1 ), 1.0 ) + vec2( f.x - float( ch ) * GLYPH_ADVANCE, f.y );
	float fpFont = fp / GLYPH_PX;
	if( fpFont <= 1.0 )
	{
		vec2 lo   = texel - 0.5 * fpFont;
		vec2 hi   = texel + 0.5 * fpFont;
		ivec2 a   = ivec2( floor( lo ) );
		float sum = 0.0;
		for( int j = 0; j < 2; ++j )
			for( int i = 0; i < 2; ++i )
			{
				ivec2 t = a + ivec2( i, j );
				vec2 o  = max( min( hi, vec2( t ) + 1.0 ) - max( lo, vec2( t ) ), vec2( 0.0 ) );
				sum += texelFetch( FontTex, clamp( t, ivec2( 0 ), ivec2( FontSize ) - 1 ), 0 ).r * o.x * o.y;
			}
		return sum / ( fpFont * fpFont );
	}
	return textureLod( FontTex, texel / FontSize, log2( fpFont ) ).r;
}

//What frame `cell` was exposed to at ( u, v ), v down, in linear light.
vec3 frameContent( ivec2 cell, vec2 fuv, float fp )
{
	vec2 st = vec2( fuv.x, 1.0 - fuv.y );
	if( Filmed == 0 )
	{
		float lod = log2( max( fp / FrameSize.x * LiveSize.x, 1.0 ) );
		return textureLod( LiveTex, st, min( lod, LiveLevels ) ).rgb;
	}
	int k = ( Hooks & 128 ) != 0 ? cell.x * Grid.y + cell.y : cell.y * Grid.x + cell.x;
	if( k >= Written )
		return vec3( 0.0 );
	float lod = log2( max( fp / FrameSize.x * StoreSize.x, 1.0 ) );
	return toLinear( textureLod( StoreTex, vec3( st, float( k ) ), min( lod, StoreLevels ) ).rgb );
}

//The light the film was exposed to at p, over a footprint fp: the nearest
//frame (whose rectangle the footprint covers by box), the title, and nothing
//anywhere else.
vec3 exposure( vec2 p, float fp )
{
	vec3 e = vec3( 0.0 );
	if( p.y < HeaderHeight )
		e += vec3( titleCover( p, fp ) );
	vec2 pitch  = FrameSize + Gutter;
	ivec2 cell  = clamp( ivec2( floor( ( p - GridOrigin + 0.5 * Gutter ) / pitch ) ), ivec2( 0 ), Grid - 1 );
	vec2 local  = p - GridOrigin - vec2( cell ) * pitch;
	vec2 cover  = clamp( ( min( local + 0.5 * fp, FrameSize ) - max( local - 0.5 * fp, vec2( 0.0 ) ) ) / fp, 0.0, 1.0 );
	float c     = cover.x * cover.y;
	if( c > 0.0 )
		e += c * frameContent( cell, clamp( local / FrameSize, 0.0, 1.0 ), fp );
	return e;
}

//An opaque particle, at most one per half-millimetre cell and inside it. A
//footprint wider than the particle spreads it at the same area.
float dustCover( vec2 p, float fp )
{
	if( TDust == 0u )
		return 0.0;
	ivec2 cell = ivec2( floor( p / DUST_CELL ) );
	uint key   = pack2( cell + ivec2( 4096 ) );
	if( hash3( key, Seed, SALT_DUST ) >= TDust )
		return 0.0;
	float a      = 0.008 + 0.037 * unit( hash3( key, Seed, SALT_DUST_R ) );
	vec2 jitter  = vec2( unit( hash3( key, Seed, SALT_DUST_X ) ), unit( hash3( key, Seed, SALT_DUST_Y ) ) );
	vec2 centre  = ( vec2( cell ) + 0.5 + ( jitter - 0.5 ) * ( 1.0 - 2.0 * a / DUST_CELL ) ) * DUST_CELL;
	float r      = max( a, 0.5 * fp );
	float weight = ( a / r ) * ( a / r );
	return weight * clamp( ( r - length( p - centre ) ) / fp + 0.5, 0.0, 1.0 );
}

//A scratch through the emulsion, along the card (the way it slides into the
//carrier): clear base where there was image.
float scratchCover( vec2 p, float fp )
{
	if( TScratch == 0u )
		return 0.0;
	int band = int( floor( p.y / SCRATCH_BAND ) );
	uint key = uint( band + 4096 );
	if( hash3( key, Seed, SALT_SCRATCH ) >= TScratch )
		return 0.0;
	float y         = ( float( band ) + unit( hash3( key, Seed, SALT_SCRATCH_Y ) ) ) * SCRATCH_BAND;
	float x0        = unit( hash3( key, Seed, SALT_SCRATCH_X ) ) * CardSize.x;
	float len       = 10.0 + 90.0 * unit( hash3( key, Seed, SALT_SCRATCH_L ) );
	float w         = 0.004 + 0.010 * unit( hash3( key, Seed, SALT_SCRATCH_W ) );
	float halfWidth = 0.5 * max( w, fp );
	float along     = clamp( ( min( p.x + 0.5 * fp, x0 + len ) - max( p.x - 0.5 * fp, x0 ) ) / fp, 0.0, 1.0 );
	return ( w / max( w, fp ) ) * along * clamp( ( halfWidth - abs( p.y - y ) ) / fp + 0.5, 0.0, 1.0 );
}

//What the film transmits at p: a unit-gamma print, then the scratches and the
//dust that sit in its emulsion. `s` is the screen point, for one hook.
vec3 transmit( vec2 p, float fp, vec2 s )
{
	//Off the card there is only the carrier's glass: the lamp, straight through.
	vec2 inside = clamp( ( min( p + 0.5 * fp, CardSize ) - max( p - 0.5 * fp, vec2( 0.0 ) ) ) / fp, 0.0, 1.0 );
	float onCard = inside.x * inside.y;
	if( onCard <= 0.0 )
		return vec3( 1.0 );
	vec3 e = exposure( p, fp );
	vec3 f = Colour != 0 ? e : vec3( dot( e, vec3( 0.2126, 0.7152, 0.0722 ) ) );
	if( Negative != 0 && ( Hooks & 32 ) == 0 )
		f = 1.0 - f;
	vec3 T = Dense + ( Base - Dense ) * clamp( f, 0.0, 1.0 );
	T      = mix( T, Base, scratchCover( p, fp ) );
	if( ( Hooks & 256 ) == 0 )
		T *= 1.0 - dustCover( p, fp );
	return mix( vec3( 1.0 ), T, onCard );
}

//--- the screen ---------------------------------------------------------------
//The diffuser's grain: value noise on a quarter-millimetre lattice of the
//SCREEN, so it stays put while the picture moves across it.
float grainAt( vec2 s )
{
	vec2 g  = s / GRAIN_CELL;
	ivec2 i = ivec2( floor( g ) );
	vec2 f  = g - vec2( i );
	f       = f * f * ( 3.0 - 2.0 * f );
	ivec2 o = i + ivec2( 8192 );
	float a = 2.0 * unit( hash3( pack2( o ), Seed, SALT_GRAIN ) ) - 1.0;
	float b = 2.0 * unit( hash3( pack2( o + ivec2( 1, 0 ) ), Seed, SALT_GRAIN ) ) - 1.0;
	float c = 2.0 * unit( hash3( pack2( o + ivec2( 0, 1 ) ), Seed, SALT_GRAIN ) ) - 1.0;
	float d = 2.0 * unit( hash3( pack2( o + ivec2( 1, 1 ) ), Seed, SALT_GRAIN ) ) - 1.0;
	return mix( mix( a, b, f.x ), mix( c, d, f.x ), f.y );
}

void main()
{
	ivec2 px = clamp( ivec2( floor( uv * vec2( Size ) ) ), ivec2( 0 ), Size - 1 );
	vec2 s   = ( vec2( px ) + 0.5 - 0.5 * vec2( Size ) ) / PxPerMm;
	vec4 now = States[ StateCount - 1 ];

	//How far this pixel's film point travels in the exposure, and the
	//largest blur circle on the way: they set the taps and their spacing.
	float pathMm = 0.0, rMax = 0.0;
	vec2 prev    = filmPoint( s, States[ 0 ] );
	for( int k = 0; k < MAX_STATES; ++k )
	{
		if( k >= StateCount )
			break;
		vec2 f = filmPoint( s, States[ k ] );
		pathMm += length( f - prev );
		prev = f;
		rMax = max( rMax, filmBlur( defocusAt( f, States[ k ] ), States[ k ].z ) );
	}
	float toPx  = now.z * PxPerMm;
	float rPx   = rMax * toPx;
	float lPx   = pathMm * toPx;
	float area  = 3.14159265 * rPx * rPx + 2.0 * rPx * lPx;
	int n       = int( clamp( ceil( max( area / 4.0, lPx / 1.5 ) ), 1.0, float( MAX_TAPS ) ) );
	float fp    = max( 1.0, max( sqrt( area / float( n ) ), lPx / float( n ) ) ) / toPx;

	//Each tap its own moment of the exposure (a golden-ratio sequence) and
	//its own point of the aperture (a Vogel disc), through the card.
	vec3 sum = vec3( 0.0 );
	for( int j = 0; j < MAX_TAPS; ++j )
	{
		if( j >= n )
			break;
		float u      = n == 1 ? 1.0 : fract( 0.5 + float( j ) * 0.6180339887 );
		vec4 st      = stateAt( u );
		vec2 f       = filmPoint( s, st );
		float r      = filmBlur( defocusAt( f, st ), st.z );
		float radial = ( Hooks & 2 ) != 0 ? ( float( j ) + 0.5 ) / float( n ) : sqrt( ( float( j ) + 0.5 ) / float( n ) );
		sum += transmit( f + r * radial * Disc[ j ], fp, s );
	}
	vec3 T = sum / float( n );
	if( ( Hooks & 256 ) != 0 )
		T *= 1.0 - dustCover( s, 1.0 / PxPerMm );

	//The lamp through it, the projection's cos^4, the diffuser, the room.
	float fall = 1.0;
	if( Throw > 0.0 )
	{
		float q = Throw * Throw / ( Throw * Throw + dot( s, s ) );
		fall    = ( Hooks & 8 ) != 0 ? q * sqrt( q ) : q * q;
	}
	vec2 grainPos = ( Hooks & 16 ) != 0 ? filmPoint( s, now ) * now.z : s;
	float g       = 1.0 + Grain * grainAt( grainPos ) * min( 1.0, GRAIN_CELL * PxPerMm );
	vec3 light    = Lamp * T * fall * g + vec3( Room );

	vec4 src  = texelFetch( InputTexture, px, 0 );
	fragColor = vec4( mix( src.rgb, toCode( clamp( light, 0.0, 1.0 ) ), MixAmount ), mix( src.a, 1.0, MixAmount ) );
}
)";

std::string Assemble( Pass pass )
{
	std::string out = kVersion;
	out += kCommon;
	switch( pass )
	{
	case Pass::Copy: out += kCopyFragment; break;
	case Pass::Mip: out += kMipFragment; break;
	case Pass::Film: out += kFilmFragment; break;
	case Pass::MipLayer: out += kMipLayerFragment; break;
	case Pass::Screen: out += kScreenFragment; break;
	}
	return out;
}

} // namespace fiche::shaders
