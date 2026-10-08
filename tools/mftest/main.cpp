/**
    mftest -- render Fiche offline, and measure what the reader and the hand
    are doing.

    It drives the REAL plugin class, through the same ProcessOpenGL a host
    calls, on a synthetic clock, in a headless CGL context, and reads the
    host's output back as floats. The hand's own record (its segments and
    hunts, kept when logging is on) is what the picture is measured against;
    the checks that need no picture run with no GL at all.

        mftest --out /tmp/fiche.png    the defaults on the harness's card
        mftest --list                  every parameter and its default
        mftest --pipe                  raw RGBA frames in on stdin, out on stdout
        mftest --film N                N frames of the card, raw RGBA on stdout
        mftest --offline               the checks that need no GL context (CI)

    `--script` is the fleet's cue format: `frame  Parameter Name  value`
    lines, held before the first key and after the last. A STANDARD (0..1)
    control is linear between its keys; an option, a boolean, an event or an
    integer STEPS.

    MFTEST_RENDERER=software asks for Apple's software renderer by id, on a
    Mac with a GPU: what a GPU-less CI runner falls back to.
*/

#include "Controls.h"
#include "Fiche.h"
#include "Font.h"
#include "Hand.h"
#include "Hash.h"
#include "Reader.h"
#include "Shaders.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

using namespace fiche;

