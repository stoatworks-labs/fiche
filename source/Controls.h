#pragma once

#include "StoatworksAboutLinks.h"

#include <cstdint>

/**
    The host's parameters, and what they mean in the reader's units.

    Every ranged parameter the host sees is 0..1, because `SetParamInfo`
    clamps an `FF_TYPE_STANDARD` default into 0..1 before `SetParamRange`
    could widen it. The conversions live in Controls.cpp, one function per
    control, each with its inverse where a default or the harness needs it.
    Option parameters hold the element index; Columns, Rows and Seed are real
    integers (`FF_TYPE_INTEGER`); Title is text; Jump is a press.

    Units: millimetres on the card and on the screen, magnification as a
    ratio, the focus knob in millimetres of travel at the film, seconds.
*/
namespace fiche
{
/// Every control, in the order Resolume shows them. The About block is
/// LAST, so the day a user guide adds a button no control moves.
enum ParamId : unsigned int
{
	// -- Operator: the hand --------------------------------------------------
	PT_OPERATOR = 0,
	PT_BROWSE,
	PT_DWELL,
	PT_SYNC,
	PT_HAND_SPEED,
	PT_ACCURACY,
	PT_CRASH_ZOOM,
	PT_FOCUS_SKILL,
	PT_CARRIAGE_PLAY,
	PT_JUMP,

	// -- View: the lens, and the hand when it is yours ------------------------
	PT_ZOOM,
	PT_POSITION_X,
	PT_POSITION_Y,
	PT_FOCUS,

	// -- Fiche: the card -----------------------------------------------------
	PT_COLUMNS,
	PT_ROWS,
	PT_GUTTER,
	PT_CONTENT,
	PT_INTERVAL,
	PT_FILM,
	PT_FLATNESS,
	PT_DUST,
	PT_SCRATCHES,
	PT_TITLE,

	// -- Reader: the optics and the screen ------------------------------------
	PT_APERTURE,
	PT_PARFOCAL,
	PT_SHUTTER,
	PT_HOTSPOT,
	PT_LAMP,
	PT_SCREEN_GRAIN,
	PT_ROOM_LIGHT,

	// -- Output --------------------------------------------------------------
	PT_SEED,
	PT_MIX,

	// -- The Stoatworks About block: a text line, then one button per link.
	PT_ABOUT_FIRST,
	PT_COUNT = PT_ABOUT_FIRST + 1 + stoatworks::about::kButtonCount
};

enum class Kind
{
	Standard,///< 0..1, converted in Controls.cpp
	Integer, ///< a real integer with a real range
	Option,  ///< an element index
	Event,   ///< a press
	Text     ///< a string
};

/// One control as the host is told about it.
struct ParamInfo
{
	unsigned int id;
	const char* name;  ///< at most 16 characters: FFGL's name field is not terminated
	const char* group;
	Kind kind;
	float defaultValue;///< in the host's units: 0..1, the integer, or the index
	float minimum;     ///< integers only
	float maximum;     ///< integers only
	const char* const* options;
	int optionCount;
};

/// The table, indexed by ParamId, for every id below PT_ABOUT_FIRST.
const ParamInfo& InfoOf( unsigned int id );

/// The title the header strip carries until someone types another.
extern const char* const kDefaultTitle;

//---------------------------------------------------------------------------
// Options.
//---------------------------------------------------------------------------
enum class Operator
{
	Auto = 0,///< the hand browses by itself
	Manual,  ///< the hand is Position X/Y, the lens Zoom, the knob Focus
	Count
};
enum class Browse
{
	Reading = 0,///< the next view in reading order
	Skimming,   ///< a few views on
	Searching,  ///< anywhere on the card
	Mixed,      ///< each act picks one of the three
	Count
};
enum class Sync
{
	Free = 0,///< the operator's own dwell
	Beat,    ///< acts start on the host's beat
	TwoBeats,
	Bar,
	Count
};
enum class Content
{
	Live = 0,///< every frame on the card is the clip as it plays
	Filmed,  ///< a step-and-repeat camera films the clip onto the card, a frame per Interval
	Count
};
enum class Film
{
	Ideal = 0,     ///< a clear base, a black dye, in colour: the clip itself
	Silver,        ///< black-and-white silver halide, positive
	SilverNegative,///< ...negative: a picture in reverse, clear between the frames
	DiazoBlue,     ///< a blue-violet diazo dye
	DiazoBlack,    ///< diazo's blue-black
	Vesicular,     ///< light scattered by vesicles in a tan base: soft blacks
	Colour,        ///< a colour stock
	Count
};

//---------------------------------------------------------------------------
// The mappings.
//---------------------------------------------------------------------------
int IntegerOf( unsigned int id, float value );
int OptionIndex( float value, int count );

/// Dwell: the mean pause between acts, 0.1 s to 10 s, geometrically.
double DwellFromParam( float v );
float ParamFromDwell( double seconds );
/// Hand Speed: Fitts's slope b, 0.25 s/bit (slow) to 0.03 s/bit (fast).
double FittsSlopeFromParam( float v );
float ParamFromFittsSlope( double secondsPerBit );
/// Accuracy: the endpoint scatter k (SD / distance), 0.25 to 0.01.
double NoiseFromParam( float v );
float ParamFromNoise( double k );
/// Carriage Play: 0 is a rigid carriage; above it the grip's natural
/// frequency falls from 40 Hz to 3 Hz and its damping from 0.5 to 0.2.
bool CarriageRigid( float v );
double CarriageHertzFromParam( float v );
double CarriageZetaFromParam( float v );
/// Zoom: magnification, 2x to 75x, geometrically.
double ZoomFromParam( float v );
float ParamFromZoom( double magnification );
/// Focus (Manual): the defocus at the screen's centre, -1 mm to +1 mm.
double DefocusFromParam( float v );
float ParamFromDefocus( double mm );
/// Gutter: the clear space between frames, 0 to 3 mm.
double GutterFromParam( float v );
float ParamFromGutter( double mm );
/// Interval: seconds between frames filmed, 0.05 to 10, geometrically.
double IntervalFromParam( float v );
float ParamFromInterval( double seconds );
/// Flatness: the largest bow of the card between its glass plates, 0 to 0.5 mm.
double FlatnessFromParam( float v );
float ParamFromFlatness( double mm );
/// Dust: particles per square centimetre of card, 0 to 40, square law.
double DustFromParam( float v );
/// Scratches: the chance that a 2 mm band of the card carries a scratch, 0 to 0.6.
double ScratchFromParam( float v );
/// Aperture: the projection lens's f-number, f/2 to f/16, geometrically.
double ApertureFromParam( float v );
float ParamFromAperture( double fNumber );
/// Parfocal: how far the zoom throws the focus, 0 to 1 mm per doubling.
double ParfocalFromParam( float v );
float ParamFromParfocal( double mmPerDoubling );
/// Hotspot: the lens-to-screen throw that sets the cos^4 falloff, from no
/// falloff at 0 to a 250 mm throw at 1. Returns the throw in mm, 0 for none.
double ThrowFromParam( float v );
/// Lamp: colour temperature, 2000 K to 6504 K (white).
double LampFromParam( float v );
float ParamFromLamp( double kelvin );
/// Screen Grain: the diffuser's relative brightness scatter, 0 to 0.08.
double GrainFromParam( float v );
/// Room Light: the room's light off the screen, 0 to 0.2 of the lamp's white.
double RoomFromParam( float v );

} // namespace fiche
