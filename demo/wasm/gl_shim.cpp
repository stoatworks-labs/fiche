/*
    The GL entry points emscripten's WebGL2 library cannot take from the plugin
    as they are: glShaderSource, glEnable, glDisable, glIsEnabled, glTexImage2D
    and glTexImage3D. Every other GL call the plugin and the FFGL SDK make goes
    straight to emscripten's implementation, on the page's own WebGL2 context.

    glShaderSource
        The plugin hands GL desktop GLSL 4.10 (`#version 410 core`); WebGL2
        compiles GLSL ES 3.00. The page's `ficheShaderSource` first REQUIRES
        the text to be byte for byte one of the page's own assemblies of the
        plugin's shaders (demo/shaders.js, which demo/tools/check_shaders.py
        holds to source/Shaders.cpp, and its ASSEMBLY table to Assemble()),
        then applies the demo kit's `port()`: the version line and the ES
        precision defaults, nothing else. A shader the plugin assembled
        differently from the page's copy is refused (an empty source, so the
        plugin's InitGL fails and says which pass), never compiled quietly.

    glEnable / glDisable / glIsEnabled
        GL_PROGRAM_POINT_SIZE is desktop-only state (in ES a point is always
        sized by the shader, and WebGL2 rejects the enum with INVALID_ENUM).
        The plugin's GLState.h saves and restores it on every frame, as a
        plugin inside Resolume must. Here it reads as off and enabling it is
        ignored; the plugin draws no points. Every other capability is passed
        through unchanged.

    glTexImage2D / glTexImage3D, for a render target
        The plugin allocates the clip's RGBA16F texture and the filmed store
        (a 2D array of RGBA8) level by level, with no data, and writes their
        mips by hand: it sets BASE = MAX = the level below and renders into the
        next (Fiche.cpp, ensureLive / ensureStore and the mip passes). GL 4.1
        allows that. WebGL2 is OpenGL ES 3.0, where a level of a MUTABLE
        texture is a complete framebuffer attachment only inside [BASE, MAX]
        (ES 3.0.6, 4.4.4.1), so every mip draw fails with
        INVALID_FRAMEBUFFER_OPERATION and leaves the levels black -- found by
        comparing the page with mftest, where it showed as a picture 0.6 times
        as bright. An immutable texture is not bound by that rule. So a level-0
        allocation with no data becomes glTexStorage2D / glTexStorage3D of the
        full chain (floor( log2( max( w, h ) ) ) + 1 levels, the count Fiche.cpp's
        levelsFor gives), in the same format and size, and the plugin's own
        allocations of the levels above it are checked against that storage and
        skipped. Anything that does not fit the storage throws, so a change in
        the plugin cannot be absorbed quietly. Allocations WITH data (the bow,
        the title's atlas, the 1 x 1 x 1 stand-in store) pass through as they
        are.

    glTexImage2D, for the bow
        One more exception. The card's bow is an
        R32F texture the plugin samples with GL_LINEAR, and a WebGL2 context
        filters a 32-bit float texture only with OES_texture_float_linear.
        Without it the texture is incomplete, every read of it is 0, and the
        card would look flat: a plausible wrong picture. So where the browser
        lacks that extension -- and only then -- the R32F upload is made R16F
        (core-filterable in WebGL2) from the same float data, and the page says
        which one this browser got. The bow is at most 0.5 mm, so a half
        float holds it to about 0.0002 mm.
*/
#include <GLES3/gl3.h>
#include <emscripten/emscripten.h>

#include <cstring>
#include <string>

extern "C" void emscripten_glTexImage2D( GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border,
                                         GLenum format, GLenum type, const void* pixels );
extern "C" void emscripten_glTexImage3D( GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLsizei depth,
                                         GLint border, GLenum format, GLenum type, const void* pixels );

