#include "Fiche.h"

#include "Diag.h"
#include "Font.h"
#include "GLState.h"

#include <ffglex/FFGLScopedFBOBinding.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>

using namespace ffglex;
using namespace fiche;

static CFFGLPluginInfo PluginInfo(
	PluginFactory< Fiche >,// Create method
	"MF01",                // Plugin unique ID of maximum length 4.
	"SW Fiche",            // Plugin name
	2,                     // API major version number
	1,                     // API minor version number
	0,                     // Plugin major version number
	1,                     // Plugin minor version number
	FF_EFFECT,             // Plugin type
	"Browsing a microfiche reader.\n\nThe clip is printed in a grid of frames on a card, and an operator browses it through a reader's lens: a hand that obeys Fitts's law on a carriage with some give in it, a zoom that does not hold focus, a card that is not flat, and a focus knob turned by someone with a reaction time.\n\nWhat falls out: whip pans, overshoot and correction, crash zooms that throw the picture out of focus, the focus hunt, soft corners, and a motion and zoom blur that are one exposure of the moving reader.",
	"Fiche FFGL effect" // About
);

namespace
{
/// Frames that must agree before the host's clock unit is settled.
constexpr int kClockVotes = 4;

/// Seconds of host time a single frame is allowed to advance the clock by. A
/// stalled host must not dump a minute of browsing into one frame.
constexpr double kMaxFrameDelta = 0.25;

/// The filmed store may take this much memory, mips included.
constexpr double kStoreBudgetBytes = 256.0 * 1024.0 * 1024.0;
/// The most frames a camera exposes in one host frame.
constexpr int kMaxExposuresPerFrame = 8;

/// Wall clock, for hosts that never call SetTime.
double wallSeconds()
{
	using namespace std::chrono;
	static const steady_clock::time_point start = steady_clock::now();
	return duration_cast< duration< double > >( steady_clock::now() - start ).count();
}

/// glGetString returns nullptr when there is no current context.
std::string glStringOrUnknown( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "unknown";
}

void setUint( FFGLShader& shader, const char* name, uint32_t value )
{
	glUniform1ui( shader.FindUniform( name ), value );
}
void setInt2( FFGLShader& shader, const char* name, int x, int y )
{
	glUniform2i( shader.FindUniform( name ), x, y );
}
void setVec2( FFGLShader& shader, const char* name, double x, double y )
{
	glUniform2f( shader.FindUniform( name ), static_cast< float >( x ), static_cast< float >( y ) );
}
void setVec3( FFGLShader& shader, const char* name, const double* v )
{
	glUniform3f( shader.FindUniform( name ), static_cast< float >( v[ 0 ] ), static_cast< float >( v[ 1 ] ), static_cast< float >( v[ 2 ] ) );
}

int levelsFor( int width, int height )
{
	return static_cast< int >( std::floor( std::log2( static_cast< double >( std::max( width, height ) ) ) ) );
}

/// A probability as the integer threshold a 32-bit hash is compared with.
uint32_t thresholdU32( double probability )
{
	if( !( probability > 0.0 ) )
		return 0u;
	const double scaled = std::round( probability * 4294967296.0 );
	return scaled >= 4294967295.0 ? 0xFFFFFFFFu : static_cast< uint32_t >( scaled );
}
} // namespace

