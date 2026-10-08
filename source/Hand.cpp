#include "Hand.h"

#include "Hash.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace fiche::hand
{
namespace
{
constexpr uint32_t kSaltDraw = 0x68616e64u;
constexpr double kTwoPi      = 6.283185307179586476925;
constexpr double kNever      = std::numeric_limits< double >::infinity();

double clampLogZoom( double logM )
{
	return std::clamp( logM, std::log2( reader::kMinZoom ), std::log2( reader::kMaxZoom ) );
}
} // namespace

std::vector< View > MakeViews( const reader::Card& card, double magnification, double screenW, double screenH )
{
	const double fx = card.frameW * magnification / screenW;
	const double fy = card.frameH * magnification / screenH;
	const int nx    = std::max( 1, static_cast< int >( std::ceil( fx - 0.2 ) ) );
	const int ny    = std::max( 1, static_cast< int >( std::ceil( fy - 0.2 ) ) );
	std::vector< View > out;
	out.reserve( static_cast< size_t >( card.Frames() ) * nx * ny );
	for( int r = 0; r < card.rows; ++r )
		for( int c = 0; c < card.cols; ++c )
		{
			const double left = card.FrameLeft( c ), top = card.FrameTop( r );
			for( int j = 0; j < ny; ++j )
				for( int i = 0; i < nx; ++i )
					out.push_back( { left + card.frameW * ( i + 0.5 ) / nx, top + card.frameH * ( j + 0.5 ) / ny, r * card.cols + c } );
		}
	return out;
}

//---------------------------------------------------------------------------
double Hand::uniform()
{
	const uint32_t h = Hash3( seed, draws++, kSaltDraw );
	return ( static_cast< double >( h ) + 0.5 ) * ( 1.0 / 4294967296.0 );
}

double Hand::normal()
{
	const double u1 = uniform(), u2 = uniform();
	return std::sqrt( -2.0 * std::log( u1 ) ) * std::cos( kTwoPi * u2 );
}

double Hand::Profile( double tau ) const
{
	const double x = std::clamp( tau, 0.0, 1.0 );
	if( hooks & kHookCubicProfile )
		return x * x * ( 3.0 - 2.0 * x );
	return x * x * x * ( 10.0 - 15.0 * x + 6.0 * x * x );
}

double Hand::FittsTime( const Settings& s, double distance, double width ) const
{
	const double id = ( hooks & kHookFittsNoOne ) ? std::log2( std::max( distance / width, 1.0 ) ) : std::log2( distance / width + 1.0 );
	return kFittsA + s.fittsB * id;
}

double Hand::BestFocus( const Settings& s, const reader::Bow& bow, double x, double y, double logM )
{
	return bow.At( x, y ) + reader::ParfocalShift( std::exp2( logM ), s.parfocal );
}

//---------------------------------------------------------------------------
void Hand::rebuildViews( const Settings& s, const reader::Card& card )
{
	views     = MakeViews( card, std::clamp( s.readZoom, reader::kMinZoom, reader::kMaxZoom ), s.screenW, s.screenH );
	viewsCard = card;
	viewsZoom = s.readZoom;
	viewsW    = s.screenW;
	viewsH    = s.screenH;
	current   = std::clamp( current, 0, static_cast< int >( views.size() ) - 1 );
}

void Hand::Reset( const Settings& s, const reader::Card& card, const reader::Bow& bow )
{
	started = true;
	t       = 0.0;
	seed    = s.seed;
	draws   = 0;
	queue.clear();
	log.clear();
	hunts.clear();
	current = 0;
	rebuildViews( s, card );

	manualMode = s.manual;
	hx         = manualMode ? s.manualX : views.front().x;
	hy         = manualMode ? s.manualY : views.front().y;
	cx         = hx;
	cy         = hy;
	cvx = cvy   = 0.0;
	logM        = clampLogZoom( std::log2( s.readZoom ) );
	const double offset = bow.Amplitude() * ( 2.0 * uniform() - 1.0 );
	z           = BestFocus( s, bow, hx, hy, logM ) + ( manualMode ? s.manualDefocus : offset );
	manualFromX = manualToX = hx;
	manualFromY = manualToY = hy;
	manualDt    = 0.0;

	stage       = Stage::Focus;
	lastEnd     = 0.0;
	corrections = 0;
	crashing    = false;
	pendingJump = pendingCue = false;

	history.assign( kHistoryLength, Record {} );
	historyHead = historyCount = 0;
	record();
}

//---------------------------------------------------------------------------
// Planning. Segments chain in continuous time: each starts where the last
// ends, from the values the last ends on. `push` keeps that end state.
//---------------------------------------------------------------------------
void Hand::push( const Segment& segment )
{
	queue.push_back( segment );
	if( logging )
		log.push_back( segment );
	planned.t = segment.End();
	switch( segment.kind )
	{
	case Segment::Kind::Pan:
		planned.x = segment.bx;
		planned.y = segment.by;
		break;
	case Segment::Kind::Zoom: planned.logM = segment.b; break;
	case Segment::Kind::Focus: planned.z = segment.b; break;
	case Segment::Kind::Wait: break;
	}
}

void Hand::planPan( double at, Tag tag, double tx, double ty, const Settings& s, const reader::Card& card )
{
	(void)at;
	const double ax = planned.x, ay = planned.y;
	const double dx = tx - ax, dy = ty - ay;
	const double distance = std::hypot( dx, dy );
	const double n1 = normal(), n2 = normal();
	if( distance < 1e-9 )
		return;
	const double width   = kTargetShare * s.screenW / std::exp2( planned.logM );
	const double scatter = s.noiseK * ( ( hooks & kHookFlatScatter ) ? 20.0 : distance );
	const double ux = dx / distance, uy = dy / distance;
	const double along = distance * kPrimaryGain + scatter * n1;
	const double across = kAcrossShare * scatter * n2;
	Segment seg;
	seg.kind      = Segment::Kind::Pan;
	seg.tag       = tag;
	seg.t0        = planned.t;
	seg.T         = FittsTime( s, distance, width );
	seg.ax        = ax;
	seg.ay        = ay;
	seg.bx        = std::clamp( ax + ux * along - uy * across, 0.0, card.width );
	seg.by        = std::clamp( ay + uy * along + ux * across, 0.0, card.height );
	seg.aim       = distance;
	seg.tolerance = width;
	push( seg );
}

void Hand::planZoom( double at, Tag tag, double toLogM, const Settings& s )
{
	(void)at;
	const double to    = clampLogZoom( toLogM );
	const double stops = std::fabs( to - planned.logM );
	if( stops < 1e-9 )
		return;
	Segment seg;
	seg.kind = Segment::Kind::Zoom;
	seg.tag  = tag;
	seg.t0   = planned.t;
	seg.T    = ( kZoomBase + kZoomPerStop * stops ) * ( s.fittsB / 0.1 );
	seg.a    = planned.logM;
	seg.b    = to;
	push( seg );
}

void Hand::planDwell( double at, const Settings& s )
{
	(void)at;
	Segment seg;
	seg.kind = Segment::Kind::Wait;
	seg.tag  = Tag::Dwell;
	seg.t0   = planned.t;
	// Two draws whether synced or not, so a seed's later choices do not
	// depend on Sync.
	const double u1 = uniform(), u2 = uniform();
	if( s.synced )
	{
		seg.T        = kNever;
		seg.untilCue = true;
	}
	else
		seg.T = -0.5 * s.dwell * ( std::log( u1 ) + std::log( u2 ) );// Gamma( 2 ), mean Dwell
	push( seg );
}

void Hand::planHunt( double at, const Settings& s, const reader::Bow& bow )
{
	const double M     = std::exp2( planned.logM );
	const double best  = BestFocus( s, bow, planned.x, planned.y, planned.logM );
	const double start = planned.z - best;
	const double band  = reader::DepthOfFocus( M, s.aperture );
	const double wrong = uniform();
	if( std::fabs( start ) <= band )
		return;

	Hunt hunt;
	hunt.time          = at;
	hunt.start         = start;
	hunt.band          = band;
	hunt.reaction      = 0.22 - 0.1 * std::clamp( s.skill, 0.0, 1.0 );
	hunt.gain          = 0.5 - 0.25 * std::clamp( s.skill, 0.0, 1.0 );
	hunt.speed         = std::fabs( start ) / kFirstTurn + kKnobFloor;
	hunt.magnification = M;
	hunt.fNumber       = s.aperture;
	const double rightWay = 0.5 + 0.45 * std::clamp( s.skill, 0.0, 1.0 );
	hunt.wrongWay         = wrong >= rightWay;
	const double tau      = ( hooks & kHookNoReaction ) ? 0.0 : hunt.reaction;

	auto turnTo = [ & ]( double from, double to, double speed ) {
		Segment seg;
		seg.kind = Segment::Kind::Focus;
		seg.tag  = Tag::Hunt;
		seg.t0   = planned.t;
		seg.T    = std::fabs( to - from ) / speed;
		seg.a    = best + from;
		seg.b    = best + to;
		push( seg );
	};

	double at_ = start, v = hunt.speed;
	if( hunt.wrongWay )
	{
		const double away = ( start > 0.0 ? 1.0 : -1.0 ) * ( ( 1.0 + kWeber ) * std::fabs( start ) + v * tau );
		turnTo( at_, away, v );
		hunt.turns.push_back( away );
		hunt.speeds.push_back( v );
		at_ = away;
	}
	for( int pass = 0; pass < kMaxPasses; ++pass )
	{
		const double dir = at_ > 0.0 ? -1.0 : 1.0;
		hunt.speeds.push_back( v );
		if( v * hunt.reaction <= 2.0 * band || pass == kMaxPasses - 1 )
		{
			// Slow enough to stop inside the band: a reaction time after it
			// first looks sharp.
			const double stop = dir * ( -band + v * tau );
			turnTo( at_, stop, v );
			hunt.end = stop;
			break;
		}
		const double turn = dir * ( band + v * tau );
		turnTo( at_, turn, v );
		hunt.turns.push_back( turn );
		at_ = turn;
		v *= hunt.gain;
	}
	if( logging )
		hunts.push_back( hunt );
}

bool Hand::needsCorrection() const
{
	const double width = kTargetShare * viewsW / std::exp2( planned.logM );
	return std::hypot( planned.x - targetX, planned.y - targetY ) > 0.5 * width;
}

void Hand::decide( double at, const Settings& s, const reader::Card& card, const reader::Bow& bow )
{
	planned = { at, hx, hy, logM, z };
	const double readLog = clampLogZoom( std::log2( s.readZoom ) );
	switch( stage )
	{
	case Stage::Focus:
		planHunt( at, s, bow );
		stage = Stage::Dwell;
		break;

	case Stage::Dwell:
		planDwell( at, s );
		stage = Stage::Act;
		break;

	case Stage::Act:
	{
		// Which view next.
		const int count = static_cast< int >( views.size() );
		Browse browse   = s.browse;
		const double pick = uniform();
		if( browse == Browse::Mixed )
			browse = pick < 0.15 ? Browse::Reading : pick < 0.55 ? Browse::Skimming : Browse::Searching;
		int next = current;
		if( browse == Browse::Reading )
			next = current + 1;
		else if( browse == Browse::Skimming )
		{
			int on = 1;
			while( on < 8 && uniform() < 0.5 )
				++on;
			next = current + on;
		}
		else
		{
			const double u = uniform();
			next = count > 1 ? current + 1 + std::min( static_cast< int >( u * ( count - 1 ) ), count - 2 ) : current;
		}
		current = count > 0 ? next % count : 0;
		targetX = views[ static_cast< size_t >( current ) ].x;
		targetY = views[ static_cast< size_t >( current ) ].y;

		// Back to the reading magnification if Zoom has been moved.
		const double readNoise = normal();
		if( std::fabs( planned.logM - readLog ) > 0.1 )
			planZoom( at, Tag::ZoomRead, readLog + 0.5 * s.noiseK * std::fabs( readLog - planned.logM ) * readNoise, s );

		// A long way: zoom out to travel.
		const double distance = std::hypot( targetX - planned.x, targetY - planned.y );
		const double crash    = uniform();
		const double inNoise  = normal();
		crashing              = false;
		const double M        = std::exp2( planned.logM );
		if( distance > kCrashDistance * card.frameW && crash < s.crashZoom && M / 2.0 >= reader::kMinZoom )
		{
			const double out = std::clamp( s.screenW / ( distance + 3.0 * card.frameW ), reader::kMinZoom, M / 2.0 );
			planZoom( at, Tag::ZoomOut, std::log2( out ), s );
			crashing   = true;
			travelLogM = readLog + 0.5 * s.noiseK * std::fabs( readLog - std::log2( out ) ) * inNoise;
		}
		planPan( at, Tag::Primary, targetX, targetY, s, card );
		corrections = 0;
		stage       = Stage::Travel;
		break;
	}

	case Stage::Travel:
	case Stage::Settle:
		if( corrections < kMaxCorrections && needsCorrection() )
		{
			Segment wait;
			wait.kind = Segment::Kind::Wait;
			wait.tag  = Tag::Wait;
			wait.t0   = planned.t;
			wait.T    = kReactionGap;
			push( wait );
			planPan( at, Tag::Correction, targetX, targetY, s, card );
			++corrections;
		}
		else if( stage == Stage::Travel && crashing )
		{
			planZoom( at, Tag::ZoomIn, travelLogM, s );
			crashing    = false;
			corrections = 0;
			stage       = Stage::Settle;
		}
		else
			stage = Stage::Focus;
		break;
	}
}

//---------------------------------------------------------------------------
// Execution.
//---------------------------------------------------------------------------
void Hand::applyAt( const Segment& seg, double time )
{
	const double p = Profile( seg.T > 0.0 ? ( time - seg.t0 ) / seg.T : 1.0 );
	switch( seg.kind )
	{
	case Segment::Kind::Pan:
		hx = seg.ax + ( seg.bx - seg.ax ) * p;
		hy = seg.ay + ( seg.by - seg.ay ) * p;
		break;
	case Segment::Kind::Zoom: logM = seg.a + ( seg.b - seg.a ) * p; break;
	case Segment::Kind::Focus: z = seg.a + ( seg.b - seg.a ) * p; break;
	case Segment::Kind::Wait: break;
	}
}

void Hand::applyEnd( const Segment& seg )
{
	switch( seg.kind )
	{
	case Segment::Kind::Pan:
		hx = seg.bx;
		hy = seg.by;
		break;
	case Segment::Kind::Zoom: logM = seg.b; break;
	case Segment::Kind::Focus: z = seg.b; break;
	case Segment::Kind::Wait: break;
	}
}

void Hand::advanceGrip( double h, double x0, double y0, double x1, double y1, const Settings& s )
{
	if( s.rigid )
	{
		cvx = ( x1 - x0 ) / h;
		cvy = ( y1 - y0 ) / h;
		cx  = x1;
		cy  = y1;
		return;
	}
	const double w = kTwoPi * s.gripHz, zeta = s.gripZeta;
	const double sigma = zeta * w, wd = w * std::sqrt( 1.0 - zeta * zeta );
	const double decay = std::exp( -sigma * h ), co = std::cos( wd * h ), si = std::sin( wd * h );
	auto axis = [ & ]( double& c, double& cv, double a0, double a1 ) {
		if( hooks & kHookEulerGrip )
		{
			const double acc = w * w * ( a0 - c ) - 2.0 * zeta * w * cv;
			c += cv * h;
			cv += acc * h;
			return;
		}
		// x'' = w^2 ( hand - x ) - 2 zeta w x', the hand linear over the
		// substep: with e = x - hand, e'' + 2 zeta w e' + w^2 e = -2 zeta w
		// v_hand, whose particular solution is a constant. The rest decays
		// as the damped oscillator's closed form.
		const double vh = ( a1 - a0 ) / h;
		const double ep = -2.0 * zeta * vh / w;
		const double e0 = c - a0 - ep;
		const double d0 = cv - vh;
		const double e1 = decay * ( e0 * co + ( d0 + sigma * e0 ) / wd * si );
		const double d1 = decay * ( d0 * co - ( w * w * e0 + sigma * d0 ) / wd * si );
		c               = a1 + e1 + ep;
		cv              = vh + d1;
	};
	axis( cx, cvx, x0, x1 );
	axis( cy, cvy, y0, y1 );
}

void Hand::record()
{
	history[ static_cast< size_t >( historyHead ) ] = { t, State { cx, cy, logM, z } };
	historyHead  = ( historyHead + 1 ) % kHistoryLength;
	historyCount = std::min( historyCount + 1, kHistoryLength );
}

void Hand::Advance( double dt, const Settings& s, const reader::Card& card, const reader::Bow& bow, bool jump, bool cue )
{
	if( !started || s.seed != seed || card != viewsCard )
		Reset( s, card, bow );
	if( s.readZoom != viewsZoom || s.screenW != viewsW || s.screenH != viewsH )
		rebuildViews( s, card );

	frameStart = t;
	pendingJump = pendingJump || jump;
	pendingCue  = cue;

	if( s.manual != manualMode )
	{
		manualMode = s.manual;
		queue.clear();
		if( !manualMode )
		{
			stage       = Stage::Focus;
			lastEnd     = t;
			corrections = 0;
			crashing    = false;
		}
	}
	if( manualMode )
	{
		manualFromX = hx;
		manualFromY = hy;
		manualToX   = s.manualX;
		manualToY   = s.manualY;
		manualDt    = dt;
	}

	// Plan up to now, then step. A zero dt still settles the plan (and a
	// manual hand jumps to its controls), but nothing moves in time.
	auto resolve = [ & ]( double t1 ) {
		for( int guard = 0; guard < 256; ++guard )
		{
			if( queue.empty() )
			{
				decide( lastEnd, s, card, bow );
				continue;
			}
			Segment& seg = queue.front();
			if( seg.tag == Tag::Dwell )
			{
				if( pendingJump )
				{
					seg.T        = std::max( frameStart, seg.t0 ) - seg.t0;
					seg.untilCue = false;
					pendingJump  = false;
				}
				else if( seg.untilCue && pendingCue && frameStart - seg.t0 >= kMinSyncDwell )
				{
					seg.T        = frameStart - seg.t0;
					seg.untilCue = false;
				}
			}
			if( seg.End() <= t1 )
			{
				applyEnd( seg );
				lastEnd = seg.End();
				queue.pop_front();
				continue;
			}
			applyAt( seg, t1 );
			return;
		}
	};

	if( !( dt > 0.0 ) )
	{
		if( manualMode )
		{
			hx = cx = s.manualX;
			hy = cy = s.manualY;
			cvx = cvy = 0.0;
			logM      = clampLogZoom( std::log2( s.readZoom ) );
			z         = BestFocus( s, bow, cx, cy, logM ) + s.manualDefocus;
		}
		else
			resolve( t );
		history[ static_cast< size_t >( ( historyHead + kHistoryLength - 1 ) % kHistoryLength ) ] = { t, State { cx, cy, logM, z } };
		pendingCue = false;
		return;
	}

	double remaining = dt;
	while( remaining > 0.0 )
	{
		double h = std::min( kSubstep, remaining );
		if( remaining - h < 1e-9 )
			h = remaining;
		const double t1 = t + h;
		const double x0 = hx, y0 = hy;
		if( manualMode )
		{
			const double frac = manualDt > 0.0 ? std::clamp( ( t1 - frameStart ) / manualDt, 0.0, 1.0 ) : 1.0;
			hx                = manualFromX + ( manualToX - manualFromX ) * frac;
			hy                = manualFromY + ( manualToY - manualFromY ) * frac;
			logM              = clampLogZoom( std::log2( s.readZoom ) );
		}
		else
			resolve( t1 );
		advanceGrip( h, x0, y0, hx, hy, s );
		t = t1;
		if( manualMode )
			z = BestFocus( s, bow, cx, cy, logM ) + s.manualDefocus;
		record();
		remaining -= h;
	}
	pendingCue = false;
}

//---------------------------------------------------------------------------
State Hand::Now() const
{
	return history[ static_cast< size_t >( ( historyHead + kHistoryLength - 1 ) % kHistoryLength ) ].s;
}

State Hand::At( double time ) const
{
	if( historyCount == 0 )
		return State {};
	auto rec = [ & ]( int i ) -> const Record& {
		return history[ static_cast< size_t >( ( historyHead - historyCount + i + 2 * kHistoryLength ) % kHistoryLength ) ];
	};
	if( time <= rec( 0 ).t )
		return rec( 0 ).s;
	if( time >= rec( historyCount - 1 ).t )
		return rec( historyCount - 1 ).s;
	int lo = 0, hi = historyCount - 1;
	while( hi - lo > 1 )
	{
		const int mid = ( lo + hi ) / 2;
		( rec( mid ).t <= time ? lo : hi ) = mid;
	}
	const Record& a = rec( lo );
	const Record& b = rec( hi );
	const double f  = b.t > a.t ? ( time - a.t ) / ( b.t - a.t ) : 0.0;
	return State { a.s.x + ( b.s.x - a.s.x ) * f, a.s.y + ( b.s.y - a.s.y ) * f, a.s.logM + ( b.s.logM - a.s.logM ) * f,
		           a.s.z + ( b.s.z - a.s.z ) * f };
}

void Hand::Exposure( double window, int count, State* out ) const
{
	for( int k = 0; k < count; ++k )
		out[ k ] = At( t - window + window * ( k + 0.5 ) / count );
}

} // namespace fiche::hand
