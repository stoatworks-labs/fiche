#pragma once

#include <string>

/**
	The passes.

	1. **copy** -- the clip into the live texture, by texelFetch, in LINEAR
	   light (RGBA16F), so every average after it -- the mips, the defocus,
	   the motion -- adds light, as a lens and an exposure do.
	2. **mip** -- one level of the live texture's box mip chain from the one
	   below it, by hand: glGenerateMipmap's filter is the driver's choice, and
	   a check of a blur's size cannot depend on whose driver it ran on.
	3. **film** -- a step-and-repeat camera's exposure: the live clip into one
	   layer of the filmed store (RGBA8, sRGB-coded so eight bits are enough).
	4. **mip layer** -- the store layer's box mips, averaged in linear light.
	5. **screen** -- to the host. Per pixel, the reader integrated: N taps,
	   each at its own moment of the exposure and its own point of the
	   aperture, through the card (frame or gutter, the film, dust, scratches,
	   the title), lit by the lamp, fallen off as cos^4, on a grainy screen in
	   a lit room.

	The fragment shaders are assembled from pieces at InitGL -- kVersion +
	kCommon + the pass -- by `Assemble()`, the one place the order is written.
	tools/glslc.sh's ASSEMBLED table mirrors it, so the text it compiles is
	the text the plugin runs.
*/
namespace fiche::shaders
{

extern const char* const kVersion;
extern const char* const kQuadVertex;
extern const char* const kCommon;
extern const char* const kCopyFragment;
extern const char* const kMipFragment;
extern const char* const kFilmFragment;
extern const char* const kMipLayerFragment;
extern const char* const kScreenFragment;

enum class Pass
{
	Copy,
	Mip,
	Film,
	MipLayer,
	Screen
};
std::string Assemble( Pass pass );

/// The most taps a pixel takes, and the exposure's states.
constexpr int kMaxTaps   = 32;
constexpr int kMaxStates = 17;
/// The most characters of the title the header holds.
constexpr int kMaxTitle = 40;

/// Negative-control hooks: bits of the `Hooks` uniform. A shipped instance
/// sets 0, and every branch they guard is then the shipped model. Each one
/// exists so a harness check can be shown to fail against a wrong model.
enum Hook : int
{
	kHookNoPlusOne    = 1 << 0,///< the blur circle without the ( M + 1 ): a thin lens at infinity
	kHookLinearDisc   = 1 << 1,///< aperture taps at radius ( j + 0.5 ) / N: a cone, not a disc
	kHookMagnify      = 1 << 2,///< the lens 2% stronger than it says
	kHookCubeFalloff  = 1 << 3,///< cos^3 instead of cos^4
	kHookGrainOnCard  = 1 << 4,///< the screen's grain moves with the card
	kHookNoNegative   = 1 << 5,///< a negative stock printed positive
	kHookBowFlip      = 1 << 6,///< the bow read with the wrong sign
	kHookColumnOrder  = 1 << 7,///< the step-and-repeat camera fills columns first
	kHookDustOnScreen = 1 << 8,///< dust that stays on the screen
	kHookNaiveCover   = 1 << 9,///< box coverage as min - max of absolute card positions: cancels in float32
	kHookPlainBox     = 1 << 10///< the clip's mips a plain 2 x 2 box, dropping an odd last row
};

} // namespace fiche::shaders
