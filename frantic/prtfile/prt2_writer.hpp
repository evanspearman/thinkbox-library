// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <exception>
#include <iostream>
#include <memory>
#include <set>
#include <sstream>

#include <frantic/channels/channel_map.hpp>
#include <frantic/channels/property_map.hpp>
#include <frantic/files/files.hpp>
#include <frantic/logging/progress_logger.hpp>
#include <frantic/prtfile/prt2_common.hpp>

#include <oneapi/tbb/global_control.h>
#include <oneapi/tbb/parallel_pipeline.h>

#include <zlib.h>

#include <filesystem>

namespace frantic {
namespace prtfile {

/**
 * An object to write to PRT2 files using a tbb::pipeline for multithreading.
 *
 * To use this class:
 * 1. Call open on the file or stream you want to write to.
 * 2. Call write_particle_chunks for each 'Part' or 'PrtO' file chunk you want to write, and call
 *    write_prt2_buffered_filechunk, write_metadata_filechunk, or write_channel_metadata_filechunk for each metadata
 *    file chunk to write. Metadata file chunks and particle file chunks can be written in any order.
 * 3. Call close.
 *
 */
class prt2_writer {
  public:
    struct particle_chunk_index_info {
        boost::int64_t PRTChunkSize, PRTChunkParticleCount;
    };
    /**
     * A particle chunk. A chunk generator must generate these so that they can be fed into the pipeline.
     */
    struct particle_chunk {
        particle_chunk()
            : particleCount( 0 )
            , uncompressed()
            , positionOffset()
            , usePositionOffset( false )
            , compressed() {}

        // These fields are initialized by the chunk generator.
        boost::uint64_t particleCount;     // The number of particles in the chunk.
        std::vector<char> uncompressed;    // The uncompressed bytes of the particles in the chunk.
        graphics::vector3f positionOffset; // The position offset of the particles in the chunk.
        bool usePositionOffset;            // Whether or not to use the position index.

        // These fields are initialized by the compressor.
        std::vector<char> compressed; // The compressed bytes.
    };

    /**
     * A special particle chunk- the ignore chunk. If a chunk generator returns the address of this chunk, the pipeline
     * will ignore the chunk and the generator will continue to run.
     */
    static particle_chunk IGNORE_CHUNK;

    /**
     * A special particle chunk- the termination chunk. A chunk generator must return the address of this chunk once it
     * is done generating chunks so that the chunk_writer can write the end of the file chunk.
     */
    static particle_chunk TERMINATION_CHUNK;

    /**
     * Writes a 'Part' or 'PrtO' file chunk to an output stream. This also handles deleting the chunks that came from
     * the generator.

     * This class is public as classes that use the prt2_writer and manually initialize their pipeline need to be aware
     * of it.
     */
    class chunk_writer {
        // prt2_writer& m_writer;

        std::ostream& m_outputStream;
        const frantic::tstring m_fileStreamName;
        const frantic::tstring m_particleStreamName;
        bool m_usePositionOffset;
        prt2_compression_t m_compressionScheme;

        // The positions within the file where the PRTs values started.
        boost::int64_t m_prtsChunkSizeSeek, m_prtsParticleCountSeek;

        boost::uint64_t m_particleCountTotal;
        logging::progress_logger& m_progress;
        bool m_isCancelled;
        bool m_hasWrittenHeader;

        // The number of particles and number of particle chunks written out so far, respectively.
        boost::uint64_t m_particleCount, m_particleChunkCount;

        // The particle chunk indexes written to this file chunk.
        std::vector<prt2_writer::particle_chunk_index_info> m_particleChunkIndex;

        // Write the 'Part' or 'PrtO' filechunk header.
        void begin_particle_chunks();

        // Fill in any missing information in the filechunk header and write the 'PInd' particle chunk index.
        void end_particle_chunks();

      public:
        chunk_writer( /*prt2_writer& writer,*/ std::ostream& outputStream, const frantic::tstring& fileStreamName,
                      const frantic::tstring& particleStreamName, bool usePositionOffset,
                      prt2_compression_t compressionScheme, boost::uint64_t totalParticleCount,
                      logging::progress_logger& progress )
            // : m_writer( writer )
            : m_outputStream( outputStream )
            , m_fileStreamName( fileStreamName )
            , m_particleStreamName( particleStreamName )
            , m_usePositionOffset( usePositionOffset )
            , m_compressionScheme( compressionScheme )
            , m_prtsChunkSizeSeek( -1 )
            , m_prtsParticleCountSeek( -1 )
            , m_particleCountTotal( totalParticleCount )
            , m_progress( progress )
            , m_isCancelled( false )
            , m_hasWrittenHeader( false )
            , m_particleCount( 0 )
            , m_particleChunkCount( 0 )
            , m_particleChunkIndex() {}

