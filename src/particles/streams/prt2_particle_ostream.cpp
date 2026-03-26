// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
// clang-format off
#include "stdafx.h"
// clang-format on

#include <exception>
#include <frantic/particles/streams/prt2_particle_ostream.hpp>
#include <oneapi/tbb/parallel_pipeline.h>

using namespace std;
using namespace frantic;
using namespace frantic::prtfile;
using namespace frantic::particles::streams;
using frantic::prtfile::prt2_writer;

namespace {

bool try_pop( tbb::concurrent_queue<vector<char>>& q, std::vector<char>& buffer ) {
    return q.try_pop( buffer );
}

} // anonymous namespace

namespace detail {
/**
 * A particle chunk generator which reads from a concurrent queue and creates particle chunks from it. This is
 * needed to translate the push interface of the ostream into the pull interface of the prt2_writer.
 */
class queued_chunk_generator {
    typedef prt2_writer::particle_chunk chunk_type;

    std::size_t m_structureSize;
    oneapi::tbb::concurrent_queue<vector<char>>& m_particleChunkQueue;
    std::atomic<bool>& m_closeRequested;
    mutable bool m_sentTerminationChunk;

  public:
    queued_chunk_generator( std::size_t structureSize, oneapi::tbb::concurrent_queue<vector<char>>& particleChunkQueue,
                            std::atomic<bool>& closeRequested )
        : m_structureSize( structureSize )
        , m_particleChunkQueue( particleChunkQueue )
        , m_closeRequested( closeRequested )
        , m_sentTerminationChunk( false ) {}

    /**
     * Pop the particle chunk queue and return the result. Waits for a chunk in the queue if none is available.
     */
    chunk_type* operator()( oneapi::tbb::flow_control& fc ) const {
        std::vector<char> chunkBuffer;

        // If a chunk can be retrieved, pass it along.
        if( try_pop( m_particleChunkQueue, chunkBuffer ) ) {
            chunk_type* chunk = new chunk_type();
            chunk->particleCount = chunkBuffer.size() / m_structureSize;
            chunk->uncompressed = std::move( chunkBuffer );
            return chunk;
        }

        // If no close has been requested, send an ignore chunk. Otherwise, return NULL to terminate the generator.
        if( !m_closeRequested.load() ) {
            return &prt2_writer::IGNORE_CHUNK;
        }

        // If we have not yet sent a termination chunk, send one before terminating.
        if( !m_sentTerminationChunk ) {
            m_sentTerminationChunk = true;
            return &prt2_writer::TERMINATION_CHUNK;
        }
        fc.stop();
        return nullptr;
    }
};

} // namespace detail


/**
 * prt2_particle_ostream
 */
prt2_particle_ostream::prt2_particle_ostream(
    const frantic::tstring& file, const frantic::channels::channel_map& particleChannelMap,
    const frantic::channels::channel_map& particleChannelMapForFile,
    frantic::prtfile::prt2_compression_t compressionScheme, bool useTempFile, const std::filesystem::path& tempDir,
    const frantic::channels::property_map* globalMetadata,
    const std::map<frantic::tstring, frantic::channels::property_map>* channelMetadata,
    intptr_t desiredChunkSizeInBytes )
    : m_closeRequested( false ) {
    // Open the output file, this writes the header and the initial 'Chan' chunk,
    // and initializes the file-layout channel_map inside of m_prt2
    m_prt2.open( file, particleChannelMapForFile, useTempFile, tempDir );

    // Write all the metadata
    if( globalMetadata != NULL ) {
        m_prt2.write_general_metadata_filechunks( *globalMetadata );
    }
    if( channelMetadata != NULL ) {
        for( std::map<frantic::tstring, frantic::channels::property_map>::const_iterator i = channelMetadata->begin(),
                                                                                         iend = channelMetadata->end();
             i != iend; ++i ) {
            m_prt2.write_channel_metadata_filechunks( i->first, i->second );
        }
    }

    set_channel_map( particleChannelMap );

    // The particle chunk buffer uses the channel map from the prt2_writer
    m_particleChunkBuffer.reserve( desiredChunkSizeInBytes );
    m_desiredChunkSizeInBytes = desiredChunkSizeInBytes;

    m_writerThread = std::thread( [this, compressionScheme]() {
          try {
              ::detail::queued_chunk_generator chunkGenerator(
                  m_prt2.get_channel_map().structure_size(),
                  m_particleChunkQueue,
                  m_closeRequested
              );
              m_prt2.write_particle_chunks(
                  chunkGenerator,
                  0,
                  m_nullProgress,
                  _T(""),
                  false,
                  compressionScheme
              );
          } catch (...) {
              m_writerException = std::current_exception();
          }
      });
}