//---------------------------------------------------------------------------
Fiche::Fiche() : title( kDefaultTitle )
{
	SetMinInputs( 1 );
	SetMaxInputs( 1 );

	//The hand, the camera and the exposure all run on the host's clock, so a
	//re-render of a composition browses the same way.
	SetTimeSupported( true );

	//---------------------------------------------------------------------
	// Declaration, from the table in Controls.cpp. Every ranged
	// FF_TYPE_STANDARD parameter is a plain 0..1 float: SetParamInfo clamps a
	// STANDARD default into 0..1 before a range could be attached (SDK
	// b1afaf9). FF_TYPE_INTEGER is exempt, so Columns, Rows and the seed are
	// declared with their real ranges.
	//---------------------------------------------------------------------
	for( unsigned int id = 0; id < PT_ABOUT_FIRST; ++id )
	{
		const fiche::ParamInfo& info = InfoOf( id );
		params[ id ]                 = info.defaultValue;
		switch( info.kind )
		{
		case Kind::Standard: SetParamInfo( id, info.name, FF_TYPE_STANDARD, info.defaultValue ); break;
		case Kind::Integer:
			SetParamInfo( id, info.name, FF_TYPE_INTEGER, info.defaultValue );
			SetParamRange( id, info.minimum, info.maximum );
			break;
		case Kind::Option:
			SetOptionParamInfo( id, info.name, static_cast< unsigned int >( info.optionCount ), info.defaultValue );
			for( int i = 0; i < info.optionCount; ++i )
				SetParamElementInfo( id, static_cast< unsigned int >( i ), info.options[ i ], static_cast< float >( i ) );
			break;
		case Kind::Event: SetParamInfo( id, info.name, FF_TYPE_EVENT, false ); break;
		case Kind::Text: SetParamInfo( id, info.name, FF_TYPE_TEXT, kDefaultTitle ); break;
		}
		SetParamGroup( id, info.group );
	}

	// The About block. Inline rather than through a helper: SetParamInfo is
	// protected on CFFGLPlugin, so nothing outside the class can call it.
	SetParamInfo( PT_ABOUT_FIRST, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = PT_ABOUT_FIRST + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}
	for( FFUInt32 i = PT_ABOUT_FIRST; i < PT_COUNT; ++i )
		SetParamGroup( i, "About" );

	// The aperture's taps: golden-angle directions, the Vogel spiral's. Here
	// rather than in the shader, which has no trig (verify.sh greps).
	const double golden = 3.14159265358979323846 * ( 3.0 - std::sqrt( 5.0 ) );
	for( int j = 0; j < shaders::kMaxTaps; ++j )
	{
		disc[ 2 * j ]     = static_cast< float >( std::cos( golden * j ) );
		disc[ 2 * j + 1 ] = static_cast< float >( std::sin( golden * j ) );
	}

	FFGLLog::LogToHost( "Created Fiche effect" );

	diag::init();
}

//---------------------------------------------------------------------------
FFResult Fiche::InitGL( const FFGLViewportStruct* vp )
{
	diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR ) + " renderer=" + glStringOrUnknown( GL_RENDERER )
	            + " version=" + glStringOrUnknown( GL_VERSION ) );

	const std::string vertex = std::string( shaders::kVersion ) + shaders::kQuadVertex;
	struct
	{
		FFGLShader* shader;
		shaders::Pass pass;
		const char* name;
	} const stages[] = {
		{ &copyShader, shaders::Pass::Copy, "copy" },
		{ &mipShader, shaders::Pass::Mip, "mip" },
		{ &filmShader, shaders::Pass::Film, "film" },
		{ &mipLayerShader, shaders::Pass::MipLayer, "mip layer" },
		{ &screenShader, shaders::Pass::Screen, "screen" },
	};
	for( const auto& stage : stages )
	{
		const std::string fragment = shaders::Assemble( stage.pass );
		if( stage.shader->Compile( vertex.c_str(), fragment.c_str() ) )
			continue;

		//Returning FF_FAIL here is invisible to the operator: the effect
		//simply does nothing in Resolume. This line is the only record of
		//which pass it was.
		diag::error( std::string( "the " ) + stage.name + " shader failed to compile - the effect will do nothing" );
		FFGLLog::LogToHost( "Fiche: shader failed to compile" );
		DeInitGL();
		return FF_FAIL;
	}

	if( !quad.Initialise() )
	{
		diag::error( "quad geometry failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}
	glGenFramebuffers( 1, &workFBO );

	glGenTextures( 1, &emptyStore );
	glBindTexture( GL_TEXTURE_2D_ARRAY, emptyStore );
	const unsigned char black[ 4 ] = { 0, 0, 0, 255 };
	glTexImage3D( GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, 1, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, black );
	glTexParameteri( GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glBindTexture( GL_TEXTURE_2D_ARRAY, 0 );

	// The title's glyphs, with the box mips Font.cpp built.
	const std::vector< font::Level > atlas = font::BuildAtlas();
	glGenTextures( 1, &fontTexture );
	glBindTexture( GL_TEXTURE_2D, fontTexture );
	glPixelStorei( GL_UNPACK_ALIGNMENT, 1 );
	for( size_t level = 0; level < atlas.size(); ++level )
		glTexImage2D( GL_TEXTURE_2D, static_cast< GLint >( level ), GL_R8, atlas[ level ].width, atlas[ level ].height, 0, GL_RED,
		              GL_UNSIGNED_BYTE, atlas[ level ].texels.data() );
	glPixelStorei( GL_UNPACK_ALIGNMENT, 4 );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0 );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, static_cast< GLint >( atlas.size() ) - 1 );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	fontW = atlas.front().width;
	fontH = atlas.front().height;

	glGenTextures( 1, &bowTexture );
	bowBuilt = false;

	diag::info( "initialised" );
	return CFFGLPlugin::InitGL( vp );
}

