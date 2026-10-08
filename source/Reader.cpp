#include "Reader.h"

#include "Hash.h"

#include <algorithm>
#include <cmath>

namespace fiche::reader
{
Card MakeCard( int cols, int rows, double gutterMm, double aspect )
{
	Card card;
	card.cols   = std::max( cols, 1 );
	card.rows   = std::max( rows, 1 );
	card.aspect = aspect > 0.0 ? aspect : 16.0 / 9.0;
	const double span = kCardWidth - 2.0 * kMargin;
	double gutter     = std::max( gutterMm, 0.0 );
	if( card.cols > 1 )
		gutter = std::min( gutter, ( span - card.cols * 1.0 ) / ( card.cols - 1 ) );
	card.gutter = gutter;
	card.frameW = ( span - ( card.cols - 1 ) * gutter ) / card.cols;
	card.frameH = card.frameW / card.aspect;
	card.width  = kCardWidth;
	card.gridX  = kMargin;
	card.gridY  = kHeader + kMargin;
	card.height = card.gridY + card.rows * card.frameH + ( card.rows - 1 ) * gutter + kMargin;
	return card;
}

double FilmBlurRadius( double defocusMm, double magnification, double fNumber )
{
	return std::fabs( defocusMm ) * magnification / ( 2.0 * fNumber * ( magnification + 1.0 ) );
}
double ScreenBlurRadius( double defocusMm, double magnification, double fNumber )
{
	return magnification * FilmBlurRadius( defocusMm, magnification, fNumber );
}
double DepthOfFocus( double magnification, double fNumber )
{
	return kAcceptableBlur * fNumber * ( magnification + 1.0 ) / ( magnification * magnification );
}
double ParfocalShift( double magnification, double mmPerDoubling )
{
	return mmPerDoubling * std::log2( magnification / kDesignZoom );
}
double Falloff( double rMm, double throwMm )
{
	if( !( throwMm > 0.0 ) )
		return 1.0;
	const double q = throwMm * throwMm / ( throwMm * throwMm + rMm * rMm );
	return q * q;
}

//---------------------------------------------------------------------------
namespace
{
/// The uniform cubic B-spline's four weights at fraction t.
void bspline( double t, double w[ 4 ] )
{
	const double t2 = t * t, t3 = t2 * t;
	w[ 0 ] = ( 1.0 - t ) * ( 1.0 - t ) * ( 1.0 - t ) / 6.0;
	w[ 1 ] = ( 3.0 * t3 - 6.0 * t2 + 4.0 ) / 6.0;
	w[ 2 ] = ( -3.0 * t3 + 3.0 * t2 + 3.0 * t + 1.0 ) / 6.0;
	w[ 3 ] = t3 / 6.0;
}
constexpr uint32_t kSaltBow = 0x6b6f7742u;
} // namespace

void Bow::Build( const Card& card, double amplitudeMm, uint32_t seed )
{
	amplitude = std::max( amplitudeMm, 0.0 );
	width     = card.width;
	height    = card.height;
	nx        = static_cast< int >( std::ceil( width / kBowSpacing ) ) + 4;
	ny        = static_cast< int >( std::ceil( height / kBowSpacing ) ) + 4;
	lattice.assign( static_cast< size_t >( nx ) * ny, 0.0 );
	for( int j = 0; j < ny; ++j )
		for( int i = 0; i < nx; ++i )
		{
			const uint32_t h = Hash3( Pack2( i, j ), seed, kSaltBow );
			lattice[ static_cast< size_t >( j ) * nx + i ] = amplitude * ( 2.0 * ( ( h >> 8 ) * ( 1.0 / 16777216.0 ) ) - 1.0 );
		}

	sampleW = static_cast< int >( std::floor( width ) ) + 2;
	sampleH = static_cast< int >( std::floor( height ) ) + 2;
	samples.assign( static_cast< size_t >( sampleW ) * sampleH, 0.0f );
	for( int j = 0; j < sampleH; ++j )
		for( int i = 0; i < sampleW; ++i )
			samples[ static_cast< size_t >( j ) * sampleW + i ] = static_cast< float >( At( i, j ) );
}

double Bow::At( double x, double y ) const
{
	if( nx == 0 || amplitude == 0.0 )
		return 0.0;
	const double u = std::clamp( x, 0.0, width ) / kBowSpacing + 1.0;
	const double v = std::clamp( y, 0.0, height ) / kBowSpacing + 1.0;
	const int i0 = static_cast< int >( std::floor( u ) ), j0 = static_cast< int >( std::floor( v ) );
	double wu[ 4 ], wv[ 4 ];
	bspline( u - i0, wu );
	bspline( v - j0, wv );
	double sum = 0.0;
	for( int b = 0; b < 4; ++b )
		for( int a = 0; a < 4; ++a )
		{
			const int i = std::clamp( i0 - 1 + a, 0, nx - 1 ), j = std::clamp( j0 - 1 + b, 0, ny - 1 );
			sum += wu[ a ] * wv[ b ] * lattice[ static_cast< size_t >( j ) * nx + i ];
		}
	return sum;
}

//---------------------------------------------------------------------------
namespace
{
/// One lobe of the multi-lobe fit: a Gaussian with a different width either
/// side of its peak.
double lobe( double lambda, double mu, double below, double above )
{
	const double s = ( lambda - mu ) / ( lambda < mu ? below : above );
	return std::exp( -0.5 * s * s );
}

/// Wyman, Sloan and Shirley (2013), "Simple Analytic Approximations to the
/// CIE XYZ Color Matching Functions", the multi-lobe fit.
void observer( double lambda, double xyz[ 3 ] )
{
	xyz[ 0 ] = 1.056 * lobe( lambda, 599.8, 37.9, 31.0 ) + 0.362 * lobe( lambda, 442.0, 16.0, 26.7 )
	         - 0.065 * lobe( lambda, 501.1, 20.4, 26.2 );
	xyz[ 1 ] = 0.821 * lobe( lambda, 568.8, 46.9, 40.5 ) + 0.286 * lobe( lambda, 530.9, 16.3, 31.1 );
	xyz[ 2 ] = 1.217 * lobe( lambda, 437.0, 11.8, 36.0 ) + 0.681 * lobe( lambda, 459.0, 26.0, 13.8 );
}

void blackBodyRgb( double kelvin, double rgb[ 3 ] )
{
	constexpr double c2 = 1.4387769e7;// nm K
	double X = 0.0, Y = 0.0, Z = 0.0;
	for( double lambda = 380.0; lambda <= 780.0; lambda += 1.0 )
	{
		const double planck = 1.0 / ( std::pow( lambda * 1e-3, 5.0 ) * ( std::exp( c2 / ( lambda * kelvin ) ) - 1.0 ) );
		double xyz[ 3 ];
		observer( lambda, xyz );
		X += planck * xyz[ 0 ];
		Y += planck * xyz[ 1 ];
		Z += planck * xyz[ 2 ];
	}
	rgb[ 0 ] = 3.2406 * X - 1.5372 * Y - 0.4986 * Z;
	rgb[ 1 ] = -0.9689 * X + 1.8758 * Y + 0.0415 * Z;
	rgb[ 2 ] = 0.0557 * X - 0.2040 * Y + 1.0570 * Z;
}
} // namespace

void LampColour( double kelvin, double rgb[ 3 ] )
{
	double white[ 3 ], lamp[ 3 ];
	blackBodyRgb( 6504.0, white );
	blackBodyRgb( kelvin, lamp );
	double top = 0.0;
	for( int c = 0; c < 3; ++c )
	{
		rgb[ c ] = std::max( lamp[ c ] / white[ c ], 0.0 );
		top      = std::max( top, rgb[ c ] );
	}
	for( int c = 0; c < 3; ++c )
		rgb[ c ] = top > 0.0 ? rgb[ c ] / top : 1.0;
}

const Stock& StockOf( Film film )
{
	// Linear transmittances. Silver: base density 0.07, maximum 2.2, neutral.
	// Diazo: a clear polyester base with a blue-violet (or blue-black) dye.
	// Vesicular: a tan base, and a "density" that is light scattered out of
	// the lens's cone, so its blacks are soft (about 1.3). Colour: a
	// slightly warm base. Ideal: clear and black, the clip itself.
	static const Stock stocks[ static_cast< int >( Film::Count ) ] = {
		{ false, true, { 1.0f, 1.0f, 1.0f }, { 0.0f, 0.0f, 0.0f } },
		{ false, false, { 0.85f, 0.85f, 0.85f }, { 0.0063f, 0.0063f, 0.0063f } },
		{ true, false, { 0.85f, 0.85f, 0.85f }, { 0.0063f, 0.0063f, 0.0063f } },
		{ false, false, { 0.90f, 0.91f, 0.89f }, { 0.006f, 0.02f, 0.12f } },
		{ false, false, { 0.90f, 0.91f, 0.90f }, { 0.012f, 0.008f, 0.022f } },
		{ false, false, { 0.80f, 0.70f, 0.48f }, { 0.06f, 0.05f, 0.04f } },
		{ false, true, { 0.82f, 0.80f, 0.77f }, { 0.004f, 0.004f, 0.006f } },
	};
	return stocks[ std::clamp( static_cast< int >( film ), 0, static_cast< int >( Film::Count ) - 1 ) ];
}

double ToLinear( double code )
{
	return code <= 0.04045 ? code / 12.92 : std::pow( ( code + 0.055 ) / 1.055, 2.4 );
}
double ToCode( double linear )
{
	return linear <= 0.0031308 ? 12.92 * linear : 1.055 * std::pow( linear, 1.0 / 2.4 ) - 0.055;
}

} // namespace fiche::reader
