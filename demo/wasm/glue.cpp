/*
    The page's way in to the plugin: what an FFGL host does to a plugin
    instance, as C functions JavaScript can call.

    Nothing here is the plugin, and nothing here does the plugin's work. The
    plugin is `source/Fiche.cpp` and everything it calls (Hand, Reader, Font,
    Controls, Shaders, Diag), compiled UNMODIFIED into the same WebAssembly
    module as this file, with the FFGL SDK pieces it needs (see
    demo/tools/build-wasm.sh for the list). This file only plays the host:

      - it constructs a `Fiche`, as the SDK's plugMain would through the
        CFFGLPluginInfo that Fiche.cpp registers, and calls InitGL / DeInitGL;
      - it reads the parameter declarations back through the FFGL SDK's own
        host-facing getters (GetParamName, GetParamType, GetParamGroup,
        GetParamDefault, the element and range getters -- the SDK compiled
        unmodified too), so the page's panel is built from the plugin's
        constructor rather than from a copy of it;
      - it forwards the host's calls: SetFloatParameter, SetTextParameter,
        SetTime and ProcessOpenGL with one input texture, as tools/mftest's Rig
        does. It never calls SetBeatInfo, because the page has no beat.

    The clock is the page's: SetTime is handed the kit's seconds, and the
    plugin's own vote decides the unit (seconds, within four frames, as in
    Arena). Before the vote settles the plugin runs on its wall clock, which
    here is the browser's performance.now().

    Three read-outs use the plugin's harness accessors (ExposureForTest,
    CardForTest) or its own tables (Controls.cpp, the About block's buttons);
    they report and never change anything.

    GL calls go to the page's WebGL2 context through emscripten's GL library;
    demo/wasm/gl_shim.cpp replaces the entry points that cannot be passed
    straight through.
*/
#include "Controls.h"
#include "Diag.h"
#include "Fiche.h"

#include <emscripten/emscripten.h>

#include <sys/stat.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>

using namespace fiche;

namespace
{
/// One plugin instance and the host-side structs it is handed.
struct Instance
{
	Fiche plugin;
	bool initialised = false;

	FFGLTextureStruct input {};
	FFGLTextureStruct* inputs[ 1 ] = { &input };
	ProcessOpenGLStruct process {};

	std::string scratch;///< a string handed back to JS lives here until the next call
};

float asFloat( FFMixed mixed )
{
	float value = 0.0f;
	static_assert( sizeof( value ) == sizeof( mixed.UIntValue ), "FFMixed carries a float in its bits" );
	std::memcpy( &value, &mixed.UIntValue, sizeof( value ) );
	return value;
}

const char* hold( Instance* instance, std::string text )
{
	instance->scratch = std::move( text );
	return instance->scratch.c_str();
}

constexpr double kNaN = std::numeric_limits< double >::quiet_NaN();
} // namespace