//---------------------------------------------------------------------------
FFResult Fiche::SetTime( double time )
{
	hostTimeSeen = true;
	return CFFGLPlugin::SetTime( time );
}

//The unit voting is readout's, by way of pitch, unchanged: the ratio of the
//host's clock delta to a steady clock's names the unit outright, and nothing
//plausible sits between 1 and 1000.
double Fiche::nowSeconds()
{
	const double wallNow = wallSeconds();
	if( wallStart < 0.0 )
		wallStart = wallNow;

	if( !hostTimeSeen || hostTime < 0.0 )
		return wallNow - wallStart;

	const double raw = hostTime;

	if( clockScale == 0.0 && lastRawTime >= 0.0 && lastWallTime >= 0.0 )
	{
		const double hostDelta = raw - lastRawTime;
		const double wallDelta = wallNow - lastWallTime;
		if( hostDelta > 0.0 && wallDelta >= 0.0005 )
		{
			const double ratio = hostDelta / wallDelta;
			if( ratio > 0.1 && ratio < 10.0 )
				++secondsVotes;
			else if( ratio > 100.0 && ratio < 10000.0 )
				++millisVotes;

			if( secondsVotes >= kClockVotes || millisVotes >= kClockVotes )
				clockScale = millisVotes > secondsVotes ? 0.001 : 1.0;
		}
	}
	lastRawTime  = raw;
	lastWallTime = wallNow;

	return clockScale != 0.0 ? raw * clockScale : wallNow - wallStart;
}

//---------------------------------------------------------------------------
// Textures. Every allocation happens before anything is bound for a pass:
// allocating leaves the active unit bound to nothing (tinsel's trap).
//---------------------------------------------------------------------------
bool Fiche::ensureLive( int width, int height )
{
	if( liveTexture != 0 && liveW == width && liveH == height )
		return true;
	releaseLive();
	liveLevels = levelsFor( width, height );
	glGenTextures( 1, &liveTexture );
	glBindTexture( GL_TEXTURE_2D, liveTexture );
	for( int level = 0; level <= liveLevels; ++level )
		glTexImage2D( GL_TEXTURE_2D, level, GL_RGBA16F, std::max( 1, width >> level ), std::max( 1, height >> level ), 0, GL_RGBA,
		              GL_FLOAT, nullptr );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0 );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, liveLevels );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	if( glGetError() != GL_NO_ERROR )
	{
		releaseLive();
		return false;
	}
	liveW = width;
	liveH = height;
	return true;
}

void Fiche::releaseLive()
{
	if( liveTexture != 0 )
		glDeleteTextures( 1, &liveTexture );
	liveTexture = 0;
	liveW = liveH = liveLevels = 0;
}

bool Fiche::ensureStore( int width, int height, int frames )
{
	// Each layer as large as the budget allows, up to the clip's own size.
	const double perLayer = kStoreBudgetBytes / ( 4.0 * 4.0 / 3.0 * frames );
	const double scale    = std::min( 1.0, std::sqrt( perLayer / ( static_cast< double >( width ) * height ) ) );
	const int w           = std::max( 16, static_cast< int >( std::lround( width * scale ) ) );
	const int h           = std::max( 16, static_cast< int >( std::lround( height * scale ) ) );
	if( storeTexture != 0 && storeW == w && storeH == h && storeLayers == frames )
		return true;
	const bool keep = resizeKeepsStore && storeTexture != 0;
	const int keptExposures = exposures;
	releaseStore();
	storeLevels = levelsFor( w, h );
	glGenTextures( 1, &storeTexture );
	glBindTexture( GL_TEXTURE_2D_ARRAY, storeTexture );
	for( int level = 0; level <= storeLevels; ++level )
		glTexImage3D( GL_TEXTURE_2D_ARRAY, level, GL_RGBA8, std::max( 1, w >> level ), std::max( 1, h >> level ), frames, 0, GL_RGBA,
		              GL_UNSIGNED_BYTE, nullptr );
	glTexParameteri( GL_TEXTURE_2D_ARRAY, GL_TEXTURE_BASE_LEVEL, 0 );
	glTexParameteri( GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL, storeLevels );
	glTexParameteri( GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR );
	glTexParameteri( GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D_ARRAY, 0 );
	if( glGetError() != GL_NO_ERROR )
	{
		releaseStore();
		return false;
	}
	storeW      = w;
	storeH      = h;
	storeLayers = frames;
	// A new card is unexposed film: what was filmed at another size or on
	// another grid means nothing here.
	exposures = keep ? keptExposures : 0;
	written   = std::min( exposures, frames );
	filmClock = 0.0;
	return true;
}

