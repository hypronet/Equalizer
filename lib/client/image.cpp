
/* Copyright (c) 2006-2010, Stefan Eilemann <eile@equalizergraphics.com>
 *                    2010, Cedric Stalder <cedric.stalder@gmail.com>
 *
 * This library is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License version 2.1 as published
 * by the Free Software Foundation.
 *  
 * This library is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for more
 * details.
 * 
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "image.h"

#include "frame.h"
#include "frameData.h"
#include "log.h"
#include "windowSystem.h"

#include <eq/util/frameBufferObject.h>
#include <eq/util/objectManager.h>

#include <eq/fabric/colorMask.h>
#include <eq/net/node.h>

#include <eq/base/global.h>
#include <eq/base/memoryMap.h>
#include <eq/base/omp.h>
#include <eq/base/pluginRegistry.h>
#include <eq/base/compressorDataCPU.h>  // member
#include <eq/util/compressorDataGPU.h>  // member

#include <fstream>

#ifdef WIN32
#  include <malloc.h>
#  define bzero( ptr, size ) memset( ptr, 0, size );
#else
#  include <alloca.h>
#endif

namespace eq
{
#define glewGetContext glObjects->glewGetContext

Image::Image()
        : _type( Frame::TYPE_MEMORY )
{
    reset();
}

Image::~Image(){}

void Image::reset()
{
    _ignoreAlpha = false;
    setPixelViewport( PixelViewport( ));
}

void Image::flush()
{
    _color.flush();
    _depth.flush();
}

void Image::Attachment::flush()
{
    memory.flush();
    texture.flush();
    fullCompressor.reset();
    lossyCompressor.reset();
    fullTransfer.reset();
    lossyTransfer.reset();
}

uint32_t Image::getPixelDataSize( const Frame::Buffer buffer ) const
{
    const Memory& memory = _getMemory( buffer );
    return memory.pvp.getArea() * memory.pixelSize;
}


void Image::_setExternalFormat( const Frame::Buffer buffer,
                                const uint32_t externalFormat,
                                const uint32_t pixelSize )
{
    Memory& memory = _getMemory( buffer );
    if( memory.externalFormat == externalFormat )
        return;

    memory.externalFormat = externalFormat;
    memory.pixelSize = pixelSize;
    memory.state = Memory::INVALID;
}

void Image::setInternalFormat( const Frame::Buffer buffer, 
                               const uint32_t internalFormat ) 
{
    Memory& memory = _getMemory( buffer );
    if( memory.internalFormat == internalFormat )
        return;

    memory.internalFormat = internalFormat;
    allocCompressor( buffer, EQ_COMPRESSOR_INVALID );
    if ( internalFormat == 0 )
        return;

    _getAttachment( buffer ).texture.setInternalFormat( internalFormat );
}

uint32_t Image::getInternalFormat( const Frame::Buffer buffer ) const
{
    const Memory& memory = _getAttachment( buffer ).memory;
    EQASSERT( memory.internalFormat );
    return memory.internalFormat;
}

bool Image::_canIgnoreAlpha( const Frame::Buffer buffer ) const
{ 
    const Attachment& attachment = _getAttachment( buffer );
    
    EQASSERT( attachment.transfer );
    if ( attachment.transfer->isValid( attachment.transfer->getName()))
        return buffer == Frame::BUFFER_COLOR && _ignoreAlpha 
                     && !attachment.transfer->ignoreAlpha();
    else
        return buffer == Frame::BUFFER_COLOR && _ignoreAlpha;
}

std::vector< uint32_t > Image::findCompressors( const Frame::Buffer buffer )
    const
{
    const uint32_t tokenType = getExternalFormat( buffer );
    EQINFO << "Searching compressors for token type 0x" << std::hex << tokenType
           << std::dec << std::endl;

    const base::PluginRegistry& registry = base::Global::getPluginRegistry();
    const base::Compressors& compressors = registry.getCompressors();
    std::vector< uint32_t > names;

    for( base::Compressors::const_iterator i = compressors.begin();
         i != compressors.end(); ++i )
    {
        const base::Compressor* compressor = *i;
        const base::CompressorInfos& infos = compressor->getInfos();

        EQINFO << "Searching in DSO " << (void*)compressor << std::endl;
        for( base::CompressorInfos::const_iterator j = infos.begin();
             j != infos.end(); ++j )
        {
            const EqCompressorInfo& info = *j;

            if( info.capabilities & EQ_COMPRESSOR_TRANSFER )
                continue;

            if( info.tokenType == tokenType )
                names.push_back( info.name );
        }
    }

    return names;
}

uint32_t Image::_getCompressorName( const Frame::Buffer buffer ) const
{
    const Attachment& attachment = _getAttachment( buffer );
    if( !attachment.memory.externalFormat )
        return EQ_COMPRESSOR_NONE;

    const uint32_t tokenType = getExternalFormat( buffer );
    const bool noAlpha = _canIgnoreAlpha( buffer );

    return base::CompressorDataCPU::chooseCompressor( tokenType,
                                                      attachment.quality,
                                                      noAlpha );
}

bool Image::hasAlpha() const
{
    // only the base type has an alpha channel which can be usable
    switch( getExternalFormat( Frame::BUFFER_COLOR ))
    {
        case EQ_COMPRESSOR_DATATYPE_RGB10_A2:
        case EQ_COMPRESSOR_DATATYPE_BGR10_A2:
        case EQ_COMPRESSOR_DATATYPE_RGBA16F:
        case EQ_COMPRESSOR_DATATYPE_RGBA32F:
        case EQ_COMPRESSOR_DATATYPE_BGRA16F:
        case EQ_COMPRESSOR_DATATYPE_BGRA32F:
        case EQ_COMPRESSOR_DATATYPE_RGBA_UINT_8_8_8_8_REV:
        case EQ_COMPRESSOR_DATATYPE_RGBA:
        case EQ_COMPRESSOR_DATATYPE_BGRA:
            return true;

        default:
            return false;
    }
}

bool Image::hasData( const Frame::Buffer buffer ) const
{
    if( _type == Frame::TYPE_MEMORY )
        return hasPixelData( buffer );

    EQASSERT( _type == Frame::TYPE_TEXTURE );
    return hasTextureData( buffer );
}

void Image::enableAlphaUsage()
{
    if( !_ignoreAlpha )
        return;

    _ignoreAlpha = false;
    _color.memory.isCompressed = false;
    _depth.memory.isCompressed = false;
}    

void Image::disableAlphaUsage()
{
    if( _ignoreAlpha )
        return;

    _ignoreAlpha = true;
    _color.memory.isCompressed = false;
    _depth.memory.isCompressed = false;
}

void Image::setQuality( const Frame::Buffer buffer, const float quality )
{
    Attachment& attachment = _getAttachment( buffer );
    if( attachment.quality == quality )
        return;
    
    attachment.quality = quality;
    if( quality == 1.f )
    {
        attachment.compressor = &attachment.fullCompressor;
        attachment.transfer = &attachment.fullTransfer;
    }
    else
    {
        attachment.lossyCompressor.reset();
        attachment.lossyTransfer.reset();
        attachment.compressor = &attachment.lossyCompressor;
        attachment.transfer = &attachment.lossyTransfer;
    }
}

bool Image::hasTextureData( const Frame::Buffer buffer ) const
{
    return getTexture( buffer ).isValid(); 
}

const util::Texture& Image::getTexture( const Frame::Buffer buffer ) const
{
    return _getAttachment( buffer ).texture;
}

const uint8_t* Image::getPixelPointer( const Frame::Buffer buffer ) const
{
    EQASSERT( hasPixelData( buffer ));
    return reinterpret_cast< const uint8_t* >
          ( _getAttachment( buffer ).memory.pixels );
}

uint8_t* Image::getPixelPointer( const Frame::Buffer buffer )
{
    EQASSERT( hasPixelData( buffer ));
    return  reinterpret_cast< uint8_t* >
          ( _getAttachment( buffer ).memory.pixels );
}

const Image::PixelData& Image::getPixelData( const Frame::Buffer buffer ) const
{
    EQASSERT(hasPixelData(buffer));
    return _getAttachment( buffer ).memory;
}

void Image::readbackFromTexture(  const Frame::Buffer buffer, 
                                  const PixelViewport& pvp,
                                  uint32_t texture,
                                  GLEWContext* glewContext )
{
    _download( buffer,  
               EQ_COMPRESSOR_DATA_2D | EQ_COMPRESSOR_USE_TEXTURE,
               texture, glewContext );
}

void Image::uploadToTexture( const Frame::Buffer buffer, 
                             uint32_t texture,
                             util::ObjectManager< const void* >* glObjects ) const
{
    util::CompressorDataGPU* uploader = glObjects->obtainEqUploader(
                                            _getCompressorKey( buffer ));

    const Image::PixelData& pixelData = getPixelData( buffer );
    const uint32_t inputToken = pixelData.externalFormat;
    const uint32_t outputToken = pixelData.internalFormat;

    if ( !uploader->isValidUploader( inputToken, outputToken ) )
        uploader->initUploader( inputToken, outputToken );

    uploader->upload( pixelData.pixels,
                      pixelData.pvp,
                      EQ_COMPRESSOR_USE_TEXTURE, 
                      getPixelViewport(), texture );
}

void Image::uploadToTexture( const Frame::Buffer buffer, 
                             uint32_t texture,
                             GLEWContext* const glewContext ) const
{
    util::CompressorDataGPU uploader = util::CompressorDataGPU( glewContext );

    const Image::PixelData& pixelData = getPixelData( buffer );
    const uint32_t inputToken = pixelData.externalFormat;
    const uint32_t outputToken = pixelData.internalFormat;

    if( !uploader.isValidUploader( inputToken, outputToken ))
        uploader.initUploader( inputToken, outputToken );

    uploader.upload( pixelData.pixels,
                     pixelData.pvp,
                     EQ_COMPRESSOR_USE_TEXTURE, 
                     getPixelViewport(), texture );
}

void Image::upload( const Frame::Buffer buffer, 
                    const Vector2i offset,
                    util::ObjectManager< const void* >* glObjects ) const
{
    util::CompressorDataGPU* uploader = glObjects->obtainEqUploader(
                                            _getCompressorKey( buffer ));

    const Image::PixelData& pixelData = getPixelData( buffer );
    const uint32_t inputToken = pixelData.externalFormat;
    const uint32_t outputToken = pixelData.internalFormat;

    if( !uploader->isValidUploader( inputToken, outputToken ))
        uploader->initUploader( inputToken, outputToken );
    
    PixelViewport pvp = getPixelViewport();
    pvp.x = offset.x() + pvp.x; 
    pvp.y = offset.y() + pvp.y;
    uploader->upload( pixelData.pixels, pixelData.pvp, 
                      EQ_COMPRESSOR_USE_FRAMEBUFFER, pvp, 0 );
}

void Image::readback( const uint32_t buffers, const PixelViewport& pvp,
                      const Zoom& zoom, 
                      util::ObjectManager< const void* >* glObjects )
{
    EQASSERT( glObjects );
    EQLOG( LOG_ASSEMBLY ) << "startReadback " << pvp << ", buffers " << buffers
                          << std::endl;

    _pvp = pvp;
    _color.memory.state = Memory::INVALID;
    _depth.memory.state = Memory::INVALID;

    if( buffers & Frame::BUFFER_COLOR )
        _readback( Frame::BUFFER_COLOR, zoom, glObjects );

    if( buffers & Frame::BUFFER_DEPTH )
        _readback( Frame::BUFFER_DEPTH, zoom, glObjects );

    _pvp.x = 0;
    _pvp.y = 0;
}

const void* Image::_getBufferKey( const Frame::Buffer buffer ) const
{
    switch( buffer )
    {
        case Frame::BUFFER_COLOR:
            return ( reinterpret_cast< const char* >( this ) + 0 );
        case Frame::BUFFER_DEPTH:
            return ( reinterpret_cast< const char* >( this ) + 1 );
        default:
            EQUNIMPLEMENTED;
            return ( reinterpret_cast< const char* >( this ) + 2 );
    }
}

const void* Image::_getCompressorKey( const Frame::Buffer buffer ) const
{
    const Attachment& attachment = _getAttachment( buffer );
    
    switch( buffer )
    {
        case Frame::BUFFER_COLOR:
            if( attachment.quality == 1.0f )
                return ( reinterpret_cast< const char* >( this ) + 0 );
            else
                return ( reinterpret_cast< const char* >( this ) + 1 );
        case Frame::BUFFER_DEPTH:
            if( attachment.quality == 1.0f )
                return ( reinterpret_cast< const char* >( this ) + 2 );
            else
                return ( reinterpret_cast< const char* >( this ) + 3 );
        default:
            EQUNIMPLEMENTED;
            return ( reinterpret_cast< const char* >( this ) + 0 );
    }
}
void Image::_readback( const Frame::Buffer buffer, const Zoom& zoom,
                       util::ObjectManager< const void* >* glObjects )
{
    Attachment& attachment = _getAttachment( buffer );
    attachment.memory.isCompressed = false;

    if( _type == Frame::TYPE_TEXTURE )
    {
        EQASSERTINFO( zoom == Zoom::NONE, "Texture readback zoom not "
                      << "implemented, zoom happens during compositing" );
        _readbackTexture( buffer, glObjects );
    }
    else if( zoom == Zoom::NONE ) // normal glReadPixels
        _download( buffer, 
               EQ_COMPRESSOR_DATA_2D | EQ_COMPRESSOR_USE_FRAMEBUFFER, 
               0, glewGetContext() );
    else // copy to texture, draw zoomed quad into FBO, (read FBO texture)
        _readbackZoom( buffer, zoom, glObjects );
}

void Image::_readbackTexture( const Frame::Buffer buffer,
                              util::ObjectManager< const void* >* glObjects )
{
    util::Texture& texture = _getAttachment( buffer ).texture;
    texture.setGLEWContext( glewGetContext( ));
    texture.copyFromFrameBuffer( _pvp );
    texture.setGLEWContext( 0 );
}

void Image::_readbackZoom( const Frame::Buffer buffer, const Zoom& zoom,
                           util::ObjectManager< const void* >* glObjects )
{
    EQASSERT( glObjects );
    EQASSERT( glObjects->supportsEqTexture( ));
    EQASSERT( glObjects->supportsEqFrameBufferObject( ));

    PixelViewport pvp = _pvp;
    pvp.apply( zoom );
    if( !pvp.hasArea( ))
        return;

    // copy frame buffer to texture
    const void* bufferKey = _getBufferKey( buffer );
    util::Texture* texture = glObjects->obtainEqTexture( bufferKey );

    texture->setInternalFormat( getInternalFormat( buffer ) );
    texture->copyFromFrameBuffer( _pvp );

    // draw zoomed quad into FBO
    //  uses the same FBO for color and depth, with masking.
    const void*     fboKey = _getBufferKey( Frame::BUFFER_COLOR );
    util::FrameBufferObject* fbo = glObjects->getEqFrameBufferObject( fboKey );

    if( fbo )
    {
        EQCHECK( fbo->resize( pvp.w, pvp.h ));
    }
    else
    {
        fbo = glObjects->newEqFrameBufferObject( fboKey );
        fbo->setColorFormat( getInternalFormat( buffer ) );
        fbo->init( pvp.w, pvp.h, 24, 0 );
    }
    fbo->bind();
    texture->bind();

    if ( buffer == Frame::BUFFER_COLOR )
        glDepthMask( false );
    else
    {
        EQASSERT( buffer == Frame::BUFFER_DEPTH )
        glColorMask( false, false, false, false );
    }

    glDisable( GL_LIGHTING );
    glEnable( GL_TEXTURE_RECTANGLE_ARB );
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glColor3f( 1.0f, 1.0f, 1.0f );

    glBegin( GL_QUADS );
        glTexCoord2f( 0.0f, 0.0f );
        glVertex3f( 0, 0, 0.0f );

        glTexCoord2f( static_cast< float >( _pvp.w ), 0.0f );
        glVertex3f( static_cast< float >( pvp.w ), 0, 0.0f );

        glTexCoord2f( static_cast< float >( _pvp.w ),
                      static_cast< float >( _pvp.h ));
        glVertex3f( static_cast< float >( pvp.w ),
                    static_cast< float >( pvp.h ), 0.0f );

        glTexCoord2f( 0.0f, static_cast< float >( _pvp.h ));
        glVertex3f( 0, static_cast< float >( pvp.h ), 0.0f );
    glEnd();

    // restore state
    glDisable( GL_TEXTURE_RECTANGLE_ARB );

    if ( buffer == Frame::BUFFER_COLOR )
        glDepthMask( true );
    else
    {
        const ColorMask colorMask; // TODO = channel->getDrawBufferMask();
        glColorMask( colorMask.red, colorMask.green, colorMask.blue, true );
    }
    // TODO channel->bindFramebuffer()
    fbo->unbind();

    EQLOG( LOG_ASSEMBLY ) << "Scale " << _pvp << " -> " << pvp << std::endl;
    _pvp = pvp;

    EQLOG( LOG_ASSEMBLY ) << "Read texture " << _pvp << std::endl;

    uint32_t id;
    if ( buffer == Frame::BUFFER_COLOR )
        id = fbo->getColorTextures()[0]->getID();
    else if ( buffer == Frame::BUFFER_DEPTH )
        id = fbo->getDepthTexture().getID();

    _download( buffer, EQ_COMPRESSOR_DATA_2D | EQ_COMPRESSOR_USE_TEXTURE, 
               id, glewGetContext( ));

    EQLOG( LOG_ASSEMBLY ) << "Read texture " 
                          << getPixelDataSize( buffer ) << std::endl;
}

void Image::_download( const Frame::Buffer buffer, 
                       uint32_t flags,
                       uint32_t texture,
                       GLEWContext* glewContext )
{
    Attachment& attachment = _getAttachment( buffer );
    Memory& memory = attachment.memory;    
    const uint32_t inputToken = memory.internalFormat;
    attachment.transfer->setGLEWContext( glewContext );
    if( !attachment.transfer->isValidDownloader( inputToken ))
    {
        
        attachment.transfer->initDownloader( attachment.quality, inputToken );
            
        // get the pixel type produced by the downloader
        _setExternalFormat( buffer, attachment.transfer->getExternalFormat(),
                            attachment.transfer->getTokenSize( ));
        const bool noAlpha = _canIgnoreAlpha( buffer );

        // init a compressor which is compatible with the downloader 
        // output token
        float qualityComp = attachment.quality / 
                            attachment.lossyTransfer.getQuality();
        attachment.compressor->initCompressor( memory.externalFormat, 
                                               qualityComp, noAlpha );
    }

    attachment.transfer->download( _pvp, texture, flags, 
                                   memory.pvp, &memory.pixels );
    // get the pixel type produced ba the downloader
    _setExternalFormat( buffer, attachment.transfer->getExternalFormat(),
                        attachment.transfer->getTokenSize( ));
    memory.state = Memory::VALID;
}

void Image::setPixelViewport( const PixelViewport& pvp )
{
    _pvp = pvp;
    _color.memory.state = Memory::INVALID;
    _depth.memory.state = Memory::INVALID;
    _color.memory.isCompressed = false;
    _depth.memory.isCompressed = false;
}

void Image::clearPixelData( const Frame::Buffer buffer )
{
    Memory& memory = _getAttachment( buffer ).memory;
    memory.pvp = _pvp;
    const ssize_t size = getPixelDataSize( buffer );
    if( size == 0 )
        return;

    validatePixelData( buffer );

    if( buffer == Frame::BUFFER_DEPTH )
        memset( memory.pixels, 0xFF, size );
    else
    {
        if( getPixelSize( Frame::BUFFER_COLOR ) == 4 )
        {
            uint8_t* data = reinterpret_cast< uint8_t* >( memory.pixels );
#ifdef Darwin
            const unsigned char pixel[4] = { 0, 0, 0, 255 };
            memset_pattern4( data, &pixel, size );
#else
            bzero( data, size );

            if( getPixelSize( Frame::BUFFER_COLOR ) == 4 )
#ifdef EQ_USE_OPENMP
#pragma omp parallel for
#endif
                for( ssize_t i = 3; i < size; i+=4 )
                    data[i] = 255;
#endif
        }
        else
            bzero( memory.pixels, size );
    }
}

void Image::validatePixelData( const Frame::Buffer buffer )
{
    Memory& memory = _getAttachment( buffer ).memory;
    memory.useLocalBuffer( );
    memory.state = Memory::VALID;
    memory.isCompressed = false;
}

void Image::setPixelData( const Frame::Buffer buffer, const PixelData& pixels )
{
    Memory& memory   = _getMemory( buffer );
    memory.externalFormat = pixels.externalFormat;
    memory.internalFormat = pixels.internalFormat;
    memory.pixelSize = pixels.pixelSize;
    memory.pvp       = pixels.pvp;
    memory.state     = Memory::INVALID;
    memory.isCompressed = false;

    const uint32_t size = getPixelDataSize( buffer );
    EQASSERT( size > 0 );
    if( size == 0 )
        return;

    validatePixelData( buffer ); // alloc memory for pixels

    if( pixels.compressorName <= EQ_COMPRESSOR_NONE )
    {
        // if no data in pixels, it is only a memory setup parameter
        if( pixels.pixels )
        {
            memcpy( memory.pixels, pixels.pixels, size );
            memory.state = Memory::VALID;
        }
        return;
    }

    EQASSERT( !pixels.compressedData.empty( ));
    Attachment& attachment = _getAttachment( buffer );
    if( !_allocDecompressor( attachment, pixels.compressorName ))
    {
        EQASSERTINFO( false,
                      "Can't allocate decompressor " << pixels.compressorName <<
                      ", mismatched compressor installation?" );
        return;
    }

    uint64_t outDims[4] = { memory.pvp.x,  memory.pvp.w,  
                           memory.pvp.y,  memory.pvp.h }; 
    const uint64_t nBlocks  = pixels.compressedSize.size();
    uint64_t flags = EQ_COMPRESSOR_DATA_2D;
    if( _canIgnoreAlpha( buffer ))
        flags |= EQ_COMPRESSOR_IGNORE_MSE;

    EQASSERT( nBlocks == pixels.compressedData.size( ));
    attachment.compressor->decompress( &pixels.compressedData.front(),
                                       &pixels.compressedSize.front(),
                                       nBlocks, memory.pixels, 
                                       outDims, flags );
}

Image::Attachment& Image::_getAttachment( const Frame::Buffer buffer )
{
   switch( buffer )
   {
       case Frame::BUFFER_COLOR:
           return _color;
       case Frame::BUFFER_DEPTH:
           return _depth;
       default:
           EQUNIMPLEMENTED;
   }
   return _color;
}

const Image::Attachment& Image::_getAttachment( const Frame::Buffer buffer ) 
    const
{
   switch( buffer )
   {
       case Frame::BUFFER_COLOR:
           return _color;
       case Frame::BUFFER_DEPTH:
           return _depth;
       default:
           EQUNIMPLEMENTED;
   }
   return _color;
}

/** Find and activate a compression engine */
bool Image::allocCompressor( const Frame::Buffer buffer, 
                             const uint32_t name )
{
    Attachment& attachment = _getAttachment( buffer );
    if( name <= EQ_COMPRESSOR_NONE )
    {
        attachment.memory.isCompressed = false;
        attachment.compressor->initCompressor( name );
        return true;
    }

    if( !attachment.compressor->isValid( name ) )
    {
        attachment.memory.isCompressed = false;
        if( !attachment.compressor->initCompressor( name ) )
            return false;

        EQINFO << "Instantiated compressor of type " << name << std::endl;
    }
    return true;
}

