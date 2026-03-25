// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
// clang-format off
#include "stdafx.h"
#include <random>
#include <stdexcept>
// clang-format on

// Problem with C++11 and linking Boost Filesystem -> undefined reference to copy_file(). This function uses
// BOOST_SCOPED_ENUMS and the fix is to add the following line.
#ifdef __linux__
#define BOOST_NO_CXX11_SCOPED_ENUMS
#endif

#include <frantic/logging/logging_level.hpp>
#include <frantic/prtfile/prt2_common.hpp>
#include <frantic/prtfile/prt2_writer.hpp>
#include <frantic/strings/utf8.hpp>

#include <boost/exception/all.hpp>

#ifdef _WIN32
#include <Shlwapi.h>
#pragma comment( lib, "Shlwapi.lib" )
#endif

#if defined( LZ4_AVAILABLE )
#include <lz4.h>
#endif

#include <filesystem>

using namespace std;
using namespace frantic;
using namespace frantic::strings;
using namespace frantic::prtfile;
using namespace frantic::channels;
using frantic::graphics::raw_byte_buffer;

class prt2_ostream_exception : public virtual boost::exception, public virtual std::exception {
  public:
    typedef boost::error_info<struct tag_target_file, std::filesystem::path> errinfo_target_file;
    typedef boost::error_info<struct tag_target_file, std::string> errinfo_message;

    virtual const char* what() const throw() { return boost::diagnostic_information_what( *this ); }
};

/** Writes the PRT2 header to the output stream */
static void write_prt2_header( ostream& out, const frantic::tstring& streamName ) {
    boost::uint64_t magic = PRT2_MAGIC_NUMBER;
    out.write( reinterpret_cast<const char*>( &magic ), 8u );
    boost::uint32_t version = 3;
    out.write( reinterpret_cast<const char*>( &version ), 4u );
    if( out.fail() ) {
        stringstream ss;
        ss << "prt2_file_writer: Failed to write PRT2 header to output stream \""
           << frantic::strings::to_string( streamName ) << "\"";
        throw std::runtime_error( ss.str() );
    }
}

/** Creates the packed channel map for the output file */
static void create_packed_channel_map( const channel_map& input, channel_map& outPacked ) {
    outPacked.reset();
    size_t channelCount = input.channel_count();
    frantic::tstring channelName;
    size_t channelArity, channelOffset = 0;
    data_type_t channelDataType;
    for( size_t i = 0; i != channelCount; ++i ) {
        input.get_channel_definition( i, channelName, channelDataType, channelArity );
        outPacked.define_channel( channelName, channelArity, channelDataType, channelOffset );
        channelOffset += channelArity * sizeof_channel_data_type( channelDataType );
    }

    outPacked.end_channel_definition( 1, true, false );
}

void frantic::prtfile::detail::write_prt2_buffered_filechunk( std::ostream& out,
                                                              const frantic::graphics::raw_byte_buffer& buf,
                                                              boost::uint32_t chunkName,
                                                              const frantic::tstring& streamName ) {
    boost::uint64_t chunkSize = buf.size();
    out.write( reinterpret_cast<const char*>( &chunkName ), 4u );
    out.write( reinterpret_cast<const char*>( &chunkSize ), 8u );
    out.write( buf.begin(), chunkSize );

    if( !out ) {
        std::stringstream ss;
        ss << "prt2_file_writer: Failed to write PRT2 'Chan' chunk to output stream \""
           << frantic::strings::to_string( streamName ) << "\"";
        throw std::runtime_error( ss.str() );
    }
}