        void operator()( particle_chunk* item );

        bool is_cancelled() const { return m_isCancelled; }
    };


    prt2_writer();
    ~prt2_writer();

    const channels::channel_map& get_channel_map() const { return m_fileParticleChannelMap; }

    /**
     * Gets the name of this stream.  Usually this is exactly the same as the path of the file being written to.
     * However, it can differ if it is being written to a temporary file or folder first (In this case, it will not be a
     * valid filename at all)
     */
    const frantic::tstring& get_stream_name() const { return m_streamname; }

    /**
     * Gets the name of the file that is ultimately being written to
     */
    const std::filesystem::path& get_target_file() const { return m_targetFile; }

    // Writes a filechunk that was buffered in memory.
    void write_prt2_buffered_filechunk( const frantic::graphics::raw_byte_buffer& buf, boost::uint32_t chunkName );

    // Writes a 'Meta' filechunk for a general metadata property.
    template <class T>
    void write_general_metadata_filechunk( const frantic::tstring& propertyName, const T& value ) {
        write_metadata_filechunk( propertyName, value );
    }

    // Writes a 'Meta' filechunk for the channel metadata.
    template <class T>
    void write_channel_metadata_filechunk( const frantic::tstring& channelName, const frantic::tstring& propertyName,
                                           const T& value ) {
        write_metadata_filechunk( channelName + _T(".") + propertyName, value );
    }
    // Writes a 'Meta' filechunk for each property in the property_map.
    void write_general_metadata_filechunks( const frantic::channels::property_map& pm );

    // Writes a 'Meta' filechunk for each property in the property_map.
    void write_channel_metadata_filechunks( const frantic::tstring& channelName,
                                            const frantic::channels::property_map& pm );

    /**
     * Write the header and the 'Chan' filechunk.
     *
     * \note The provided particleChannelMap is NOT preserved as the layout of particles that
     *       must be provided when writing particle chunks. The prt2_writer creates a new channel map
     *       according to the PRT spec, and the caller must call prt2_writer::get_channel_map() to
     *       get the actual particle layout for writing. This is handled by the prt2_ostream, for
     *       example.
     */
    void open( const frantic::tstring& filename, const channels::channel_map& particleChannelMap,
               bool useTempFile = true, const std::filesystem::path& tempDir = std::filesystem::path() );
    void open( std::ostream* os, const frantic::tstring& filename, const channels::channel_map& particleChannelMap );

    void close();

    /**
     * Get the filters this PRT2 writer would add to its pipeline to produce the correct 'Part' or 'PrtO' file chunk.
     *
     * \param totalParticleCount The total number of particles to write. Only used for progress logging.
     * \param progress Where to log progress to.
     * \param compressionScheme The compression method for the particle chunks in this file chunk.
     * \param outParticleChunkIndex The particle chunk index that outChunkWriter will write to.
     * \param outFilters The intermidiary filters in the pipeline.
     * \param outChunkWriter The filter which writes to the file. This is provided explicitly as it is guaranteed to
     *                       exist and holds information about cancellation.
     */
    // void get_filters( boost::uint64_t totalParticleCount, frantic::logging::progress_logger& progress,
    //                   const frantic::tstring& particleStreamName, bool usePositionOffset,
    //                   prt2_compression_t compressionScheme, std::vector<boost::shared_ptr<tbb::filter>>& outFilters,
    //                   boost::shared_ptr<chunk_writer>& outChunkWriter );

    std::shared_ptr<prt2_writer::chunk_writer> make_chunk_writer(
      boost::uint64_t totalParticleCount,
      frantic::logging::progress_logger& progress,
      const frantic::tstring& particleStreamName,
      bool usePositionOffset,
      prt2_compression_t compressionScheme
    );