namespace
{
constexpr GLenum kProgramPointSize = 0x8642;///< GL_PROGRAM_POINT_SIZE, desktop GL 3.2

// clang-format off
EM_JS( void, ficheShaderSource, ( GLuint shader, const char* source ), {
	const object = GL.shaders[ shader ];
	const stage  = GLctx.getShaderParameter( object, 0x8B4F /* GL_SHADER_TYPE */ ) === 0x8B31 ? 'vertex' : 'fragment';
	GLctx.shaderSource( object, Module[ 'ficheShaderSource' ]( stage, UTF8ToString( source ) ) );
} );
EM_JS( void, ficheEnable, ( GLenum cap ), { GLctx.enable( cap ); } );
EM_JS( void, ficheDisable, ( GLenum cap ), { GLctx.disable( cap ); } );
EM_JS( int, ficheIsEnabled, ( GLenum cap ), { return GLctx.isEnabled( cap ) ? 1 : 0; } );
// Whether this context filters 32-bit float textures. getExtension also turns
// it on, which the kit has already done; asking again is harmless. The answer
// is left on Module for the page to report.
EM_JS( int, ficheFloatLinear, (), {
	const ok = GLctx.getExtension( 'OES_texture_float_linear' ) !== null;
	Module[ 'ficheBowFormat' ] = ok ? 'R32F' : 'R16F';
	return ok ? 1 : 0;
} );
// A data-less allocation of a TEXTURE_2D or TEXTURE_2D_ARRAY level, made
// immutable storage of the whole chain at level 0 and checked against it
// above. 1 when handled here, 0 to pass the call through. The storage is
// recorded on the WebGLTexture object, which the plugin deletes and
// regenerates on every reallocation.
EM_JS( int, ficheStorage, ( GLenum target, GLint level, GLint internalformat, GLsizei w, GLsizei h, GLsizei d ), {
	const binding = target === 0x0DE1 ? 0x8069 /* TEXTURE_BINDING_2D */ : target === 0x8C1A ? 0x8C1D /* TEXTURE_BINDING_2D_ARRAY */ : 0;
	if( !binding )
		return 0;
	const texture = GLctx.getParameter( binding );
	if( !texture )
		return 0;
	const name = target === 0x0DE1 ? 'glTexImage2D' : 'glTexImage3D';
	if( level === 0 )
	{
		if( texture.ficheStorage )
			throw new Error( `${name}: level 0 respecified on a texture already given immutable storage (demo/wasm/gl_shim.cpp)` );
		const levels = Math.floor( Math.log2( Math.max( w, h ) ) ) + 1;
		if( target === 0x0DE1 )
			GLctx.texStorage2D( target, levels, internalformat, w, h );
		else
			GLctx.texStorage3D( target, levels, internalformat, w, h, d );
		texture.ficheStorage = { levels, internalformat, w, h, d };
		Module[ 'ficheStorageCount' ] = ( Module[ 'ficheStorageCount' ] || 0 ) + 1;
		return 1;
	}
	const s = texture.ficheStorage;
	if( !s )
		return 0;
	if( level < s.levels && internalformat === s.internalformat && w === Math.max( 1, s.w >> level ) && h === Math.max( 1, s.h >> level ) && d === s.d )
		return 1;
	throw new Error( `${name}: level ${level} at ${w} x ${h} x ${d} does not fit the immutable storage made at level 0 (${s.w} x ${s.h} x ${s.d}, ${s.levels} levels) -- demo/wasm/gl_shim.cpp` );
} );
// clang-format on
} // namespace

extern "C"
{
void glShaderSource( GLuint shader, GLsizei count, const GLchar* const* string, const GLint* length )
{
	std::string source;
	for( GLsizei i = 0; i < count; ++i )
	{
		if( string[ i ] == nullptr )
			continue;
		const size_t n = ( length != nullptr && length[ i ] >= 0 ) ? static_cast< size_t >( length[ i ] ) : std::strlen( string[ i ] );
		source.append( string[ i ], n );
	}
	ficheShaderSource( shader, source.c_str() );
}

void glEnable( GLenum cap )
{
	if( cap != kProgramPointSize )
		ficheEnable( cap );
}

void glDisable( GLenum cap )
{
	if( cap != kProgramPointSize )
		ficheDisable( cap );
}

GLboolean glIsEnabled( GLenum cap )
{
	if( cap == kProgramPointSize )
		return GL_FALSE;
	return ficheIsEnabled( cap ) ? GL_TRUE : GL_FALSE;
}

void glTexImage2D( GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format,
                   GLenum type, const void* pixels )
{
	if( pixels == nullptr && ficheStorage( target, level, internalformat, width, height, 1 ) )
		return;
	if( internalformat == GL_R32F && !ficheFloatLinear() )
		internalformat = GL_R16F;
	emscripten_glTexImage2D( target, level, internalformat, width, height, border, format, type, pixels );
}

void glTexImage3D( GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLsizei depth, GLint border,
                   GLenum format, GLenum type, const void* pixels )
{
	if( pixels == nullptr && ficheStorage( target, level, internalformat, width, height, depth ) )
		return;
	emscripten_glTexImage3D( target, level, internalformat, width, height, depth, border, format, type, pixels );
}
} // extern "C"