/** Writes the PRT2 'Chan' chunk to the output stream, using the provided channel map */
static void write_prt2_channels( ostream& out, const frantic::tstring& streamName, const channel_map& cm ) {
    // Serialize the data into a buffer
    raw_byte_buffer buf;
    size_t channelCount = cm.channel_count();
    serialize::write_varint( buf, channelCount );
    frantic::tstring channelName;
    size_t channelArity;
    data_type_t channelDataType;
    for( size_t i = 0; i != channelCount; ++i ) {
        cm.get_channel_definition( i, channelName, channelDataType, channelArity );
        serialize::write_varstring( buf, channelName );
        serialize::write_type_id( buf, (boost::int32_t)channelArity, channelDataType );
        serialize::write_varint( buf, channelArity * sizeof_channel_data_type( channelDataType ) );
    }

    // Write the chunk
    frantic::prtfile::detail::write_prt2_buffered_filechunk( out, buf, PRT2_FILECHUNK_NAME_Chan, streamName );
}

/**
 * PRT2 Writer.
 */

prt2_writer::particle_chunk prt2_writer::IGNORE_CHUNK = prt2_writer::particle_chunk();
prt2_writer::particle_chunk prt2_writer::TERMINATION_CHUNK = prt2_writer::particle_chunk();

prt2_writer::prt2_writer()
    : m_useTempFile( false ) {}

prt2_writer::~prt2_writer() { close(); }

void prt2_writer::write_prt2_buffered_filechunk( const raw_byte_buffer& buf, boost::uint32_t chunkName ) {
    frantic::prtfile::detail::write_prt2_buffered_filechunk( *m_file, buf, chunkName, m_streamname );
}

void prt2_writer::write_general_metadata_filechunks( const frantic::channels::property_map& pm ) {
    const frantic::channels::channel_map& cm = pm.get_channel_map();
    size_t metaCount = cm.channel_count();
    frantic::tstring metaName;
    size_t metaArity;
    data_type_t metaDataType;
    for( size_t i = 0; i != metaCount; ++i ) {
        cm.get_channel_definition( i, metaName, metaDataType, metaArity );
        if( metaDataType == data_type_string ) {
            write_metadata_filechunk( metaName, pm.get_cvt<frantic::tstring>( metaName ) );
        } else {
            frantic::graphics::raw_byte_buffer buf;
            serialize::write_varstring( buf, metaName );
            serialize::write_type_id( buf, (boost::int32_t)metaArity, metaDataType );
            buf.add_element( pm.get_raw_buffer() + cm.channel_offset( metaName ),
                             metaArity * sizeof_channel_data_type( metaDataType ) );
            write_prt2_buffered_filechunk( buf, PRT2_FILECHUNK_NAME_Meta );
        }
    }
}

void prt2_writer::write_channel_metadata_filechunks( const frantic::tstring& channelName,
                                                     const frantic::channels::property_map& pm ) {
    const frantic::channels::channel_map& cm = pm.get_channel_map();
    size_t metaCount = cm.channel_count();
    frantic::tstring metaName;
    size_t metaArity;
    data_type_t metaDataType;
    for( size_t i = 0; i != metaCount; ++i ) {
        cm.get_channel_definition( i, metaName, metaDataType, metaArity );
        if( metaDataType == data_type_string ) {
            write_channel_metadata_filechunk( channelName, metaName, pm.get_cvt<frantic::tstring>( metaName ) );
        } else {
            frantic::graphics::raw_byte_buffer buf;
            serialize::write_varstring( buf, channelName + _T(".") + metaName );
            serialize::write_type_id( buf, (boost::int32_t)metaArity, metaDataType );
            buf.add_element( pm.get_raw_buffer() + cm.channel_offset( metaName ),
                             metaArity * sizeof_channel_data_type( metaDataType ) );
            write_prt2_buffered_filechunk( buf, PRT2_FILECHUNK_NAME_Meta );
        }
    }
}

void prt2_writer::write_header() {
    // Write the header (it only contains a magic number and version info)
    write_prt2_header( *m_file, m_streamname );

    // Create a packed channel map for the file, then write the 'Chan' chunk
    write_prt2_channels( *m_file, m_streamname, m_fileParticleChannelMap );
}

