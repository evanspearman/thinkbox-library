// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>
#include <exception>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

#include <oneapi/tbb/concurrent_queue.h>


namespace frantic {
namespace threads {

namespace detail {

struct thread_exception : std::runtime_error {
    explicit thread_exception( const std::string& msg )
        : std::runtime_error( msg ) {}
};

} // namespace

template <class ProducerConsumerModel>
class buffered_producer_consumer {
    typedef typename ProducerConsumerModel::buffer_type buffer_type;

    /**
     * This class is designed to hold a temporary ptr to an object, deleting it if destroyed while holding an item.
     * I made this instead of using std::unique_ptr because I wanted to be able to set the stored ptr by assigning to
     * a reference (tbb::concurrent_queue::push for example).
     */
    class ptr_guard {
        buffer_type* item;

      public:
        ptr_guard() { item = nullptr; }
        ~ptr_guard() {
            if( item )
                delete item;
        }

        inline buffer_type* get() { return item; } // Get a ptr to the currenly owned item
        inline buffer_type*& get_ref() {
            assert( !item );
            return item;
        } // Returns a ref so this pointer can be set. Must not be called with item is non-NULL.
        inline buffer_type* release() {
            buffer_type* result = item;
            item = nullptr;
            return result;
        } // Releases ownership of the current item.
    };

    // A counter of the number of active threads. When it becomes zero, all producer threads have finished.
    std::atomic<int> numThreadsRemaining;

    // Will be atomically set to true when a thread has registered an exception.
    std::atomic<bool> errorOccurred;

    std::atomic<bool> stopRequested;

    std::exception_ptr error;

    // A pair of queues for passing empty and full buffers between producers and the consumer. The producer threads will
    // be responsible for creating the initial buffers.
    oneapi::tbb::concurrent_queue<buffer_type*> emptyItems, fullItems;

    template <class T>
    inline static bool concurrent_queue_try_pop( tbb::concurrent_queue<T>& queue, T& outValue ) {
        return queue.try_pop( outValue );
    }

    /**
     * This is the code executed by worker threads.
     * @param prod The producer meta-object used to create buffers and Producer::thread_instance objects that do the
     * actual production.
     */
    void producer_fn() {
        try {
            typedef typename ProducerConsumerModel::producer_instance producer_instance;
            producer_instance threadProd( *m_pcModel );

            ptr_guard theItem;

            // Create an extra buffer for later use by this thread.
            theItem.get_ref() = m_pcModel->create_buffer();
            emptyItems.push( theItem.release() );

            // Create the buffer we will be filling.
            theItem.get_ref() = m_pcModel->create_buffer();
            threadProd.init_buffer( theItem.get() );

            while( 1 ) {
                if( stopRequested.load( std::memory_order_relaxed ) ) {
                    throw detail::thread_exception("worker interrupted" );
                }

                if( !threadProd.can_produce_more() ) {
                    // This thread should exit since there is no more data to produce. We may need to flush if our
                    // current buffer is non-empty.
                    if( !threadProd.is_buffer_empty( theItem.get() ) ) {
                        threadProd.finish_buffer( theItem.get() );
                        fullItems.push( theItem.release() );
                    } else {
                        emptyItems.push( theItem.release() );
                    }

                    break;
                }

                if( threadProd.is_buffer_full( theItem.get() ) ) {
                    threadProd.finish_buffer( theItem.get() );
                    fullItems.push( theItem.release() );

                    while( !concurrent_queue_try_pop( emptyItems, theItem.get_ref() ) ) {
                        if( stopRequested.load( std::memory_order_relaxed ) ) {
                            throw detail::thread_exception("worker interrupted" );
                        }
                        std::this_thread::yield();
                    }

                    threadProd.init_buffer( theItem.get() );
                }

                // We have a non-full buffer so produce data to go into it.
                threadProd.fill_buffer( theItem.get() );
            }
        } catch( ... ) {
            bool expected = false;
            if( errorOccurred.compare_exchange_strong( expected, true ) ) {
                error = std::current_exception();
                stopRequested.store( true, std::memory_order_relaxed );
            }
        }

        --numThreadsRemaining;
        return;
    }

  private:
    ProducerConsumerModel* m_pcModel;

    std::vector<std::thread> m_threads;

  public:
    buffered_producer_consumer()
    : numThreadsRemaining(0)
    , errorOccurred(false)
    , stopRequested(false)
    , error() {}

    ~buffered_producer_consumer() {
        stopRequested.store( true, std::memory_order_relaxed );
        for ( std::thread& t : m_threads ) {
            if( t.joinable() ) {
                t.join();
            }
        }

        buffer_type* item = nullptr;
        while( concurrent_queue_try_pop( emptyItems, item ) )
            delete item;
        while( concurrent_queue_try_pop( fullItems, item ) )
            delete item;
    }