/** Find and activate a compression engine */
bool Image::allocDownloader( const Frame::Buffer buffer, 
                             const uint32_t name,
                             GLEWContext* glewContext )
{

    Attachment& attachment = _getAttachment( buffer );    
    EQASSERT( name > EQ_COMPRESSOR_NONE )
    if( name <= EQ_COMPRESSOR_NONE )
    {
        attachment.transfer->initDownloader( name );
        _setExternalFormat( buffer, EQ_COMPRESSOR_DATATYPE_NONE, 0 );
        return false;
    }

    if( !attachment.transfer->isValid( name ) )
    {
        attachment.transfer->setGLEWContext( glewContext );
        if( !attachment.transfer->initDownloader( name ) )
            return false;
        attachment.memory.internalFormat = 
            attachment.transfer->getInternalFormat();
        _setExternalFormat( buffer, attachment.transfer->getExternalFormat(),
                            attachment.transfer->getTokenSize( ));
        EQINFO << "Instantiated compressor of type " << name << std::endl;
    }
    return true;
}

/** Find and activate a decompression engine */
bool Image::_allocDecompressor( Attachment& attachment, uint32_t name )
{
    if( !attachment.compressor->isValid( name ) )
    {
        if( !attachment.compressor->initDecompressor( name ) )
            return false;
    }
    return true;
}

