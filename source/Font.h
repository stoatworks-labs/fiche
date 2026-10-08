#pragma once

#include <string>
#include <vector>

/**
    The header's lettering: a 5 x 7 dot font, the kind a computer-output
    microfilm recorder wrote titles in, as an atlas the screen pass samples.

    Each glyph sits in an 8 x 8 cell of the atlas, its 5 x 7 pixels from
    texel ( 1, 1 ), top row first, so a box mip of the atlas never bleeds one
    glyph into the next until the cell is a single texel. The mips are box
    averages built here, not by the driver.
*/
namespace fiche::font
{
/// The characters the font has, in atlas order. Anything else is a space.
extern const char* const kCharacters;
constexpr int kCell   = 8;
constexpr int kGlyphs = 64;///< atlas cells across

/// The atlas index of a character (lower case is drawn as upper case).
int GlyphOf( char c );
/// A title as atlas indices, at most `limit` characters.
std::vector< int > Encode( const std::string& title, int limit );

/// The atlas, level 0 first: 512 x 8 texels of coverage 0..255, then its box
/// mips down to 64 x 1.
struct Level
{
	int width, height;
	std::vector< unsigned char > texels;
};
std::vector< Level > BuildAtlas();

} // namespace fiche::font
