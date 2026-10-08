#include "Controls.h"

#include <algorithm>
#include <cmath>

namespace fiche
{
namespace
{
double clamp01( float v )
{
	return std::clamp( static_cast< double >( v ), 0.0, 1.0 );
}
double geometric( float v, double low, double high )
{
	return low * std::pow( high / low, clamp01( v ) );
}
float inverseGeometric( double value, double low, double high )
{
	const double lo = std::min( low, high ), hi = std::max( low, high );
	return static_cast< float >( std::log( std::clamp( value, lo, hi ) / low ) / std::log( high / low ) );
}
double linear( float v, double low, double high )
{
	return low + ( high - low ) * clamp01( v );
}
float inverseLinear( double value, double low, double high )
{
	return static_cast< float >( std::clamp( ( value - low ) / ( high - low ), 0.0, 1.0 ) );
}

constexpr double kDwellLow = 0.1, kDwellHigh = 10.0;
constexpr double kSlopeSlow = 0.25, kSlopeFast = 0.03;
constexpr double kNoiseLow = 0.25, kNoiseHigh = 0.01;
constexpr double kGripStiff = 40.0, kGripLoose = 3.0;
constexpr double kZoomLow = 2.0, kZoomHigh = 75.0;
constexpr double kMaxDefocus = 1.0;
constexpr double kMaxGutter = 3.0;
constexpr double kIntervalLow = 0.05, kIntervalHigh = 10.0;
constexpr double kMaxBow = 0.5;
constexpr double kMaxDust = 150.0;
constexpr double kMaxScratch = 0.6;
constexpr double kApertureLow = 2.0, kApertureHigh = 16.0;
constexpr double kMaxParfocal = 1.0;
constexpr double kShortestThrow = 250.0;
constexpr double kLampLow = 2000.0, kLampWhite = 6504.0;
constexpr double kMaxGrain = 0.08;
constexpr double kMaxRoom = 0.2;

const char* const kOperatorNames[] = { "Auto", "Manual" };
const char* const kBrowseNames[]   = { "Reading", "Skimming", "Searching", "Mixed" };
const char* const kSyncNames[]     = { "Free", "Beat", "2 Beats", "Bar" };
const char* const kContentNames[]  = { "Live", "Filmed" };
const char* const kFilmNames[]     = { "Ideal", "Silver", "Silver Negative", "Diazo Blue", "Diazo Black", "Vesicular", "Colour" };

ParamInfo standard( unsigned int id, const char* name, const char* group, float value )
{
	return { id, name, group, Kind::Standard, value, 0.0f, 1.0f, nullptr, 0 };
}
ParamInfo integer( unsigned int id, const char* name, const char* group, float value, float lo, float hi )
{
	return { id, name, group, Kind::Integer, value, lo, hi, nullptr, 0 };
}
ParamInfo option( unsigned int id, const char* name, const char* group, float value, const char* const* names, int count )
{
	return { id, name, group, Kind::Option, value, 0.0f, static_cast< float >( count - 1 ), names, count };
}
ParamInfo event( unsigned int id, const char* name, const char* group )
{
	return { id, name, group, Kind::Event, 0.0f, 0.0f, 1.0f, nullptr, 0 };
}
ParamInfo text( unsigned int id, const char* name, const char* group )
{
	return { id, name, group, Kind::Text, 0.0f, 0.0f, 1.0f, nullptr, 0 };
}
} // namespace

const char* const kDefaultTitle = "FICHE MF01   CARD 1 OF 1";

//---------------------------------------------------------------------------
// The table. The defaults are somebody in a library reading room going
// through a card in a hurry: a 14 x 7 card of the clip on silver film,
// read at 24x through an f/2.8 lens on a reader whose zoom does not hold
// focus, the card bowed between plates that no longer close, a carriage
// with some give in it, and an operator who skims and searches, zooms out
// to travel more often than not and is not good at focusing.
//---------------------------------------------------------------------------
const ParamInfo& InfoOf( unsigned int id )
{
	static const ParamInfo table[ PT_ABOUT_FIRST ] = {
		option( PT_OPERATOR, "Operator", "Operator", 0.0f, kOperatorNames, static_cast< int >( Operator::Count ) ),
		option( PT_BROWSE, "Browse", "Operator", static_cast< float >( Browse::Mixed ), kBrowseNames, static_cast< int >( Browse::Count ) ),
		standard( PT_DWELL, "Dwell", "Operator", ParamFromDwell( 0.5 ) ),
		option( PT_SYNC, "Sync", "Operator", 0.0f, kSyncNames, static_cast< int >( Sync::Count ) ),
		standard( PT_HAND_SPEED, "Hand Speed", "Operator", ParamFromFittsSlope( 0.09 ) ),
		standard( PT_ACCURACY, "Accuracy", "Operator", ParamFromNoise( 0.07 ) ),
		standard( PT_CRASH_ZOOM, "Crash Zoom", "Operator", 0.6f ),
		standard( PT_FOCUS_SKILL, "Focus Skill", "Operator", 0.35f ),
		standard( PT_CARRIAGE_PLAY, "Carriage Play", "Operator", 0.5f ),
		event( PT_JUMP, "Jump", "Operator" ),

		standard( PT_ZOOM, "Zoom", "View", ParamFromZoom( 24.0 ) ),
		standard( PT_POSITION_X, "Position X", "View", 0.5f ),
		standard( PT_POSITION_Y, "Position Y", "View", 0.5f ),
		standard( PT_FOCUS, "Focus", "View", 0.5f ),

		integer( PT_COLUMNS, "Columns", "Fiche", 14.0f, 1.0f, 16.0f ),
		integer( PT_ROWS, "Rows", "Fiche", 7.0f, 1.0f, 16.0f ),
		standard( PT_GUTTER, "Gutter", "Fiche", ParamFromGutter( 0.8 ) ),
		option( PT_CONTENT, "Content", "Fiche", 0.0f, kContentNames, static_cast< int >( Content::Count ) ),
		standard( PT_INTERVAL, "Interval", "Fiche", ParamFromInterval( 0.5 ) ),
		option( PT_FILM, "Film", "Fiche", static_cast< float >( Film::Silver ), kFilmNames, static_cast< int >( Film::Count ) ),
		standard( PT_FLATNESS, "Flatness", "Fiche", ParamFromFlatness( 0.3 ) ),
		standard( PT_DUST, "Dust", "Fiche", 0.3f ),
		standard( PT_SCRATCHES, "Scratches", "Fiche", 0.3f ),
		text( PT_TITLE, "Title", "Fiche" ),

		standard( PT_APERTURE, "Aperture", "Reader", ParamFromAperture( 2.8 ) ),
		standard( PT_PARFOCAL, "Parfocal", "Reader", ParamFromParfocal( 0.5 ) ),
		standard( PT_SHUTTER, "Shutter", "Reader", 0.5f ),
		standard( PT_HOTSPOT, "Hotspot", "Reader", 0.5f ),
		standard( PT_LAMP, "Lamp", "Reader", ParamFromLamp( 4500.0 ) ),
		standard( PT_SCREEN_GRAIN, "Screen Grain", "Reader", 0.3f ),
		standard( PT_ROOM_LIGHT, "Room Light", "Reader", 0.1f ),

		integer( PT_SEED, "Seed", "Output", 1.0f, 0.0f, 999.0f ),
		standard( PT_MIX, "Mix", "Output", 1.0f ),
	};
	static const ParamInfo none = { PT_COUNT, "?", "?", Kind::Standard, 0.0f, 0.0f, 1.0f, nullptr, 0 };
	return id < PT_ABOUT_FIRST ? table[ id ] : none;
}

int IntegerOf( unsigned int id, float value )
{
	const ParamInfo& info = InfoOf( id );
	return std::clamp( static_cast< int >( std::lround( value ) ), static_cast< int >( info.minimum ),
	                   static_cast< int >( info.maximum ) );
}

int OptionIndex( float value, int count )
{
	return std::clamp( static_cast< int >( std::lround( value ) ), 0, count - 1 );
}

double DwellFromParam( float v )
{
	return geometric( v, kDwellLow, kDwellHigh );
}
float ParamFromDwell( double seconds )
{
	return inverseGeometric( seconds, kDwellLow, kDwellHigh );
}
double FittsSlopeFromParam( float v )
{
	return geometric( v, kSlopeSlow, kSlopeFast );
}
float ParamFromFittsSlope( double secondsPerBit )
{
	return inverseGeometric( secondsPerBit, kSlopeSlow, kSlopeFast );
}
double NoiseFromParam( float v )
{
	return geometric( v, kNoiseLow, kNoiseHigh );
}
float ParamFromNoise( double k )
{
	return inverseGeometric( k, kNoiseLow, kNoiseHigh );
}
bool CarriageRigid( float v )
{
	return !( v > 0.0f );
}
double CarriageHertzFromParam( float v )
{
	return geometric( v, kGripStiff, kGripLoose );
}
double CarriageZetaFromParam( float v )
{
	return 0.5 - 0.3 * clamp01( v );
}
double ZoomFromParam( float v )
{
	return geometric( v, kZoomLow, kZoomHigh );
}
float ParamFromZoom( double magnification )
{
	return inverseGeometric( magnification, kZoomLow, kZoomHigh );
}
double DefocusFromParam( float v )
{
	return linear( v, -kMaxDefocus, kMaxDefocus );
}
float ParamFromDefocus( double mm )
{
	return inverseLinear( mm, -kMaxDefocus, kMaxDefocus );
}
double GutterFromParam( float v )
{
	return linear( v, 0.0, kMaxGutter );
}
float ParamFromGutter( double mm )
{
	return inverseLinear( mm, 0.0, kMaxGutter );
}
double IntervalFromParam( float v )
{
	return geometric( v, kIntervalLow, kIntervalHigh );
}
float ParamFromInterval( double seconds )
{
	return inverseGeometric( seconds, kIntervalLow, kIntervalHigh );
}
double FlatnessFromParam( float v )
{
	return linear( v, 0.0, kMaxBow );
}
float ParamFromFlatness( double mm )
{
	return inverseLinear( mm, 0.0, kMaxBow );
}
double DustFromParam( float v )
{
	const double x = clamp01( v );
	return kMaxDust * x * x;
}
double ScratchFromParam( float v )
{
	return kMaxScratch * clamp01( v );
}
double ApertureFromParam( float v )
{
	return geometric( v, kApertureLow, kApertureHigh );
}
float ParamFromAperture( double fNumber )
{
	return inverseGeometric( fNumber, kApertureLow, kApertureHigh );
}
double ParfocalFromParam( float v )
{
	return linear( v, 0.0, kMaxParfocal );
}
float ParamFromParfocal( double mmPerDoubling )
{
	return inverseLinear( mmPerDoubling, 0.0, kMaxParfocal );
}
double ThrowFromParam( float v )
{
	const double x = clamp01( v );
	return x > 0.0 ? kShortestThrow / x : 0.0;
}
double LampFromParam( float v )
{
	return linear( v, kLampLow, kLampWhite );
}
float ParamFromLamp( double kelvin )
{
	return inverseLinear( kelvin, kLampLow, kLampWhite );
}
double GrainFromParam( float v )
{
	return kMaxGrain * clamp01( v );
}
double RoomFromParam( float v )
{
	return kMaxRoom * clamp01( v );
}

} // namespace fiche
