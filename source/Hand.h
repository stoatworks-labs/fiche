#pragma once

#include "Controls.h"
#include "Reader.h"

#include <cstdint>
#include <deque>
#include <vector>

/**
    The hand: an operator at a microfiche reader, in double, deterministic per
    seed. No GL.

    **The one idea is that a reader magnifies a hand**, so this is the half of
    the plugin that makes the look; the GPU only shows it. Auto mode is a loop
    of acts -- dwell, pick a view, move there, focus -- and every move is a
    model of a person, not an animation curve:

    - **Aimed movements.** A pan is a minimum-jerk submovement whose duration
      is Fitts's law, `a + b log2( D / W + 1 )`, with the target W a tenth of
      the screen's width ON THE CARD (so it shrinks as the magnification
      rises), landing a little short (primary submovements undershoot) with a
      scatter proportional to the distance (signal-dependent noise). A miss by
      more than W/2 is followed, after a reaction gap, by a correction under
      the same law.
    - **Crash zooms.** A long move may zoom out first, travel where the target
      is wide and quick to hit, and zoom back in -- landing on a magnification
      a little off the one it left, with the focus shift that brings.
    - **The hunt.** The operator turns the focus knob towards sharp, passes it,
      notices only when the blur has grown past what they accept, and turns
      back a reaction time later at a fraction of the speed; when a pass is
      slow enough to stop inside the depth of focus, they stop.
    - **The carriage.** The hand holds the card's carriage through a compliant
      grip: a damped spring, integrated exactly over 1 ms substeps with the
      hand moving linearly inside each, so a hard stop overshoots and settles.

    Manual mode makes the hand Position X/Y, the lens Zoom and the knob Focus.

    Time is the host's, accumulated; segments are scheduled in continuous
    time, never quantised to frames, and the states of the last quarter-second
    are kept at 1 ms so the exposure can be integrated across a frame.
*/
namespace fiche::hand
{
/// A place the operator reads: a frame's centre, or one screen-sized tile of
/// a frame that is bigger than the screen at the reading magnification.
struct View
{
	double x, y;///< card mm
	int frame;  ///< reading order
};
std::vector< View > MakeViews( const reader::Card& card, double magnification, double screenW, double screenH );

//---------------------------------------------------------------------------
// The fixed numbers, and where they come from.
//---------------------------------------------------------------------------
/// Fitts's intercept, seconds: a typical value for hand movements.
constexpr double kFittsA = 0.08;
/// The target an operator centres a view within: a tenth of the screen.
constexpr double kTargetShare = 0.1;
/// Primary submovements undershoot (Elliott, Helsen and Chua 2001): they
/// cover 96% of the distance on average.
constexpr double kPrimaryGain = 0.96;
/// Endpoint scatter across the movement, relative to the scatter along it.
constexpr double kAcrossShare = 0.4;
/// The pause before a correction: seeing the miss and reacting.
constexpr double kReactionGap = 0.15;
constexpr int kMaxCorrections = 4;
/// A move longer than this many frame widths may zoom out to travel.
constexpr double kCrashDistance = 1.5;
/// A zoom takes kZoomBase + kZoomPerStop per doubling, scaled by Hand Speed.
constexpr double kZoomBase    = 0.1;
constexpr double kZoomPerStop = 0.07;
/// A wrong first turn is noticed when the blur has grown by a quarter.
constexpr double kWeber = 0.25;
/// The first turn towards focus is meant to take this long; the knob never
/// starts slower than kKnobFloor mm/s (a quick twist: about two turns a
/// second of a knob that moves the lens a millimetre a turn).
constexpr double kFirstTurn = 0.35;
constexpr double kKnobFloor = 1.5;
/// The most passes a hunt makes before the operator gives up where they are.
constexpr int kMaxPasses = 12;
/// A synced dwell lasts at least this long before a beat can end it.
constexpr double kMinSyncDwell = 0.15;
/// The integration substep, and how much history the exposure can reach.
constexpr double kSubstep    = 0.001;
constexpr int kHistoryLength = 640;

struct Settings
{
	bool manual          = false;
	Browse browse        = Browse::Mixed;
	double dwell         = 0.8;
	bool synced          = false;
	double fittsB        = 0.09;
	double noiseK        = 0.07;
	double crashZoom     = 0.6;
	double skill         = 0.35;
	bool rigid           = false;
	double gripHz        = 11.0;
	double gripZeta      = 0.35;
	double readZoom      = 24.0;
	double manualX       = 74.0;///< card mm
	double manualY       = 30.0;
	double manualDefocus = 0.0; ///< mm at the screen's centre
	double aperture      = 4.0;
	double parfocal      = 0.25;
	double screenW       = reader::kScreenWidth;
	double screenH       = reader::kScreenWidth * 9.0 / 16.0;
	uint32_t seed        = 1;
};

/// What the reader is doing at one instant: the carriage (the card point at
/// the screen's centre, mm), the magnification as log2, the focus knob (mm).
struct State
{
	double x = 0.0, y = 0.0, logM = 0.0, z = 0.0;
};

enum class Tag : int
{
	Dwell,
	ZoomRead,///< back to the reading magnification after Zoom was changed
	ZoomOut, ///< a crash zoom's way out
	Primary, ///< an aimed movement's first submovement
	Wait,    ///< the reaction before a correction
	Correction,
	ZoomIn,  ///< a crash zoom's way back
	Hunt
};

struct Segment
{
	enum class Kind
	{
		Pan,
		Zoom,
		Focus,
		Wait
	} kind = Kind::Wait;
	Tag tag   = Tag::Wait;
	double t0 = 0.0, T = 0.0;
	double ax = 0.0, ay = 0.0, bx = 0.0, by = 0.0;///< Pan: the hand from A to B
	double a = 0.0, b = 0.0;                      ///< Zoom: log2 M; Focus: the knob
	double aim = 0.0;      ///< Pan: the distance to the TARGET, which the scatter scales with
	double tolerance = 0.0;///< Pan: the target W on the card
	double tx = 0.0, ty = 0.0;///< Pan: the target itself
	bool untilCue = false; ///< a synced dwell: ends on a beat
	double End() const
	{
		return t0 + T;
	}
};

/// One focus hunt as planned, for the harness: signed defocus throughout,
/// positive with the knob too far from the film.
struct Hunt
{
	double start = 0.0, band = 0.0, reaction = 0.0, gain = 0.0, speed = 0.0;
	double magnification = 0.0, fNumber = 0.0;
	bool wrongWay = false;
	std::vector< double > turns; ///< the defocus at each reversal towards focus
	std::vector< double > speeds;///< each pass's speed, mm/s
	double end  = 0.0;
	double time = 0.0;           ///< when it began
};

/// Negative-control hooks: wrong models, each one a bit. Zero when shipped.
enum HandHook : int
{
	kHookFittsNoOne   = 1 << 0,///< ID = log2( D / W ), without Shannon's + 1
	kHookCubicProfile = 1 << 1,///< a cubic ease instead of minimum jerk
	kHookFlatScatter  = 1 << 2,///< endpoint scatter that ignores the distance
	kHookNoReaction   = 1 << 3,///< a hunt that turns at the band's edge, no reaction time
	kHookEulerGrip    = 1 << 4,///< the carriage by forward Euler
	kHookBiasedSearch = 1 << 5,///< Searching only ever looks in the next half of the card
	kHookFarImage     = 1 << 6 ///< on an endless page, go to the view on the first tile, not the nearest copy
};

class Hand
{
public:
	/// Start again: at the first view, at the reading magnification, the
	/// knob where the card's bow under it puts the focus off by as much as the
	/// card is bowed (so a flat card on a parfocal lens starts sharp).
	void Reset( const Settings& s, const reader::Card& card, const reader::Bow& bow );