extern "C"
{
//---------------------------------------------------------------------------
// The plugin's log. Diag.cpp opens it in the CONSTRUCTOR (diag::init), so this
// must run before the first fiche_new: a directory in the page's in-memory file
// system, named through the override Diag.cpp reads. Its own `mkdir -p` goes
// through system(), which a browser does not have.
//---------------------------------------------------------------------------
EMSCRIPTEN_KEEPALIVE void fiche_prepare_log()
{
	setenv( "FICHE_LOG_DIR", "/fiche/logs", 1 );
	mkdir( "/fiche", 0755 );
	mkdir( "/fiche/logs", 0755 );
}

EMSCRIPTEN_KEEPALIVE const char* fiche_log_path()
{
	static std::string path;
	path = diag::logPath();
	return path.c_str();
}

//---------------------------------------------------------------------------
// Lifetime.
//---------------------------------------------------------------------------
EMSCRIPTEN_KEEPALIVE Instance* fiche_new()
{
	return new Instance();
}

EMSCRIPTEN_KEEPALIVE void fiche_delete( Instance* instance )
{
	if( instance == nullptr )
		return;
	if( instance->initialised )
		instance->plugin.DeInitGL();
	delete instance;
}

/// InitGL, with the viewport a host would pass. 1 on success.
EMSCRIPTEN_KEEPALIVE int fiche_init_gl( Instance* instance, int width, int height )
{
	FFGLViewportStruct viewport {};
	viewport.width        = static_cast< GLuint >( width );
	viewport.height       = static_cast< GLuint >( height );
	instance->initialised = instance->plugin.InitGL( &viewport ) == FF_SUCCESS;
	return instance->initialised ? 1 : 0;
}

//---------------------------------------------------------------------------
// The declarations, read as a host reads them.
//---------------------------------------------------------------------------
EMSCRIPTEN_KEEPALIVE int fiche_param_count( Instance* instance )
{
	return static_cast< int >( instance->plugin.GetNumParams() );
}

EMSCRIPTEN_KEEPALIVE const char* fiche_param_name( Instance* instance, int index )
{
	const char* name = instance->plugin.GetParamName( static_cast< unsigned int >( index ) );
	return name ? name : "";
}

EMSCRIPTEN_KEEPALIVE int fiche_param_type( Instance* instance, int index )
{
	return static_cast< int >( instance->plugin.GetParamType( static_cast< unsigned int >( index ) ) );
}

EMSCRIPTEN_KEEPALIVE const char* fiche_param_group( Instance* instance, int index )
{
	return hold( instance, instance->plugin.GetParamGroup( static_cast< unsigned int >( index ) ) );
}

/// The declared default of a numeric parameter (FF_TYPE_STANDARD already
/// clamped into 0..1 by the SDK, as a host sees it).
EMSCRIPTEN_KEEPALIVE float fiche_param_default( Instance* instance, int index )
{
	return asFloat( instance->plugin.GetParamDefault( static_cast< unsigned int >( index ) ) );
}

/// The declared default of a text parameter.
EMSCRIPTEN_KEEPALIVE const char* fiche_param_default_text( Instance* instance, int index )
{
	const FFMixed mixed = instance->plugin.GetParamDefault( static_cast< unsigned int >( index ) );
	return mixed.PointerValue ? static_cast< const char* >( mixed.PointerValue ) : "";
}

EMSCRIPTEN_KEEPALIVE int fiche_param_element_count( Instance* instance, int index )
{
	return static_cast< int >( instance->plugin.GetNumParamElements( static_cast< unsigned int >( index ) ) );
}

EMSCRIPTEN_KEEPALIVE const char* fiche_param_element_name( Instance* instance, int index, int element )
{
	const char* name = instance->plugin.GetParamElementName( static_cast< unsigned int >( index ), static_cast< unsigned int >( element ) );
	return name ? name : "";
}

EMSCRIPTEN_KEEPALIVE float fiche_param_element_value( Instance* instance, int index, int element )
{
	return asFloat( instance->plugin.GetParamElementDefault( static_cast< unsigned int >( index ), static_cast< unsigned int >( element ) ) );
}

EMSCRIPTEN_KEEPALIVE float fiche_param_range_min( Instance* instance, int index )
{
	return instance->plugin.GetParamRange( static_cast< unsigned int >( index ) ).min;
}

EMSCRIPTEN_KEEPALIVE float fiche_param_range_max( Instance* instance, int index )
{
	return instance->plugin.GetParamRange( static_cast< unsigned int >( index ) ).max;
}

EMSCRIPTEN_KEEPALIVE int fiche_max_inputs( Instance* instance )
{
	return static_cast< int >( instance->plugin.GetMaxInputs() );
}

//---------------------------------------------------------------------------
// The host's calls.
//---------------------------------------------------------------------------
EMSCRIPTEN_KEEPALIVE int fiche_set_float( Instance* instance, int index, float value )
{
	return instance->plugin.SetFloatParameter( static_cast< unsigned int >( index ), value ) == FF_SUCCESS ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE float fiche_get_float( Instance* instance, int index )
{
	return instance->plugin.GetFloatParameter( static_cast< unsigned int >( index ) );
}

EMSCRIPTEN_KEEPALIVE int fiche_set_text( Instance* instance, int index, const char* value )
{
	return instance->plugin.SetTextParameter( static_cast< unsigned int >( index ), value ) == FF_SUCCESS ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE const char* fiche_get_text( Instance* instance, int index )
{
	const char* text = instance->plugin.GetTextParameter( static_cast< unsigned int >( index ) );
	return text ? text : "";
}

/// One frame: SetTime, then ProcessOpenGL with one input of clipWidth x
/// clipHeight whose GL name is `clip` (a texture the page registered with
/// emscripten's GL tables), drawing into whatever framebuffer and viewport the
/// page has bound, as a host's. 1 on success.
EMSCRIPTEN_KEEPALIVE int fiche_process( Instance* instance, double seconds, int clip, int clipWidth, int clipHeight )
{
	instance->plugin.SetTime( seconds );
	instance->input.Width = instance->input.HardwareWidth = static_cast< FFUInt32 >( clipWidth );
	instance->input.Height = instance->input.HardwareHeight = static_cast< FFUInt32 >( clipHeight );
	instance->input.Handle                                 = static_cast< GLuint >( clip );
	instance->process.numInputTextures                     = 1;
	instance->process.inputTextures                        = instance->inputs;
	instance->process.HostFBO                              = 0;
	return instance->plugin.ProcessOpenGL( &instance->process ) == FF_SUCCESS ? 1 : 0;
}

//---------------------------------------------------------------------------
// For the panel's read-outs: Controls.cpp's conversion for a parameter, the
// one Fiche.cpp's ProcessOpenGL applies to it. NaN where the plugin uses the
// host's 0..1 as it is (Crash Zoom, Focus Skill, Position X/Y, Shutter, Mix).
//---------------------------------------------------------------------------
EMSCRIPTEN_KEEPALIVE double fiche_convert( int index, float v )
{
	switch( index )
	{
	case PT_DWELL: return DwellFromParam( v );
	case PT_HAND_SPEED: return FittsSlopeFromParam( v );
	case PT_ACCURACY: return NoiseFromParam( v );
	case PT_CARRIAGE_PLAY: return CarriageRigid( v ) ? 0.0 : CarriageHertzFromParam( v );
	case PT_ZOOM: return ZoomFromParam( v );
	case PT_FOCUS: return DefocusFromParam( v );
	case PT_GUTTER: return GutterFromParam( v );
	case PT_INTERVAL: return IntervalFromParam( v );
	case PT_FLATNESS: return FlatnessFromParam( v );
	case PT_DUST: return DustFromParam( v );
	case PT_SCRATCHES: return ScratchFromParam( v );
	case PT_APERTURE: return ApertureFromParam( v );
	case PT_PARFOCAL: return ParfocalFromParam( v );
	case PT_HOTSPOT: return ThrowFromParam( v );
	case PT_LAMP: return LampFromParam( v );
	case PT_SCREEN_GRAIN: return GrainFromParam( v );
	case PT_ROOM_LIGHT: return RoomFromParam( v );
	default: return kNaN;
	}
}

/// Carriage Play's damping ratio (CarriageZetaFromParam), beside its frequency.
EMSCRIPTEN_KEEPALIVE double fiche_carriage_zeta( float v )
{
	return CarriageZetaFromParam( v );
}

//---------------------------------------------------------------------------
// The About block's buttons: the URL each one opens, from the generated
// StoatworksAboutLinks.h the plugin is built with. The host getters give only
// a button's label; the page needs the address the plugin's own press would
// have opened, because that press calls system(), which a browser does not
// have.
//---------------------------------------------------------------------------
EMSCRIPTEN_KEEPALIVE int fiche_about_first()
{
	return static_cast< int >( PT_ABOUT_FIRST );
}

EMSCRIPTEN_KEEPALIVE const char* fiche_about_button_label( int button )
{
	const auto& list = stoatworks::about::buttons();
	return button >= 0 && static_cast< size_t >( button ) < list.size() ? list[ static_cast< size_t >( button ) ].label : "";
}

EMSCRIPTEN_KEEPALIVE const char* fiche_about_button_url( int button )
{
	const auto& list = stoatworks::about::buttons();
	return button >= 0 && static_cast< size_t >( button ) < list.size() ? list[ static_cast< size_t >( button ) ].url : "";
}

//---------------------------------------------------------------------------
// What the reader is doing, for the status line: the newest of the states the
// last frame's screen pass was given (ExposureForTest, the harness's accessor),
// and the card it was given (CardForTest). 0 = carriage x mm, 1 = carriage y
// mm, 2 = magnification, 3 = the focus knob mm, 4 = card width mm, 5 = card
// height mm. NaN before the first frame.
//---------------------------------------------------------------------------
EMSCRIPTEN_KEEPALIVE double fiche_state( Instance* instance, int what )
{
	const auto& exposure = instance->plugin.ExposureForTest();
	if( exposure.empty() )
		return kNaN;
	const hand::State& now = exposure.back();
	switch( what )
	{
	case 0: return now.x;
	case 1: return now.y;
	case 2: return std::exp2( now.logM );
	case 3: return now.z;
	case 4: return instance->plugin.CardForTest().width;
	case 5: return instance->plugin.CardForTest().height;
	default: return kNaN;
	}
}

/// Frames the step-and-repeat camera has exposed onto the card (Content Filmed).
EMSCRIPTEN_KEEPALIVE int fiche_written( Instance* instance )
{
	return instance->plugin.WrittenForTest();
}

} // extern "C"