std::filesystem::path make_unique_path(
    const std::filesystem::path& baseDir,
    const std::wstring& base
) {
    static thread_local std::mt19937_64 rng { std::random_device {}() };
    std::uniform_int_distribution<uint64_t> dist;

    for (int i = 0; i < 100; ++i) {
        std::wstringstream ss;
        ss << base << L"."
           << std::hex << dist(rng)
           << L".tmp";
        auto candidate = baseDir / ss.str();
        if (!std::filesystem::exists(candidate)) {
            return candidate;
        }
    }
    throw std::runtime_error("Failed to generate unique path");
}

void prt2_writer::initialize_temp_file_state( bool useTempFile, const frantic::tstring& filename,
                                              const std::filesystem::path& tempDir ) {
    m_useTempFile = useTempFile;
    if( !m_useTempFile ) {
        m_streamname = filename;
        m_targetFile = std::filesystem::path( filename );
        m_tempLocalFile = std::filesystem::path( filename );
        m_tempRemoteFile = std::filesystem::path( filename );
        return;
    }

    if( !tempDir.empty() ) {
        m_tempDir = tempDir;
    } else {
        m_tempDir = std::filesystem::temp_directory_path();
    }

    m_targetFile = std::filesystem::path( filename );
    bool useTempfolder = true;
#ifdef _WIN32
    if( !PathIsUNCW( m_targetFile.c_str() ) ) {
        UINT dt = GetDriveTypeW( m_targetFile.root_path().c_str() );

        // We want to use the temporary folder for output if we are targetting a remote or removable drive.
        useTempfolder = ( dt == DRIVE_REMOTE || dt == DRIVE_REMOVABLE );

        if( frantic::logging::is_logging_debug() ) {
            frantic::logging::debug << _T("Target file: \"") << m_targetFile.string<frantic::tstring>()
                                    << _T("\" has drive type: ");
            switch( dt ) {
            case DRIVE_UNKNOWN:
                frantic::logging::debug << _T("DRIVE_UNKNOWN");
                break;
            case DRIVE_NO_ROOT_DIR:
                frantic::logging::debug << _T("DRIVE_NO_ROOT_DIR");
                break;
            case DRIVE_REMOVABLE:
                frantic::logging::debug << _T("DRIVE_REMOVABLE");
                break;
            case DRIVE_FIXED:
                frantic::logging::debug << _T("DRIVE_FIXED");
                break;
            case DRIVE_REMOTE:
                frantic::logging::debug << _T("DRIVE_REMOTE");
                break;
            case DRIVE_CDROM:
                frantic::logging::debug << _T("DRIVE_CDROM");
                break;
            case DRIVE_RAMDISK:
                frantic::logging::debug << _T("DRIVE_RAMDISK");
                break;
            default:
                frantic::logging::debug << _T("Unknown");
                break;
            }

            frantic::logging::debug << std::endl;
        }
    }
#endif

    assert( !useTempfolder ||
            ( std::filesystem::exists( m_tempDir ) && std::filesystem::is_directory( m_tempDir ) ) );

    try {
        m_tempRemoteFile = make_unique_path(
            m_targetFile.parent_path(), ( m_targetFile.filename().wstring() + L".%%%%-%%%%-%%%%-%%%%.tmp" ) );
        if( useTempfolder ) {
            m_tempLocalFile = make_unique_path(
                m_tempDir, ( m_targetFile.filename().wstring() + L".%%%%-%%%%-%%%%-%%%%.tmp" ) );
        } else {
            m_tempLocalFile = m_tempRemoteFile;
        }
    } catch( std::filesystem::filesystem_error& e ) {
        throw prt2_ostream_exception() << prt2_ostream_exception::errinfo_target_file( m_targetFile )
                                       << boost::errinfo_errno( e.code().value() )
                                       << prt2_ostream_exception::errinfo_message( e.code().message() )
                                       << boost::errinfo_file_name( e.path1().string() )
                                       << boost::errinfo_nested_exception( boost::copy_exception( e ) );
    }

    // Give this stream a name that indicates both the temporary file path and the target file path.
    // 
    m_streamname =
        m_targetFile.native() + _T(" via temp file: ") + m_tempLocalFile.native();
}