    void reset( ProducerConsumerModel& pcModel, unsigned int numThreads = 0 ) {
        m_pcModel = &pcModel;

        numThreads = std::max( 1u, numThreads == 0 ? std::thread::hardware_concurrency() - 1u : numThreads );

        // Set the atomic counter to track the number of outstanding worker threads.
        numThreadsRemaining = static_cast<int>( numThreads );
        errorOccurred = false;
        stopRequested = false;
        error = nullptr;
        m_threads.clear();
        m_threads.reserve( numThreads );

        for( unsigned int i = 0; i < numThreads; ++i ) {
            m_threads.emplace_back( [this]() { producer_fn(); });
        }
    }

    /**
     * This function will consume data produced by the worker threads until they have all exited.
     * @param cons The consumer implementation object that will process filled buffers.
     */
    void run( bool untilDone = true ) {
        try {
            typedef typename ProducerConsumerModel::consumer_instance consumer_instance;
            consumer_instance threadCons( *m_pcModel );

            ptr_guard theItem;

            do {
                if( !concurrent_queue_try_pop( fullItems, theItem.get_ref() ) ) {
                    threadCons.do_idle_process();

                    while( !concurrent_queue_try_pop( fullItems, theItem.get_ref() ) ) {
                        if( errorOccurred.load( std::memory_order_relaxed ) ) {
                            stopRequested.store( true, std::memory_order_relaxed );
                            break;
                        }
                        if( numThreadsRemaining == 0 ) {
                            if( concurrent_queue_try_pop( fullItems, theItem.get_ref() ) )
                                break;
                            goto done;
                        }
                        std::this_thread::yield();
                    }

                    if( errorOccurred.load( std::memory_order_relaxed ) && !theItem.get() ) {
                        break;
                    }
                }

                threadCons.consume_buffer( theItem.get() );
                emptyItems.push( theItem.release() );
            } while( untilDone );

    done:
            stopRequested.store( true, std::memory_order_relaxed );

            for( std::thread& t : m_threads ) {
                if( t.joinable() ) {
                    t.join();
                }
            }

            if( error ) {
                std::rethrow_exception( error );
            }
        } catch( ... ) {
            stopRequested.store( true, std::memory_order_relaxed );

            for( std::thread& t : m_threads ) {
                if( t.joinable() ) {
                    t.join();
                }
            }

            throw;
        }
    }

    void return_finished_item( std::unique_ptr<buffer_type> item ) { emptyItems.push( item.release() ); }

    std::unique_ptr<buffer_type> steal_finished_item( bool waitForItem = true ) {
        std::unique_ptr<buffer_type> result;

        try {
            buffer_type* theItem = nullptr;

            if( waitForItem ) {
                while( !concurrent_queue_try_pop( fullItems, theItem ) ) {
                    if( errorOccurred.load( std::memory_order_relaxed ) ) {
                        stopRequested.store( true, std::memory_order_relaxed );
                        break;
                    }
                    if( numThreadsRemaining == 0 ) {
                        concurrent_queue_try_pop(
                            fullItems,
                            theItem ); // Check again to make sure we didn't get an item before #threads went to 0.
                        break;
                    }
                    std::this_thread::yield();
                }
            } else if( !concurrent_queue_try_pop( fullItems, theItem ) && errorOccurred ) {
                std::rethrow_exception( error );
            }

            result.reset( theItem );
        } catch( ... ) {
            stopRequested.store( true, std::memory_order_relaxed );

            for( std::thread& t : m_threads ) {
                if( t.joinable() ) {
                    t.join();
                }
            }

            if( error ) {
                std::rethrow_exception( error );
            }

            throw;
        }

        return result;
    }

    bool is_done() { return ( numThreadsRemaining == 0 ); }
};

/**
 * This algorithm will operate the Poducer/Consumer pattern for the given template arguments. The construction of the
 * types is defined above.
 *
 * This algorithm will (in parallel) fill buffer objects using ProducerConsumerModel::producer_instance::fill_buffer()
 * and pass them serially to a single Consumer object via. ProducerConsumerModel::consumer_instance::consume_buffer.
 * Once the buffer is consumed it will be made available to another ProducerConsumerModel::producer_instance.
 *
 * @note ProducerConsumerModel::consumer_instance::consume_buffer() will always be called in the context of the thread
 * calling buffered_producer_consumer().
 *
 * @param pcModel An instance of ProducerConsumerModel that supports the interface described above.
 * @param numWorkers The number of worker threads to use, or 0 if the system should decide.
 */
template <class ProducerConsumerModel>
void do_buffered_producer_consumer( ProducerConsumerModel& pcModel, unsigned int numWorkers = 0 ) {
    buffered_producer_consumer<ProducerConsumerModel> theImpl;
    theImpl.reset( pcModel, numWorkers );
    theImpl.run();
}


} // namespace threads
} // namespace frantic