void Image::Memory::flush()
{
    state = INVALID;
    isCompressed = false;
    PixelData::flush();
    localBuffer.clear();
}

void Image::Memory::useLocalBuffer()
{
    EQASSERT( internalFormat != 0 );
    EQASSERT( externalFormat != 0 );
    EQASSERT( pixelSize > 0 );
    EQASSERT( pvp.hasArea( ));

    localBuffer.resize( pvp.getArea() * pixelSize );
    pixels = localBuffer.getData();
}

Image::PixelData::PixelData()
        : internalFormat( 0 )
        , externalFormat( 0 )
        , pixelSize( 0 )
        , pixels( 0 )
        , compressorName( 0 )
        , isCompressed( false )
{}

void Image::PixelData::flush()
{
    pixels = 0;
    internalFormat = 0;
    externalFormat = 0;
    pixelSize = 0;
    compressorName = 0;
    isCompressed = false;
    compressedSize.clear();
    compressedData.clear();
}

Image::PixelData::~PixelData()
{
    flush();
}

const Image::PixelData& Image::compressPixelData( const Frame::Buffer buffer )
{
    EQASSERT( getPixelDataSize( buffer ) > 0 );

    Attachment& attachment = _getAttachment( buffer );
    Memory& memory = attachment.memory;
    if( memory.isCompressed )
        return memory;

    if( attachment.compressor->isValid( attachment.compressor->getName( )))
        memory.compressorName = attachment.compressor->getName();
    else
    {
        memory.compressorName = _getCompressorName( buffer );
        if( !allocCompressor( buffer, memory.compressorName ) || 
            memory.compressorName == EQ_COMPRESSOR_NONE )
        {
            EQWARN << "No compressor found for token type " 
                   << getExternalFormat( buffer ) << std::endl;
            return memory;
        }
    }

    EQASSERT( memory.compressorName != 0 );

    uint64_t flags = EQ_COMPRESSOR_DATA_2D;
    if( _canIgnoreAlpha( buffer ))
        flags |= EQ_COMPRESSOR_IGNORE_MSE;

    const uint64_t inDims[4] = { memory.pvp.x, memory.pvp.w,
                                 memory.pvp.y, memory.pvp.h }; 
    attachment.compressor->compress( memory.pixels, inDims, flags );

    const size_t numResults = attachment.compressor->getNumResults();
    
    memory.compressedSize.resize( numResults );
    memory.compressedData.resize( numResults );

    for( size_t i = 0; i < numResults ; ++i )
        attachment.compressor->getResult( i, &memory.compressedData[i], 
                                          &memory.compressedSize[i] );

    memory.isCompressed = true;
    return memory;
}