	/// `dt` seconds of the host's clock. `jump` is a press of Jump this frame;
	/// `cue` is a beat (at Sync's division) that fell `cueOffset` seconds
	/// into this frame.
	void Advance( double dt, const Settings& s, const reader::Card& card, const reader::Bow& bow, bool jump, bool cue,
	              double cueOffset = 0.0 );

	State Now() const;
	/// `count` states evenly spread over the last `window` seconds, each at
	/// the centre of its share, the last ending now. A window of 0 is now.
	void Exposure( double window, int count, State* out ) const;
	/// The state at any time the history still holds (clamped to it).
	State At( double time ) const;
	double Time() const
	{
		return t;
	}
	bool Started() const
	{
		return started;
	}

	//--- for the harness ------------------------------------------------------
	void SetHooks( int bits )
	{
		hooks = bits;
	}
	/// Keep every segment that starts, and every hunt planned.
	void SetLogging( bool on )
	{
		logging = on;
	}
	const std::vector< Segment >& Log() const
	{
		return log;
	}
	const std::vector< Hunt >& Hunts() const
	{
		return hunts;
	}
	/// The hand's own position, before the carriage's grip.
	double HandX() const
	{
		return hx;
	}
	double HandY() const
	{
		return hy;
	}
	/// Where the focus knob must be for the card point under the screen's
	/// centre to be sharp, at this magnification.
	static double BestFocus( const Settings& s, const reader::Bow& bow, double x, double y, double logM );
	/// The minimum-jerk profile (or the hooked cubic), 0..1 over 0..1.
	double Profile( double tau ) const;
	/// A pan's primary duration: Fitts's law (or the hooked ID).
	double FittsTime( const Settings& s, double distance, double width ) const;

private:
	enum class Stage
	{
		Focus,
		Dwell,
		Act,
		Travel,
		Settle
	};