    /**
     * Run the chunk pipeline to write particle chunks to a file within a 'Part' or 'PrtO' file chunk.
     *
     * \param chunkPipeline The pipeline which generates chunks, modifies them, and writes them to the file.
     * \param chunkWriter The filter which is writing chunks to the file. Needed to check for cancellation.
     * \param particleChunkIndex The spatial index of this file chunk, which must be filled by the time the pipeline
     *                           finishes running and is written at the end of the file chunk.
     * \param particleStreamName The name of this file chunk.
     * \param usePositionOffset If true, this is a 'PrtO' file chunk. Otherwise, this is a 'Part' file chunk.
     * \param compressionScheme The compression method for the particle chunks in this file chunk.
     */
    template <typename Pipeline>
    void write_particle_chunks( boost::shared_ptr<Pipeline> chunkPipeline,
                                boost::shared_ptr<chunk_writer> chunkWriter ) {
        // Run the pipeline.
        try {
            const std::size_t tokenCount = oneapi::tbb::global_control::active_value(oneapi::tbb::global_control::max_allowed_parallelism );
            chunkPipeline->run( tokenCount );
        } catch( std::exception& e ) {
            if( chunkWriter->is_cancelled() ) {
                throw frantic::logging::progress_cancel_exception( e.what() );
            } else {
                // Translate into runtime_error, because Sequoia is currently swallowing tbb_exception.
                throw std::runtime_error( e.what() );
            }
        }
    }

    template <typename ChunkGenerator>
    void write_particle_chunks( ChunkGenerator chunkGenerator,
                                boost::uint64_t totalParticleCount,
                                frantic::logging::progress_logger& progress,
                                const frantic::tstring& particleStreamName,
                                bool usePositionOffset,
                                prt2_compression_t compressionScheme );


  private:
    void write_header();

    void initialize_temp_file_state( bool useTempFile, const frantic::tstring& filename,
                                     const std::filesystem::path& tempDir );

    template <class T>
    void write_metadata_filechunk( const frantic::tstring& metaName, const T& value ) {
        frantic::graphics::raw_byte_buffer buf;
        serialize::write_metadata_filechunk( buf, metaName, value );
        write_prt2_buffered_filechunk( buf, PRT2_FILECHUNK_NAME_Meta );
    }

    // The channel map of the particles in the file.
    channels::channel_map m_fileParticleChannelMap;

    // Stream object of the file being saved.
    std::unique_ptr<std::ostream> m_file; // In C++11, unique_ptr is preferable
    frantic::tstring m_streamname;

    // The set of particle streams that have already been written. Used for catching duplicates.
    std::set<frantic::tstring> m_writtenParticleStreamNames;

    // It's generally good to write the PRT to a temp file, then move it into place.
    bool m_useTempFile;
    std::filesystem::path m_tempDir;
    std::filesystem::path m_targetFile;
    std::filesystem::path m_tempLocalFile;
    std::filesystem::path m_tempRemoteFile;
};

namespace detail {
void write_prt2_buffered_filechunk( std::ostream& out, const frantic::graphics::raw_byte_buffer& buf,
                                    boost::uint32_t chunkName, const frantic::tstring& streamName );


} // namespace detail

/**
 * Transposes particle chunks from the chunk generator if a transposing step was specified.
 */
class chunk_transposer {
    const std::size_t m_particleSize;

  public:
    explicit chunk_transposer( std::size_t particleSize )
        : m_particleSize( particleSize ) {}

    prt2_writer::particle_chunk* operator()( prt2_writer::particle_chunk* item ) const {
        // Skip ignore chunks and termination chunks.
        if( item == &prt2_writer::IGNORE_CHUNK || item == &prt2_writer::TERMINATION_CHUNK ) {
            return item;
        }

        // Wrap the chunk in an unique_ptr to prevent memory leaks in case this function exits with an error or is
        // cancelled.
        std::unique_ptr<prt2_writer::particle_chunk> chunk( item );

        // Skip zero-sized chunks.
        if( chunk->particleCount == 0 ) {
            return chunk.release();
        }

        std::vector<char> transposeBuffer;
        transposeBuffer.resize( chunk->uncompressed.size() );
        transpose_bytes_forward( m_particleSize, chunk->particleCount, &chunk->uncompressed[0], &transposeBuffer[0] );

        chunk->uncompressed.swap( transposeBuffer );

        return chunk.release();
    }
};

/**
 * The chunk compressor is a filter that takes in particle chunks provided by the chunk generator or transposer and
 * compresses them before they are handed off to the chunk_writer.
 */
namespace chunk_compressors {
class zlib_compression;
#if defined( LZ4_AVAILABLE )
class lz4_compression;
#endif

// zlib compressor.
class zlib_compression {
    const frantic::tstring m_streamname;

  public:
    explicit zlib_compression( const frantic::tstring& streamname )
        : m_streamname( streamname ) {}