void Fiche::releaseStore()
{
	if( storeTexture != 0 )
		glDeleteTextures( 1, &storeTexture );
	storeTexture = 0;
	storeW = storeH = storeLayers = storeLevels = 0;
	exposures = written = 0;
	filmClock           = 0.0;
}

void Fiche::uploadBow()
{
	glBindTexture( GL_TEXTURE_2D, bowTexture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_R32F, bow.SampleWidth(), bow.SampleHeight(), 0, GL_RED, GL_FLOAT, bow.Samples().data() );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0 );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0 );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
}

/// The camera exposes one frame: the live clip into `layer`, then its mips.
void Fiche::exposeFrame( int layer )
{
	glBindFramebuffer( GL_FRAMEBUFFER, workFBO );
	glFramebufferTextureLayer( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, storeTexture, 0, layer );
	glViewport( 0, 0, storeW, storeH );
	{
		ScopedShaderBinding shader( filmShader.GetGLID() );
		bindUnit( 0, liveTexture );
		filmShader.Set( "LiveTex", 0 );
		setVec2( filmShader, "LiveSize", liveW, liveH );
		setVec2( filmShader, "StoreSize", storeW, storeH );
		filmShader.Set( "LiveLevels", static_cast< float >( liveLevels ) );
		quad.Draw();
		unbindTextureUnits( 1 );
	}
	ScopedShaderBinding shader( mipLayerShader.GetGLID() );
	glActiveTexture( GL_TEXTURE0 );
	glBindTexture( GL_TEXTURE_2D_ARRAY, storeTexture );
	mipLayerShader.Set( "Source", 0 );
	mipLayerShader.Set( "Layer", layer );
	for( int level = 1; level <= storeLevels; ++level )
	{
		glTexParameteri( GL_TEXTURE_2D_ARRAY, GL_TEXTURE_BASE_LEVEL, level - 1 );
		glTexParameteri( GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL, level - 1 );
		glFramebufferTextureLayer( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, storeTexture, level, layer );
		glViewport( 0, 0, std::max( 1, storeW >> level ), std::max( 1, storeH >> level ) );
		setInt2( mipLayerShader, "SourceSize", std::max( 1, storeW >> ( level - 1 ) ), std::max( 1, storeH >> ( level - 1 ) ) );
		setInt2( mipLayerShader, "TargetSize", std::max( 1, storeW >> level ), std::max( 1, storeH >> level ) );
		quad.Draw();
	}
	glTexParameteri( GL_TEXTURE_2D_ARRAY, GL_TEXTURE_BASE_LEVEL, 0 );
	glTexParameteri( GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL, storeLevels );
	glBindTexture( GL_TEXTURE_2D_ARRAY, 0 );
	glFramebufferTextureLayer( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, 0, 0, 0 );
}

