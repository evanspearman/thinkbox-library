// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <exception>
#include <frantic/channels/channel_map_adaptor.hpp>
#include <frantic/particles/particle_array.hpp>
#include <frantic/particles/streams/particle_ostream.hpp>
#include <frantic/prtfile/prt2_writer.hpp>

#include <oneapi/tbb/concurrent_queue.h>
#include <oneapi/tbb/task_group.h>
#include <oneapi/tbb/parallel_pipeline.h>

#include <atomic>

namespace frantic {
namespace particles {
namespace streams {

class prt2_particle_ostream : public frantic::particles::streams::particle_ostream {
  public:

  private:
    oneapi::tbb::task_group m_taskGroup;
    frantic::prtfile::prt2_writer m_prt2;
    oneapi::tbb::concurrent_queue<std::vector<char>> m_particleChunkQueue;

    std::atomic<bool> m_closeRequested;
    std::thread m_writerThread;
    std::exception_ptr m_writerException;

    using pipeline_item = frantic::prtfile::prt2_writer::particle_chunk*;
    using stage_fn = std::function<pipeline_item( pipeline_item )>;
    using source_fn = std::function<pipeline_item()>;
    using sink_fn = std::function<void( pipeline_item )>;

    std::vector<stage_fn> m_filters;

    // A null progress logger we can count on to be around as long as this instance is.
    frantic::logging::null_progress_logger m_nullProgress;

    frantic::channels::channel_map m_particleChannelMap;
    frantic::channels::channel_map_adaptor m_pcmAdaptor;

    std::vector<char> m_particleChunkBuffer;
    boost::int64_t m_desiredChunkSizeInBytes;

    frantic::channels::channel_cvt_accessor<frantic::graphics::vector3fd> m_posAccessor;
    frantic::graphics::boundbox3fd m_boundbox;

    // Private copy constructor and assignment operator to disable copying
    prt2_particle_ostream( const prt2_particle_ostream& ) = delete;
    prt2_particle_ostream& operator=( const prt2_particle_ostream& ) = delete;

  public:
    prt2_particle_ostream(
        const frantic::tstring& file, const frantic::channels::channel_map& particleChannelMap,
        const frantic::channels::channel_map& particleChannelMapForFile,
        frantic::prtfile::prt2_compression_t compressionScheme = frantic::prtfile::prt2_compression_default,
        bool useTempFile = true, const std::filesystem::path& tempDir = std::filesystem::path(),
        const frantic::channels::property_map* generalMetadata = NULL,
        const std::map<frantic::tstring, frantic::channels::property_map>* channelMetadata = NULL,
        intptr_t desiredChunkSizeInBytes = 1000000 );

    virtual ~prt2_particle_ostream() noexcept override;

    void close() override;

    /** Get the file path where we are writing. */
    const std::filesystem::path& get_target_file() const;

    /** Get the name of this ostream
     *  (this is not guaranteed to be the same as the output file.  Use get_target_file() instead for that) */
    const frantic::tstring& get_stream_name() const { return m_prt2.get_stream_name(); }

    /** This is the particle layout for particles provided to the stream. */
    const frantic::channels::channel_map& get_channel_map() const override { return m_particleChannelMap; }

    /** Sets the particle layout for particles provided to the stream. */
    void set_channel_map( const frantic::channels::channel_map& particleChannelMap ) override;

    /** This is how big one particle is */
    std::size_t particle_size() const override { return m_particleChannelMap.structure_size(); }

    void put_particle( const char* rawParticleData ) override;
};

} // namespace streams
} // namespace particles
} // namespace frantic