//---------------------------------------------------------------------------
// File IO
//---------------------------------------------------------------------------

bool Image::writeImages( const std::string& filenameTemplate ) const
{
    return( writeImage( filenameTemplate + "_color.rgb", Frame::BUFFER_COLOR) &&
            writeImage( filenameTemplate + "_depth.rgb", Frame::BUFFER_DEPTH ));
}

#define SWAP_SHORT(v) ( v = (v&0xff) << 8 | (v&0xff00) >> 8 )
#define SWAP_INT(v)   ( v = (v&0xff) << 24 | (v&0xff00) << 8 |      \
                        (v&0xff0000) >> 8 | (v&0xff000000) >> 24)

#ifdef WIN32
#  pragma pack(1)
#endif
/** @cond IGNORE */
struct RGBHeader
{
    RGBHeader()
        {
            memset( this, 0, sizeof( RGBHeader ));
            magic           = 474;
            bytesPerChannel = 1;
            nDimensions     = 3;
            maxValue        = 255;
        }

        /**
         * Convert to and from big endian by swapping bytes on little endian
         * machines.
         */
        void convert()
        {
#if defined(__i386__) || defined(__amd64__) || defined (__ia64) || \
    defined(__x86_64) || defined(WIN32)
            SWAP_SHORT(magic);
            SWAP_SHORT(nDimensions);
            SWAP_SHORT(width);
            SWAP_SHORT(height);
            SWAP_SHORT(depth);
            SWAP_INT(minValue);
            SWAP_INT(maxValue);
            SWAP_INT(colorMode);
#endif
        }