//---------------------------------------------------------------------------
FFResult Fiche::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	if( pGL->numInputTextures < 1 || pGL->inputTextures[ 0 ] == nullptr )
		return FF_FAIL;

	const FFGLTextureStruct& input = *pGL->inputTextures[ 0 ];
	if( input.Width == 0 || input.Height == 0 )
		return FF_FAIL;

	const int width  = static_cast< int >( input.Width );
	const int height = static_cast< int >( input.Height );

	//The host's state, read before anything of ours changes it, and put back
	//whichever way out of here it is.
	ScopedGLState scopedState;
	const GLint* hostViewport = scopedState.saved.viewport;

	//---------------------------------------------------------------------
	// The clock. dt is bounded so a stall does not dump seconds of browsing
	// into one frame.
	//---------------------------------------------------------------------
	const double now = nowSeconds();
	const double dt  = lastNow >= 0.0 && now > lastNow ? std::min( now - lastNow, kMaxFrameDelta ) : 0.0;
	lastNow          = now;
	if( ++clockFrames == 60 )
		diag::info( "host clock at frame 60: raw=" + std::to_string( hostTime ) + " scale=" + std::to_string( clockScale )
		            + " seconds=" + std::to_string( now ) );

	//---------------------------------------------------------------------
	// The card, and the bow it has between its plates.
	//---------------------------------------------------------------------
	const int cols     = IntegerOf( PT_COLUMNS, params[ PT_COLUMNS ] );
	const int rows     = IntegerOf( PT_ROWS, params[ PT_ROWS ] );
	const uint32_t seedValue = static_cast< uint32_t >( IntegerOf( PT_SEED, params[ PT_SEED ] ) );
	card               = reader::MakeCard( cols, rows, GutterFromParam( params[ PT_GUTTER ] ), static_cast< double >( width ) / height );
	const double flatness = FlatnessFromParam( params[ PT_FLATNESS ] );
	if( !bowBuilt || card != bowCard || flatness != bowAmplitude || seedValue != bowSeed )
	{
		bow.Build( card, flatness, seedValue );
		uploadBow();
		bowBuilt     = true;
		bowCard      = card;
		bowAmplitude = flatness;
		bowSeed      = seedValue;
	}

	//---------------------------------------------------------------------
	// Buffers.
	//---------------------------------------------------------------------
	if( !ensureLive( width, height ) )
	{
		diag::error( "could not allocate the clip's buffer: " + std::to_string( width ) + "x" + std::to_string( height ) );
		return FF_FAIL;
	}
	const bool filmed = OptionIndex( params[ PT_CONTENT ], static_cast< int >( Content::Count ) ) == static_cast< int >( Content::Filmed );
	if( filmed )
	{
		if( !ensureStore( width, height, card.Frames() ) )
		{
			diag::warn( "could not allocate the filmed store for " + std::to_string( card.Frames() ) + " frames - showing the clip live" );
			releaseStore();
		}
	}
	else if( storeTexture != 0 )
		releaseStore();
	const bool storeLive = filmed && storeTexture != 0;

	//---------------------------------------------------------------------
	// The hand.
	//---------------------------------------------------------------------
	settings.manual    = OptionIndex( params[ PT_OPERATOR ], static_cast< int >( Operator::Count ) ) == static_cast< int >( Operator::Manual );
	settings.browse    = static_cast< Browse >( OptionIndex( params[ PT_BROWSE ], static_cast< int >( Browse::Count ) ) );
	settings.dwell     = DwellFromParam( params[ PT_DWELL ] );
	const auto sync    = static_cast< Sync >( OptionIndex( params[ PT_SYNC ], static_cast< int >( Sync::Count ) ) );
	settings.synced    = sync != Sync::Free;
	settings.fittsB    = FittsSlopeFromParam( params[ PT_HAND_SPEED ] );
	settings.noiseK    = NoiseFromParam( params[ PT_ACCURACY ] );
	settings.crashZoom = std::clamp( static_cast< double >( params[ PT_CRASH_ZOOM ] ), 0.0, 1.0 );
	settings.skill     = std::clamp( static_cast< double >( params[ PT_FOCUS_SKILL ] ), 0.0, 1.0 );
	settings.rigid     = CarriageRigid( params[ PT_CARRIAGE_PLAY ] );
	settings.gripHz    = CarriageHertzFromParam( params[ PT_CARRIAGE_PLAY ] );
	settings.gripZeta  = CarriageZetaFromParam( params[ PT_CARRIAGE_PLAY ] );
	settings.readZoom  = ZoomFromParam( params[ PT_ZOOM ] );
	settings.manualX   = std::clamp( static_cast< double >( params[ PT_POSITION_X ] ), 0.0, 1.0 ) * card.width;
	settings.manualY   = std::clamp( static_cast< double >( params[ PT_POSITION_Y ] ), 0.0, 1.0 ) * card.height;
	settings.manualDefocus = DefocusFromParam( params[ PT_FOCUS ] );
	settings.aperture  = ApertureFromParam( params[ PT_APERTURE ] );
	settings.parfocal  = ParfocalFromParam( params[ PT_PARFOCAL ] );
	settings.screenW   = reader::kScreenWidth;
	settings.screenH   = reader::kScreenWidth * height / width;
	settings.seed      = seedValue;

	// The beat. Resolume calls SetBeatInfo every frame; a host that never
	// does gets a 120 BPM clock of the plugin's own, so Sync is never dead.
	// The cue is placed where the beat fell INSIDE the frame, by the bar
	// phase interpolated between this frame and the last, so an act starts on
	// the beat and not on the frame after it.
	bool cue         = false;
	double cueOffset = 0.0;
	if( barPhase != lastBarPhase && lastBarPhase >= 0.0f )
		hostBeatSeen = true;
	lastBarPhase = barPhase;
	const double tempo    = bpm > 1.0f ? static_cast< double >( bpm ) : 120.0;
	const double phaseNow = hostBeatSeen ? std::clamp( static_cast< double >( barPhase ), 0.0, 1.0 ) : std::fmod( now * tempo / 240.0, 1.0 );
	if( sync != Sync::Free && lastPhase >= 0.0 && dt > 0.0 )
	{
		const double step = ( sync == Sync::Beat ? 1.0 : sync == Sync::TwoBeats ? 2.0 : 4.0 ) / 4.0;
		double p0 = lastPhase, p1 = phaseNow;
		if( p1 < p0 )
			p1 += 1.0;
		const double boundary = ( std::floor( p0 / step + 1e-9 ) + 1.0 ) * step;
		if( p1 > p0 && boundary <= p1 )
		{
			cue       = true;
			cueOffset = cueAtFrameStart ? 0.0 : dt * ( boundary - p0 ) / ( p1 - p0 );
		}
	}
	lastPhase = phaseNow;

	hand.Advance( dt, settings, card, bow, jumpPressed, cue, cueOffset );
	jumpPressed = false;

	constexpr int kStates = shaders::kMaxStates;
	exposure.assign( kStates, hand::State {} );
	const double window = std::clamp( static_cast< double >( params[ PT_SHUTTER ] ), 0.0, 1.0 ) * dt * shutterScale;
	for( int k = 0; k < kStates; ++k )
		exposure[ static_cast< size_t >( k ) ] = hand.At( hand.Time() - window + window * k / ( kStates - 1 ) );
	if( showHand )
		for( auto& s : exposure )
		{
			s.x = hand.HandX();
			s.y = hand.HandY();
		}

	//---------------------------------------------------------------------
	// 1 and 2. The clip in linear light, and its box mips.
	//---------------------------------------------------------------------
	glDisable( GL_BLEND );
	glBindFramebuffer( GL_FRAMEBUFFER, workFBO );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, liveTexture, 0 );
	glViewport( 0, 0, width, height );
	{
		ScopedShaderBinding shader( copyShader.GetGLID() );
		bindUnit( 0, input.Handle );
		copyShader.Set( "InputTexture", 0 );
		quad.Draw();
		unbindTextureUnits( 1 );
	}
	{
		ScopedShaderBinding shader( mipShader.GetGLID() );
		bindUnit( 0, liveTexture );
		mipShader.Set( "Source", 0 );
		mipShader.Set( "Hooks", hooks );
		for( int level = 1; level <= liveLevels; ++level )
		{
			glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, level - 1 );
			glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, level - 1 );
			glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, liveTexture, level );
			glViewport( 0, 0, std::max( 1, width >> level ), std::max( 1, height >> level ) );
			setInt2( mipShader, "SourceSize", std::max( 1, width >> ( level - 1 ) ), std::max( 1, height >> ( level - 1 ) ) );
			setInt2( mipShader, "TargetSize", std::max( 1, width >> level ), std::max( 1, height >> level ) );
			quad.Draw();
		}
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0 );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, liveLevels );
		unbindTextureUnits( 1 );
	}
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0 );

	//---------------------------------------------------------------------
	// 3 and 4. The step-and-repeat camera: a frame every Interval, in
	// reading order, wrapping round the card.
	//---------------------------------------------------------------------
	if( storeLive )
	{
		const double interval = IntervalFromParam( params[ PT_INTERVAL ] );
		int due               = 0;
		if( exposures == 0 )
		{
			// A fresh card's first frame is the clip as it arrives; the
			// camera's clock starts from it.
			due       = 1;
			filmClock = 0.0;
		}
		else
		{
			filmClock += dt;
			while( filmClock >= interval && due < kMaxExposuresPerFrame )
			{
				filmClock -= interval;
				++due;
			}
			if( filmClock >= interval )
				filmClock = std::fmod( filmClock, interval );
		}
		for( int i = 0; i < due; ++i )
		{
			exposeFrame( exposures % storeLayers );
			++exposures;
		}
		written = std::min( exposures, storeLayers );
	}

	//---------------------------------------------------------------------
	// 5. The screen.
	//---------------------------------------------------------------------
	if( lampKelvin != LampFromParam( params[ PT_LAMP ] ) )
	{
		lampKelvin = LampFromParam( params[ PT_LAMP ] );
		reader::LampColour( lampKelvin, lamp );
	}
	const reader::Stock& stock = reader::StockOf( static_cast< Film >( OptionIndex( params[ PT_FILM ], static_cast< int >( Film::Count ) ) ) );
	const std::vector< int > glyphs = font::Encode( title, shaders::kMaxTitle );

	glBindFramebuffer( GL_FRAMEBUFFER, pGL->HostFBO );
	glViewport( hostViewport[ 0 ], hostViewport[ 1 ], hostViewport[ 2 ], hostViewport[ 3 ] );
	{
		ScopedShaderBinding shader( screenShader.GetGLID() );
		FFGLShader& s = screenShader;
		bindUnit( 0, liveTexture );
		glActiveTexture( GL_TEXTURE1 );
		glBindTexture( GL_TEXTURE_2D_ARRAY, storeLive ? storeTexture : emptyStore );
		bindUnit( 2, bowTexture );
		bindUnit( 3, fontTexture );
		bindUnit( 4, input.Handle );
		glActiveTexture( GL_TEXTURE0 );
		s.Set( "LiveTex", 0 );
		s.Set( "StoreTex", 1 );
		s.Set( "BowTex", 2 );
		s.Set( "FontTex", 3 );
		s.Set( "InputTexture", 4 );

		setUint( s, "Seed", seedValue );
		s.Set( "Hooks", hooks );
		setInt2( s, "Size", width, height );
		s.Set( "PxPerMm", static_cast< float >( width / reader::kScreenWidth ) );
		setVec2( s, "LiveSize", liveW, liveH );
		s.Set( "LiveLevels", static_cast< float >( liveLevels ) );
		setVec2( s, "StoreSize", std::max( storeW, 1 ), std::max( storeH, 1 ) );
		s.Set( "StoreLevels", static_cast< float >( storeLevels ) );
		s.Set( "Filmed", storeLive ? 1 : 0 );
		s.Set( "Written", written );

		setInt2( s, "Grid", card.cols, card.rows );
		setVec2( s, "FrameSize", card.frameW, card.frameH );
		setVec2( s, "GridOrigin", card.gridX, card.gridY );
		s.Set( "Gutter", static_cast< float >( card.gutter ) );
		setVec2( s, "CardSize", card.width, card.height );
		setVec2( s, "BowSize", bow.SampleWidth(), bow.SampleHeight() );
		s.Set( "HeaderHeight", static_cast< float >( reader::kHeader ) );

		float states[ 4 * kStates ];
		for( int k = 0; k < kStates; ++k )
		{
			const hand::State& st = exposure[ static_cast< size_t >( k ) ];
			states[ 4 * k ]       = static_cast< float >( st.x );
			states[ 4 * k + 1 ]   = static_cast< float >( st.y );
			states[ 4 * k + 2 ]   = static_cast< float >( std::exp2( st.logM ) );
			states[ 4 * k + 3 ]   = static_cast< float >( st.z );
		}
		glUniform4fv( s.FindUniform( "States" ), kStates, states );
		s.Set( "StateCount", kStates );
		s.Set( "FNumber", static_cast< float >( settings.aperture ) );
		s.Set( "Parfocal", static_cast< float >( settings.parfocal ) );
		s.Set( "DesignZoom", static_cast< float >( reader::kDesignZoom ) );
		glUniform2fv( s.FindUniform( "Disc" ), shaders::kMaxTaps, disc );

		s.Set( "Negative", stock.negative ? 1 : 0 );
		s.Set( "Colour", stock.colour ? 1 : 0 );
		glUniform3fv( s.FindUniform( "Base" ), 1, stock.base );
		glUniform3fv( s.FindUniform( "Dense" ), 1, stock.dense );
		// Dust: particles per square centimetre, one at most per 0.25 mm^2 cell.
		setUint( s, "TDust", thresholdU32( DustFromParam( params[ PT_DUST ] ) / 100.0 * 0.25 ) );
		setUint( s, "TScratch", thresholdU32( ScratchFromParam( params[ PT_SCRATCHES ] ) ) );

		int titleCodes[ shaders::kMaxTitle ] = {};
		for( size_t i = 0; i < glyphs.size(); ++i )
			titleCodes[ i ] = glyphs[ i ];
		glUniform1iv( s.FindUniform( "Title" ), shaders::kMaxTitle, titleCodes );
		s.Set( "TitleLength", static_cast< int >( glyphs.size() ) );
		setVec2( s, "FontSize", fontW, fontH );

		setVec3( s, "Lamp", lamp );
		s.Set( "Throw", static_cast< float >( ThrowFromParam( params[ PT_HOTSPOT ] ) ) );
		s.Set( "Grain", static_cast< float >( GrainFromParam( params[ PT_SCREEN_GRAIN ] ) ) );
		s.Set( "Room", static_cast< float >( RoomFromParam( params[ PT_ROOM_LIGHT ] ) ) );
		s.Set( "MixAmount", std::clamp( params[ PT_MIX ], 0.0f, 1.0f ) );
		s.Set( "Prefilter", prefilter ? 1 : 0 );
		quad.Draw();

		glActiveTexture( GL_TEXTURE1 );
		glBindTexture( GL_TEXTURE_2D_ARRAY, 0 );
		unbindTextureUnits( 5 );
	}

	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Fiche::DeInitGL()
{
	for( FFGLShader* s : { &copyShader, &mipShader, &filmShader, &mipLayerShader, &screenShader } )
		s->FreeGLResources();
	quad.Release();
	releaseLive();
	releaseStore();
	if( workFBO != 0 )
		glDeleteFramebuffers( 1, &workFBO );
	workFBO = 0;
	for( GLuint* t : { &emptyStore, &bowTexture, &fontTexture } )
		if( *t != 0 )
		{
			glDeleteTextures( 1, t );
			*t = 0;
		}
	bowBuilt = false;
	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Fiche::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

	// An About button is a press, not a value to keep.
	if( index >= PT_ABOUT_FIRST )
		return stoatworks::about::handleParam( index - PT_ABOUT_FIRST, value ) ? FF_SUCCESS : FF_FAIL;

	// Jump is a press: it arrives as 1 then 0, and only the press counts.
	if( index == PT_JUMP )
	{
		const bool down = value >= 0.5f;
		if( down && !jumpHeld )
			jumpPressed = true;
		jumpHeld = down;
	}
	if( index == PT_TITLE )
		return FF_SUCCESS;

	params[ index ] = value;
	return FF_SUCCESS;
}