void prt2_writer::open( const frantic::tstring& filename, const channels::channel_map& particleChannelMap,
                        bool useTempFile, const std::filesystem::path& tempDir ) {
    initialize_temp_file_state( useTempFile, filename, std::filesystem::path( tempDir ) );
    m_file.reset( new fstream( m_tempLocalFile.c_str(), ios::out | ios::binary ) );
    create_packed_channel_map( particleChannelMap, m_fileParticleChannelMap );
    write_header();
}

void prt2_writer::open( std::ostream* os, const frantic::tstring& filename,
                        const channels::channel_map& particleChannelMap ) {
    initialize_temp_file_state( false, filename, std::filesystem::path() );
    m_file.reset( os );
    create_packed_channel_map( particleChannelMap, m_fileParticleChannelMap );
    write_header();
}

void prt2_writer::close() {
    if( m_useTempFile ) {
        if( m_file.get() != NULL ) {
            // Only finish the file if there is no thrown exception.
            if( !std::uncaught_exceptions() ) {
                m_file->flush();
                if( m_file->fail() ) {
                    FF_LOG( warning ) << _T("Failed to flush particle data for ") << m_tempLocalFile << std::endl;
                }
                m_file.reset();

                // We have to do this carefully:
                // 1. Move the temporary local file to the temporary remote destination. This part might fail, so we
                // don't
                //    want it to clobber the local temp file, and also we don't want it to clobber any existing file at
                //    the target destination.
                // 2. Move the remote local file to the target remote destination. This should be a simple rename, and
                // hence
                //    atomic.

                boost::system::error_code errcode;

                if( m_tempLocalFile != m_tempRemoteFile ) {
                    std::filesystem::rename( m_tempLocalFile, m_tempRemoteFile, errcode );
                    if( errcode ) {
                        if( errcode != boost::system::errc::cross_device_link )
                            BOOST_THROW_EXCEPTION( std::filesystem::filesystem_error(
                                "PRT2 writer failed to rename local temp file to remote location", m_tempLocalFile,
                                m_tempRemoteFile, errcode ) );

                        // If the error is a result of a cross-device link error, we must perform a copy-remove instead.
                        // We want to fail if the file exists in the remote destination since we chosen an appropriately
                        // random name that its a problem if that file now exists.
                        std::filesystem::copy_file( m_tempLocalFile, m_tempRemoteFile,
                                                      std::filesystem::copy_options::none, errcode );
                        if( errcode )
                            BOOST_THROW_EXCEPTION( std::filesystem::filesystem_error(
                                "PRT2 writer failed to copy local temp file to remote location", m_tempLocalFile,
                                m_tempRemoteFile, errcode ) );

                        std::filesystem::remove( m_tempLocalFile, errcode );
                        if( errcode )
                            FF_LOG( error ) << _T("PRT2 writer failed to remove local temp file: ")
                                            << m_tempLocalFile.native() << _T(" after copying to ")
                                            << m_tempRemoteFile.native() << std::endl;
                    }
                }

                // Delete the target file (if it exists) so that we can rename our new file to the final name it needs.
                std::filesystem::remove( m_targetFile, errcode );
                if( errcode ) {
                    BOOST_THROW_EXCEPTION( std::filesystem::filesystem_error(
                        "PRT2 writer failed to remove existing file with target filename", m_targetFile, errcode ) );
                }

                std::filesystem::rename( m_tempRemoteFile, m_targetFile, errcode );
                if( errcode ) {
                    BOOST_THROW_EXCEPTION( std::filesystem::filesystem_error(
                        "PRT2 writer failed to rename remote temp file to target filename", m_tempRemoteFile,
                        m_targetFile, errcode ) );
                }
            } else {
                m_file.reset();
                if( std::filesystem::exists( m_tempLocalFile ) ) {
                    std::filesystem::remove( m_tempLocalFile );
                }
                if( std::filesystem::exists( m_tempRemoteFile ) ) {
                    std::filesystem::remove( m_tempRemoteFile );
                }
            }
        }
    }
    m_file.reset();
    m_streamname.clear();
    m_targetFile.clear();
    m_tempLocalFile.clear();
    m_tempRemoteFile.clear();
    m_writtenParticleStreamNames.clear();
}