    unsigned short magic;
    char compression;
    char bytesPerChannel;
    unsigned short nDimensions;
    unsigned short width;
    unsigned short height;
    unsigned short depth;
    unsigned minValue;
    unsigned maxValue;
    char unused[4];
    char filename[80];
    unsigned colorMode;
    char fill[404];
}
/** @endcond */
#ifndef WIN32
  __attribute__((packed))
#endif
;

bool Image::writeImage( const std::string& filename,
                        const Frame::Buffer buffer ) const
{
    const Memory& memory = _getMemory( buffer );

    const PixelViewport& pvp = memory.pvp;
    const size_t  nPixels = pvp.w * pvp.h;

    if( nPixels == 0 || memory.state != Memory::VALID )
        return false;

    std::ofstream image( filename.c_str(), std::ios::out | std::ios::binary );
    if( !image.is_open( ))
    {
        EQERROR << "Can't open " << filename << " for writing" << std::endl;
        return false;
    }

    RGBHeader    header;

    header.width  = pvp.w;
    header.height = pvp.h;

    switch( getExternalFormat( buffer ))
    {      
        case EQ_COMPRESSOR_DATATYPE_BGR10_A2:
        case EQ_COMPRESSOR_DATATYPE_RGB10_A2:
            header.maxValue = 1023;
        case EQ_COMPRESSOR_DATATYPE_BGRA :
        case EQ_COMPRESSOR_DATATYPE_BGRA_UINT_8_8_8_8_REV :        
        case EQ_COMPRESSOR_DATATYPE_RGBA :
        case EQ_COMPRESSOR_DATATYPE_RGBA_UINT_8_8_8_8_REV :
            header.bytesPerChannel = 1;
            header.depth = 4;
            break;
        case EQ_COMPRESSOR_DATATYPE_BGR :
        case EQ_COMPRESSOR_DATATYPE_RGB :
            header.bytesPerChannel = 1;
            header.depth = 3;
            break;
        case EQ_COMPRESSOR_DATATYPE_BGRA32F:
        case EQ_COMPRESSOR_DATATYPE_RGBA32F:
            header.bytesPerChannel = 4;
            header.depth = 4;
            break;
        case EQ_COMPRESSOR_DATATYPE_BGR32F:
        case EQ_COMPRESSOR_DATATYPE_RGB32F:
            header.bytesPerChannel = 4;
            header.depth = 3;
            break;
        case EQ_COMPRESSOR_DATATYPE_BGRA16F:
        case EQ_COMPRESSOR_DATATYPE_RGBA16F:
            header.bytesPerChannel = 2;
            header.depth = 4;
            break;
        case EQ_COMPRESSOR_DATATYPE_BGR16F:
        case EQ_COMPRESSOR_DATATYPE_RGB16F:
            header.bytesPerChannel = 2;
            header.depth = 3;
            break;
        case EQ_COMPRESSOR_DATATYPE_DEPTH_UNSIGNED_INT:
            header.bytesPerChannel = 4;
            header.depth = 1;
            break;

        default:
            EQERROR << "Unknown image pixel data type" << std::endl;
            return false;
    }

    // if the data picture has a RGB format, we can easy translate it in 
    // a BGR format
    bool invertChannel = false;
    switch( getExternalFormat( buffer ))
    {      
        case EQ_COMPRESSOR_DATATYPE_RGB10_A2:
        case EQ_COMPRESSOR_DATATYPE_RGBA :
        case EQ_COMPRESSOR_DATATYPE_RGBA_UINT_8_8_8_8_REV :
        case EQ_COMPRESSOR_DATATYPE_RGB :
        case EQ_COMPRESSOR_DATATYPE_RGBA32F:
        case EQ_COMPRESSOR_DATATYPE_RGB32F:
        case EQ_COMPRESSOR_DATATYPE_RGBA16F:
        case EQ_COMPRESSOR_DATATYPE_RGB16F:
            invertChannel = true;
    }

    if( header.depth == 1 ) // depth
    {
        EQASSERT( (header.bytesPerChannel % 4) == 0 );
        header.depth = 4;
        header.bytesPerChannel /= 4;
    }
    EQASSERT( header.bytesPerChannel > 0 );
    if( header.bytesPerChannel > 2 )
        EQWARN << static_cast< int >( header.bytesPerChannel ) 
               << " bytes per channel not supported by RGB spec" << std::endl;

    const uint8_t bpc = header.bytesPerChannel;
    const uint16_t nChannels = header.depth;

    strncpy( header.filename, filename.c_str(), 80 );

    header.convert();
    image.write( reinterpret_cast<const char *>( &header ), sizeof( header ));

    // Each channel is saved separately
    const size_t depth  = nChannels * bpc;
    const size_t nBytes = nPixels * depth;
    const char* data = reinterpret_cast<const char*>( getPixelPointer( buffer));

    if( nChannels == 3 || nChannels == 4 )
    {
        // channel one is R or B
        if ( invertChannel )
        {
            for( size_t j = 0 * bpc; j < nBytes; j += depth )
                image.write( &data[j], bpc );
        }
        else
        {
            for( size_t j = 2 * bpc; j < nBytes; j += depth )
                image.write( &data[j], bpc );
        }

        // channel two is G
        for( size_t j = 1 * bpc; j < nBytes; j += depth )
            image.write( &data[j], bpc );

        // channel three is B or G
        if ( invertChannel )
        {
            for( size_t j = 2 * bpc; j < nBytes; j += depth )
                image.write( &data[j], bpc );
        }
        else
        {
            for( size_t j = 0; j < nBytes; j += depth )
                image.write( &data[j], bpc );
        }

        // channel four is Alpha
        if( nChannels == 4 )
        {
            // invert alpha
            for( size_t j = 3 * bpc; j < nBytes; j += depth )
            {
                if( bpc == 1 && header.maxValue == 255 )
                {
                    const uint8_t val = 255 - 
                        *reinterpret_cast< const uint8_t* >( &data[j] );
                    image.write( reinterpret_cast<const char*>( &val ), 1 );
                }
                else
                    image.write( &data[j], bpc );
            }
        }
    }
    else
    {
        for( size_t i = 0; i < nChannels; i += bpc )
           for( size_t j = i * bpc; j < nBytes; j += depth )
              image.write(&data[j], bpc );
    }

    image.close();
    return true;
}