    prt2_writer::particle_chunk* operator()( prt2_writer::particle_chunk* item ) const {
        // Skip ignore chunks and termination chunks.
        if( item == &prt2_writer::IGNORE_CHUNK || item == &prt2_writer::TERMINATION_CHUNK ) {
            return item;
        }

        // Wrap the chunk in an unique_ptr to prevent memory leaks in case this function exits with an error or is
        // cancelled.
        std::unique_ptr<prt2_writer::particle_chunk> chunk( item );

        // Skip zero-sized chunks.
        if( chunk->particleCount == 0 ) {
            return chunk.release();
        }

        size_t chunkSizeUncompressed = chunk->uncompressed.size();
        uLongf chunkSize = compressBound( static_cast<uLongf>( chunkSizeUncompressed ) );
        // Detect overflow by making sure the compressBound function returned a value that's bigger
        if( chunkSize < chunkSizeUncompressed || ( chunkSize & ~0xffffffffULL ) != 0 ) {
            std::stringstream ss;
            ss << "prt2_file_writer: Tried to write a particle chunk to output stream \"" << frantic::strings::to_string( m_streamname )
               << "\" which was too big";
            throw std::runtime_error( ss.str() );
        }

        chunk->compressed.resize( chunkSize );
        if( compress( reinterpret_cast<Bytef*>( &chunk->compressed[0] ), &chunkSize,
                      reinterpret_cast<const Bytef*>( &chunk->uncompressed[0] ),
                      static_cast<uLongf>( chunkSizeUncompressed ) ) != Z_OK ) {
            std::stringstream ss;
            ss << "prt2_file_writer: ZLib compression failure writing to output stream \"" << frantic::strings::to_string( m_streamname )
               << "\"";
            throw std::runtime_error( ss.str() );
        }
        chunk->compressed.resize( chunkSize );

        return chunk.release();
    }
};

#if defined( LZ4_AVAILABLE )
// lz4 compressor.
class lz4_compression {
    const frantic::tstring m_streamname;

  public:
    explicit lz4_compression( const frantic::tstring& streamname )
        : m_streamname( streamname ) {}