void prt2_writer::chunk_writer::begin_particle_chunks() {
    if( m_compressionScheme < 0 || m_compressionScheme >= prt2_compression_count ) {
        stringstream ss;
        ss << "prt2_writer: Unsupported PRT2 compression scheme " << m_compressionScheme
           << " while writing to output stream \"" << to_string( m_fileStreamName ) << "\"";
        throw runtime_error( ss.str() );
    }

    m_particleCount = 0;
    m_particleChunkCount = 0;

    // The filechunk header
    boost::uint32_t chunkName = ( m_usePositionOffset ? PRT2_FILECHUNK_NAME_PrtO : PRT2_FILECHUNK_NAME_Part );
    m_outputStream.write( reinterpret_cast<const char*>( &chunkName ), 4u );
    m_prtsChunkSizeSeek = m_outputStream.tellp();
    boost::uint64_t chunkSize = 0xffffffffffffffffULL; // Gets fixed up later
    m_outputStream.write( reinterpret_cast<const char*>( &chunkSize ), 8u );

    // The 'Part'/'PrtO' header
    serialize::write_varstring( m_outputStream, m_particleStreamName );
    serialize::write_compression_scheme( m_outputStream, m_compressionScheme );
    m_prtsParticleCountSeek = m_outputStream.tellp();
    boost::uint64_t particleCount = 0xffffffffffffffffULL; // Gets fixed up later
    m_outputStream.write( reinterpret_cast<const char*>( &particleCount ), 8u );
    boost::uint64_t particleChunkCount = 0xffffffffffffffffULL; // Gets fixed up later
    m_outputStream.write( reinterpret_cast<const char*>( &particleChunkCount ), 8u );
}

std::shared_ptr<prt2_writer::chunk_writer>
prt2_writer::make_chunk_writer( boost::uint64_t totalParticleCount,
                                frantic::logging::progress_logger& progress,
                                const frantic::tstring& particleStreamName,
                                bool usePositionOffset,
                                prt2_compression_t compressionScheme ) {
    if( m_writtenParticleStreamNames.find( particleStreamName ) != m_writtenParticleStreamNames.end() ) {
        stringstream ss;
        ss << "prt2_writer: Internal error: already wrote a ";
        if( particleStreamName.empty() ) {
            ss << "default";
        } else {
            ss << "named \"" << to_string( particleStreamName ) << "\"";
        }
        ss << " 'Part'/'PrtO' filechunk to output stream \"" << to_string( m_streamname ) << "\"";
        throw runtime_error( ss.str() );
    }
    m_writtenParticleStreamNames.insert( particleStreamName );

    return std::make_shared<chunk_writer>(
        /**this,*/ *m_file, m_streamname, particleStreamName,
        usePositionOffset, compressionScheme, totalParticleCount, progress );
}


