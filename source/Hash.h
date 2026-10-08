#pragma once

#include <cstdint>

namespace fiche
{
/// The PCG output mix, the same integer arithmetic as the GLSL's pcg().
inline uint32_t Pcg( uint32_t v )
{
	const uint32_t state = v * 747796405u + 2891336453u;
	const uint32_t word  = ( ( state >> ( ( state >> 28u ) + 4u ) ) ^ state ) * 277803737u;
	return ( word >> 22u ) ^ word;
}

/// The GLSL's hash3( a, b, c ) and hash4( a, b, c, d ).
inline uint32_t Hash3( uint32_t a, uint32_t b, uint32_t c )
{
	return Pcg( a ^ Pcg( b ^ Pcg( c ) ) );
}
inline uint32_t Hash4( uint32_t a, uint32_t b, uint32_t c, uint32_t d )
{
	return Pcg( a ^ Pcg( b ^ Pcg( c ^ Pcg( d ) ) ) );
}

/// The GLSL's pack2( ivec2 ).
inline uint32_t Pack2( int x, int y )
{
	return static_cast< uint32_t >( x ) | ( static_cast< uint32_t >( y ) << 16u );
}

} // namespace fiche
