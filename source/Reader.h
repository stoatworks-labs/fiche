#pragma once

#include "Controls.h"

#include <cstdint>
#include <vector>

/**
    The microfiche reader, as geometry and optics, in millimetres. No GL.

    The card is the clip printed in a grid of frames; the screen is the output
    raster, 300 mm across; the lens between them has a magnification M and an
    f-number N. Everything here is a closed form the GLSL repeats and the
    harness measures the GPU against: where a frame is, how big a blur circle
    a defocus makes, how deep the focus is, how the card is bowed, how the
    light falls off across the screen, what colour the lamp is, what the film
    does to the light.
*/
namespace fiche::reader
{
/// The card is A6 wide. Its height follows the grid.
constexpr double kCardWidth = 148.0;
/// Clear film round the grid.
constexpr double kMargin = 4.0;
/// The eye-readable header strip across the top, with the title in it.
constexpr double kHeader = 10.0;
/// The reader's screen. The output raster is mapped onto it.
constexpr double kScreenWidth = 300.0;
/// The lens: 2x (the whole card) to 75x. A real reader's zoom covers about
/// 2:1, so this is a stretch, made so a crash zoom can reach the whole card.
constexpr double kMinZoom = 2.0;
constexpr double kMaxZoom = 75.0;
/// The magnification the lens is focused at when the zoom's focus shift is zero.
constexpr double kDesignZoom = 24.0;
/// The blur circle (diameter, on the screen) an operator accepts as sharp:
/// about what an eye resolves at reading distance.
constexpr double kAcceptableBlur = 0.4;
/// The bow's control lattice: one height every 25 mm.
constexpr double kBowSpacing = 25.0;

//---------------------------------------------------------------------------
// The card.
//
// An endless card (Layout Endless) is the grid with no header and no
// margins, repeated for ever in both directions: `width` x `height` is then
// ONE TILE of C x R cells, each a frame with half a gutter round it, and a
// point anywhere on the page is the tile's point at ( x mod width, y mod
// height ). The frames keep the size they have on the card, so a layout
// change does not change what a magnification shows.
//---------------------------------------------------------------------------
struct Card
{
	int cols = 1, rows = 1;
	double gutter = 0.0;///< mm between frames
	double aspect = 16.0 / 9.0;
	double frameW = 1.0, frameH = 1.0;///< mm
	double width = kCardWidth, height = 1.0;///< the card, or one tile of the endless page
	double gridX = kMargin, gridY = kHeader + kMargin;///< frame ( 0, 0 )'s top-left, mm
	bool endless = false;

	int Frames() const
	{
		return cols * rows;
	}
	double PitchX() const
	{
		return frameW + gutter;
	}
	double PitchY() const
	{
		return frameH + gutter;
	}
	double FrameLeft( int column ) const
	{
		return gridX + column * PitchX();
	}
	double FrameTop( int row ) const
	{
		return gridY + row * PitchY();
	}
	bool operator==( const Card& o ) const
	{
		return cols == o.cols && rows == o.rows && gutter == o.gutter && aspect == o.aspect && endless == o.endless;
	}
	bool operator!=( const Card& o ) const
	{
		return !( *this == o );
	}
};

/// C x R frames of the clip's aspect, `gutter` mm apart, across 140 mm. A
/// gutter that would leave a frame narrower than 1 mm is reduced. Endless:
/// the same frames as one tile of an endless page, half a gutter round each.
Card MakeCard( int cols, int rows, double gutterMm, double aspect, bool endless = false );

/// x reduced to [ 0, period ): where a point of the endless page falls on its tile.
double Wrap( double x, double period );

//---------------------------------------------------------------------------
// The lens.
//---------------------------------------------------------------------------
/// The radius on the FILM of the blur circle a film point `defocus` mm out of
/// focus makes, through a lens of magnification M at f-number N:
/// |delta| M / ( 2 N ( M + 1 ) ). On the screen it is M times this.
double FilmBlurRadius( double defocusMm, double magnification, double fNumber );
double ScreenBlurRadius( double defocusMm, double magnification, double fNumber );
/// The defocus at which the screen's blur circle reaches kAcceptableBlur:
/// 0.4 N ( M + 1 ) / M^2. Shallower as the magnification rises.
double DepthOfFocus( double magnification, double fNumber );
/// Where a zoom that is not parfocal moves best focus to: Parfocal mm per
/// doubling of magnification away from kDesignZoom.
double ParfocalShift( double magnification, double mmPerDoubling );
/// Natural vignetting of the projection, cos^4 of the ray's angle at a
/// screen point r mm from the axis for a throw of `throwMm`:
/// ( L^2 / ( L^2 + r^2 ) )^2. A throw of 0 means none.
double Falloff( double rMm, double throwMm );

//---------------------------------------------------------------------------
// The bow: the card is not flat between its glass plates.
//---------------------------------------------------------------------------
class Bow
{
public:
	/// Heights in [-amplitude, amplitude] on a 25 mm lattice over the card,
	/// smoothed by a uniform cubic B-spline. Seeded. On an endless card the
	/// lattice is periodic with the tile (the nearest whole number of cells
	/// to 25 mm each way, wrapped), so the page has no seam in its bow.
	void Build( const Card& card, double amplitudeMm, uint32_t seed );
	/// The film's height at card point ( x, y ), mm; positive is towards the lens.
	double At( double x, double y ) const;

	/// The field sampled every millimetre, for the GPU's bilinear lookup:
	/// sample ( i, j ) is At( i, j ). Width floor( card.width ) + 2, so a
	/// tile point in [ 0, width ) always has both neighbours.
	int SampleWidth() const
	{
		return sampleW;
	}
	int SampleHeight() const
	{
		return sampleH;
	}
	const std::vector< float >& Samples() const
	{
		return samples;
	}
	double Amplitude() const
	{
		return amplitude;
	}

private:
	int nx = 0, ny = 0;
	bool periodic = false;
	double spacingX = kBowSpacing, spacingY = kBowSpacing;
	double amplitude = 0.0;
	double width = kCardWidth, height = 1.0;
	std::vector< double > lattice;
	int sampleW = 0, sampleH = 0;
	std::vector< float > samples;
};

//---------------------------------------------------------------------------
// The lamp and the film.
//---------------------------------------------------------------------------
/// A black body at `kelvin`, seen through the CIE 1931 observer (Wyman,
/// Sloan and Shirley's multi-lobe fit), in linear sRGB, white-balanced so
/// 6504 K is exactly ( 1, 1, 1 ), then scaled so its largest channel is 1.
void LampColour( double kelvin, double rgb[ 3 ] );

/// A film stock: what it transmits where it is clear (`base`) and where it
/// is densest (`dense`), per channel, in linear light. A unit-gamma print:
/// T = dense + ( base - dense ) f, with f the exposure (or 1 - exposure on a
/// negative stock). A mono stock is exposed by Rec.709 luminance.
struct Stock
{
	bool negative;
	bool colour;
	float base[ 3 ];
	float dense[ 3 ];
};
const Stock& StockOf( Film film );

/// The sRGB transfer, both ways.
double ToLinear( double code );
double ToCode( double linear );

} // namespace fiche::reader