void prt2_writer::chunk_writer::end_particle_chunks() {
    // Seek back and rewrite the chunk size, particle count, and particle chunk count
    boost::uint64_t chunkSize = boost::uint64_t( m_outputStream.tellp() ) - boost::uint64_t( m_prtsChunkSizeSeek + 8 );
    m_outputStream.seekp( m_prtsChunkSizeSeek, ios::beg );
    // Rewrite the chunk size
    m_outputStream.write( reinterpret_cast<const char*>( &chunkSize ), 8u );
    // Seek to where in the 'Part'/'PrtO' header we want to rewrite the counts
    m_outputStream.seekp( m_prtsParticleCountSeek, ios::beg );
    // Rewrite the particle count
    m_outputStream.write( reinterpret_cast<const char*>( &m_particleCount ), 8u );
    // Rewrite the particle chunk count
    m_outputStream.write( reinterpret_cast<const char*>( &m_particleChunkCount ), 8u );

    // Seek to the end again, and switch back to the filechunks state
    m_outputStream.seekp( 0, ios::end );

    // Write the 'PInd' particle chunk index
    frantic::graphics::raw_byte_buffer buf;
    serialize::write_varstring( buf, m_particleStreamName );
    serialize::write_value<boost::uint64_t>( buf, m_particleChunkCount );
    for( std::vector<particle_chunk_index_info>::const_iterator i = m_particleChunkIndex.begin(),
                                                                end = m_particleChunkIndex.end();
         i != end; ++i ) {
        serialize::write_varint( buf, i->PRTChunkSize );
        serialize::write_varint( buf, i->PRTChunkParticleCount );
    }
    frantic::prtfile::detail::write_prt2_buffered_filechunk( m_outputStream, buf, PRT2_FILECHUNK_NAME_PIdx,
                                                             m_particleStreamName );
}

void prt2_writer::chunk_writer::operator()( prt2_writer::particle_chunk* item ) {
    // Skip ignore chunks.
    if( item == &prt2_writer::IGNORE_CHUNK ) {
        return;
    }

    // Write the file chunk header before the first chunk. Cannot just detect that 0 particles have been written as the
    // first particle chunk to be written could have zero particles in it.
    if( !m_hasWrittenHeader ) {
        begin_particle_chunks();
        m_hasWrittenHeader = true;
    }

    // A termination chunk signals to write the end of the file chunk. Since it is not valid to write more particle
    // chunks after the end of the file chunk, this ends the writer as well. This must happen after the
    // begin_particle_chunks() call so that empty particle files still write successfully.
    if( item == &prt2_writer::TERMINATION_CHUNK ) {
        end_particle_chunks();
        m_progress.update_progress( 100.0f );
        return;
    }

    // Wrap the chunk in an unique_ptr so it is automatically disposed of at the end of this function.
    std::unique_ptr<prt2_writer::particle_chunk> chunk( item );

    bool isPrtO = chunk->usePositionOffset;

    std::vector<char>& particlesToWrite = chunk->compressed.empty() ? chunk->uncompressed : chunk->compressed;

    boost::uint32_t chunkSizeUint32 = static_cast<boost::uint32_t>( particlesToWrite.size() ) + ( isPrtO ? 12u : 0 );
    m_outputStream.write( reinterpret_cast<const char*>( &chunkSizeUint32 ), 4u );
    boost::uint32_t particleCountUint32 = static_cast<boost::uint32_t>( chunk->particleCount );
    m_outputStream.write( reinterpret_cast<const char*>( &particleCountUint32 ), 4u );
    if( isPrtO ) {
        m_outputStream.write( reinterpret_cast<const char*>( &chunk->positionOffset.x ), 12u );
    }
    if( !particlesToWrite.empty() ) {
        m_outputStream.write( &particlesToWrite[0], particlesToWrite.size() );
    }
    // Accumulate the PIdx
    m_particleChunkIndex.push_back( prt2_writer::particle_chunk_index_info() );
    m_particleChunkIndex.back().PRTChunkParticleCount = chunk->particleCount;
    m_particleChunkIndex.back().PRTChunkSize = chunkSizeUint32 + 8;

    // There is not always a known total particle count, so sometimes progress cannot be logged.
    if( m_particleCountTotal != 0 ) {
        if( ( chunk->particleCount & ~0xfff ) > 0 || ( m_particleCount & 0xfff ) == 0 ) {
            try {
                m_progress.update_progress( m_particleCount, m_particleCountTotal );
            } catch( frantic::logging::progress_cancel_exception& ) {
                m_isCancelled = true;
                throw;
            }
        }
    } else {
        try {
            m_progress.check_for_abort();
        } catch( frantic::logging::progress_cancel_exception& ) {
            m_isCancelled = true;
            throw;
        }
    }

    m_particleCount += chunk->particleCount;
    ++m_particleChunkCount;
}