prt2_particle_ostream::~prt2_particle_ostream() noexcept { close(); }

void prt2_particle_ostream::close() {
    if( !m_closeRequested ) {
        // Finish the particle chunks
        if( !m_particleChunkBuffer.empty() ) {
            m_particleChunkQueue.push( m_particleChunkBuffer );
            m_particleChunkBuffer.clear();
        }
        m_closeRequested = true;

        if(m_writerThread.joinable()) {
            m_writerThread.join();
        }

        if( m_writerException ) {
            std::rethrow_exception(m_writerException);
        }

        // Write the bounding box if we were accumulating it
        if( m_posAccessor.is_valid() ) {
            if( m_particleChannelMap[_T("Position")].data_type() == frantic::channels::data_type_float64 ) {
                m_prt2.write_channel_metadata_filechunk<frantic::graphics::boundbox3fd>( _T("Position"), _T("Extents"),
                                                                                         m_boundbox );
            } else {
                m_prt2.write_channel_metadata_filechunk<frantic::graphics::boundbox3f>(
                    _T("Position"), _T("Extents"), frantic::graphics::boundbox3f( m_boundbox ) );
            }
        }

        m_prt2.close();
    }
}

const std::filesystem::path& prt2_particle_ostream::get_target_file() const { return m_prt2.get_target_file(); }

void prt2_particle_ostream::set_channel_map( const frantic::channels::channel_map& particleChannelMap ) {
    m_particleChannelMap = particleChannelMap;
    // Initialize the adaptor for converting the particle format to the one in the file
    m_pcmAdaptor.set( m_prt2.get_channel_map(), m_particleChannelMap );

    m_posAccessor.reset();
    if( m_particleChannelMap.has_channel( _T("Position") ) )
        m_posAccessor = m_particleChannelMap.get_cvt_accessor<frantic::graphics::vector3fd>( _T("Position") );
}

void prt2_particle_ostream::put_particle( const char* rawParticleData ) {
    if( m_closeRequested )
        throw std::runtime_error( "prt2_particle_ostream.put_particle: Tried to write to particle file \"" +
                                  frantic::strings::to_string( m_prt2.get_stream_name() ) + "\" after it was closed." );

    if( m_posAccessor.is_valid() )
        m_boundbox += m_posAccessor( rawParticleData );

    // Append the particle to the particle chunk buffer
    std::size_t nextI = m_particleChunkBuffer.size();
    m_particleChunkBuffer.resize( m_particleChunkBuffer.size() + m_pcmAdaptor.dest_size() );
    m_pcmAdaptor.copy_structure( &m_particleChunkBuffer[nextI], rawParticleData );

    // Flush the particle chunk buffer if it has reached the threshold
    boost::int64_t currentChunkSize =
        static_cast<boost::int64_t>( m_particleChunkBuffer.size() + m_prt2.get_channel_map().structure_size() );
    if( currentChunkSize > m_desiredChunkSizeInBytes ) {
        m_particleChunkQueue.push( m_particleChunkBuffer );
        m_particleChunkBuffer.clear();
    }
}
