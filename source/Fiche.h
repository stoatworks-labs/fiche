#pragma once

#include "Controls.h"
#include "Hand.h"
#include "Reader.h"
#include "Shaders.h"

#include <FFGLSDK.h>

#include "StoatworksAboutParams.h"

#include <string>
#include <vector>

/**
	Fiche -- browsing a microfiche reader, as an FFGL effect.

	**The one idea.** A microfiche reader magnifies a hand. The screen shows
	about a centimetre of film at 24x, so every movement of the hand on the
	carriage, every flick of the zoom and every turn of the focus knob
	arrives on the screen multiplied by twenty-four. The whip pans, the
	overshoot and correction, the crash zooms that lose focus and the focus
	hunt are what a person's hand does, seen through that magnifier.

	**The CPU** is the hand (`Hand.h`: Fitts's law, minimum jerk, signal-
	dependent scatter and corrections, crash zooms, a focus search with a
	reaction time, a carriage on a compliant grip) and the reader's geometry
	(`Reader.h`). **The GPU** is the light: the clip in linear light with box
	mips, a step-and-repeat camera's store of filmed frames, and one screen
	pass that integrates every pixel over the lens's aperture and the frame's
	exposure, through the card, the film and its dirt, onto a screen.
*/
class Fiche : public CFFGLPlugin
{
public:
	Fiche();

	//CFFGLPlugin
	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;
	FFResult SetTime( double time ) override;

	char* GetTextParameter( unsigned int index ) override;
	/// The Title, and the About line's own default: instantiateGL pushes every
	/// declared default back through the setters and deletes the whole
	/// instance if one fails, and CFFGLPlugin's SetTextParameter is a stub that
	/// returns exactly that failure.
	FFResult SetTextParameter( unsigned int index, const char* value ) override;

	//--- for the harness ------------------------------------------------------
	/// The offline harness DECLARES its clock unit rather than leaving the
	/// vote to infer one from a synthetic clock.
	void SetClockScaleForTest( double scale );
	/// Negative-control hooks: `shaders::Hook` bits for the GLSL, the hand's
	/// own (`hand::HandHook`), and two of the plugin's.
	void SetHooksForTest( int glslHooks );
	void SetHandHooksForTest( int handHooks );
	/// The GPU is shown the hand, not the carriage on its grip.
	void SetShowHandForTest( bool on );
	/// The exposure window scaled (2 is a shutter twice as long as it says).
	void SetShutterScaleForTest( double scale );
	/// The store survives a reallocation.
	void SetResizeKeepsStoreForTest( bool keep );
	/// Log every segment and hunt the hand plans.
	void SetHandLoggingForTest( bool on );
	/// Off, each tap samples the card at a single pixel's footprint: the
	/// defocus and shutter checks measure the taps' geometry bare.
	void SetPrefilterForTest( bool on );
	/// A beat starts an act at the start of the frame it fell in.
	void SetCueAtFrameStartForTest( bool on );

	const fiche::hand::Hand& HandForTest() const
	{
		return hand;
	}
	const fiche::reader::Card& CardForTest() const
	{
		return card;
	}
	const fiche::reader::Bow& BowForTest() const
	{
		return bow;
	}
	/// The states the last frame's screen pass was given.
	const std::vector< fiche::hand::State >& ExposureForTest() const
	{
		return exposure;
	}
	int WrittenForTest() const
	{
		return written;
	}
	int StoreWidthForTest() const
	{
		return storeW;
	}
	GLuint LiveTextureForTest() const
	{
		return liveTexture;
	}
	int LiveLevelsForTest() const
	{
		return liveLevels;
	}
	const fiche::hand::Settings& SettingsForTest() const
	{
		return settings;
	}

	static constexpr unsigned int ParamCount()
	{
		return fiche::PT_COUNT;
	}

private:
	/// The host's clock in seconds, whatever unit it arrived in.
	double nowSeconds();

	bool ensureLive( int width, int height );
	void releaseLive();
	bool ensureStore( int width, int height, int frames );
	void releaseStore();
	void uploadBow();
	void exposeFrame( int layer );

	ffglex::FFGLShader copyShader, mipShader, filmShader, mipLayerShader, screenShader;
	ffglex::FFGLScreenQuad quad;
	GLuint workFBO = 0;

	/// The clip in linear light, RGBA16F, with a box mip chain.
	GLuint liveTexture = 0;
	int liveW = 0, liveH = 0, liveLevels = 0;

	/// The filmed frames: a 2D array, one layer per frame, RGBA8 sRGB-coded,
	/// with box mips. A 1 x 1 x 1 array stands in while there is none: a
	/// sampler2DArray on an empty unit is "unloadable" (patchwork's trap).
	GLuint storeTexture = 0, emptyStore = 0;
	int storeW = 0, storeH = 0, storeLayers = 0, storeLevels = 0;
	int exposures = 0, written = 0;
	double filmClock = 0.0;

	GLuint bowTexture = 0, fontTexture = 0;
	int fontW = 0, fontH = 0;

	fiche::reader::Card card;
	fiche::reader::Bow bow;
	bool bowBuilt       = false;
	double bowAmplitude = -1.0;
	uint32_t bowSeed    = 0;
	fiche::reader::Card bowCard;

	fiche::hand::Hand hand;
	fiche::hand::Settings settings;
	std::vector< fiche::hand::State > exposure;

	double lampKelvin = -1.0;
	double lamp[ 3 ]  = { 1.0, 1.0, 1.0 };
	float disc[ 2 * fiche::shaders::kMaxTaps ] = {};

	std::string title;
	bool jumpPressed  = false;
	bool jumpHeld     = false;
	double lastPhase   = -1.0;
	float lastBarPhase = -1.0f;
	bool hostBeatSeen = false;

	//--- the clock (readout's unit voting, via pitch and patchwork) -----------
	bool hostTimeSeen   = false;
	double clockScale   = 0.0;///< 0 until decided; then 1.0 or 0.001
	double wallStart    = -1.0;
	double lastWallTime = -1.0;
	double lastRawTime  = -1.0;
	int secondsVotes    = 0;
	int millisVotes     = 0;
	double lastNow      = -1.0;
	int clockFrames     = 0;

	//--- test hooks ------------------------------------------------------------
	int hooks              = 0;
	bool showHand          = false;
	double shutterScale    = 1.0;
	bool resizeKeepsStore  = false;
	bool prefilter         = true;
	bool cueAtFrameStart   = false;

	/// Zero-initialised: the About block's ids are never stored to, so
	/// without this GetFloatParameter hands the host whatever was on the
	/// stack for them.
	float params[ fiche::PT_COUNT ] = {};

	/// GetTextParameter hands the host a bare pointer, so the string has to
	/// outlive the call.
	std::string aboutText;
};