	struct Record
	{
		double t;
		State s;
	};
	/// The state the queue ends on: where the next segment starts from.
	struct Planned
	{
		double t = 0.0, x = 0.0, y = 0.0, logM = 0.0, z = 0.0;
	};

	double uniform();
	double normal();
	void push( const Segment& segment );
	void decide( double at, const Settings& s, const reader::Card& card, const reader::Bow& bow );
	void planPan( double at, Tag tag, double tx, double ty, const Settings& s, const reader::Card& card );
	void planZoom( double at, Tag tag, double toLogM, const Settings& s );
	void planHunt( double at, const Settings& s, const reader::Bow& bow );
	void planDwell( double at, const Settings& s );
	bool needsCorrection() const;
	void applyEnd( const Segment& seg );
	void applyAt( const Segment& seg, double time );
	void advanceGrip( double h, double x0, double y0, double x1, double y1, const Settings& s );
	void record();
	void rebuildViews( const Settings& s, const reader::Card& card );

	bool started = false;
	double t     = 0.0;
	uint32_t seed = 1, draws = 0;
	int hooks     = 0;

	// The hand's intent, and the carriage on its grip.
	double hx = 0.0, hy = 0.0, logM = 0.0, z = 0.0;
	double cx = 0.0, cy = 0.0, cvx = 0.0, cvy = 0.0;
	double travelW = reader::kCardWidth, travelH = 1e9;///< the carriage's end stops: the card
	bool endStops  = true;                              ///< none on an endless page

	// The plan.
	std::deque< Segment > queue;
	Planned planned;
	Stage stage = Stage::Focus;
	double lastEnd = 0.0;
	int corrections = 0;
	bool crashing   = false;
	double targetX = 0.0, targetY = 0.0;
	double travelLogM = 0.0;
	bool pendingJump = false, pendingCue = false;
	double frameStart = 0.0, cueAt = 0.0;

	// Manual.
	bool manualMode = false;
	double manualFromX = 0.0, manualFromY = 0.0, manualToX = 0.0, manualToY = 0.0, manualDt = 0.0;
	/// On the endless page, the tile Position X / Y are read on: the one
	/// nearest the carriage when the hand became yours, so taking over does
	/// not whip the carriage back to the first tile.
	double manualTileX = 0.0, manualTileY = 0.0;

	// The views.
	std::vector< View > views;
	int current = 0;
	reader::Card viewsCard;
	double viewsZoom = 0.0, viewsW = 0.0, viewsH = 0.0;

	// History for the exposure: a ring.
	std::vector< Record > history;
	int historyHead = 0, historyCount = 0;

	bool logging = false;
	std::vector< Segment > log;
	std::vector< Hunt > hunts;
};

} // namespace fiche::hand