bool Image::readImage( const std::string& filename, const Frame::Buffer buffer )
{
    base::MemoryMap image;
    const uint8_t* addr = static_cast< const uint8_t* >( image.map( filename ));

    if( !addr )
    {
        EQERROR << "Can't open " << filename << " for reading" << std::endl;
        return false;
    }

    const size_t size = image.getSize();
    if( size < sizeof( RGBHeader ))
    {
        EQERROR << "Image " << filename << " too small" << std::endl;
        return false;
    }

    RGBHeader header;
    memcpy( &header, addr, sizeof( header ));
    addr += sizeof( header );

    header.convert();

    if( header.magic != 474)
    {
        EQERROR << "Bad magic number " << filename << std::endl;
        return false;
    }
    if( header.width == 0 || header.height == 0 )
    {
        EQERROR << "Zero-sized image " << filename << std::endl;
        return false;
    }
    if( header.compression != 0)
    {
        EQERROR << "Unsupported compression " << filename << std::endl;
        return false;
    }

    const size_t nChannels = header.depth;

    if( header.nDimensions != 3 ||
        header.minValue != 0 ||
        ( header.maxValue != 255 && header.maxValue != 1023 ) ||
        header.colorMode != 0 ||
        ( buffer == Frame::BUFFER_COLOR && nChannels != 3 && nChannels != 4 ) ||
        ( buffer == Frame::BUFFER_DEPTH && nChannels != 4 ))
    {
        EQERROR << "Unsupported image type " << filename << std::endl;
        return false;
    }

    if(( header.bytesPerChannel != 1 || nChannels == 1 ) &&
         header.maxValue != 255 )
    {
        EQERROR << "Unsupported value range " << header.maxValue << std::endl;
        return false;
    }

    const uint8_t bpc     = header.bytesPerChannel;
    const size_t  depth   = nChannels * bpc;
    const size_t  nPixels = header.width * header.height;
    const size_t  nComponents = nPixels * nChannels;
    const size_t  nBytes  = nComponents * bpc;

    if( size < sizeof( RGBHeader ) + nBytes )
    {
        EQERROR << "Image " << filename << " too small" << std::endl;
        return false;
    }
    EQASSERT( size == sizeof( RGBHeader ) + nBytes );

    switch( buffer )
    {
        case Frame::BUFFER_DEPTH:
            if( header.bytesPerChannel != 1 )
            {
                EQERROR << "Unsupported channel depth " 
                        << static_cast< int >( header.bytesPerChannel )
                        << std::endl;
                return false;
            }
            _setExternalFormat( Frame::BUFFER_DEPTH,
                                EQ_COMPRESSOR_DATATYPE_DEPTH_UNSIGNED_INT, 4 );
            setInternalFormat( Frame::BUFFER_DEPTH,
                               EQ_COMPRESSOR_DATATYPE_DEPTH_UNSIGNED_INT );
            break;

        default:
            EQUNREACHABLE;
        case Frame::BUFFER_COLOR:
            switch( header.bytesPerChannel )
            {
                case 1:
                    if( header.maxValue == 1023 )
                    {
                        EQASSERT( nChannels==4 );
                        _setExternalFormat( Frame::BUFFER_COLOR,
                                            EQ_COMPRESSOR_DATATYPE_RGB10_A2, 4);
                        setInternalFormat( Frame::BUFFER_COLOR,
                                           EQ_COMPRESSOR_DATATYPE_RGB10_A2 );
                    }
                    else
                    {
                        _setExternalFormat( Frame::BUFFER_COLOR,
                                    nChannels==4 ? EQ_COMPRESSOR_DATATYPE_RGBA :
                                                   EQ_COMPRESSOR_DATATYPE_RGB,
                                            nChannels );
                        setInternalFormat( Frame::BUFFER_COLOR,
                                           EQ_COMPRESSOR_DATATYPE_RGBA );
                    }
                    break;

                case 2:
                    _setExternalFormat( Frame::BUFFER_COLOR,
                                 nChannels==4 ? EQ_COMPRESSOR_DATATYPE_RGBA16F :
                                                EQ_COMPRESSOR_DATATYPE_RGB16F,
                                        nChannels * 2 );
                    setInternalFormat( Frame::BUFFER_COLOR,
                                       EQ_COMPRESSOR_DATATYPE_RGBA16F );
                    break;

                case 4:
                    _setExternalFormat( Frame::BUFFER_COLOR,
                                 nChannels==4 ? EQ_COMPRESSOR_DATATYPE_RGBA32F :
                                                EQ_COMPRESSOR_DATATYPE_RGB32F,
                                        nChannels * 4 );
                    setInternalFormat( Frame::BUFFER_COLOR,
                                       EQ_COMPRESSOR_DATATYPE_RGBA32F );
                    break;

                default:
                    EQERROR << "Unsupported channel depth " 
                            << static_cast< int >( header.bytesPerChannel )
                            << std::endl;
                    return false;
            }
            break;
    }
	Memory& memory = _getMemory( buffer );
    const PixelViewport pvp( 0, 0, header.width, header.height );
    if( pvp != _pvp )
    {
        setPixelViewport( pvp );
    }
    
    if ( memory.pvp != pvp )
    {
        memory.pvp = pvp;
        memory.state = Memory::INVALID;
    }
    validatePixelData( buffer );

    uint8_t* data = reinterpret_cast< uint8_t* >( memory.pixels );
    uint64_t sizeData = getPixelDataSize( buffer );
    EQASSERTINFO( nBytes <= sizeData, 
                  nBytes << " > " << sizeData );
    // Each channel is saved separately
    switch( bpc )
    {
        case 1:
            for( size_t i = 0; i < nChannels; ++i )
            {
                for( size_t j = i; j < nComponents; j += nChannels )
                {
                    data[j] = *addr;
                    ++addr;
                }
            }
            break;

        case 2:
            for( size_t i = 0; i < nChannels; ++i )
            {
                for( size_t j = i; j < nComponents; j += nChannels )
                {
                    reinterpret_cast< uint16_t* >( data )[ j ] = 
                        *reinterpret_cast< const uint16_t* >( addr );
                    addr += bpc;
                }
            }
            break;

        case 4:
            for( size_t i = 0; i < nChannels; ++i )
            {
                for( size_t j = i; j < nComponents; j += nChannels )
                {
                    reinterpret_cast< uint32_t* >( data )[ j ] = 
                        *reinterpret_cast< const uint32_t* >( addr );
                    addr += bpc;
                }
            }
            break;

        default:
            for( size_t i = 0; i < depth; i += bpc )
            {
                for( size_t j = i * bpc; j < nBytes; j += depth )
                {
                    memcpy( &data[j], addr, bpc );
                    addr += bpc;
                }
            }
    }
    return true;
}

std::ostream& operator << ( std::ostream& os, const Image* image )
{
    os << "image " << image->_pvp;
    return os;
}
}