float Fiche::GetFloatParameter( unsigned int index )
{
	return index < PT_COUNT ? params[ index ] : 0.0f;
}

char* Fiche::GetTextParameter( unsigned int index )
{
	if( index == PT_TITLE )
		return const_cast< char* >( title.c_str() );
	if( index == PT_ABOUT_FIRST )
	{
		aboutText = stoatworks::about::textParam( 0 );
		return const_cast< char* >( aboutText.c_str() );
	}
	return CFFGLPlugin::GetTextParameter( index );
}

FFResult Fiche::SetTextParameter( unsigned int index, const char* value )
{
	if( index == PT_TITLE )
	{
		title = value ? value : "";
		return FF_SUCCESS;
	}
	// See the declaration: the base class fails, and a failed default deletes
	// the instance. The About line is display-only.
	if( index == PT_ABOUT_FIRST )
		return FF_SUCCESS;
	return CFFGLPlugin::SetTextParameter( index, value );
}

//---------------------------------------------------------------------------
void Fiche::SetClockScaleForTest( double scale )
{
	clockScale = scale;
}
void Fiche::SetHooksForTest( int glslHooks )
{
	hooks = glslHooks;
}
void Fiche::SetHandHooksForTest( int handHooks )
{
	hand.SetHooks( handHooks );
}
void Fiche::SetShowHandForTest( bool on )
{
	showHand = on;
}
void Fiche::SetShutterScaleForTest( double scale )
{
	shutterScale = scale;
}
void Fiche::SetResizeKeepsStoreForTest( bool keep )
{
	resizeKeepsStore = keep;
}
void Fiche::SetHandLoggingForTest( bool on )
{
	hand.SetLogging( on );
}
void Fiche::SetPrefilterForTest( bool on )
{
	prefilter = on;
}
void Fiche::SetCueAtFrameStartForTest( bool on )
{
	cueAtFrameStart = on;
}