namespace
{
using Floats = std::vector< float >;
using Bytes  = std::vector< unsigned char >;

//---------------------------------------------------------------------------
// Reporting.
//---------------------------------------------------------------------------
int g_failures = 0;
int g_checks   = 0;

std::string fmt( const char* format, ... )
{
	char buffer[ 4096 ];
	va_list args;
	va_start( args, format );
	std::vsnprintf( buffer, sizeof( buffer ), format, args );
	va_end( args );
	return buffer;
}

void Check( bool condition, const std::string& message )
{
	++g_checks;
	std::printf( "  %s  %s\n", condition ? "ok  " : "FAIL", message.c_str() );
	if( !condition )
		++g_failures;
}

int Verdict()
{
	std::printf( "\n  %s\n", g_failures == 0 ? "PASS" : "FAIL" );
	return g_failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// Tolerances, each with where it comes from.
//---------------------------------------------------------------------------
/// One half-float ULP just below 1.0 (2^-11), plus float32 slack. The clip is
/// held in RGBA16F in LINEAR light, so a value is rounded once on the way in;
/// back in sRGB code that error shrinks by the transfer's slope (1/2.4 at the
/// top), so a whole ULP of the code is a bound with room, whether the GPU
/// rounds or truncates (Apple's truncates: patchwork's trap).
constexpr double kHalfUlp = 1.0 / 2048.0 + 1e-6;

//---------------------------------------------------------------------------
// PNG. zlib ships with the OS.
//---------------------------------------------------------------------------
void putU32( Bytes& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( Bytes& out, const char* type, const Bytes& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

/// `rgba` is floats, row 0 at the BOTTOM (GL's order); the file is written
/// top row first, which is the only place anything here flips.
bool writePng( const std::string& path, int width, int height, const Floats& rgba )
{
	Bytes raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = height - 1; y >= 0; --y )
	{
		raw.push_back( 0 );
		for( int x = 0; x < width; ++x )
			for( int c = 0; c < 4; ++c )
			{
				const float v = c == 3 ? 1.0f : rgba[ ( static_cast< size_t >( y ) * width + x ) * 4 + c ];
				raw.push_back( static_cast< unsigned char >( std::lround( std::clamp( v, 0.0f, 1.0f ) * 255.0f ) ) );
			}
	}
	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	Bytes compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	Bytes png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
	Bytes ihdr;
	putU32( ihdr, static_cast< uint32_t >( width ) );
	putU32( ihdr, static_cast< uint32_t >( height ) );
	ihdr.insert( ihdr.end(), { 8, 6, 0, 0, 0 } );
	putChunk( png, "IHDR", ihdr );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );
	FILE* file = std::fopen( path.c_str(), "wb" );
	if( !file )
		return false;
	const size_t written = std::fwrite( png.data(), 1, png.size(), file );
	std::fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// The context.
//---------------------------------------------------------------------------
bool g_software = false;

CGLContextObj createContext()
{
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute fallback[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute generic[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFARendererID, static_cast< CGLPixelFormatAttribute >( kCGLRendererGenericFloatID ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};

	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	const char* renderer     = std::getenv( "MFTEST_RENDERER" );
	if( renderer != nullptr && std::strcmp( renderer, "software" ) == 0 )
	{
		if( CGLChoosePixelFormat( generic, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
		g_software = true;
		std::fprintf( stderr, "mftest: MFTEST_RENDERER=software, Apple's software renderer\n" );
	}
	else if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
	{
		if( CGLChoosePixelFormat( fallback, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
	}
	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;
	CGLSetCurrentContext( context );
	return context;
}

const char* kindName( unsigned int type )
{
	switch( type )
	{
	case FF_TYPE_BOOLEAN: return "bool";
	case FF_TYPE_EVENT: return "event";
	case FF_TYPE_OPTION: return "option";
	case FF_TYPE_STANDARD: return "standard";
	case FF_TYPE_TEXT: return "text";
	case FF_TYPE_INTEGER: return "integer";
	default: return "other";
	}
}

/// A control whose value is a choice, a switch, a press or a count: cues
/// STEP between keys for these, and only a standard control ramps.
bool stepsBetweenCues( unsigned int type )
{
	return type == FF_TYPE_OPTION || type == FF_TYPE_BOOLEAN || type == FF_TYPE_EVENT || type == FF_TYPE_INTEGER;
}

//---------------------------------------------------------------------------
// The cue sheet.
//---------------------------------------------------------------------------
using Track = std::vector< std::pair< int, float > >;

std::map< std::string, Track > loadScript( std::istream& in, const std::string& path, std::string& error )
{
	std::map< std::string, Track > tracks;
	std::string line;
	int lineNumber = 0;
	while( std::getline( in, line ) )
	{
		++lineNumber;
		const size_t hash = line.find( '#' );
		if( hash != std::string::npos )
			line.erase( hash );
		std::istringstream words( line );
		int frame = 0;
		if( !( words >> frame ) )
			continue;
		std::vector< std::string > parts;
		std::string word;
		while( words >> word )
			parts.push_back( word );
		if( parts.size() < 2 )
		{
			error = path + ":" + std::to_string( lineNumber ) + ": expected `frame Parameter Name value`";
			return {};
		}
		char* end         = nullptr;
		const float value = std::strtof( parts.back().c_str(), &end );
		if( end == parts.back().c_str() || *end != '\0' )
		{
			error = path + ":" + std::to_string( lineNumber ) + ": '" + parts.back() + "' is not a number";
			return {};
		}
		parts.pop_back();
		std::string name = parts.front();
		for( size_t i = 1; i < parts.size(); ++i )
			name += " " + parts[ i ];
		tracks[ name ].emplace_back( frame, value );
	}
	for( auto& entry : tracks )
		std::stable_sort( entry.second.begin(), entry.second.end(),
		                  []( const std::pair< int, float >& a, const std::pair< int, float >& b ) { return a.first < b.first; } );
	return tracks;
}

float valueAt( const Track& track, int frame, bool ramp )
{
	if( track.empty() )
		return 0.0f;
	if( frame <= track.front().first )
		return track.front().second;
	if( frame >= track.back().first )
		return track.back().second;
	for( size_t i = 0; i + 1 < track.size(); ++i )
	{
		const auto& a = track[ i ];
		const auto& b = track[ i + 1 ];
		if( frame >= a.first && frame < b.first )
		{
			if( !ramp )
				return a.second;
			const float t = static_cast< float >( frame - a.first ) / static_cast< float >( b.first - a.first );
			return a.second + ( b.second - a.second ) * t;
		}
	}
	return track.back().second;
}

//---------------------------------------------------------------------------
// Pictures. Every one is rows BOTTOM first (GL's order), as uploaded.
//---------------------------------------------------------------------------
double hash01( uint32_t a, uint32_t b = 0 )
{
	return Pcg( a * 2654435761u ^ Pcg( b + 0x9e3779b9u ) ) * ( 1.0 / 4294967296.0 );
}

/// The look card: a sunset sky, a sun, a skyline, a stripe of saturated bars
/// and a white title block -- enough edges, flats and colour for a lens to
/// have something to blur.
Floats buildCard( int width, int height )
{
	Floats card( static_cast< size_t >( width ) * height * 4 );
	const double s = std::min( width, height );
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			const double u = ( x + 0.5 - 0.5 * width ) / s, v = ( y + 0.5 - 0.5 * height ) / s;
			const double k = std::clamp( 0.5 + v, 0.0, 1.0 );
			double r = 0.95 - 0.8 * k, g = 0.45 - 0.3 * k, b = 0.25 + 0.5 * k;
			const double d = std::sqrt( ( u + 0.25 ) * ( u + 0.25 ) + ( v + 0.02 ) * ( v + 0.02 ) );
			if( d < 0.2 )
			{
				r = 1.0;
				g = 0.85 - 0.5 * d;
				b = 0.35;
			}
			const int block = static_cast< int >( std::floor( ( u + 2.0 ) * 11.0 ) );
			const double top = -0.12 + 0.16 * hash01( static_cast< uint32_t >( block ), 7 );
			if( v < top )
			{
				r = g = b = 0.05;
				const int wx = static_cast< int >( std::floor( ( u + 2.0 ) * 70.0 ) ), wy = static_cast< int >( std::floor( v * 60.0 + 100.0 ) );
				if( ( wx % 3 ) != 0 && ( wy % 2 ) == 0 && hash01( static_cast< uint32_t >( wx * 131 + wy ), 9 ) > 0.55 )
				{
					r = 1.0;
					g = 0.9;
					b = 0.55;
				}
			}
			if( v < -0.36 )
			{
				const int bar               = std::clamp( static_cast< int >( std::floor( ( x + 0.5 ) / width * 7.0 ) ), 0, 6 );
				const double bars[ 7 ][ 3 ] = { { 1, 1, 1 }, { 1, 1, 0 }, { 0, 1, 1 }, { 0, 1, 0 }, { 1, 0, 1 }, { 1, 0, 0 }, { 0, 0, 1 } };
				r = 0.75 * bars[ bar ][ 0 ];
				g = 0.75 * bars[ bar ][ 1 ];
				b = 0.75 * bars[ bar ][ 2 ];
			}
			if( std::fabs( u - 0.42 ) < 0.28 && std::fabs( v - 0.3 ) < 0.06 )
				r = g = b = 0.95;
			float* o = &card[ ( static_cast< size_t >( y ) * width + x ) * 4 ];
			o[ 0 ]   = static_cast< float >( r );
			o[ 1 ]   = static_cast< float >( g );
			o[ 2 ]   = static_cast< float >( b );
			o[ 3 ]   = 1.0f;
		}
	return card;
}

Floats flatCard( int width, int height, float r, float g, float b )
{
	Floats card( static_cast< size_t >( width ) * height * 4 );
	for( size_t i = 0; i < card.size(); i += 4 )
	{
		card[ i ]     = r;
		card[ i + 1 ] = g;
		card[ i + 2 ] = b;
		card[ i + 3 ] = 1.0f;
	}
	return card;
}

/// The card rolled `shift` pixels to the right, wrapping: --moving pans the
/// card three pixels a frame, so the filmed store has a different frame to
/// take each time.
Floats pannedCard( const Floats& card, int width, int height, int shift )
{
	Floats out( card.size() );
	shift = ( ( shift % width ) + width ) % width;
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
			for( int c = 0; c < 4; ++c )
				out[ ( static_cast< size_t >( y ) * width + ( x + shift ) % width ) * 4 + c ] = card[ ( static_cast< size_t >( y ) * width + x ) * 4 + c ];
	return out;
}

/// Eight-bit values from a hash of the pixel: every pixel different.
Floats noiseCard( int width, int height, uint32_t salt )
{
	Floats card( static_cast< size_t >( width ) * height * 4 );
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
			for( int c = 0; c < 4; ++c )
			{
				const uint32_t h = Hash3( Pack2( x, height - 1 - y ), salt, static_cast< uint32_t >( c ) );
				card[ ( static_cast< size_t >( y ) * width + x ) * 4 + c ] = c == 3 ? 1.0f : static_cast< float >( h % 256u ) / 255.0f;
			}
	return card;
}

GLuint makeTexture( int width, int height, const float* pixels, GLint format = GL_RGBA32F )
{
	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, format, width, height, 0, GL_RGBA, GL_FLOAT, pixels );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

//---------------------------------------------------------------------------
// A rig: the real plugin, a float output framebuffer, a synthetic clock and,
// when asked for, a synthetic beat.
//---------------------------------------------------------------------------
struct Rig
{
	Fiche plugin;
	int width = 0, height = 0;
	GLuint sourceTexture = 0, outputTexture = 0, outputFBO = 0;
	int frame          = 0;
	double fps         = 60.0;
	double bpm         = 0.0;///< 0: no beat sent
	std::function< void( int ) > beforeFrame;

	ProcessOpenGLStruct process    = {};
	FFGLTextureStruct inputStruct  = {};
	FFGLTextureStruct* inputs[ 1 ] = { nullptr };

	~Rig()
	{
		plugin.DeInitGL();
		release();
	}

	void release()
	{
		if( outputFBO )
			glDeleteFramebuffers( 1, &outputFBO );
		if( outputTexture )
			glDeleteTextures( 1, &outputTexture );
		if( sourceTexture )
			glDeleteTextures( 1, &sourceTexture );
		outputFBO = outputTexture = sourceTexture = 0;
	}

	bool attach( int w, int h, const Floats* picture )
	{
		width         = w;
		height        = h;
		outputTexture = makeTexture( width, height, nullptr );
		glGenFramebuffers( 1, &outputFBO );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, outputTexture, 0 );
		if( glCheckFramebufferStatus( GL_FRAMEBUFFER ) != GL_FRAMEBUFFER_COMPLETE )
			return false;
		process.HostFBO   = outputFBO;
		const Floats card = picture ? *picture : buildCard( width, height );
		sourceTexture     = makeTexture( width, height, card.data() );
		inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( width );
		inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( height );
		inputStruct.Handle                              = sourceTexture;
		inputs[ 0 ]                                     = &inputStruct;
		process.numInputTextures                        = 1;
		process.inputTextures                           = inputs;
		return true;
	}

	bool Init( int w, int h, const Floats* picture = nullptr )
	{
		FFGLViewportStruct viewport = {};
		viewport.width              = static_cast< FFUInt32 >( w );
		viewport.height             = static_cast< FFUInt32 >( h );
		if( plugin.InitGL( &viewport ) != FF_SUCCESS )
		{
			std::fprintf( stderr, "InitGL failed -- see ~/Library/Logs/fiche for which shader\n" );
			return false;
		}
		plugin.SetClockScaleForTest( 1.0 );
		return attach( w, h, picture );
	}

	/// The host's raster changes under a running instance: no InitGL.
	bool Resize( int w, int h, const Floats* picture = nullptr )
	{
		release();
		return attach( w, h, picture );
	}

	void Upload( const Floats& picture )
	{
		glBindTexture( GL_TEXTURE_2D, sourceTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_FLOAT, picture.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}

	void Set( unsigned int id, float value )
	{
		plugin.SetFloatParameter( id, value );
	}

	double Seconds( int f ) const
	{
		return static_cast< double >( f ) / fps;
	}

	bool Render( int frames = 1 )
	{
		for( int i = 0; i < frames; ++i )
		{
			if( beforeFrame )
				beforeFrame( frame );
			plugin.SetTime( Seconds( frame ) );
			if( bpm > 0.0 )
				plugin.SetBeatInfo( static_cast< float >( bpm ), static_cast< float >( std::fmod( Seconds( frame ) * bpm / 240.0, 1.0 ) ) );
			++frame;
			glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
			glViewport( 0, 0, width, height );
			glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
			glClear( GL_COLOR_BUFFER_BIT );
			if( plugin.ProcessOpenGL( &process ) != FF_SUCCESS )
			{
				std::fprintf( stderr, "ProcessOpenGL failed\n" );
				return false;
			}
		}
		return true;
	}

	Floats Output() const
	{
		Floats pixels( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_FLOAT, pixels.data() );
		return pixels;
	}

	/// Screen millimetres per pixel.
	double MmPerPx() const
	{
		return reader::kScreenWidth / width;
	}
};

/// The picture's value at ( x, y ), y from the TOP, from a bottom-up buffer.
const float* pixelTop( const Floats& picture, int width, int height, int x, int y )
{
	return &picture[ ( static_cast< size_t >( height - 1 - y ) * width + x ) * 4 ];
}

int byteOf( float v )
{
	return static_cast< int >( std::lround( std::clamp( v, 0.0f, 1.0f ) * 255.0f ) );
}

//---------------------------------------------------------------------------
// Parameters by display name.
//---------------------------------------------------------------------------
struct NamedParameter
{
	std::string name;
	unsigned int index;
	float value;
	unsigned int type;
};

std::vector< NamedParameter > listParameters( Fiche& plugin )
{
	std::vector< NamedParameter > list;
	for( unsigned int i = 0; i < plugin.GetNumParams(); ++i )
	{
		const char* const name = plugin.GetParamName( i );
		list.push_back( NamedParameter { name ? name : "?", i, plugin.GetFloatParameter( i ), plugin.GetParamType( i ) } );
	}
	return list;
}

/// `Name=Value`. The value is a number, or for an option the name of one of
/// its elements, or for a text parameter any text; anything else is refused,
/// never read as 0 (polyhedral's trap: strtof( "Fixed" ) is 0, silently).
bool applySetting( Fiche& plugin, const std::string& assignment, std::string& error )
{
	const size_t equals = assignment.find( '=' );
	if( equals == std::string::npos )
	{
		error = "expected Name=Value";
		return false;
	}
	const std::string name  = assignment.substr( 0, equals );
	const std::string value = assignment.substr( equals + 1 );
	for( const NamedParameter& parameter : listParameters( plugin ) )
		if( parameter.name == name )
		{
			if( parameter.type == FF_TYPE_TEXT )
				return plugin.SetTextParameter( parameter.index, value.c_str() ) == FF_SUCCESS;
			if( parameter.type == FF_TYPE_OPTION )
				for( unsigned int e = 0; e < plugin.GetNumParamElements( parameter.index ); ++e )
					if( value == plugin.GetParamElementName( parameter.index, e ) )
					{
						plugin.SetFloatParameter( parameter.index, static_cast< float >( e ) );
						return true;
					}
			char* end       = nullptr;
			const float got = std::strtof( value.c_str(), &end );
			if( value.empty() || end == value.c_str() || *end != '\0' )
			{
				error = "'" + value + "' is not a number" + ( parameter.type == FF_TYPE_OPTION ? " or an option of " + name : "" );
				return false;
			}
			plugin.SetFloatParameter( parameter.index, got );
			return true;
		}
	error = "no parameter called '" + name + "'";
	return false;
}

struct Cue
{
	Track track;
	bool ramp;
};

bool bindScript( Fiche& plugin, const std::string& path, std::map< unsigned int, Cue >& out, std::string& error )
{
	std::ifstream file( path );
	if( !file )
	{
		error = "cannot open " + path;
		return false;
	}
	const std::map< std::string, Track > tracks = loadScript( file, path, error );
	if( !error.empty() )
		return false;
	const std::vector< NamedParameter > known = listParameters( plugin );
	for( const auto& entry : tracks )
	{
		bool found = false;
		for( const NamedParameter& parameter : known )
			if( parameter.name == entry.first )
			{
				out[ parameter.index ] = Cue { entry.second, !stepsBetweenCues( parameter.type ) };
				found                  = true;
			}
		if( !found )
		{
			error = "script names '" + entry.first + "', which is not a parameter (try --list)";
			return false;
		}
	}
	return true;
}

//===========================================================================
// --pipe and --film. Raw RGBA, top row first, on the synthetic clock.
//===========================================================================
/// `readStdin`: frames come in on stdin, one out per one in, until a partial
/// frame or EOF. Otherwise `count` frames of the card are made.
int runPipe( int width, int height, double fps, const std::string& scriptPath, int count, bool readStdin,
             const std::vector< std::string >& settings, bool moving )
{
	const Floats card = buildCard( width, height );
	Rig rig;
	rig.fps = fps;
	if( !rig.Init( width, height ) )
		return 1;
	for( const std::string& setting : settings )
	{
		std::string error;
		if( !applySetting( rig.plugin, setting, error ) )
		{
			std::fprintf( stderr, "--set %s: %s\n", setting.c_str(), error.c_str() );
			return 2;
		}
	}
	std::map< unsigned int, Cue > automation;
	if( !scriptPath.empty() )
	{
		std::string error;
		if( !bindScript( rig.plugin, scriptPath, automation, error ) )
		{
			std::fprintf( stderr, "%s\n", error.c_str() );
			return 2;
		}
	}

	Bytes in( static_cast< size_t >( width ) * height * 4 );
	Floats picture( in.size() );
	for( int index = 0; readStdin || index < count; ++index )
	{
		if( readStdin )
		{
			size_t filled = 0;
			while( filled < in.size() )
			{
				const ssize_t got = read( STDIN_FILENO, in.data() + filled, in.size() - filled );
				if( got <= 0 )
					break;
				filled += static_cast< size_t >( got );
			}
			//A partial frame is the end of the stream, never a frame.
			if( filled < in.size() )
			{
				if( filled > 0 )
					std::fprintf( stderr, "partial frame at the end (%zu of %zu bytes): dropped\n", filled, in.size() );
				break;
			}
			for( int y = 0; y < height; ++y )
				for( int x = 0; x < width * 4; ++x )
					picture[ static_cast< size_t >( height - 1 - y ) * width * 4 + x ] = in[ static_cast< size_t >( y ) * width * 4 + x ] / 255.0f;
			rig.Upload( picture );
		}
		else if( moving )
			rig.Upload( pannedCard( card, width, height, 3 * index ) );

		for( const auto& cue : automation )
			rig.plugin.SetFloatParameter( cue.first, valueAt( cue.second.track, index, cue.second.ramp ) );
		if( !rig.Render( 1 ) )
			return 1;

		const Floats out = rig.Output();
		Bytes bytes( in.size() );
		for( int y = 0; y < height; ++y )
			for( int x = 0; x < width * 4; ++x )
				bytes[ static_cast< size_t >( y ) * width * 4 + x ] = static_cast< unsigned char >(
					std::lround( std::clamp( out[ static_cast< size_t >( height - 1 - y ) * width * 4 + x ], 0.0f, 1.0f ) * 255.0f ) );
		size_t written = 0;
		while( written < bytes.size() )
		{
			const ssize_t put = write( STDOUT_FILENO, bytes.data() + written, bytes.size() - written );
			//The reader has gone. SIGPIPE is ignored in main(), so this is
			//EPIPE and not a silent 141: say so and stop.
			if( put <= 0 )
			{
				std::fprintf( stderr, "stdout closed at frame %d\n", index );
				return 1;
			}
			written += static_cast< size_t >( put );
		}
	}
	return 0;
}

//===========================================================================
// --expect: a draft of the fleet Arena gate's expectation, from what the
// plugin really declares. Defaults are the floats the constructor sets, never
// rounded literals (containment's trap). The gate compares single frames of a
// STILL carrier, and an Auto operator moves the picture between any two
// grabs, so every row holds the operator in Manual unless the control steers
// the operator itself -- and those are expected inconclusive.
//===========================================================================
struct GateRow
{
	const char* name;
	const char* needs;///< JSON object body, without the braces, beyond Operator Manual; nullptr for the operator's own
	const char* probe;///< JSON array body, or nullptr for the gate's own
	const char* note;
};
const char* const kSteers = "steers the operator, whose acts play out over seconds: a single grab of a moving reader may read inconclusive";
const GateRow kGateRows[] = {
	{ "Operator", "", nullptr, "Auto moves the picture between grabs" },
	{ "Browse", nullptr, nullptr, kSteers },
	{ "Dwell", nullptr, nullptr, kSteers },
	{ "Sync", nullptr, nullptr, kSteers },
	{ "Hand Speed", nullptr, nullptr, kSteers },
	{ "Accuracy", nullptr, nullptr, kSteers },
	{ "Crash Zoom", nullptr, nullptr, kSteers },
	{ "Focus Skill", nullptr, nullptr, kSteers },
	{ "Carriage Play", nullptr, nullptr, kSteers },
	{ "Parfocal", nullptr, nullptr, "acts through the operator's zooms: Manual's Focus is measured from best focus, so only Auto shows it" },
	{ "Shutter", nullptr, nullptr, "acts only on a moving carriage: inconclusive on a still reader" },
	{ "Columns", "\"Zoom\": 0.0", nullptr, nullptr },
	{ "Rows", "\"Zoom\": 0.0", nullptr, nullptr },
	{ "Gutter", "\"Zoom\": 0.3", nullptr, nullptr },
	{ "Interval", "\"Content\": \"Filmed\", \"Zoom\": 0.0", nullptr, "the camera fills the card a frame per Interval: acts over seconds" },
	{ "Flatness", "\"Zoom\": 0.3, \"Aperture\": 0.0", nullptr, nullptr },
	{ "Dust", "\"Zoom\": 0.85", nullptr, nullptr },
	{ "Scratches", "\"Zoom\": 0.3", nullptr, nullptr },
	{ "Aperture", "\"Focus\": 0.8", nullptr, nullptr },
	{ "Seed", "\"Dust\": 1.0, \"Zoom\": 0.85", "1, 2", nullptr },
};

int runExpect()
{
	Fiche plugin;
	auto escape = []( const std::string& s ) {
		std::string out;
		for( char c : s )
		{
			if( std::isalnum( static_cast< unsigned char >( c ) ) )
				out += c;
			else
				out += std::string( "\\\\" ) + c;
		}
		return out;
	};
	std::printf( "{\n  \"plugin\": \"fiche\",\n  \"dlls\": [\"Fiche.dll\"],\n"
	             "  \"register\": [\n    {\"name\": \"SW Fiche\", \"uid\": \"MF01\", \"kind\": \"effect\"}\n  ],\n"
	             "  \"params\": {\n    \"SW Fiche\": [\n" );
	//Arena's own first: Opacity, held where the effect is plainly not the input.
	std::printf( "      {\"name\": \"Opacity\", \"type\": \"ParamRange\", \"min\": 0.0, \"max\": 1.0, \"default\": 1.0, "
	             "\"needs\": {\"Operator\": \"Manual\", \"Film\": \"Silver Negative\"}},\n" );
	const std::vector< NamedParameter > list = listParameters( plugin );
	for( size_t i = 0; i < list.size(); ++i )
	{
		const NamedParameter& p = list[ i ];
		std::string line        = "      {\"name\": \"" + p.name + "\", ";
		if( p.type == FF_TYPE_OPTION )
			line += "\"type\": \"ParamChoice\", \"default\": \"" + std::string( plugin.GetParamElementName( p.index, static_cast< unsigned int >( std::lround( p.value ) ) ) ) + "\"";
		else if( p.type == FF_TYPE_EVENT )
			line += "\"type\": \"ParamEvent\"";
		else if( p.type == FF_TYPE_TEXT && p.index == PT_TITLE )
			line += "\"type\": \"ParamString\", \"default\": \"" + std::string( kDefaultTitle ) + "\"";
		else if( p.type == FF_TYPE_TEXT )
			line += "\"type\": \"ParamString\", \"default_pattern\": \"^" + escape( "Fiche v" ) + "{version}" + escape( " - MIT - Stoatworks Labs, stoatworks-labs.com" ) + "$\"";
		else
		{
			const float lo = p.type == FF_TYPE_INTEGER ? plugin.GetParamRange( p.index ).min : 0.0f;
			const float hi = p.type == FF_TYPE_INTEGER ? plugin.GetParamRange( p.index ).max : 1.0f;
			line += fmt( "\"type\": \"ParamRange\", \"min\": %.1f, \"max\": %.1f, \"default\": %.17g", lo, hi, static_cast< double >( p.value ) );
		}
		if( p.name.size() == 16 )
			line += ", \"declared\": \"" + p.name + "\"";
		const bool control = p.type != FF_TYPE_EVENT && p.type != FF_TYPE_TEXT;
		const GateRow* row = nullptr;
		for( const GateRow& r : kGateRows )
			if( p.name == r.name )
				row = &r;
		if( control )
		{
			std::string needs;
			if( row && row->needs == nullptr )
				needs = "\"Operator\": \"Auto\"";
			else if( !( row && p.name == "Operator" ) )
				needs = "\"Operator\": \"Manual\"";
			if( row && row->needs && *row->needs )
				needs += ( needs.empty() ? "" : ", " ) + std::string( row->needs );
			if( !needs.empty() )
				line += ", \"needs\": {" + needs + "}";
			if( row && row->probe )
				line += ", \"probe\": [" + std::string( row->probe ) + "]";
			if( row && row->note )
				line += ", \"note\": \"" + std::string( row->note ) + "\"";
		}
		line += i + 1 < list.size() ? "},\n" : "}\n";
		std::printf( "%s", line.c_str() );
	}
	std::printf( "    ]\n  }\n}\n" );
	return 0;
}

//===========================================================================
// The checks.
//
// Each takes a Perturb. With every field at its default the check scores the
// plugin against the stated law; `--negative` sets one field at a time to a
// deliberately wrong MODEL -- a hook in the plugin, so the shipped code
// computes the wrong thing -- and requires the check to FAIL.
//===========================================================================
struct Perturb
{
	int hooks         = 0;    ///< shaders::Hook bits
	int handHooks     = 0;    ///< hand::HandHook bits
	bool showHand     = false;///< the GPU is shown the hand, not the carriage
	double shutter    = 1.0;  ///< the exposure window, scaled
	bool resizeKeeps  = false;///< the store survives a reallocation
	bool cuesRamp     = false;///< --cues: every control ramps
	bool cueEarly     = false;///< a beat starts an act at the start of its frame
};

using CheckFn = int ( * )( const Perturb& );

struct Raster
{
	int w, h;
};
std::vector< Raster > kRasters = { { 1280, 720 }, { 320, 180 } };

/// A perfect reader in the operator's absence: Manual, an Ideal film under a
/// white lamp, a flat card, a parfocal lens, no shutter, a rigid carriage,
/// nothing on the screen or in the room, no title.
void quiet( Rig& rig )
{
	rig.Set( PT_OPERATOR, static_cast< float >( Operator::Manual ) );
	rig.Set( PT_FILM, static_cast< float >( Film::Ideal ) );
	rig.Set( PT_LAMP, 1.0f );
	rig.Set( PT_HOTSPOT, 0.0f );
	rig.Set( PT_SCREEN_GRAIN, 0.0f );
	rig.Set( PT_ROOM_LIGHT, 0.0f );
	rig.Set( PT_DUST, 0.0f );
	rig.Set( PT_SCRATCHES, 0.0f );
	rig.Set( PT_FLATNESS, 0.0f );
	rig.Set( PT_PARFOCAL, 0.0f );
	rig.Set( PT_SHUTTER, 0.0f );
	rig.Set( PT_CARRIAGE_PLAY, 0.0f );
	rig.Set( PT_FOCUS, ParamFromDefocus( 0.0 ) );
	rig.Set( PT_MIX, 1.0f );
	rig.plugin.SetTextParameter( PT_TITLE, "" );
}

bool startRig( Rig& rig, const Raster& raster, const Floats& picture, const Perturb& perturb )
{
	if( !rig.Init( raster.w, raster.h, &picture ) )
		return false;
	rig.plugin.SetHooksForTest( perturb.hooks );
	rig.plugin.SetHandHooksForTest( perturb.handHooks );
	rig.plugin.SetShowHandForTest( perturb.showHand );
	rig.plugin.SetShutterScaleForTest( perturb.shutter );
	rig.plugin.SetResizeKeepsStoreForTest( perturb.resizeKeeps );
	rig.plugin.SetCueAtFrameStartForTest( perturb.cueEarly );
	quiet( rig );
	return true;
}

/// Point the Manual hand at card point ( x, y ) mm, at magnification M.
void aim( Rig& rig, double x, double y, double M )
{
	const reader::Card card = reader::MakeCard( IntegerOf( PT_COLUMNS, rig.plugin.GetFloatParameter( PT_COLUMNS ) ),
	                                            IntegerOf( PT_ROWS, rig.plugin.GetFloatParameter( PT_ROWS ) ),
	                                            GutterFromParam( rig.plugin.GetFloatParameter( PT_GUTTER ) ),
	                                            static_cast< double >( rig.width ) / rig.height );
	rig.Set( PT_POSITION_X, static_cast< float >( x / card.width ) );
	rig.Set( PT_POSITION_Y, static_cast< float >( y / card.height ) );
	rig.Set( PT_ZOOM, ParamFromZoom( M ) );
}

reader::Card cardOf( Rig& rig )
{
	return reader::MakeCard( IntegerOf( PT_COLUMNS, rig.plugin.GetFloatParameter( PT_COLUMNS ) ),
	                         IntegerOf( PT_ROWS, rig.plugin.GetFloatParameter( PT_ROWS ) ),
	                         GutterFromParam( rig.plugin.GetFloatParameter( PT_GUTTER ) ),
	                         static_cast< double >( rig.width ) / rig.height );
}

//===========================================================================
// --identity: one frame filling the screen of a perfect reader is the clip.
//===========================================================================
int runIdentity( const Perturb& perturb )
{
	std::printf( "\n=== identity: a perfect reader with one frame filling its screen shows the clip\n" );
	for( const Raster& raster : kRasters )
	{
		const Floats card = noiseCard( raster.w, raster.h, 11 );
		Rig rig;
		if( !startRig( rig, raster, card, perturb ) )
			return 1;
		rig.Set( PT_COLUMNS, 1.0f );
		rig.Set( PT_ROWS, 1.0f );
		const reader::Card c = cardOf( rig );
		aim( rig, c.gridX + 0.5 * c.frameW, c.gridY + 0.5 * c.frameH, reader::kScreenWidth / c.frameW );
		if( !rig.Render( 2 ) )
			return 1;
		const Floats out = rig.Output();
		double worst     = 0.0;
		int bytesWrong   = 0;
		for( size_t i = 0; i < out.size(); ++i )
		{
			if( i % 4 == 3 )
				continue;
			worst = std::max( worst, static_cast< double >( std::fabs( out[ i ] - card[ i ] ) ) );
			bytesWrong += byteOf( out[ i ] ) != byteOf( card[ i ] );
		}
		Check( worst <= kHalfUlp && bytesWrong == 0,
		       fmt( "%dx%d at %.4fx: worst |out - clip| %.3g (bound %.3g, a half-float ULP); %d of %zu bytes differ", raster.w, raster.h,
		            reader::kScreenWidth / c.frameW, worst, kHalfUlp, bytesWrong, out.size() / 4 * 3 ) );
	}
	return Verdict();
}

//===========================================================================
// Measuring helpers.
//===========================================================================
double toLinear( double code )
{
	return reader::ToLinear( std::clamp( code, 0.0, 1.0 ) );
}

double pxPerMm( const Rig& rig )
{
	return rig.width / reader::kScreenWidth;
}

/// Where card point ( x, y ) mm lands on the screen, in pixel-index units
/// (pixel i's centre is at i), x right and y DOWN from the top row, for the
/// carriage at ( px, py ) and magnification M.
void toScreen( const Rig& rig, double x, double y, double px, double py, double M, double& i, double& j )
{
	const double k = M * pxPerMm( rig );
	i              = ( x - px ) * k + 0.5 * rig.width - 0.5;
	j              = ( y - py ) * k + 0.5 * rig.height - 0.5;
}

/// Light in a window: its total, centroid and second moment about the
/// centroid, in pixels, of the linear intensity of channel R (y down).
struct Moments
{
	double total = 0.0, cx = 0.0, cy = 0.0, m2 = 0.0;
};
Moments momentsOf( const Floats& out, int w, int h, double cx, double cy, double half )
{
	Moments m;
	const int x0 = std::max( 0, static_cast< int >( std::floor( cx - half ) ) ), x1 = std::min( w - 1, static_cast< int >( std::ceil( cx + half ) ) );
	const int y0 = std::max( 0, static_cast< int >( std::floor( cy - half ) ) ), y1 = std::min( h - 1, static_cast< int >( std::ceil( cy + half ) ) );
	double sx = 0.0, sy = 0.0, sxx = 0.0;
	for( int y = y0; y <= y1; ++y )
		for( int x = x0; x <= x1; ++x )
		{
			const double v = toLinear( pixelTop( out, w, h, x, y )[ 0 ] );
			m.total += v;
			sx += v * x;
			sy += v * y;
			sxx += v * ( static_cast< double >( x ) * x + static_cast< double >( y ) * y );
		}
	if( m.total > 0.0 )
	{
		m.cx = sx / m.total;
		m.cy = sy / m.total;
		m.m2 = sxx / m.total - m.cx * m.cx - m.cy * m.cy;
	}
	return m;
}

/// One white texel on black at ( tx, ty ), GL rows (bottom first).
Floats dotCard( int w, int h, const std::vector< std::pair< int, int > >& dots )
{
	Floats card = flatCard( w, h, 0.0f, 0.0f, 0.0f );
	for( const auto& d : dots )
		for( int c = 0; c < 3; ++c )
			card[ ( static_cast< size_t >( d.second ) * w + d.first ) * 4 + c ] = 1.0f;
	return card;
}

/// The film point of texel ( tx, ty ) (GL rows) in frame ( col, row ).
void texelOnCard( const reader::Card& c, int w, int h, int col, int row, double tx, double ty, double& x, double& y )
{
	x = c.FrameLeft( col ) + c.frameW * ( tx + 0.5 ) / w;
	y = c.FrameTop( row ) + c.frameH * ( 1.0 - ( ty + 0.5 ) / h );
}

/// R is u across a frame and G is v DOWN it, in LINEAR light (coded as
/// sRGB): the picture's centre then says where on the card the reader is
/// pointed. Linear in light, because the reader averages light -- through the
/// mips, the bilinear lookups and the taps -- and an average of a linear
/// ramp is exact where an average of a code ramp is bent by the transfer.
Floats coordinateCard( int w, int h )
{
	Floats card( static_cast< size_t >( w ) * h * 4 );
	for( int y = 0; y < h; ++y )
		for( int x = 0; x < w; ++x )
		{
			float* o = &card[ ( static_cast< size_t >( y ) * w + x ) * 4 ];
			o[ 0 ]   = static_cast< float >( reader::ToCode( ( x + 0.5 ) / w ) );
			o[ 1 ]   = static_cast< float >( reader::ToCode( 1.0 - ( y + 0.5 ) / h ) );
			o[ 2 ]   = 0.0f;
			o[ 3 ]   = 1.0f;
		}
	return card;
}

/// The coordinate card's reading at the screen's centre: the mean of the four
/// central pixels, which straddle it symmetrically.
void centreReading( const Rig& rig, const Floats& out, double& u, double& v )
{
	u = v = 0.0;
	for( int dy = 0; dy < 2; ++dy )
		for( int dx = 0; dx < 2; ++dx )
		{
			const float* p = pixelTop( out, rig.width, rig.height, rig.width / 2 - 1 + dx, rig.height / 2 - 1 + dy );
			u += 0.25 * toLinear( p[ 0 ] );
			v += 0.25 * toLinear( p[ 1 ] );
		}
}

/// Which frame of the card holds film point ( x, y ), and where in it, or
/// false if it is in a gutter or within `margin` mm of a frame's edge.
bool frameAt( const reader::Card& c, double x, double y, double margin, double& u, double& v )
{
	const int col = static_cast< int >( std::floor( ( x - c.gridX + 0.5 * c.gutter ) / c.PitchX() ) );
	const int row = static_cast< int >( std::floor( ( y - c.gridY + 0.5 * c.gutter ) / c.PitchY() ) );
	if( col < 0 || col >= c.cols || row < 0 || row >= c.rows )
		return false;
	const double lx = x - c.FrameLeft( col ), ly = y - c.FrameTop( row );
	if( lx < margin || lx > c.frameW - margin || ly < margin || ly > c.frameH - margin )
		return false;
	u = lx / c.frameW;
	v = ly / c.frameH;
	return true;
}

/// The bound on a position read off the coordinate card, in mm of film. The
/// clip is held in linear light in RGBA16F, and each mip level is stored
/// again in RGBA16F: a value below 1 loses up to one half-float ULP (2^-11)
/// at every store when the GPU truncates (Apple's does), so a reading taken
/// at a level-of-detail of L has passed 1 + ceil( L ) of them -- and one more
/// in the filter, which Apple's GPU computes in the texture's own precision
/// (a bilinear reading came back as an exact half, 1.04 ULP off). Across a
/// frame of w mm, read at `texelsPerPixel` clip texels a screen pixel.
double coordinateBound( double frameMm, double texelsPerPixel = 1.0 )
{
	const double stores = 2.0 + std::ceil( std::log2( std::max( texelsPerPixel, 1.0 ) ) - 1e-9 );
	return frameMm * stores / 2048.0 + 1e-5;
}

//===========================================================================
// --mips: the clip's mip chain is box averages, level by level.
//===========================================================================
int runMips( const Perturb& perturb )
{
	std::printf( "\n=== mips: every texel of the clip's mip chain is the area average of its share of the level below (2 x 2 where the\n"
	             "    size is even, two and a bit where it is odd), in linear light\n" );
	for( const Raster& raster : kRasters )
	{
		Rig rig;
		if( !startRig( rig, raster, noiseCard( raster.w, raster.h, 3 ), perturb ) )
			return 1;
		if( !rig.Render( 1 ) )
			return 1;
		const GLuint live = rig.plugin.LiveTextureForTest();
		const int levels  = rig.plugin.LiveLevelsForTest();
		std::vector< Floats > chain;
		glBindTexture( GL_TEXTURE_2D, live );
		for( int level = 0; level <= levels; ++level )
		{
			const int w = std::max( 1, raster.w >> level ), h = std::max( 1, raster.h >> level );
			Floats texels( static_cast< size_t >( w ) * h * 4 );
			glPixelStorei( GL_PACK_ALIGNMENT, 1 );
			glGetTexImage( GL_TEXTURE_2D, level, GL_RGBA, GL_FLOAT, texels.data() );
			chain.push_back( texels );
		}
		glBindTexture( GL_TEXTURE_2D, 0 );
		double worst = 0.0;
		int worstLevel = 0;
		for( int level = 1; level <= levels; ++level )
		{
			const int pw = std::max( 1, raster.w >> ( level - 1 ) ), ph = std::max( 1, raster.h >> ( level - 1 ) );
			const int w = std::max( 1, raster.w >> level ), h = std::max( 1, raster.h >> level );
			const double sx = static_cast< double >( pw ) / w, sy = static_cast< double >( ph ) / h;
			for( int y = 0; y < h; ++y )
				for( int x = 0; x < w; ++x )
					for( int ch = 0; ch < 3; ++ch )
					{
						// The texel's own share of the level below, by area.
						double sum = 0.0;
						for( int ty = static_cast< int >( std::floor( y * sy ) ); ty < std::ceil( ( y + 1 ) * sy ); ++ty )
							for( int tx = static_cast< int >( std::floor( x * sx ) ); tx < std::ceil( ( x + 1 ) * sx ); ++tx )
							{
								const double wx = std::min( ( x + 1 ) * sx, tx + 1.0 ) - std::max( x * sx, static_cast< double >( tx ) );
								const double wy = std::min( ( y + 1 ) * sy, ty + 1.0 ) - std::max( y * sy, static_cast< double >( ty ) );
								sum += wx * wy * chain[ level - 1 ][ ( static_cast< size_t >( std::min( ty, ph - 1 ) ) * pw + std::min( tx, pw - 1 ) ) * 4 + ch ];
							}
						const double err = std::fabs( chain[ level ][ ( static_cast< size_t >( y ) * w + x ) * 4 + ch ] - sum / ( sx * sy ) );
						if( err > worst )
						{
							worst      = err;
							worstLevel = level;
						}
					}
		}
		// One half-float ULP of the average (it is rounded once on the store).
		Check( worst <= 1.0 / 2048.0, fmt( "%dx%d: %d levels, worst |level - area average of the level below| %.3g (level %d; bound a half-float ULP)",
		                                   raster.w, raster.h, levels, worst, worstLevel ) );
	}
	return Verdict();
}

//===========================================================================
// --dark: a black clip on clear film is black.
//===========================================================================
int runDark( const Perturb& perturb )
{
	std::printf( "\n=== dark: a black clip on the Ideal film is exactly black on the card, at every magnification -- no light leaks\n"
	             "    in from the glass round it (a box coverage written as min - max of card positions cancels in float32 at 75x)\n" );
	for( const Raster& raster : kRasters )
	{
		Rig rig;
		if( !startRig( rig, raster, flatCard( raster.w, raster.h, 0.0f, 0.0f, 0.0f ), perturb ) )
			return 1;
		rig.Set( PT_COLUMNS, 16.0f );
		rig.Set( PT_ROWS, 1.0f );
		const reader::Card c = cardOf( rig );
		for( double M : { 4.0, 24.0, 75.0 } )
		{
			// A frame's middle, far enough in that the whole screen is card.
			aim( rig, c.FrameLeft( 7 ) + 0.5 * c.frameW, c.FrameTop( 0 ) + 0.5 * c.frameH, M );
			rig.Set( PT_FOCUS, ParamFromDefocus( 0.0 ) );
			if( !rig.Render( 2 ) )
				return 1;
			const Floats out = rig.Output();
			const hand::State st = rig.plugin.HandForTest().Now();
			double brightest     = 0.0;
			int onCard           = 0;
			for( int y = 0; y < raster.h; y += 2 )
				for( int x = 0; x < raster.w; x += 2 )
				{
					const double fx = st.x + ( x + 0.5 - 0.5 * raster.w ) / ( M * pxPerMm( rig ) );
					const double fy = st.y + ( y + 0.5 - 0.5 * raster.h ) / ( M * pxPerMm( rig ) );
					const double margin = 2.0 / ( M * pxPerMm( rig ) );
					if( fx < margin || fy < margin || fx > c.width - margin || fy > c.height - margin )
						continue;
					brightest = std::max( brightest, toLinear( pixelTop( out, raster.w, raster.h, x, y )[ 0 ] ) );
					++onCard;
				}
			Check( onCard > 0 && brightest == 0.0,
			       fmt( "%dx%d at %.0fx: the brightest of %d card pixels %.3g (bound 0: nothing there to light it)", raster.w, raster.h, M,
			            onCard, brightest ) );
		}
	}
	return Verdict();
}

//===========================================================================
// --magnify: the card's edges land where M says, about the screen's centre.
//===========================================================================
int runMagnify( const Perturb& perturb )
{
	std::printf( "\n=== magnify: every frame edge on the screen is where M x (card mm) x (pixels per screen mm) puts it,\n"
	             "    measured about the screen's centre at three magnifications\n" );
	// A box-filtered edge read by area is the edge itself; the bound is
	// float32 arithmetic on card millimetres (an ULP of 100 mm is 8e-6 mm,
	// under 1e-3 px at the largest M here).
	constexpr double kTol = 0.01;
	for( const Raster& raster : kRasters )
	{
		Rig rig;
		if( !startRig( rig, raster, flatCard( raster.w, raster.h, 1.0f, 1.0f, 1.0f ), perturb ) )
			return 1;
		rig.Set( PT_COLUMNS, 4.0f );
		rig.Set( PT_ROWS, 3.0f );
		rig.Set( PT_GUTTER, ParamFromGutter( 2.0 ) );
		const reader::Card c = cardOf( rig );
		for( double M : { 3.0, 6.0, 11.0 } )
		{
			aim( rig, c.FrameLeft( 1 ) + 0.3 * c.frameW, c.FrameTop( 1 ) + 0.5 * c.frameH, M );
			if( !rig.Render( 2 ) )
				return 1;
			const hand::State st = rig.plugin.HandForTest().Now();
			const double mag     = std::exp2( st.logM );
			const Floats out     = rig.Output();
			const int row        = rig.height / 2;
			auto I = [ & ]( int q ) { return toLinear( pixelTop( out, rig.width, rig.height, q, row )[ 0 ] ); };
			std::vector< double > edges = { 0.0, c.width };
			for( int col = 0; col < c.cols; ++col )
			{
				edges.push_back( c.FrameLeft( col ) );
				edges.push_back( c.FrameLeft( col ) + c.frameW );
			}
			int expected = 0, found = 0;
			double worst = 0.0;
			for( double e : edges )
			{
				double i = 0.0, j = 0.0;
				toScreen( rig, e, 0.0, st.x, st.y, mag, i, j );
				const int q0 = static_cast< int >( std::floor( i ) ) - 3, q1 = static_cast< int >( std::floor( i ) ) + 4;
				if( q0 < 0 || q1 >= rig.width )
					continue;
				++expected;
				// A box-filtered step from dark to light covers exactly the
				// light side of each pixel, so the edge is where the light's
				// sum across the step says: e = ( q1 + 0.5 ) - sum, rising.
				const double lo = I( q0 ), hi = I( q1 );
				if( std::fabs( std::fabs( hi - lo ) - 1.0 ) > 1e-3 )
					continue;
				double sum = 0.0;
				for( int q = q0; q <= q1; ++q )
					sum += hi > lo ? I( q ) : 1.0 - I( q );
				const double at = ( q1 + 0.5 ) - sum;
				const double measured = hi > lo ? at : ( q0 - 0.5 ) + ( q1 - q0 + 1 ) - sum;
				if( std::getenv( "MFTEST_DEBUG" ) )
					std::printf( "    edge %.3f mm: expected %.4f px, measured %.4f\n", e, i, measured );
				++found;
				worst = std::max( worst, std::fabs( measured - i ) );
			}
			Check( expected >= 2 && found == expected && worst <= kTol,
			       fmt( "%dx%d at %.1fx: %d of %d frame and card edges found, worst %.4f px from M x mm (bound %.2f)", raster.w,
			            raster.h, mag, found, expected, worst, kTol ) );
		}
	}
	return Verdict();
}

//===========================================================================
// --defocus: a point of light out of focus is a uniform disc of the radius
// geometric optics gives, and its light is kept.
//===========================================================================
int runDefocus( const Perturb& perturb )
{
	std::printf( "\n=== defocus: a point delta mm out of focus images as a disc of radius M^2 |delta| / ( 2 N ( M + 1 ) ) on the screen,\n"
	             "    measured by its second moment (a uniform disc's is R^2 / 2), across M, N and delta; and its light is kept\n" );
	struct Case
	{
		double M, N, delta;
	};
	const Case cases[] = { { 40.0, 2.0, 1.0 }, { 40.0, 2.0, -0.6 }, { 24.0, 2.8, 1.0 }, { 60.0, 2.0, 0.4 }, { 12.0, 2.0, 1.0 } };
	for( const Raster& raster : kRasters )
	{
		const int tx = raster.w / 2, ty = raster.h / 2;
		Rig rig;
		if( !startRig( rig, raster, dotCard( raster.w, raster.h, { { tx, ty } } ), perturb ) )
			return 1;
		rig.Set( PT_COLUMNS, 16.0f );
		rig.Set( PT_ROWS, 1.0f );
		rig.Set( PT_GUTTER, 0.0f );
		const reader::Card c = cardOf( rig );
		double dx = 0.0, dy = 0.0;
		texelOnCard( c, raster.w, raster.h, 7, 0, tx, ty, dx, dy );
		for( const Case& k : cases )
		{
			const double expected = k.M * reader::FilmBlurRadius( k.delta, k.M, k.N ) * pxPerMm( rig );
			if( expected < 6.0 )
			{
				std::printf( "  skip  %dx%d at %.0fx f/%.1f, %+.1f mm: a %.1f px disc is too small to measure to 1%%\n", raster.w,
				             raster.h, k.M, k.N, k.delta, expected );
				continue;
			}
			aim( rig, dx, dy, k.M );
			rig.Set( PT_APERTURE, ParamFromAperture( k.N ) );
			rig.plugin.SetPrefilterForTest( false );
			double si = 0.0, sj = 0.0;
			rig.Set( PT_FOCUS, ParamFromDefocus( 0.0 ) );
			if( !rig.Render( 2 ) )
				return 1;
			const hand::State st = rig.plugin.HandForTest().Now();
			toScreen( rig, dx, dy, st.x, st.y, std::exp2( st.logM ), si, sj );
			const double half = expected + 12.0;
			const Moments sharp = momentsOf( rig.Output(), rig.width, rig.height, si, sj, half );
			rig.Set( PT_FOCUS, ParamFromDefocus( k.delta ) );
			if( !rig.Render( 1 ) )
				return 1;
			const Moments blur = momentsOf( rig.Output(), rig.width, rig.height, si, sj, half );
			const double shift = ( blur.cx - sharp.cx ) * ( blur.cx - sharp.cx ) + ( blur.cy - sharp.cy ) * ( blur.cy - sharp.cy );
			const double measured = std::sqrt( std::max( 0.0, 2.0 * ( blur.m2 - sharp.m2 + shift ) ) );
			// 1%: the second moment of the Vogel taps is exactly R^2 / 2 and
			// the dot's own spread is subtracted, so what is left is how a
			// sampled dot's moment moves with sub-pixel position (< 0.25 px^2,
			// under 1% of R^2 / 2 for R >= 6 px).
			Check( std::fabs( measured / expected - 1.0 ) <= 0.01,
			       fmt( "%dx%d at %.0fx f/%.1f, %+.1f mm out: radius %.2f px, geometric optics %.2f px (%+.2f%%; bound 1%%)", raster.w,
			            raster.h, k.M, k.N, k.delta, measured, expected, 100.0 * ( measured / expected - 1.0 ) ) );
			// The light: with the prefilter on, as shipped.
			rig.plugin.SetPrefilterForTest( true );
			if( !rig.Render( 1 ) )
				return 1;
			const Moments kept = momentsOf( rig.Output(), rig.width, rig.height, si, sj, half );
			// The point is one texel of linear light 1: its light on the
			// screen is the texel's area there, in px^2. (Not the sharp
			// picture's sum, which a sub-pixel dot sampled at one offset
			// misses by a few percent.)
			const double texel = c.frameW / raster.w * k.M * pxPerMm( rig );
			Check( std::fabs( kept.total / ( texel * texel ) - 1.0 ) <= 0.005,
			       fmt( "%dx%d at %.0fx f/%.1f, %+.1f mm out, prefiltered as shipped: the disc carries %.4f of the point's light (bound "
			            "0.5%%: box mips keep the mean, bilinear sampling and the taps average it)",
			            raster.w, raster.h, k.M, k.N, k.delta, kept.total / ( texel * texel ) ) );
		}
	}
	return Verdict();
}

//===========================================================================
// --field: the card's bow, measured out of the picture.
//===========================================================================
int runField( const Perturb& perturb )
{
	std::printf( "\n=== field: a grid of points on a bowed card: each comes sharpest at the knob setting that puts the focus at its\n"
	             "    height, bow( p ) - bow( centre ), read from the vertex of its blur against the knob\n" );
	for( const Raster& raster : kRasters )
	{
		const int step = std::max( 4, raster.w / 40 );
		std::vector< std::pair< int, int > > dots;
		for( int y = step / 2; y < raster.h; y += step )
			for( int x = step / 2; x < raster.w; x += step )
				dots.push_back( { x, y } );
		// A seed whose bow varies across the view by 0.15 mm or more, or the
		// check has nothing to tell a flipped bow from.
		const double M = 12.0;
		int seed       = 1;
		{
			const reader::Card c = reader::MakeCard( 1, 1, 0.0, static_cast< double >( raster.w ) / raster.h );
			for( int candidate = 1; candidate < 200; ++candidate )
			{
				reader::Bow bow;
				bow.Build( c, FlatnessFromParam( ParamFromFlatness( 0.5 ) ), static_cast< uint32_t >( candidate ) );
				const double cx = c.FrameLeft( 0 ) + 0.5 * c.frameW, cy = c.FrameTop( 0 ) + 0.5 * c.frameH;
				const double hw = 0.4 * reader::kScreenWidth / M, hh = 0.4 * reader::kScreenWidth * raster.h / raster.w / M;
				double spread = 0.0;
				for( double sx : { -hw, hw } )
					for( double sy : { -hh, hh } )
						spread = std::max( spread, std::fabs( bow.At( cx + sx, cy + sy ) - bow.At( cx, cy ) ) );
				if( spread >= 0.12 )
				{
					seed = candidate;
					break;
				}
			}
		}
		Rig rig;
		if( !startRig( rig, raster, dotCard( raster.w, raster.h, dots ), perturb ) )
			return 1;
		rig.Set( PT_SEED, static_cast< float >( seed ) );
		rig.Set( PT_COLUMNS, 1.0f );
		rig.Set( PT_ROWS, 1.0f );
		rig.Set( PT_FLATNESS, ParamFromFlatness( 0.5 ) );
		rig.Set( PT_APERTURE, ParamFromAperture( 2.0 ) );
		rig.plugin.SetPrefilterForTest( false );
		const reader::Card c = cardOf( rig );
		aim( rig, c.FrameLeft( 0 ) + 0.5 * c.frameW, c.FrameTop( 0 ) + 0.5 * c.frameH, M );
		std::vector< double > knobs;
		for( int k = -5; k <= 5; ++k )
			knobs.push_back( 0.2 * k );
		std::vector< std::vector< double > > m2( dots.size() );
		hand::State st;
		for( double knob : knobs )
		{
			rig.Set( PT_FOCUS, ParamFromDefocus( knob ) );
			if( !rig.Render( 2 ) )
				return 1;
			st              = rig.plugin.HandForTest().Now();
			const Floats out = rig.Output();
			for( size_t d = 0; d < dots.size(); ++d )
			{
				double x = 0.0, y = 0.0, i = 0.0, j = 0.0;
				texelOnCard( c, raster.w, raster.h, 0, 0, dots[ d ].first, dots[ d ].second, x, y );
				toScreen( rig, x, y, st.x, st.y, std::exp2( st.logM ), i, j );
				const double half = 0.45 * step * M / ( reader::kScreenWidth / c.frameW );
				if( i < half || j < half || i > rig.width - 1 - half || j > rig.height - 1 - half )
					continue;
				m2[ d ].push_back( momentsOf( out, rig.width, rig.height, i, j, half ).m2 );
			}
		}
		// The bound: the GPU reads the bow from a 1 mm grid, bilinearly
		// (under 0.002 mm off the B-spline on a 25 mm lattice), and the
		// vertex of a fitted parabola moves with how a sampled dot's moment
		// changes with sub-pixel position (~0.05 px^2) against the blur's
		// slope, ( M R / delta )^2 / 2 px^2 per mm^2 -- 4 at 320 px, 70 at
		// 1280: 0.03 and 0.005 mm with room.
		const double bound = raster.w >= 1280 ? 0.005 : 0.03;
		const reader::Bow& bow = rig.plugin.BowForTest();
		int measured = 0, steep = 0;
		double worst = 0.0;
		for( size_t d = 0; d < dots.size(); ++d )
		{
			if( m2[ d ].size() != knobs.size() )
				continue;
			// Least squares m2 = a k^2 + b k + c, vertex -b / 2a.
			double S[ 5 ] = {}, T[ 3 ] = {};
			for( size_t k = 0; k < knobs.size(); ++k )
			{
				double p = 1.0;
				for( int e = 0; e < 5; ++e, p *= knobs[ k ] )
					S[ e ] += p;
				T[ 0 ] += m2[ d ][ k ];
				T[ 1 ] += m2[ d ][ k ] * knobs[ k ];
				T[ 2 ] += m2[ d ][ k ] * knobs[ k ] * knobs[ k ];
			}
			const double A[ 3 ][ 3 ] = { { S[ 4 ], S[ 3 ], S[ 2 ] }, { S[ 3 ], S[ 2 ], S[ 1 ] }, { S[ 2 ], S[ 1 ], S[ 0 ] } };
			const double y3[ 3 ]     = { T[ 2 ], T[ 1 ], T[ 0 ] };
			auto det = []( const double m[ 3 ][ 3 ] ) {
				return m[ 0 ][ 0 ] * ( m[ 1 ][ 1 ] * m[ 2 ][ 2 ] - m[ 1 ][ 2 ] * m[ 2 ][ 1 ] ) - m[ 0 ][ 1 ] * ( m[ 1 ][ 0 ] * m[ 2 ][ 2 ] - m[ 1 ][ 2 ] * m[ 2 ][ 0 ] )
				     + m[ 0 ][ 2 ] * ( m[ 1 ][ 0 ] * m[ 2 ][ 1 ] - m[ 1 ][ 1 ] * m[ 2 ][ 0 ] );
			};
			double coef[ 3 ];
			for( int col = 0; col < 3; ++col )
			{
				double B[ 3 ][ 3 ];
				for( int r = 0; r < 3; ++r )
					for( int q = 0; q < 3; ++q )
						B[ r ][ q ] = q == col ? y3[ r ] : A[ r ][ q ];
				coef[ col ] = det( B ) / det( A );
			}
			const double vertex = -coef[ 1 ] / ( 2.0 * coef[ 0 ] );
			double x = 0.0, y = 0.0;
			texelOnCard( c, raster.w, raster.h, 0, 0, dots[ d ].first, dots[ d ].second, x, y );
			const double expected = bow.At( x, y ) - bow.At( st.x, st.y );
			worst                 = std::max( worst, std::fabs( vertex - expected ) );
			steep += std::fabs( expected ) >= 2.0 * bound;
			++measured;
		}
		Check( measured >= 8 && steep >= 2 && worst <= bound,
		       fmt( "%dx%d at %.0fx f/2 on a 0.5 mm bow (seed %d): %d points measured, %d of them twice the bound or more off the "
		            "centre's height (so a flipped bow is caught); worst |vertex - bow difference| %.4f mm (bound %.3f)",
		            raster.w, raster.h, M, seed, measured, steep, worst, bound ) );
	}
	return Verdict();
}

//===========================================================================
// --track: the picture is where the hand says the carriage is.
//===========================================================================
int runTrack( const Perturb& perturb )
{
	std::printf( "\n=== track: through pans, corrections and crash zooms, the screen's centre (read off a coordinate clip) is the\n"
	             "    card point the hand model has the CARRIAGE at -- not the hand, which the grip lags\n" );
	for( const Raster& raster : kRasters )
	{
		Rig rig;
		if( !startRig( rig, raster, coordinateCard( raster.w, raster.h ), perturb ) )
			return 1;
		rig.Set( PT_OPERATOR, static_cast< float >( Operator::Auto ) );
		rig.Set( PT_BROWSE, static_cast< float >( Browse::Searching ) );
		rig.Set( PT_CRASH_ZOOM, 1.0f );
		rig.Set( PT_DWELL, ParamFromDwell( 0.3 ) );
		rig.Set( PT_CARRIAGE_PLAY, 0.6f );
		rig.Set( PT_COLUMNS, 6.0f );
		rig.Set( PT_ROWS, 4.0f );
		rig.plugin.SetHandLoggingForTest( true );
		const reader::Card c = cardOf( rig );
		int checked = 0;
		double worst = 0.0, fastest = 0.0, gripLag = 0.0, slack = 1e9, worstBound = 0.0;
		double lastX = 0.0, lastY = 0.0;
		for( int f = 0; f < 600; ++f )
		{
			if( !rig.Render( 1 ) )
				return 1;
			const hand::Hand& h  = rig.plugin.HandForTest();
			const hand::State st = h.Now();
			const double M       = std::exp2( st.logM );
			if( f > 0 )
				fastest = std::max( fastest, std::hypot( st.x - lastX, st.y - lastY ) * 60.0 );
			lastX = st.x;
			lastY = st.y;
			gripLag = std::max( gripLag, std::hypot( h.HandX() - st.x, h.HandY() - st.y ) );
			double u = 0.0, v = 0.0;
			if( !frameAt( c, st.x, st.y, 3.0 / ( M * pxPerMm( rig ) ), u, v ) )
				continue;
			double ru = 0.0, rv = 0.0;
			centreReading( rig, rig.Output(), ru, rv );
			const double err = std::max( std::fabs( ru - u ) * c.frameW, std::fabs( rv - v ) * c.frameH );
			const double texelsPerPixel = raster.w / ( c.frameW * M * pxPerMm( rig ) );
			const double bound          = coordinateBound( std::max( c.frameW, c.frameH ), texelsPerPixel );
			if( err > worst && std::getenv( "MFTEST_DEBUG" ) )
				std::printf( "    frame %d: M %.2f u %.5f read %.5f, v %.5f read %.5f (bound %.4f)\n", f, M, u, ru, v, rv, bound );
			worst = std::max( worst, err );
			if( bound - err < slack )
			{
				slack      = bound - err;
				worstBound = bound;
				if( std::getenv( "MFTEST_DEBUG" ) )
					std::printf( "    tightest so far, frame %d: M %.2f texels/px %.3f u %.6f read %.6f (%.4f mm), v %.6f read %.6f (%.4f mm)\n", f, M,
					             texelsPerPixel, u, ru, ( ru - u ) * c.frameW, v, rv, ( rv - v ) * c.frameH );
			}
			++checked;
		}
		int pans = 0, corrections = 0, zooms = 0;
		for( const hand::Segment& g : rig.plugin.HandForTest().Log() )
		{
			pans += g.tag == hand::Tag::Primary;
			corrections += g.tag == hand::Tag::Correction;
			zooms += g.tag == hand::Tag::ZoomOut;
		}
		Check( checked >= 300 && pans >= 3 && corrections >= 1 && zooms >= 1 && slack >= 0.0,
		       fmt( "%dx%d, 600 frames (%d pans, %d corrections, %d crash zooms; carriage up to %.0f mm/s, grip lag up to %.2f mm): "
		            "%d frames read, worst |picture - carriage| %.4f mm; nearest its bound by %.4f (of %.4f mm: a half-float ULP per store "
		            "and one for the filter, 2 + ceil( LOD ) of them)",
		            raster.w, raster.h, pans, corrections, zooms, fastest, gripLag, checked, worst, slack, worstBound ) );
	}
	return Verdict();
}

//===========================================================================
// --carriage: a hand step through the grip is the second-order step
// response, against an independent integration.
//===========================================================================
int runCarriage( const Perturb& perturb )
{
	std::printf( "\n=== carriage: the card on a compliant grip, x'' = w^2 ( hand - x ) - 2 zeta w x', after a step of the hand:\n"
	             "    the model against an independent RK4 integration, its overshoot against exp( -pi zeta / sqrt( 1 - zeta^2 ) ),\n"
	             "    and the picture against both\n" );
	for( const Raster& raster : kRasters )
	{
		Rig rig;
		if( !startRig( rig, raster, coordinateCard( raster.w, raster.h ), perturb ) )
			return 1;
		rig.Set( PT_COLUMNS, 6.0f );
		rig.Set( PT_ROWS, 4.0f );
		rig.Set( PT_CARRIAGE_PLAY, 1.0f );
		const reader::Card c = cardOf( rig );
		const double M       = 6.0;
		const int stepFrame  = 10;
		aim( rig, 0.30 * c.width, c.FrameTop( 1 ) + 0.5 * c.frameH, M );
		std::vector< double > model, picture, when;
		double x0 = 0.0, x1 = 0.0;
		for( int f = 0; f <= stepFrame + 90; ++f )
		{
			if( f == stepFrame )
			{
				x0 = rig.plugin.SettingsForTest().manualX;
				rig.Set( PT_POSITION_X, 0.36f );
			}
			if( !rig.Render( 1 ) )
				return 1;
			if( f == stepFrame )
				x1 = rig.plugin.SettingsForTest().manualX;
			const hand::State st = rig.plugin.HandForTest().Now();
			model.push_back( st.x );
			when.push_back( rig.plugin.HandForTest().Time() );
			double u = 0.0, v = 0.0, ru = 0.0, rv = 0.0;
			const int col = static_cast< int >( std::floor( ( st.x - c.gridX + 0.5 * c.gutter ) / c.PitchX() ) );
			if( frameAt( c, st.x, st.y, 3.0 / ( M * pxPerMm( rig ) ), u, v ) )
			{
				centreReading( rig, rig.Output(), ru, rv );
				picture.push_back( c.FrameLeft( col ) + ru * c.frameW );
			}
			else
				picture.push_back( NAN );
		}
		// The reference: RK4 at 10 us, the hand still until the step frame
		// begins and linear across it, as the host's slider is.
		const double w = 2.0 * 3.14159265358979323846 * CarriageHertzFromParam( 1.0f ), zeta = CarriageZetaFromParam( 1.0f );
		const double tA = ( stepFrame - 1 ) / 60.0, tB = stepFrame / 60.0;
		auto handAt = [ & ]( double t ) { return t <= tA ? x0 : t >= tB ? x1 : x0 + ( x1 - x0 ) * ( t - tA ) / ( tB - tA ); };
		double x = x0, vx = 0.0, t = 0.0, worstModel = 0.0, worstPicture = 0.0, peak = 0.0;
		const double hstepMax = 1e-5;
		size_t k = 0;
		int read = 0;
		while( k < when.size() )
		{
			if( t >= when[ k ] )
			{
				worstModel = std::max( worstModel, std::fabs( model[ k ] - x ) );
				if( !std::isnan( picture[ k ] ) )
				{
					worstPicture = std::max( worstPicture, std::fabs( picture[ k ] - x ) );
					++read;
				}
				++k;
				continue;
			}
			// Step exactly onto the frame's time, and onto the ramp's ends.
			double hstep = std::min( hstepMax, when[ k ] - t );
			for( double edge : { tA, tB } )
				if( t < edge && t + hstep > edge )
					hstep = edge - t;
			auto acc = [ & ]( double tt, double xx, double vv ) { return w * w * ( handAt( tt ) - xx ) - 2.0 * zeta * w * vv; };
			const double k1x = vx, k1v = acc( t, x, vx );
			const double k2x = vx + 0.5 * hstep * k1v, k2v = acc( t + 0.5 * hstep, x + 0.5 * hstep * k1x, vx + 0.5 * hstep * k1v );
			const double k3x = vx + 0.5 * hstep * k2v, k3v = acc( t + 0.5 * hstep, x + 0.5 * hstep * k2x, vx + 0.5 * hstep * k2v );
			const double k4x = vx + hstep * k3v, k4v = acc( t + hstep, x + hstep * k3x, vx + hstep * k3v );
			x += hstep / 6.0 * ( k1x + 2.0 * k2x + 2.0 * k3x + k4x );
			vx += hstep / 6.0 * ( k1v + 2.0 * k2v + 2.0 * k3v + k4v );
			t += hstep;
			if( t > tB )
				peak = std::max( peak, ( x - x1 ) / ( x1 - x0 ) );
		}
		// The textbook overshoot is for an instant step; a step spread over
		// one frame overshoots by sinc( w_d T / 2 ) of it (here 0.998).
		const double wd       = w * std::sqrt( 1.0 - zeta * zeta );
		const double textbook = std::exp( -3.14159265358979323846 * zeta / std::sqrt( 1.0 - zeta * zeta ) );
		const double spread   = std::sin( 0.5 * wd / 60.0 ) / ( 0.5 * wd / 60.0 );
		Check( worstModel <= 1e-6,
		       fmt( "%dx%d: a %.2f mm step at %.0f Hz, zeta %.2f: the model's carriage within %.2g mm of RK4 over 1.5 s (bound 1e-6)", raster.w,
		            raster.h, x1 - x0, w / ( 2.0 * 3.14159265358979323846 ), zeta, worstModel ) );
		Check( std::fabs( peak - textbook * spread ) <= 0.002,
		       fmt( "%dx%d: overshoot %.4f of the step; exp( -pi zeta / sqrt( 1 - zeta^2 ) ) x sinc %.4f (bound 0.002: the "
		            "reference's sampling of its peak)",
		            raster.w, raster.h, peak, textbook * spread ) );
		const double bound = coordinateBound( c.frameW, raster.w / ( c.frameW * M * pxPerMm( rig ) ) );
		Check( read >= 60 && worstPicture <= bound,
		       fmt( "%dx%d: %d frames read off the picture, worst |picture - RK4| %.4f mm (bound %.4f)", raster.w, raster.h, read,
		            worstPicture, bound ) );
	}
	return Verdict();
}

//===========================================================================
// --shutter: a carriage at constant speed smears an edge over the exposure.
//===========================================================================
int runShutter( const Perturb& perturb )
{
	std::printf( "\n=== shutter: a carriage at constant speed smears an edge over Shutter x the frame's travel: the smear's centroid\n"
	             "    lags the edge by half of it and its spread is that of a uniform smear of that length\n" );
	for( const Raster& raster : kRasters )
	{
		Floats card = flatCard( raster.w, raster.h, 0.0f, 0.0f, 0.0f );
		for( int y = 0; y < raster.h; ++y )
			for( int x = raster.w / 2; x < raster.w; ++x )
				for( int ch = 0; ch < 3; ++ch )
					card[ ( static_cast< size_t >( y ) * raster.w + x ) * 4 + ch ] = 1.0f;
		for( double shutter : { 0.5, 1.0 } )
		{
			Rig rig;
			if( !startRig( rig, raster, card, perturb ) )
				return 1;
			rig.Set( PT_COLUMNS, 1.0f );
			rig.Set( PT_ROWS, 1.0f );
			rig.plugin.SetPrefilterForTest( false );
			const reader::Card c = cardOf( rig );
			const double M       = reader::kScreenWidth / c.frameW;
			const double edgeX   = c.FrameLeft( 0 ) + 0.5 * c.frameW;
			const double stepMm  = 48.0 * raster.w / 1280.0 / ( M * pxPerMm( rig ) );// 48 px a frame at 720p
			const double y       = c.FrameTop( 0 ) + 0.5 * c.frameH;
			// The static edge, to subtract its own spread.
			rig.Set( PT_SHUTTER, 0.0f );
			aim( rig, edgeX - 2.0 * stepMm, y, M );
			if( !rig.Render( 2 ) )
				return 1;
			// The step's line-spread, local to it: the static edge sits 2 S
			// right of the centre, the moving one ends S left of it (the image
			// moves against the carriage), S the travel a frame. The glass
			// beyond the card is far off.
			const double S = 48.0 * raster.w / 1280.0;
			const int i0   = std::max( 0, static_cast< int >( 0.5 * rig.width - S ) - 30 );
			const int i1   = std::min( rig.width - 1, static_cast< int >( 0.5 * rig.width + 2.0 * S ) + 30 );
			auto lsf = [ & ]( const Floats& out, double& mean, double& var ) {
				double s0 = 0.0, s1 = 0.0, s2 = 0.0;
				const int row = rig.height / 2;
				for( int i = i0; i < i1; ++i )
				{
					const double d = toLinear( pixelTop( out, rig.width, rig.height, i + 1, row )[ 0 ] )
					               - toLinear( pixelTop( out, rig.width, rig.height, i, row )[ 0 ] );
					s0 += d;
					s1 += d * ( i + 0.5 );
					s2 += d * ( i + 0.5 ) * ( i + 0.5 );
				}
				mean = s1 / s0;
				var  = s2 / s0 - mean * mean;
			};
			double mean0 = 0.0, var0 = 0.0;
			lsf( rig.Output(), mean0, var0 );
			const double carriage0 = rig.plugin.HandForTest().Now().x;
			// Now moving: stepMm a frame, Shutter open.
			rig.Set( PT_SHUTTER, static_cast< float >( shutter ) );
			const double xStart = edgeX - 2.0 * stepMm;
			for( int f = 1; f <= 3; ++f )
			{
				rig.Set( PT_POSITION_X, static_cast< float >( ( xStart + f * stepMm ) / c.width ) );
				if( !rig.Render( 1 ) )
					return 1;
			}
			double mean = 0.0, var = 0.0;
			lsf( rig.Output(), mean, var );
			const hand::Hand& h  = rig.plugin.HandForTest();
			const double window  = shutter / 60.0;
			const double mid     = h.At( h.Time() - 0.5 * window ).x;
			const double travel  = ( h.Now().x - h.At( h.Time() - window ).x ) * M * pxPerMm( rig );
			const double lagged  = mean0 - ( mid - carriage0 ) * M * pxPerMm( rig );
			const double spread2 = 12.0 * ( var - var0 );
			// The centroid of n evenly spaced moments of a uniform motion is
			// its middle, exactly. Their spread squared is L^2 ( 1 - 1/n^2 ),
			// n a power of two and at least 4 here (L > 4.5 px); and a sampled
			// edge's own variance depends on its sub-pixel phase, phi ( 1 -
			// phi ), so the static edge subtracted can be off by up to 1/4 px^2:
			// 3 px^2 of L^2.
			Check( std::fabs( mean - lagged ) <= 0.01,
			       fmt( "%dx%d, Shutter %.1f: the smear's centroid at %.3f px, the carriage at mid-exposure puts it at %.3f (bound 0.01 px)",
			            raster.w, raster.h, shutter, mean, lagged ) );
			Check( spread2 >= 0.9375 * travel * travel - 3.0 && spread2 <= travel * travel + 3.0,
			       fmt( "%dx%d, Shutter %.1f: smear %.2f px across, the exposure's travel %.2f px (bound on its square: 0.9375 L^2 - 3 to "
			            "L^2 + 3 px^2)",
			            raster.w, raster.h, shutter, std::sqrt( std::max( 0.0, spread2 ) ), travel ) );
		}
	}
	return Verdict();
}

//===========================================================================
// --hunt: the hunt, as the hand plans it and as the picture shows it.
//===========================================================================
int runHunt( const Perturb& perturb )
{
	std::printf( "\n=== hunt: past focus, back, past again by less: each reversal delta_a + v tau beyond focus, each pass g times the\n"
	             "    last's speed, the stop inside the depth of focus -- and the picture's blur, frame by frame, the knob's\n" );
	for( const Raster& raster : kRasters )
	{
		const int tx = raster.w / 2, ty = raster.h / 2;
		// A seed whose first hunt turns at least twice at this setting.
		int seed = -1;
		for( int candidate = 1; candidate < 60 && seed < 0; ++candidate )
		{
			hand::Settings s;
			s.dwell    = 10.0;
			s.browse   = Browse::Reading;
			s.readZoom = 60.0;
			s.aperture = 2.0;
			s.parfocal = 0.0;
			s.rigid    = true;
			s.skill    = 0.35;
			s.seed     = static_cast< uint32_t >( candidate );
			s.screenH  = reader::kScreenWidth * raster.h / raster.w;
			const reader::Card c = reader::MakeCard( 16, 1, 0.0, static_cast< double >( raster.w ) / raster.h );
			reader::Bow bow;
			bow.Build( c, 0.5, s.seed );
			hand::Hand h;
			h.SetLogging( true );
			h.Advance( 0.0, s, c, bow, false, false );
			if( !h.Hunts().empty() && h.Hunts().front().turns.size() >= 2 )
				seed = candidate;
		}
		Rig rig;
		if( !startRig( rig, raster, dotCard( raster.w, raster.h, { { tx, ty } } ), perturb ) )
			return 1;
		rig.Set( PT_OPERATOR, static_cast< float >( Operator::Auto ) );
		rig.Set( PT_BROWSE, static_cast< float >( Browse::Reading ) );
		rig.Set( PT_DWELL, 1.0f );
		rig.Set( PT_ZOOM, ParamFromZoom( 60.0 ) );
		rig.Set( PT_APERTURE, ParamFromAperture( 2.0 ) );
		rig.Set( PT_FLATNESS, ParamFromFlatness( 0.5 ) );
		rig.Set( PT_FOCUS_SKILL, 0.35f );
		rig.Set( PT_COLUMNS, 16.0f );
		rig.Set( PT_ROWS, 1.0f );
		rig.Set( PT_GUTTER, 0.0f );
		rig.Set( PT_SEED, static_cast< float >( std::max( seed, 1 ) ) );
		rig.plugin.SetPrefilterForTest( false );
		rig.plugin.SetHandLoggingForTest( true );
		const reader::Card c = cardOf( rig );
		double dx = 0.0, dy = 0.0;
		texelOnCard( c, raster.w, raster.h, 0, 0, tx, ty, dx, dy );

		// The dot in focus, for its own spread.
		double sharpM2 = 0.0;
		{
			Rig flat;
			if( !startRig( flat, raster, dotCard( raster.w, raster.h, { { tx, ty } } ), perturb ) )
				return 1;
			flat.Set( PT_COLUMNS, 16.0f );
			flat.Set( PT_ROWS, 1.0f );
			flat.Set( PT_GUTTER, 0.0f );
			flat.plugin.SetPrefilterForTest( false );
			aim( flat, dx, dy, 60.0 );
			if( !flat.Render( 2 ) )
				return 1;
			const hand::State st = flat.plugin.HandForTest().Now();
			double i = 0.0, j = 0.0;
			toScreen( flat, dx, dy, st.x, st.y, std::exp2( st.logM ), i, j );
			sharpM2 = momentsOf( flat.Output(), flat.width, flat.height, i, j, 10.0 ).m2;
		}

		double worst = 0.0, largest = 0.0;
		int compared = 0, crossings = 0, sharpestAtCrossing = 0;
		double lastDelta = 0.0, lastR = 0.0, beforeR = 1e9;
		for( int f = 0; f < 150; ++f )
		{
			if( !rig.Render( 1 ) )
				return 1;
			const hand::State st = rig.plugin.HandForTest().Now();
			const double M       = std::exp2( st.logM );
			const double delta   = st.z - rig.plugin.BowForTest().At( dx, dy );
			const double model   = M * reader::FilmBlurRadius( delta, M, 2.0 ) * pxPerMm( rig );
			double i = 0.0, j = 0.0;
			toScreen( rig, dx, dy, st.x, st.y, M, i, j );
			const Moments m = momentsOf( rig.Output(), rig.width, rig.height, i, j, model + 12.0 );
			// The Vogel centroid's own offset is part of the disc's moment
			// about the point's position: read it off the picture.
			const double shift = ( m.cx - i ) * ( m.cx - i ) + ( m.cy - j ) * ( m.cy - j );
			const double R     = std::sqrt( std::max( 0.0, 2.0 * ( m.m2 - sharpM2 + shift ) ) );
			if( model >= 6.0 )
			{
				worst = std::max( worst, std::fabs( R / model - 1.0 ) );
				++compared;
			}
			largest = std::max( largest, model );
			// Through focus: of the frames either side, the sharper picture is
			// the one the model has nearer focus, wherever a picture can tell
			// them apart (the blurrier at least 3 px, and half as far out
			// again as the other).
			const double nearer  = std::min( std::fabs( delta ), std::fabs( lastDelta ) );
			const double further = std::max( std::fabs( delta ), std::fabs( lastDelta ) );
			const double furtherPx = M * reader::FilmBlurRadius( further, M, 2.0 ) * pxPerMm( rig );
			if( f > 0 && delta * lastDelta < 0.0 && further >= 1.5 * nearer && furtherPx >= 3.0 )
			{
				++crossings;
				const bool nearerNow = std::fabs( delta ) < std::fabs( lastDelta );
				sharpestAtCrossing += nearerNow == ( R < lastR );
			}
			beforeR   = lastR;
			lastDelta = delta;
			lastR     = R;
		}
		(void)beforeR;
		const auto& hunts = rig.plugin.HandForTest().Hunts();
		Check( !hunts.empty() && hunts.front().turns.size() >= 2,
		       fmt( "%dx%d, seed %d: the first hunt starts %.3f mm out and turns %zu times", raster.w, raster.h, seed,
		            hunts.empty() ? 0.0 : hunts.front().start, hunts.empty() ? size_t( 0 ) : hunts.front().turns.size() ) );
		if( hunts.empty() )
			continue;
		const hand::Hunt& hu = hunts.front();
		double worstTurn = 0.0, worstGain = 0.0;
		size_t pass       = hu.wrongWay ? 1 : 0;
		for( size_t k = hu.wrongWay ? 1 : 0; k < hu.turns.size(); ++k, ++pass )
			worstTurn = std::max( worstTurn, std::fabs( std::fabs( hu.turns[ k ] ) - ( hu.band + hu.speeds[ pass ] * hu.reaction ) ) );
		for( size_t k = ( hu.wrongWay ? 2 : 1 ); k < hu.speeds.size(); ++k )
			worstGain = std::max( worstGain, std::fabs( hu.speeds[ k ] / hu.speeds[ k - 1 ] - hu.gain ) );
		const bool lastSlow = hu.speeds.back() * hu.reaction <= 2.0 * hu.band;
		Check( worstTurn <= 1e-12 && worstGain <= 1e-12 && std::fabs( hu.end ) <= hu.band && lastSlow,
		       fmt( "%dx%d: reversals at delta_a + v tau to %.1g mm (delta_a %.4f mm, tau %.3f s), each pass %.3f of the last's speed to "
		            "%.1g, stopped %.4f mm out, inside delta_a, on the first pass slow enough (v tau <= 2 delta_a)",
		            raster.w, raster.h, worstTurn, hu.band, hu.reaction, hu.gain, worstGain, hu.end ) );
		// 2%: the defocus check's 1% on the disc, plus the GPU's bow, read
		// bilinearly off a 1 mm grid (0.002 mm, 1% of the smallest defocus
		// compared here).
		// At 320 px no pass through focus is blurred 3 px either side, so
		// the order is only required where a picture can show it.
		const bool judgeable = raster.w >= 1280;
		Check( compared >= 10 && worst <= 0.02 && ( crossings >= 1 || !judgeable ) && sharpestAtCrossing == crossings,
		       fmt( "%dx%d: the picture's blur against the knob's, %d frames with 6 px or more (largest %.1f px): worst %.2f%% (bound 2%%); "
		            "%d passes through focus a picture can judge, the sharper frame the nearer one in %d",
		            raster.w, raster.h, compared, largest, 100.0 * worst, crossings, sharpestAtCrossing ) );
	}
	return Verdict();
}

//===========================================================================
// --stock: what each film transmits.
//===========================================================================
int runStock( const Perturb& perturb )
{
	std::printf( "\n=== stock: every film is a unit-gamma print, T = dense + ( base - dense ) f, f the exposure (1 - exposure on a\n"
	             "    negative, Rec.709 luminance on a mono stock); the gutters are unexposed and off the card there is only glass\n" );
	// The bound: the exposure is held in RGBA16F linear light (2^-11 of it,
	// doubled for truncation) and scaled by base - dense <= 1; the output's
	// float sRGB round trip adds ~1e-6.
	constexpr double kTol = 2.0 / 2048.0 + 1e-5;
	for( const Raster& raster : kRasters )
	{
		Floats card( static_cast< size_t >( raster.w ) * raster.h * 4 );
		for( int y = 0; y < raster.h; ++y )
			for( int x = 0; x < raster.w; ++x )
			{
				const double u = ( x + 0.5 ) / raster.w;
				float* o       = &card[ ( static_cast< size_t >( y ) * raster.w + x ) * 4 ];
				o[ 0 ]         = static_cast< float >( u );
				o[ 1 ]         = static_cast< float >( 1.0 - u );
				o[ 2 ]         = 0.5f;
				o[ 3 ]         = 1.0f;
			}
		Rig rig;
		if( !startRig( rig, raster, card, perturb ) )
			return 1;
		for( int film = 0; film < static_cast< int >( Film::Count ); ++film )
		{
			const reader::Stock& stock = reader::StockOf( static_cast< Film >( film ) );
			rig.Set( PT_FILM, static_cast< float >( film ) );
			rig.Set( PT_COLUMNS, 1.0f );
			rig.Set( PT_ROWS, 1.0f );
			reader::Card c = cardOf( rig );
			aim( rig, c.gridX + 0.5 * c.frameW, c.gridY + 0.5 * c.frameH, reader::kScreenWidth / c.frameW );
			if( !rig.Render( 2 ) )
				return 1;
			const Floats out = rig.Output();
			double worst     = 0.0;
			for( int x = 0; x < raster.w; x += 3 )
			{
				const float* src = &card[ ( static_cast< size_t >( raster.h / 2 ) * raster.w + x ) * 4 ];
				double e[ 3 ];
				for( int ch = 0; ch < 3; ++ch )
					e[ ch ] = reader::ToLinear( src[ ch ] );
				const double Y = 0.2126 * e[ 0 ] + 0.7152 * e[ 1 ] + 0.0722 * e[ 2 ];
				const float* got = pixelTop( out, raster.w, raster.h, x, raster.h - 1 - raster.h / 2 );
				for( int ch = 0; ch < 3; ++ch )
				{
					double f = stock.colour ? e[ ch ] : Y;
					if( stock.negative )
						f = 1.0 - f;
					const double T = stock.dense[ ch ] + ( stock.base[ ch ] - stock.dense[ ch ] ) * f;
					worst          = std::max( worst, std::fabs( toLinear( got[ ch ] ) - T ) );
				}
			}
			// A gutter, and the glass beside the card.
			rig.Set( PT_COLUMNS, 2.0f );
			rig.Set( PT_GUTTER, ParamFromGutter( 3.0 ) );
			c = cardOf( rig );
			aim( rig, c.FrameLeft( 0 ) + c.frameW + 0.5 * c.gutter, c.FrameTop( 0 ) + 0.5 * c.frameH, 20.0 );
			if( !rig.Render( 2 ) )
				return 1;
			const float* gutter = pixelTop( rig.Output(), raster.w, raster.h, raster.w / 2, raster.h / 2 );
			// The carriage cannot leave the card, so put its edge at the
			// screen's centre and look a quarter of the screen to its left.
			aim( rig, 0.0, c.FrameTop( 0 ) + 0.5 * c.frameH, 20.0 );
			if( !rig.Render( 2 ) )
				return 1;
			const float* glass = pixelTop( rig.Output(), raster.w, raster.h, raster.w / 4, raster.h / 2 );
			double gutterErr = 0.0, glassErr = 0.0;
			for( int ch = 0; ch < 3; ++ch )
			{
				const double unexposed = stock.negative ? stock.base[ ch ] : stock.dense[ ch ];
				gutterErr              = std::max( gutterErr, std::fabs( toLinear( gutter[ ch ] ) - unexposed ) );
				glassErr               = std::max( glassErr, std::fabs( toLinear( glass[ ch ] ) - 1.0 ) );
			}
			rig.Set( PT_GUTTER, 0.0f );
			Check( worst <= kTol && gutterErr <= kTol && glassErr <= kTol,
			       fmt( "%dx%d, %s: worst |T - print| %.2g across a ramp; a gutter %.2g from %s; the glass %.2g from clear (bound %.2g)",
			            raster.w, raster.h, InfoOf( PT_FILM ).options[ film ], worst, gutterErr, stock.negative ? "the base" : "the dye",
			            glassErr, kTol ) );
		}
	}
	return Verdict();
}

//===========================================================================
// --screen: the hotspot is cos^4; the grain is the screen's; the dust is the
// card's.
//===========================================================================
int runScreen( const Perturb& perturb )
{
	std::printf( "\n=== screen: the light falls off as cos^4 of the ray's angle; the diffuser's grain stays put while the card moves,\n"
	             "    and the dust in the emulsion moves with it, by the carriage's travel times M\n" );
	for( const Raster& raster : kRasters )
	{
		Rig rig;
		if( !startRig( rig, raster, flatCard( raster.w, raster.h, 1.0f, 1.0f, 1.0f ), perturb ) )
			return 1;
		rig.Set( PT_COLUMNS, 1.0f );
		rig.Set( PT_ROWS, 1.0f );
		const reader::Card c = cardOf( rig );
		const double cx = c.gridX + 0.5 * c.frameW, cy = c.gridY + 0.5 * c.frameH;
		// The hotspot. The bound: float32 arithmetic on q and its square, and
		// the output's sRGB round trip, each ~1e-6.
		for( double h : { 0.5, 1.0 } )
		{
			rig.Set( PT_HOTSPOT, static_cast< float >( h ) );
			aim( rig, cx, cy, 24.0 );
			if( !rig.Render( 2 ) )
				return 1;
			const Floats out = rig.Output();
			const double L   = ThrowFromParam( static_cast< float >( h ) );
			double worst = 0.0, corner = 1.0;
			for( int y = 0; y < raster.h; y += 2 )
				for( int x = 0; x < raster.w; x += 2 )
				{
					const double sx = ( x + 0.5 - 0.5 * raster.w ) / pxPerMm( rig ), sy = ( y + 0.5 - 0.5 * raster.h ) / pxPerMm( rig );
					const double expected = reader::Falloff( std::hypot( sx, sy ), L );
					corner                = std::min( corner, expected );
					worst = std::max( worst, std::fabs( toLinear( pixelTop( out, raster.w, raster.h, x, y )[ 0 ] ) - expected ) );
				}
			Check( worst <= 2e-4, fmt( "%dx%d, a %.0f mm throw: every pixel within %.2g of ( L^2 / ( L^2 + r^2 ) )^2 (the corners at %.3f; bound 2e-4)",
			                           raster.w, raster.h, L, worst, corner, 2e-4 ) );
		}
		rig.Set( PT_HOTSPOT, 0.0f );

		// The grain: two carriage positions on a flat frame, one pattern.
		rig.Set( PT_SCREEN_GRAIN, 1.0f );
		aim( rig, cx, cy, 24.0 );
		if( !rig.Render( 2 ) )
			return 1;
		const Floats a = rig.Output();
		aim( rig, cx + 1.3, cy - 0.7, 24.0 );
		if( !rig.Render( 2 ) )
			return 1;
		const Floats b = rig.Output();
		double differ = 0.0, mean = 0.0, sq = 0.0;
		int n = 0;
		for( size_t i = 0; i < a.size(); i += 4 )
		{
			differ = std::max( differ, static_cast< double >( std::fabs( a[ i ] - b[ i ] ) ) );
			const double v = toLinear( a[ i ] );
			mean += v;
			sq += v * v;
			++n;
		}
		mean /= n;
		const double sd = std::sqrt( std::max( 0.0, sq / n - mean * mean ) );
		Check( differ <= 1e-6 && sd >= 0.005,
		       fmt( "%dx%d: the grain (%.1f%% scatter) is the same at two carriage positions 1.5 mm apart: worst difference %.2g (bound 1e-6)",
		            raster.w, raster.h, 100.0 * sd / mean, differ ) );
		rig.Set( PT_SCREEN_GRAIN, 0.0f );

		// The dust: moved 17 px by the carriage, it is the same picture 17 px over.
		rig.Set( PT_DUST, 1.0f );
		const double M     = 24.0;
		const int shift    = 17;
		const double moved = shift / ( M * pxPerMm( rig ) );
		aim( rig, cx, cy, M );
		if( !rig.Render( 2 ) )
			return 1;
		const Floats d0 = rig.Output();
		const double p0 = rig.plugin.HandForTest().Now().x;
		aim( rig, cx + moved, cy, M );
		if( !rig.Render( 2 ) )
			return 1;
		const Floats d1   = rig.Output();
		const double p1   = rig.plugin.HandForTest().Now().x;
		const double real = ( p1 - p0 ) * M * pxPerMm( rig );
		double worst = 0.0, darkest = 1.0;
		for( int y = 0; y < raster.h; ++y )
			for( int x = 0; x + shift < raster.w; ++x )
			{
				const double was = toLinear( pixelTop( d0, raster.w, raster.h, x + shift, y )[ 0 ] );
				const double now = toLinear( pixelTop( d1, raster.w, raster.h, x, y )[ 0 ] );
				worst            = std::max( worst, std::fabs( was - now ) );
				darkest          = std::min( darkest, was );
			}
		// The bound: the carriage's position comes through a float control,
		// %.4f px from 17 here, against a dust edge a footprint wide.
		Check( darkest < 0.9 && worst <= 5e-3,
		       fmt( "%dx%d: dust (darkest %.2f) after the card moved %.3f px: the picture %d px over to %.2g (bound 5e-3)", raster.w,
		            raster.h, darkest, real, shift, worst ) );
	}
	return Verdict();
}

//===========================================================================
// --filmed: a step-and-repeat camera, one frame per Interval, in reading order.
//===========================================================================
int runFilmed( const Perturb& perturb )
{
	std::printf( "\n=== filmed: the camera exposes the clip into the card's frames in reading order, one per Interval, wrapping;\n"
	             "    each frame holds the clip as it was when it was exposed, and the rest are unexposed film\n" );
	auto code = []( int f, int ch ) { return ch == 0 ? ( f % 8 ) / 7.0 : ch == 1 ? ( ( f / 8 ) % 8 ) / 7.0 : 0.5; };
	for( const Raster& raster : kRasters )
	{
		Rig rig;
		if( !startRig( rig, raster, flatCard( raster.w, raster.h, 0.0f, 0.0f, 0.5f ), perturb ) )
			return 1;
		rig.Set( PT_CONTENT, static_cast< float >( Content::Filmed ) );
		rig.Set( PT_COLUMNS, 4.0f );
		rig.Set( PT_ROWS, 3.0f );
		const float intervalParam = ParamFromInterval( 0.0731 );
		rig.Set( PT_INTERVAL, intervalParam );
		const double interval = IntervalFromParam( intervalParam );
		const reader::Card c  = cardOf( rig );
		aim( rig, 0.5 * c.width, 0.5 * c.height, 2.0 );
		rig.beforeFrame = [ & ]( int f ) {
			Floats picture = flatCard( raster.w, raster.h, static_cast< float >( code( f, 0 ) ), static_cast< float >( code( f, 1 ) ), 0.5f );
			rig.Upload( picture );
		};
		// Exposure k at the first frame whose time reaches k intervals.
		std::vector< int > exposedAt;
		for( int k = 0; k < 40; ++k )
			exposedAt.push_back( static_cast< int >( std::ceil( k * interval * 60.0 - 1e-9 ) ) );
		for( int stop : { 20, 70 } )
		{
			if( !rig.Render( stop - rig.frame ) )
				return 1;
			const int last = rig.frame - 1;
			std::vector< int > holds( c.Frames(), -1 );
			int exposures = 0;
			for( int k = 0; k < 40 && exposedAt[ k ] <= last; ++k, ++exposures )
				holds[ k % c.Frames() ] = exposedAt[ k ];
			const Floats out = rig.Output();
			const hand::State st = rig.plugin.HandForTest().Now();
			int right = 0;
			std::string first;
			for( int cell = 0; cell < c.Frames(); ++cell )
			{
				const int col = cell % c.cols, row = cell / c.cols;
				double i = 0.0, j = 0.0;
				toScreen( rig, c.FrameLeft( col ) + 0.5 * c.frameW, c.FrameTop( row ) + 0.5 * c.frameH, st.x, st.y, std::exp2( st.logM ), i, j );
				const float* p = pixelTop( out, raster.w, raster.h, static_cast< int >( std::lround( i ) ), static_cast< int >( std::lround( j ) ) );
				bool ok        = true;
				for( int ch = 0; ch < 3; ++ch )
				{
					const double expected = holds[ cell ] < 0 ? 0.0 : code( holds[ cell ], ch );
					ok                    = ok && std::fabs( p[ ch ] - expected ) <= 1.0 / 255.0;
				}
				right += ok;
				if( !ok && first.empty() )
					first = fmt( " (frame %d: %.3f %.3f %.3f, expected clip frame %d)", cell, p[ 0 ], p[ 1 ], p[ 2 ], holds[ cell ] );
			}
			// One 8-bit level: the store keeps sRGB code in RGBA8.
			Check( right == c.Frames() && rig.plugin.WrittenForTest() == std::min( exposures, c.Frames() ),
			       fmt( "%dx%d after %d host frames, Interval %.4f s: %d exposures; %d of %d frames hold the clip frame they should "
			            "(within one 8-bit level)%s",
			            raster.w, raster.h, stop, interval, exposures, right, c.Frames(), first.c_str() ) );
		}
	}
	return Verdict();
}

//===========================================================================
// --sync: with a beat, every act starts on it.
//===========================================================================
int runSync( const Perturb& perturb )
{
	std::printf( "\n=== sync: with Sync on Beat, every act starts on a beat -- at the beat's moment inside the frame, not the frame's\n" );
	for( const Raster& raster : kRasters )
	{
		Rig rig;
		if( !startRig( rig, raster, buildCard( raster.w, raster.h ), perturb ) )
			return 1;
		rig.bpm = 117.0;
		rig.Set( PT_OPERATOR, static_cast< float >( Operator::Auto ) );
		rig.Set( PT_SYNC, static_cast< float >( Sync::Beat ) );
		rig.Set( PT_BROWSE, static_cast< float >( Browse::Skimming ) );
		rig.Set( PT_DWELL, ParamFromDwell( 0.1 ) );
		rig.Set( PT_CRASH_ZOOM, 0.0f );
		rig.plugin.SetHandLoggingForTest( true );
		if( !rig.Render( 1200 ) )
			return 1;
		const double beat = 60.0 / 117.0;
		int acts = 0;
		double worst = 0.0, shortest = 1e9;
		const auto& log = rig.plugin.HandForTest().Log();
		for( size_t k = 0; k + 1 < log.size(); ++k )
			if( log[ k ].tag == hand::Tag::Dwell && std::isfinite( log[ k ].End() ) && log[ k ].End() < rig.plugin.HandForTest().Time() )
			{
				const double end = log[ k ].End();
				worst            = std::max( worst, std::fabs( end - std::round( end / beat ) * beat ) );
				shortest         = std::min( shortest, log[ k ].T );
				++acts;
			}
		// The bound: Resolume's bar phase is a float, good to ~6e-8 of a
		// 2.05 s bar; interpolated across a frame, a microsecond is ample.
		Check( acts >= 10 && worst <= 1e-6 && shortest >= hand::kMinSyncDwell - 1e-9,
		       fmt( "%dx%d at 117 BPM: %d acts, each starting within %.2g s of a beat (bound 1e-6), none after less than %.2f s of dwell (%.3f)",
		            raster.w, raster.h, acts, worst, hand::kMinSyncDwell, shortest ) );
	}
	return Verdict();
}

//===========================================================================
// The hand, without GL.
//===========================================================================
struct HandRun
{
	hand::Settings s;
	reader::Card card;
	reader::Bow bow;
	hand::Hand h;
	std::vector< std::pair< double, std::pair< double, double > > > path;///< ( t, hand x, y )
	bool keepPath = false;

	HandRun( int cols, int rows, double flatness, uint32_t seed, int hooks )
	{
		card = reader::MakeCard( cols, rows, 0.8, 16.0 / 9.0 );
		s.seed = seed;
		bow.Build( card, flatness, seed );
		h.SetHooks( hooks );
		h.SetLogging( true );
	}
	void Run( double seconds, double dt )
	{
		const int steps = static_cast< int >( std::lround( seconds / dt ) );
		h.Advance( 0.0, s, card, bow, false, false );
		for( int k = 0; k < steps; ++k )
		{
			h.Advance( dt, s, card, bow, false, false );
			if( keepPath )
				path.push_back( { h.Time(), { h.HandX(), h.HandY() } } );
		}
	}
};

//===========================================================================
// --fitts (no GL): aimed movements.
//===========================================================================
int runFitts( const Perturb& perturb )
{
	std::printf( "\n=== fitts: every pan's first submovement takes a + b log2( D / W + 1 ) and is minimum jerk (peak speed 1.875 D / T at\n"
	             "    T / 2); it lands 4%% short with a scatter of k D along and 0.4 k D across, at every distance; a correction follows\n"
	             "    exactly when it lands outside W / 2\n" );
	HandRun run( 14, 7, 0.0, 7, perturb.handHooks );
	run.s.browse    = Browse::Searching;
	run.s.crashZoom = 0.0;
	run.s.rigid     = true;
	run.s.dwell     = 0.2;
	run.keepPath    = true;
	run.Run( 900.0, 1.0 / 240.0 );
	const auto& log = run.h.Log();

	double worstTime = 0.0, worstPeak = 0.0, worstMid = 0.0;
	int profiled = 0;
	std::vector< double > along, across, alongShort, alongLong;
	std::vector< double > distances;
	for( const hand::Segment& g : log )
		if( g.kind == hand::Segment::Kind::Pan )
			distances.push_back( g.aim );
	std::vector< double > sorted = distances;
	std::sort( sorted.begin(), sorted.end() );
	const double median = sorted.empty() ? 0.0 : sorted[ sorted.size() / 2 ];
	int corrections = 0, rightCalls = 0, calls = 0;
	size_t cursor   = 0;
	for( size_t k = 0; k < log.size(); ++k )
	{
		const hand::Segment& g = log[ k ];
		if( g.kind != hand::Segment::Kind::Pan )
			continue;
		const double fitts = hand::kFittsA + run.s.fittsB * std::log2( g.aim / g.tolerance + 1.0 );
		worstTime          = std::max( worstTime, std::fabs( g.T - fitts ) );
		// The executed motion, sampled every 1/240 s: its peak speed and its midpoint.
		const double D = std::hypot( g.bx - g.ax, g.by - g.ay );
		if( g.T > 0.15 && D > 0.5 && !run.path.empty() && g.End() < run.path.back().first )
		{
			double peak = 0.0, midErr = 1e9;
			while( cursor < run.path.size() && run.path[ cursor ].first < g.t0 )
				++cursor;
			for( size_t i = cursor; i + 1 < run.path.size() && run.path[ i + 1 ].first <= g.End(); ++i )
			{
				const double dt = run.path[ i + 1 ].first - run.path[ i ].first;
				const double v  = std::hypot( run.path[ i + 1 ].second.first - run.path[ i ].second.first,
				                              run.path[ i + 1 ].second.second - run.path[ i ].second.second ) / dt;
				peak            = std::max( peak, v );
				const double tm = 0.5 * ( run.path[ i ].first + run.path[ i + 1 ].first );
				if( std::fabs( tm - ( g.t0 + 0.5 * g.T ) ) < 0.5 * dt )
					midErr = std::fabs( 0.5 * ( run.path[ i ].second.first + run.path[ i + 1 ].second.first ) - 0.5 * ( g.ax + g.bx ) )
					       + std::fabs( 0.5 * ( run.path[ i ].second.second + run.path[ i + 1 ].second.second ) - 0.5 * ( g.ay + g.by ) );
			}
			(void)midErr;
			worstPeak = std::max( worstPeak, std::fabs( peak / ( 1.875 * D / g.T ) - 1.0 ) );
			++profiled;
		}
		// The scatter, about the target, in units of the distance aimed.
		const bool clamped = g.bx <= 0.0 || g.by <= 0.0 || g.bx >= run.card.width || g.by >= run.card.height;
		if( !clamped && g.aim > 1e-6 )
		{
			const double ux = ( g.tx - g.ax ) / g.aim, uy = ( g.ty - g.ay ) / g.aim;
			const double ex = g.bx - g.ax, ey = g.by - g.ay;
			const double a  = ( ex * ux + ey * uy ) / g.aim - hand::kPrimaryGain;
			along.push_back( a );
			across.push_back( ( -ex * uy + ey * ux ) / g.aim );
			( g.aim < median ? alongShort : alongLong ).push_back( a );
		}
		// The next pan, if any, is a correction exactly when this one missed.
		const double miss = std::hypot( g.bx - g.tx, g.by - g.ty );
		size_t next       = k + 1;
		while( next < log.size() && log[ next ].kind == hand::Segment::Kind::Wait && log[ next ].tag == hand::Tag::Wait )
			++next;
		const bool corrected = next < log.size() && log[ next ].tag == hand::Tag::Correction;
		corrections += corrected;
		// Corrections already made on this target, this one included.
		int already = 0;
		for( size_t back = k + 1; back-- > 0; )
		{
			if( log[ back ].tag == hand::Tag::Wait )
				continue;
			if( log[ back ].tag != hand::Tag::Correction )
				break;
			++already;
		}
		if( next < log.size() )
		{
			++calls;
			rightCalls += corrected == ( miss > 0.5 * g.tolerance && already < hand::kMaxCorrections );
		}
	}
	auto sd = []( const std::vector< double >& v ) {
		double m = 0.0, q = 0.0;
		for( double x : v )
			m += x;
		m /= std::max< size_t >( v.size(), 1 );
		for( double x : v )
			q += ( x - m ) * ( x - m );
		return std::sqrt( q / std::max< size_t >( v.size() - 1, 1 ) );
	};
	auto mean = []( const std::vector< double >& v ) {
		double m = 0.0;
		for( double x : v )
			m += x;
		return m / std::max< size_t >( v.size(), 1 );
	};
	const double k = run.s.noiseK;
	// 4 sigma of each estimate: an SD from n normal draws has relative SE
	// 1 / sqrt( 2 n ); a mean has SE k / sqrt( n ).
	auto sdOk = [ & ]( const std::vector< double >& v, double expected ) {
		return std::fabs( sd( v ) / expected - 1.0 ) <= 4.0 / std::sqrt( 2.0 * v.size() );
	};
	Check( distances.size() >= 500 && worstTime <= 1e-12,
	       fmt( "%zu pans over 900 s: every duration a + b log2( D / W + 1 ) to %.1g s (a %.2f s, b %.3f s/bit)", distances.size(), worstTime,
	            hand::kFittsA, run.s.fittsB ) );
	// 1%: a speed from positions 1/240 s apart is the mean over that span,
	// and minimum jerk's speed is flat at its peak (0.05% low at T = 0.15 s),
	// but the span need not straddle T / 2 (a cubic ease peaks at 1.5).
	(void)worstMid;
	Check( profiled >= 300 && worstPeak <= 0.01,
	       fmt( "%d whole pans sampled every 1/240 s: peak speed within %.2f%% of minimum jerk's 1.875 D / T (bound 1%%)", profiled,
	            100.0 * worstPeak ) );
	Check( sdOk( along, k ) && sdOk( across, hand::kAcrossShare * k ) && std::fabs( mean( along ) ) <= 4.0 * k / std::sqrt( along.size() )
	           && sdOk( alongShort, k ) && sdOk( alongLong, k ),
	       fmt( "landing: along %+.4f mean (0 after the 4%% undershoot), SD %.4f D; across SD %.4f D; short pans SD %.4f D, long %.4f D "
	            "(k %.3f, 0.4 k %.3f; bounds 4 sigma)",
	            mean( along ), sd( along ), sd( across ), sd( alongShort ), sd( alongLong ), k, hand::kAcrossShare * k ) );
	Check( calls >= 500 && rightCalls == calls && corrections >= 50,
	       fmt( "%d corrections: %d of %d pans followed by one exactly when they landed outside W / 2", corrections, rightCalls, calls ) );
	return Verdict();
}

//===========================================================================
// --operator-law (no GL): what the operator never does.
//===========================================================================
int runOperatorLaw( const Perturb& perturb )
{
	std::printf( "\n=== operator-law: in every Browse mode the carriage stays on the card and the lens in its range, acts keep coming;\n"
	             "    Reading goes view by view, Searching visits every view alike; a seed browses the same way every time\n" );
	for( int mode = 0; mode < static_cast< int >( Browse::Count ); ++mode )
	{
		HandRun run( 4, 3, 0.3, 3, perturb.handHooks );
		run.s.browse = static_cast< Browse >( mode );
		run.s.dwell  = 0.2;
		run.s.readZoom = 12.0;
		const std::vector< hand::View > views = hand::MakeViews( run.card, run.s.readZoom, run.s.screenW, run.s.screenH );
		double minX = 1e9, maxX = -1e9, minY = 1e9, maxY = -1e9, minM = 1e9, maxM = -1e9;
		run.h.Advance( 0.0, run.s, run.card, run.bow, false, false );
		for( int f = 0; f < 60 * 1500; ++f )
		{
			run.h.Advance( 1.0 / 60.0, run.s, run.card, run.bow, false, false );
			const hand::State st = run.h.Now();
			minX = std::min( minX, st.x );
			maxX = std::max( maxX, st.x );
			minY = std::min( minY, st.y );
			maxY = std::max( maxY, st.y );
			minM = std::min( minM, st.logM );
			maxM = std::max( maxM, st.logM );
		}
		std::vector< int > visits( views.size(), 0 ), offsets( views.size(), 0 );
		int acts = 0, inOrder = 0, steps = 0, last = -1;
		for( const hand::Segment& g : run.h.Log() )
			if( g.tag == hand::Tag::Primary )
			{
				int view = -1;
				for( size_t v = 0; v < views.size(); ++v )
					if( std::fabs( views[ v ].x - g.tx ) < 1e-9 && std::fabs( views[ v ].y - g.ty ) < 1e-9 )
						view = static_cast< int >( v );
				if( view >= 0 )
					++visits[ static_cast< size_t >( view ) ];
				if( last >= 0 && view >= 0 )
				{
					++steps;
					const int offset = ( view - last + static_cast< int >( views.size() ) ) % static_cast< int >( views.size() );
					inOrder += offset == 1;
					++offsets[ static_cast< size_t >( offset ) ];
				}
				last = view;
				++acts;
			}
		const bool onCard = minX >= 0.0 && minY >= 0.0 && maxX <= run.card.width && maxY <= run.card.height;
		const bool inLens = minM >= std::log2( reader::kMinZoom ) - 1e-12 && maxM <= std::log2( reader::kMaxZoom ) + 1e-12;
		std::string extra;
		bool ok = onCard && inLens && acts >= 300;
		if( mode == static_cast< int >( Browse::Reading ) )
		{
			ok    = ok && inOrder == steps;
			extra = fmt( "; %d of %d steps to the next view", inOrder, steps );
		}
		if( mode == static_cast< int >( Browse::Searching ) )
		{
			// Each jump is to any other view alike: the offset from the view it
			// leaves is uniform over 1 .. V - 1. (Visits alone cannot tell: any
			// random walk round the card visits every view alike.)
			double chi = 0.0;
			const int V = static_cast< int >( views.size() );
			const double expected = static_cast< double >( steps ) / ( V - 1 );
			for( int o = 1; o < V; ++o )
				chi += ( offsets[ static_cast< size_t >( o ) ] - expected ) * ( offsets[ static_cast< size_t >( o ) ] - expected ) / expected;
			// The 0.1% point of chi-square on V - 2 degrees of freedom, by
			// Wilson and Hilferty's cube-root approximation.
			const double k     = V - 2;
			const double point = k * std::pow( 1.0 - 2.0 / ( 9.0 * k ) + 3.0902 * std::sqrt( 2.0 / ( 9.0 * k ) ), 3.0 );
			ok    = ok && offsets[ 0 ] == 0 && chi <= point;
			extra = fmt( "; never the view it is on; jump offsets chi-square %.1f on %d degrees of freedom (0.1%% point %.1f)", chi, V - 2, point );
		}
		Check( ok, fmt( "%s, 1500 s: %d acts; carriage within x %.1f..%.1f, y %.1f..%.1f of a %.0f x %.1f mm card; lens %.1fx..%.1fx%s",
		                InfoOf( PT_BROWSE ).options[ mode ], acts, minX, maxX, minY, maxY, run.card.width, run.card.height,
		                std::exp2( minM ), std::exp2( maxM ), extra.c_str() ) );
	}
	// The same seed browses the same way; another does not.
	auto signature = [ & ]( uint32_t seed ) {
		HandRun run( 14, 7, 0.3, seed, perturb.handHooks );
		run.Run( 60.0, 1.0 / 60.0 );
		std::vector< double > out;
		for( const hand::Segment& g : run.h.Log() )
			out.insert( out.end(), { g.t0, g.T, g.bx, g.by, g.b } );
		return out;
	};
	const auto a = signature( 5 ), b = signature( 5 ), c = signature( 6 );
	Check( a == b && a != c, fmt( "seed 5 twice: %s; seed 6: %s", a == b ? "identical" : "DIFFERENT", a != c ? "different" : "IDENTICAL" ) );
	return Verdict();
}

//===========================================================================
// --resize: a new raster is a fresh card.
//===========================================================================
int runResize( const Perturb& perturb )
{
	std::printf( "\n=== resize: after the host's raster changes, the reader is exactly a fresh instance's at the new raster\n" );
	const Floats a = buildCard( 1280, 720 );
	const Floats b = noiseCard( 640, 360, 5 );
	auto configure = []( Rig& rig ) {
		quiet( rig );
		rig.Set( PT_CONTENT, static_cast< float >( Content::Filmed ) );
		rig.Set( PT_INTERVAL, 0.0f );
		rig.Set( PT_COLUMNS, 4.0f );
		rig.Set( PT_ROWS, 3.0f );
		rig.Set( PT_ZOOM, 0.0f );
		rig.Set( PT_SCREEN_GRAIN, 0.5f );
		rig.Set( PT_DUST, 0.5f );
	};
	Rig resized;
	if( !resized.Init( 1280, 720, &a ) )
		return 1;
	resized.plugin.SetHooksForTest( perturb.hooks );
	resized.plugin.SetResizeKeepsStoreForTest( perturb.resizeKeeps );
	configure( resized );
	if( !resized.Render( 30 ) || !resized.Resize( 640, 360, &b ) || !resized.Render( 3 ) )
		return 1;
	Rig fresh;
	if( !fresh.Init( 640, 360, &b ) )
		return 1;
	configure( fresh );
	fresh.frame = resized.frame - 3;
	if( !fresh.Render( 3 ) )
		return 1;
	const Floats x = resized.Output(), y = fresh.Output();
	int differ     = 0;
	for( size_t i = 0; i < x.size(); ++i )
		differ += byteOf( x[ i ] ) != byteOf( y[ i ] );
	Check( differ == 0, fmt( "1280x720 for 30 frames filming a frame each, then 640x360 for 3, against a fresh 640x360 for 3: %d bytes differ",
	                         differ ) );
	return Verdict();
}

//===========================================================================
// --state: the GL state the host hands over is the state it gets back.
//===========================================================================
int runState( const Perturb& )
{
	std::printf( "\n=== state: the GL state the host hands over is the state it gets back\n" );
	Rig rig;
	if( !rig.Init( 320, 180 ) )
		return 1;
	rig.Set( PT_CONTENT, static_cast< float >( Content::Filmed ) );
	rig.Set( PT_INTERVAL, 0.0f );
	GLuint hostArray = 0, hostBuffer = 0;
	glGenVertexArrays( 1, &hostArray );
	glGenBuffers( 1, &hostBuffer );
	int problems = 0;
	std::string what;
	for( int frame = 0; frame < 3; ++frame )
	{
		glBindFramebuffer( GL_FRAMEBUFFER, rig.outputFBO );
		glViewport( 7, 5, 300, 170 );
		glBindVertexArray( hostArray );
		glBindBuffer( GL_ARRAY_BUFFER, hostBuffer );
		glEnable( GL_BLEND );
		glBlendFuncSeparate( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO );
		glClearColor( 0.2f, 0.3f, 0.4f, 0.5f );
		glEnable( GL_SCISSOR_TEST );
		glScissor( 0, 0, 320, 180 );
		glActiveTexture( GL_TEXTURE0 );
		glUseProgram( 0 );
		rig.plugin.SetTime( frame / 60.0 );
		if( rig.plugin.ProcessOpenGL( &rig.process ) != FF_SUCCESS )
			return 1;
		GLint viewport[ 4 ] = {}, array = 0, buffer = 0, program = 0, unit = 0, fbo = 0, src = 0, dst = 0;
		GLfloat clear[ 4 ]   = {};
		GLboolean mask[ 4 ]  = {};
		glGetIntegerv( GL_VIEWPORT, viewport );
		glGetIntegerv( GL_VERTEX_ARRAY_BINDING, &array );
		glGetIntegerv( GL_ARRAY_BUFFER_BINDING, &buffer );
		glGetIntegerv( GL_CURRENT_PROGRAM, &program );
		glGetIntegerv( GL_ACTIVE_TEXTURE, &unit );
		glGetIntegerv( GL_FRAMEBUFFER_BINDING, &fbo );
		glGetIntegerv( GL_BLEND_SRC_RGB, &src );
		glGetIntegerv( GL_BLEND_DST_RGB, &dst );
		glGetFloatv( GL_COLOR_CLEAR_VALUE, clear );
		glGetBooleanv( GL_COLOR_WRITEMASK, mask );
		auto expect = [ & ]( bool ok, const char* name ) {
			if( !ok )
			{
				++problems;
				what += std::string( " " ) + name;
			}
		};
		expect( viewport[ 0 ] == 7 && viewport[ 1 ] == 5 && viewport[ 2 ] == 300 && viewport[ 3 ] == 170, "viewport" );
		expect( array == static_cast< GLint >( hostArray ), "vertex-array" );
		expect( buffer == static_cast< GLint >( hostBuffer ), "array-buffer" );
		expect( program == 0, "program" );
		expect( unit == GL_TEXTURE0, "active-unit" );
		expect( fbo == static_cast< GLint >( rig.outputFBO ), "framebuffer" );
		expect( glIsEnabled( GL_BLEND ) && src == GL_SRC_ALPHA && dst == GL_ONE_MINUS_SRC_ALPHA, "blend" );
		expect( glIsEnabled( GL_SCISSOR_TEST ), "scissor" );
		expect( clear[ 0 ] == 0.2f && clear[ 1 ] == 0.3f && clear[ 2 ] == 0.4f && clear[ 3 ] == 0.5f, "clear-colour" );
		expect( mask[ 0 ] && mask[ 1 ] && mask[ 2 ] && mask[ 3 ], "colour-mask" );
		for( int u = 0; u < 10; ++u )
		{
			GLint bound = 0, boundArray = 0;
			glActiveTexture( static_cast< GLenum >( GL_TEXTURE0 + u ) );
			glGetIntegerv( GL_TEXTURE_BINDING_2D, &bound );
			glGetIntegerv( GL_TEXTURE_BINDING_2D_ARRAY, &boundArray );
			expect( bound == 0 && boundArray == 0, "texture-unit" );
		}
		glActiveTexture( GL_TEXTURE0 );
	}
	glDisable( GL_SCISSOR_TEST );
	glDisable( GL_BLEND );
	glBindVertexArray( 0 );
	glBindBuffer( GL_ARRAY_BUFFER, 0 );
	glDeleteVertexArrays( 1, &hostArray );
	glDeleteBuffers( 1, &hostBuffer );
	Check( problems == 0, fmt( "three frames with the filmed store live: viewport, vertex array, array buffer, program, active unit, "
	                           "framebuffer, blend, scissor, clear colour, colour mask, ten texture units (%d wrong:%s)",
	                           problems, what.empty() ? " none" : what.c_str() ) );
	return Verdict();
}

//===========================================================================
// --bench: GPU time per frame from GL_TIME_ELAPSED.
//===========================================================================
int runBench()
{
	struct Size
	{
		int w, h;
		const char* name;
	};
	const Size sizes[] = { { 1280, 720, "720p" }, { 1920, 1080, "1080p" }, { 3840, 2160, "4K" } };
	struct Case
	{
		const char* what;
		std::function< void( Rig& ) > set;
	};
	const Case cases[] = {
		{ "defaults (the operator browsing)", []( Rig& ) {} },
		{ "every pixel 32 taps (1 mm out, panning)", []( Rig& r ) {
			 r.Set( PT_OPERATOR, static_cast< float >( Operator::Manual ) );
			 r.Set( PT_FOCUS, ParamFromDefocus( 1.0 ) );
			 r.Set( PT_SHUTTER, 1.0f );
			 r.beforeFrame = [ &r ]( int f ) { r.Set( PT_POSITION_X, static_cast< float >( 0.3 + 0.002 * f ) ); };
		 } },
		{ "Filmed, a frame every 0.05 s", []( Rig& r ) {
			 r.Set( PT_CONTENT, static_cast< float >( Content::Filmed ) );
			 r.Set( PT_INTERVAL, 0.0f );
		 } },
	};
	std::printf( "\n=== bench: GPU time per frame (GL_TIME_ELAPSED), after 120 frames of warm-up, median and worst of 120\n" );
	GLuint query = 0;
	glGenQueries( 1, &query );
	for( const Case& c : cases )
		for( const Size& size : sizes )
		{
			Rig rig;
			if( !rig.Init( size.w, size.h ) )
				return 1;
			c.set( rig );
			if( !rig.Render( 120 ) )
				return 1;
			constexpr int kTimed = 120;
			std::vector< double > times;
			for( int f = 0; f < kTimed; ++f )
			{
				glBeginQuery( GL_TIME_ELAPSED, query );
				if( !rig.Render( 1 ) )
					return 1;
				glEndQuery( GL_TIME_ELAPSED );
				GLuint64 ns = 0;
				glGetQueryObjectui64v( query, GL_QUERY_RESULT, &ns );
				times.push_back( static_cast< double >( ns ) * 1e-6 );
			}
			std::sort( times.begin(), times.end() );
			std::printf( "  %-42s %-6s median %6.2f ms/frame, worst %6.2f  (%4.1f%% of a 60 fps frame)\n", c.what, size.name,
			             times[ kTimed / 2 ], times.back(), 100.0 * times[ kTimed / 2 ] / ( 1000.0 / 60.0 ) );
		}
	glDeleteQueries( 1, &query );
	return 0;
}

//===========================================================================
// --cues (no GL): options, events, integers step; the rest ramp.
//===========================================================================
int runCues( const Perturb& perturb )
{
	std::printf( "\n=== cues: a cue sheet steps options and integers, and ramps standard controls\n" );
	std::istringstream sheet( "0 Film 0\n60 Film 2\n0 Columns 4\n60 Columns 12\n0 Zoom 0\n60 Zoom 1\n" );
	std::string error;
	const auto tracks = loadScript( sheet, "cues", error );
	Fiche plugin;
	int bad = 0;
	std::string what;
	for( const NamedParameter& p : listParameters( plugin ) )
	{
		const auto found = tracks.find( p.name );
		if( found == tracks.end() )
			continue;
		const bool ramp = perturb.cuesRamp ? true : !stepsBetweenCues( p.type );
		const float mid = valueAt( found->second, 29, ramp );
		const float a = found->second.front().second, b = found->second.back().second;
		const bool expectRamp = p.type == FF_TYPE_STANDARD;
		const bool ok         = expectRamp ? ( mid > a && mid < b ) : ( mid == a && valueAt( found->second, 60, ramp ) == b );
		bad += !ok;
		what += fmt( " %s %s(%g at frame 29)", p.name.c_str(), expectRamp ? "ramps " : "steps ", mid );
	}
	Check( error.empty() && bad == 0 && tracks.size() == 3, fmt( "%d wrong:%s", bad, what.c_str() ) );
	return Verdict();
}

//===========================================================================
// --names (no GL)
//===========================================================================
int runNames( const Perturb& )
{
	std::printf( "\n=== names: every parameter unique (as Arena addresses them, too) and within FFGL's 16 characters\n" );
	Fiche plugin;
	std::map< std::string, int > seen, address;
	int longNames = 0, dupes = 0, clashes = 0;
	for( unsigned int i = 0; i < plugin.GetNumParams(); ++i )
	{
		const std::string name = plugin.GetParamName( i ) ? plugin.GetParamName( i ) : "";
		if( name.size() > 16 )
		{
			std::printf( "    too long: %s\n", name.c_str() );
			++longNames;
		}
		if( seen[ name ]++ > 0 )
			++dupes;
		std::string key;
		for( char c : name )
			if( c != ' ' )
				key += static_cast< char >( std::tolower( static_cast< unsigned char >( c ) ) );
		if( address[ key ]++ > 0 )
			++clashes;
	}
	const unsigned int aboutFirst = plugin.GetNumParams() - stoatworks::about::kParamCount;
	const bool aboutLast          = std::string( plugin.GetParamName( aboutFirst ) ) == "About";
	Check( longNames == 0 && dupes == 0 && clashes == 0 && aboutLast,
	       fmt( "SW Fiche: %u parameters, %d too long, %d duplicated, %d clashing addresses; the About block is last (%s)",
	            plugin.GetNumParams(), longNames, dupes, clashes, aboutLast ? "yes" : "NO" ) );
	return Verdict();
}

struct CheckEntry
{
	const char* flag;
	CheckFn run;
	bool offline;///< needs no GL context
};

const std::vector< CheckEntry >& checks()
{
	static const std::vector< CheckEntry > list = {
		{ "identity", runIdentity, false }, { "mips", runMips, false },         { "dark", runDark, false },
		{ "magnify", runMagnify, false },   { "defocus", runDefocus, false },
		{ "field", runField, false },       { "track", runTrack, false },       { "carriage", runCarriage, false },
		{ "shutter", runShutter, false },   { "hunt", runHunt, false },         { "stock", runStock, false },
		{ "screen", runScreen, false },     { "filmed", runFilmed, false },     { "sync", runSync, false },
		{ "resize", runResize, false },     { "state", runState, false },       { "fitts", runFitts, true },
		{ "operator-law", runOperatorLaw, true }, { "cues", runCues, true },    { "names", runNames, true },
	};
	return list;
}

bool isOffline( const std::string& flag )
{
	for( const CheckEntry& c : checks() )
		if( flag == c.flag )
			return c.offline;
	return false;
}

//===========================================================================
// --negative
//===========================================================================
int runNegative( bool offlineOnly = false )
{
	struct Case
	{
		const char* name;
		CheckFn check;
		Perturb perturb;
		const char* what;
	};
	std::vector< Case > cases;
	auto add = [ & ]( const char* name, CheckFn fn, const char* what, std::function< void( Perturb& ) > set ) {
		Perturb p;
		set( p );
		cases.push_back( { name, fn, p, what } );
	};
	using namespace shaders;
	add( "identity", runIdentity, "the lens 2% stronger than it says", []( Perturb& p ) { p.hooks = kHookMagnify; } );
	add( "mips", runMips, "the clip's mips a plain 2 x 2 box, dropping odd rows", []( Perturb& p ) { p.hooks = kHookPlainBox; } );
	add( "dark", runDark, "box coverage as min - max of card positions", []( Perturb& p ) { p.hooks = kHookNaiveCover; } );
	add( "magnify", runMagnify, "the lens 2% stronger than it says", []( Perturb& p ) { p.hooks = kHookMagnify; } );
	add( "defocus", runDefocus, "the blur circle without the ( M + 1 )", []( Perturb& p ) { p.hooks = kHookNoPlusOne; } );
	add( "defocus", runDefocus, "aperture taps at radius ( j + 0.5 ) / N, a cone", []( Perturb& p ) { p.hooks = kHookLinearDisc; } );
	add( "field", runField, "the bow read with the wrong sign", []( Perturb& p ) { p.hooks = kHookBowFlip; } );
	add( "track", runTrack, "the picture shown the hand, not the carriage", []( Perturb& p ) { p.showHand = true; } );
	add( "carriage", runCarriage, "the grip integrated by forward Euler", []( Perturb& p ) { p.handHooks = hand::kHookEulerGrip; } );
	add( "shutter", runShutter, "an exposure twice as long as Shutter says", []( Perturb& p ) { p.shutter = 2.0; } );
	add( "hunt", runHunt, "the hunt turns at the band's edge, with no reaction time", []( Perturb& p ) { p.handHooks = hand::kHookNoReaction; } );
	add( "stock", runStock, "a negative stock printed positive", []( Perturb& p ) { p.hooks = kHookNoNegative; } );
	add( "screen", runScreen, "cos^3 instead of cos^4", []( Perturb& p ) { p.hooks = kHookCubeFalloff; } );
	add( "screen", runScreen, "the screen's grain riding on the card", []( Perturb& p ) { p.hooks = kHookGrainOnCard; } );
	add( "screen", runScreen, "dust that stays on the screen", []( Perturb& p ) { p.hooks = kHookDustOnScreen; } );
	add( "filmed", runFilmed, "the camera fills columns first", []( Perturb& p ) { p.hooks = kHookColumnOrder; } );
	add( "sync", runSync, "a beat starts an act at the start of its frame", []( Perturb& p ) { p.cueEarly = true; } );
	add( "resize", runResize, "the store's exposures survive a reallocation", []( Perturb& p ) { p.resizeKeeps = true; } );
	add( "fitts", runFitts, "ID = log2( D / W ), without Shannon's + 1", []( Perturb& p ) { p.handHooks = hand::kHookFittsNoOne; } );
	add( "fitts", runFitts, "a cubic ease instead of minimum jerk", []( Perturb& p ) { p.handHooks = hand::kHookCubicProfile; } );
	add( "fitts", runFitts, "endpoint scatter that ignores the distance", []( Perturb& p ) { p.handHooks = hand::kHookFlatScatter; } );
	add( "operator-law", runOperatorLaw, "Searching only ever looks in the next half of the card", []( Perturb& p ) { p.handHooks = hand::kHookBiasedSearch; } );
	add( "cues", runCues, "ramp every control between keys", []( Perturb& p ) { p.cuesRamp = true; } );

	if( offlineOnly )
		cases.erase( std::remove_if( cases.begin(), cases.end(), []( const Case& c ) { return !isOffline( c.name ); } ), cases.end() );

	int unfalsifiable = 0;
	for( const Case& c : cases )
	{
		std::printf( "\n=== negative control: %s -- %s\n", c.name, c.what );
		const int before = g_failures;
		g_failures       = 0;
		c.check( c.perturb );
		const int observed = g_failures;
		g_failures         = before;
		if( observed > 0 )
			std::printf( "  ok    %s failed %d check%s, as it must\n", c.name, observed, observed == 1 ? "" : "s" );
		else
		{
			std::printf( "  FAIL  %s PASSED against a wrong model -- it cannot fail, so it is not a check\n", c.name );
			++unfalsifiable;
		}
	}
	std::printf( "\nnegative controls: %zu wrong models, %d of them undetected\n", cases.size(), unfalsifiable );
	std::printf( "\n  %s\n", unfalsifiable == 0 ? "PASS" : "FAIL" );
	return unfalsifiable == 0 ? 0 : 1;
}
} // namespace

//---------------------------------------------------------------------------
int main( int argc, char** argv )
{
	std::string outPath = "/tmp/fiche.png";
	std::vector< std::string > settings;
	int width = 1280, height = 720, frames = 60;
	double fps = 60.0;
	std::string mode, scriptPath, clipPath;
	int filmFrames = -1;
	bool sizeGiven = false, moving = false;

	for( int i = 1; i < argc; ++i )
	{
		const std::string argument = argv[ i ];
		const bool hasNext         = i + 1 < argc;
		if( argument == "--help" || argument == "-h" )
		{
			std::printf( "mftest -- render Fiche offline and measure its reader and its hand\n\n"
			             "  --out PATH        render the card and write a PNG (default /tmp/fiche.png)\n"
			             "  --clip FILE       (with --out) a raw RGBA frame of --size to use instead of the card\n"
			             "  --size WxH        render size (default 1280x720)\n"
			             "  --frames N        frames before reading back (default 60)\n"
			             "  --moving          (with --out / --film) the picture pans 3 pixels a frame\n"
			             "  --fps N           the synthetic clock's rate (default 60)\n"
			             "  --set \"Name=V\"    set a parameter by its display name (an option by its name). Repeatable.\n"
			             "  --list            every parameter and its default\n"
			             "  --pipe            raw RGBA frames in on stdin, out on stdout\n"
			             "  --film N          N frames of the card, raw RGBA on stdout\n"
			             "  --script PATH     cues for --pipe/--film: 'frame Name value'\n"
			             "  --expect          a draft of the fleet Arena gate's expectation\n\n"
			             "  --trace           (with --frames) the hand's segments and state, frame by frame\n\n"
			             "  checks: --identity --mips --dark --magnify --defocus --field --track --carriage --shutter --hunt --stock --screen\n"
			             "          --filmed --sync --resize --state\n"
			             "          no GL: --fitts --operator-law --cues --names\n"
			             "          --negative   --bench\n"
			             "  --offline         the checks and negative controls that need no GL context (CI)\n" );
			return 0;
		}
		else if( argument == "--out" && hasNext )
			outPath = argv[ ++i ];
		else if( argument == "--set" && hasNext )
			settings.push_back( argv[ ++i ] );
		else if( argument == "--frames" && hasNext )
			frames = std::atoi( argv[ ++i ] );
		else if( argument == "--fps" && hasNext )
			fps = std::strtod( argv[ ++i ], nullptr );
		else if( argument == "--clip" && hasNext )
			clipPath = argv[ ++i ];
		else if( argument == "--moving" )
			moving = true;
		else if( argument == "--pipe" )
			mode = "pipe";
		else if( argument == "--film" && hasNext )
		{
			mode       = "film";
			filmFrames = std::max( 1, std::atoi( argv[ ++i ] ) );
		}
		else if( argument == "--script" && hasNext )
			scriptPath = argv[ ++i ];
		else if( argument == "--list" )
			mode = "list";
		else if( argument == "--size" && hasNext )
		{
			const std::string value = argv[ ++i ];
			const size_t cross      = value.find( 'x' );
			if( cross != std::string::npos )
			{
				width  = std::atoi( value.substr( 0, cross ).c_str() );
				height = std::atoi( value.substr( cross + 1 ).c_str() );
			}
			sizeGiven = true;
		}
		else if( argument.rfind( "--", 0 ) == 0 )
			mode = argument.substr( 2 );
		else
		{
			std::fprintf( stderr, "unknown argument '%s' (try --help)\n", argument.c_str() );
			return 2;
		}
	}
	if( width <= 0 || height <= 0 || !( fps > 0.0 ) )
	{
		std::fprintf( stderr, "--size and --fps must be positive\n" );
		return 2;
	}
	//A check given --size runs at that raster alone (the software pass).
	if( sizeGiven )
		kRasters = { { width, height } };

	if( mode == "expect" )
		return runExpect();
	if( mode == "list" )
	{
		Fiche plugin;
		std::printf( "%-3s %-18s %-9s %s\n", "id", "name", "kind", "default" );
		for( const NamedParameter& parameter : listParameters( plugin ) )
			std::printf( "%-3u %-18s %-9s %.4f\n", parameter.index, parameter.name.c_str(), kindName( parameter.type ), parameter.value );
		return 0;
	}

	//A reader that hangs up must end --pipe/--film with exit 1 and a message,
	//not SIGPIPE's silent 141: ignored here, the write fails with EPIPE.
	std::signal( SIGPIPE, SIG_IGN );

	if( mode == "offline" )
	{
		int failed = 0;
		for( const CheckEntry& check : checks() )
			if( check.offline )
				failed |= check.run( Perturb {} );
		failed |= runNegative( true );
		std::printf( "\n  offline: the checks that need no GL context. The picture checks were NOT run --\n"
		             "  tools/verify.sh runs them against a real driver, at 320x180 and above, and again on the software renderer.\n"
		             "\n  %s\n",
		             failed == 0 ? "PASS" : "FAIL" );
		return failed == 0 ? 0 : 1;
	}
	for( const CheckEntry& check : checks() )
		if( mode == check.flag && check.offline )
			return check.run( Perturb {} );

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "could not create an OpenGL 4.1 core context\n" );
		return 1;
	}

	int result = 0;
	bool ran   = false;
	for( const CheckEntry& check : checks() )
		if( mode == check.flag )
		{
			result = check.run( Perturb {} );
			ran    = true;
		}

	if( ran )
		;
	else if( mode == "pipe" )
		result = runPipe( width, height, fps, scriptPath, 0, true, settings, false );
	else if( mode == "film" )
		result = runPipe( width, height, fps, scriptPath, filmFrames, false, settings, moving );
	else if( mode == "negative" )
		result = runNegative();
	else if( mode == "bench" )
		result = runBench();
	else if( mode == "trace" )
	{
		// The hand, frame by frame: what the reader is doing, and what the
		// hand is doing it with.
		Rig rig;
		rig.fps = fps;
		const Floats card = buildCard( width, height );
		if( !rig.Init( width, height, &card ) )
			return 1;
		for( const std::string& setting : settings )
		{
			std::string error;
			if( !applySetting( rig.plugin, setting, error ) )
			{
				std::fprintf( stderr, "--set %s: %s\n", setting.c_str(), error.c_str() );
				return 2;
			}
		}
		rig.plugin.SetHandLoggingForTest( true );
		size_t logged = 0;
		static const char* const kTags[] = { "dwell", "zoom-read", "zoom-out", "primary", "wait", "correction", "zoom-in", "hunt" };
		for( int f = 0; f < std::max( frames, 1 ); ++f )
		{
			if( !rig.Render( 1 ) )
				return 1;
			const hand::Hand& h  = rig.plugin.HandForTest();
			const hand::State s  = h.Now();
			const auto& segments = h.Log();
			for( ; logged < segments.size(); ++logged )
			{
				const hand::Segment& g = segments[ logged ];
				std::printf( "        segment %-10s t0 %.3f T %.3f\n", kTags[ static_cast< int >( g.tag ) ], g.t0, g.T );
			}
			std::printf( "%4d t %.3f carriage %8.3f %8.3f  M %6.2f  knob %+.4f  hand %8.3f %8.3f\n", f, h.Time(), s.x, s.y,
			             std::exp2( s.logM ), s.z, h.HandX(), h.HandY() );
		}
	}
	else if( !mode.empty() )
	{
		std::fprintf( stderr, "unknown mode --%s (try --help)\n", mode.c_str() );
		result = 2;
	}
	else
	{
		Rig rig;
		rig.fps = fps;
		Floats card;
		if( !clipPath.empty() )
		{
			std::ifstream file( clipPath, std::ios::binary );
			Bytes raw( static_cast< size_t >( width ) * height * 4 );
			if( !file.read( reinterpret_cast< char* >( raw.data() ), static_cast< std::streamsize >( raw.size() ) ) )
			{
				std::fprintf( stderr, "--clip %s: not %dx%d RGBA\n", clipPath.c_str(), width, height );
				return 2;
			}
			card.resize( raw.size() );
			for( int y = 0; y < height; ++y )
				for( int x = 0; x < width * 4; ++x )
					card[ static_cast< size_t >( height - 1 - y ) * width * 4 + x ] = raw[ static_cast< size_t >( y ) * width * 4 + x ] / 255.0f;
		}
		else
			card = buildCard( width, height );
		if( !rig.Init( width, height, &card ) )
			result = 1;
		else
		{
			for( const std::string& setting : settings )
			{
				std::string error;
				if( !applySetting( rig.plugin, setting, error ) )
				{
					std::fprintf( stderr, "--set %s: %s\n", setting.c_str(), error.c_str() );
					return 2;
				}
			}
			bool rendered = true;
			for( int f = 0; f < std::max( frames, 1 ) && rendered; ++f )
			{
				if( moving )
					rig.Upload( pannedCard( card, width, height, 3 * f ) );
				rendered = rig.Render( 1 );
			}
			if( !rendered )
				result = 1;
			else if( writePng( outPath, width, height, rig.Output() ) )
				std::printf( "wrote %s -- %dx%d, %d frames at %g fps (%.2f s)\n", outPath.c_str(), width, height, frames, fps, frames / fps );
			else
				result = 1;
		}
	}

	CGLSetCurrentContext( nullptr );
	CGLDestroyContext( context );
	return result;
}