    prt2_writer::particle_chunk* operator()( prt2_writer::particle_chunk* item ) const {
        // Skip ignore chunks and termination chunks.
        if( item == &prt2_writer::IGNORE_CHUNK || item == &prt2_writer::TERMINATION_CHUNK ) {
            return item;
        }

        // Wrap the chunk in an unique_ptr to prevent memory leaks in case this function exits with an error or is
        // cancelled.
        std::unique_ptr<prt2_writer::particle_chunk> chunk( item );

        // Skip zero-sized chunks.
        if( chunk->particleCount == 0 ) {
            return chunk.release();
        }

        size_t chunkSizeUncompressed = chunk->uncompressed.size();
        size_t chunkSize = LZ4_compressBound( static_cast<int>( chunkSizeUncompressed ) );
        // Detect overflow by making sure the compressBound function returned a value that's bigger
        if( chunkSize < chunkSizeUncompressed || ( chunkSize & ~0x7fffffffULL ) != 0 ) {
            stringstream ss;
            ss << "prt2_file_writer: Tried to write a particle chunk to output stream \"" << to_string( m_streamname )
               << "\" which was too big";
            throw runtime_error( ss.str() );
        }

        chunk->compressed.resize( chunkSize );
        int compressResult =
            LZ4_compress( &chunk->uncompressed[0], &chunk->compressed[0], static_cast<int>( chunkSizeUncompressed ) );
        if( compressResult <= 0 ) {
            stringstream ss;
            ss << "prt2_file_writer: LZ4 compression failure writing to output stream \"" << to_string( m_streamname )
               << "\"";
            throw runtime_error( ss.str() );
        }
        chunk->compressed.resize( compressResult );

        return chunk.release();
    }
};
#endif
} // namespace chunk_compressors


template <typename ChunkGenerator>
void prt2_writer::write_particle_chunks( ChunkGenerator chunkGenerator,
                                         boost::uint64_t totalParticleCount,
                                         frantic::logging::progress_logger& progress,
                                         const frantic::tstring& particleStreamName,
                                         bool usePositionOffset,
                                         prt2_compression_t compressionScheme ) {
    using particle_chunk_t = prt2_writer::particle_chunk;

    auto writer = make_chunk_writer(
        totalParticleCount, progress, particleStreamName, usePositionOffset, compressionScheme );

    const std::size_t tokenCount =
        oneapi::tbb::global_control::active_value(
            oneapi::tbb::global_control::max_allowed_parallelism );

    auto source =
        oneapi::tbb::make_filter<void, particle_chunk_t*>(
            oneapi::tbb::filter_mode::serial_in_order,
            chunkGenerator );

    try {
        switch( compressionScheme ) {
        case prt2_compression_uncompressed:
        case prt2_compression_zlib:
#if defined( LZ4_AVAILABLE )
        case prt2_compression_lz4:
#endif
        {
            switch( compressionScheme ) {
            case prt2_compression_uncompressed: {
                auto sink =
                    oneapi::tbb::make_filter<particle_chunk_t*, void>(
                        oneapi::tbb::filter_mode::serial_in_order,
                        [writer]( particle_chunk_t* chunk ) { (*writer)( chunk ); } );

                oneapi::tbb::parallel_pipeline( tokenCount, source & sink );
                break;
            }
            case prt2_compression_zlib: {
                auto compressor =
                    oneapi::tbb::make_filter<particle_chunk_t*, particle_chunk_t*>(
                        oneapi::tbb::filter_mode::parallel,
                        chunk_compressors::zlib_compression( m_streamname ) );

                auto sink =
                    oneapi::tbb::make_filter<particle_chunk_t*, void>(
                        oneapi::tbb::filter_mode::serial_in_order,
                        [writer]( particle_chunk_t* chunk ) { (*writer)( chunk ); } );

                oneapi::tbb::parallel_pipeline( tokenCount, source & compressor & sink );
                break;
            }
#if defined( LZ4_AVAILABLE )
            case prt2_compression_lz4: {
                auto compressor =
                    oneapi::tbb::make_filter<particle_chunk_t*, particle_chunk_t*>(
                        oneapi::tbb::filter_mode::parallel,
                        chunk_compressors::lz4_compression( m_streamname ) );

                auto sink =
                    oneapi::tbb::make_filter<particle_chunk_t*, void>(
                        oneapi::tbb::filter_mode::serial_in_order,
                        [writer]( particle_chunk_t* chunk ) { (*writer)( chunk ); } );

                oneapi::tbb::parallel_pipeline( tokenCount, source & compressor & sink );
                break;
            }
#endif
            default:
                throw std::runtime_error( "prt2_writer::write_particle_chunks - Unknown compression type." );
            }
            break;
        }

        case prt2_compression_transpose:
        case prt2_compression_transpose_zlib:
#if defined( LZ4_AVAILABLE )
        case prt2_compression_transpose_lz4:
#endif
        {
            auto transposer =
                oneapi::tbb::make_filter<particle_chunk_t*, particle_chunk_t*>(
                    oneapi::tbb::filter_mode::parallel,
                    chunk_transposer( m_fileParticleChannelMap.structure_size() ) );

            if( compressionScheme == prt2_compression_transpose ) {
                auto sink =
                    oneapi::tbb::make_filter<particle_chunk_t*, void>(
                        oneapi::tbb::filter_mode::serial_in_order,
                        [writer]( particle_chunk_t* chunk ) { (*writer)( chunk ); } );

                oneapi::tbb::parallel_pipeline( tokenCount, source & transposer & sink );
            } else if( compressionScheme == prt2_compression_transpose_zlib ) {
                auto compressor =
                    oneapi::tbb::make_filter<particle_chunk_t*, particle_chunk_t*>(
                        oneapi::tbb::filter_mode::parallel,
                        chunk_compressors::zlib_compression( m_streamname ) );

                auto sink =
                    oneapi::tbb::make_filter<particle_chunk_t*, void>(
                        oneapi::tbb::filter_mode::serial_in_order,
                        [writer]( particle_chunk_t* chunk ) { (*writer)( chunk ); } );

                oneapi::tbb::parallel_pipeline( tokenCount, source & transposer & compressor & sink );
            }
#if defined( LZ4_AVAILABLE )
            else if( compressionScheme == prt2_compression_transpose_lz4 ) {
                auto compressor =
                    oneapi::tbb::make_filter<particle_chunk_t*, particle_chunk_t*>(
                        oneapi::tbb::filter_mode::parallel,
                        chunk_compressors::lz4_compression( m_streamname ) );

                auto sink =
                    oneapi::tbb::make_filter<particle_chunk_t*, void>(
                        oneapi::tbb::filter_mode::serial_in_order,
                        [writer]( particle_chunk_t* chunk ) { (*writer)( chunk ); } );

                oneapi::tbb::parallel_pipeline( tokenCount, source & transposer & compressor & sink );
            }
#endif
            else {
                throw std::runtime_error( "prt2_writer::write_particle_chunks - Unknown compression type." );
            }
            break;
        }

        default:
            throw std::runtime_error( "prt2_writer::write_particle_chunks - Unknown compression type." );
        }
    } catch( const std::exception& e ) {
        if( writer->is_cancelled() ) {
            throw frantic::logging::progress_cancel_exception( e.what() );
        } else {
            throw;
        }
    }
}
} // namespace prtfile
} // namespace frantic
