#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <unordered_set>

#include <unwind.h>

#include "frameobject.h"

#define UNW_LOCAL_ONLY
#include <libunwind.h>

#include "elf_shenanigans.h"
#include "frame_tree.h"
#include "hooks.h"
#include "record_writer.h"
#include "records.h"

#ifdef MEMRAY_TLS_MODEL
#    define MEMRAY_FAST_TLS __attribute__((tls_model(MEMRAY_TLS_MODEL)))
#else
#    define MEMRAY_FAST_TLS
#endif

namespace memray::tracking_api {

// Trace function interface

/**
 * Trace function to be installed in all Python treads to track function calls
 *
 * This trace function's sole purpose is to give a thread-safe, GIL-synchronized view of the Python
 * stack. To retrieve the Python stack using the C-API forces the caller to have the GIL held. Requiring
 * the GIL in the allocator function has too much impact on performance and can deadlock extension
 * modules that have native locks that are not synchronized themselves with the GIL. For this reason we
 * need a way to record and store the Python call frame information in a way that we can read without the
 * need to use the C-API. This trace function writes to disk the PUSH and POP operations so the Python
 *stack at any point can be reconstructed later.
 *
 **/
int
PyTraceFunction(PyObject* obj, PyFrameObject* frame, int what, PyObject* arg);

/**
 * Installs the trace function in the current thread.
 *
 * This function installs the trace function in the current thread using the C-API.
 *
 * */
void
install_trace_function();

class NativeTrace
{
  public:
    using ip_t = frame_id_t;

    NativeTrace()
    {
        d_data.resize(MAX_SIZE);
    };

    auto begin() const
    {
        return std::reverse_iterator(d_data.begin() + d_skip + d_size);
    }
    auto end() const
    {
        return std::reverse_iterator(d_data.begin() + d_skip);
    }
    ip_t operator[](size_t i) const
    {
        return d_data[d_skip + d_size - 1 - i];
    }
    int size() const
    {
        return d_size;
    }
    __attribute__((always_inline)) inline bool fill(size_t skip)
    {
        size_t size = unwind(d_data.data());
        if (size == MAX_SIZE) {
            d_data.resize(0);
            size = exact_unwind();
            MAX_SIZE = MAX_SIZE * 2 > size ? MAX_SIZE * 2 : size;
            d_data.resize(MAX_SIZE);
        }
        d_size = size > skip ? size - skip : 0;
        d_skip = skip;
        return d_size > 0;
    }

    static void setup()
    {
        // configure libunwind for better speed
        if (unw_set_caching_policy(unw_local_addr_space, UNW_CACHE_PER_THREAD)) {
            fprintf(stderr, "WARNING: Failed to enable per-thread libunwind caching.\n");
        }
#if (UNW_VERSION_MAJOR > 1 && UNW_VERSION_MINOR >= 3)
        if (unw_set_cache_size(unw_local_addr_space, 1024, 0)) {
            fprintf(stderr, "WARNING: Failed to set libunwind cache size.\n");
        }
#endif
    }

    static inline void flushCache()
    {
        unw_flush_cache(unw_local_addr_space, 0, 0);
    }

  private:
    MEMRAY_FAST_TLS static thread_local size_t MAX_SIZE;
    __attribute__((always_inline)) static inline int unwind(frame_id_t* data)
    {
        return unw_backtrace((void**)data, MAX_SIZE);
    }

    __attribute__((always_inline)) size_t inline exact_unwind()
    {
        unw_context_t context;
        if (unw_getcontext(&context) < 0) {
            std::cerr << "WARNING: Failed to initialize libunwind's context" << std::endl;
            return 0;
        }

        unw_cursor_t cursor;
        if (unw_init_local(&cursor, &context) < 0) {
            std::cerr << "WARNING: Failed to initialize libunwind's cursor" << std::endl;
            return 0;
        }

        do {
            unw_word_t ip;
            if (unw_get_reg(&cursor, UNW_REG_IP, &ip) < 0) {
                std::cerr << "WARNING: Failed to get instruction pointer" << std::endl;
                return 0;
            }
            d_data.emplace_back(ip);
        } while (unw_step(&cursor));
        return d_data.size();
    }

  private:
    size_t d_size = 0;
    size_t d_skip = 0;
    std::vector<ip_t> d_data;
};

/**
 * Singleton managing all the global state and functionality of the tracing mechanism
 *
 * This class acts as the only interface to the tracing functionality and encapsulates all the
 * required global state. *All access* must be done through the singleton interface as the singleton
 * has the same lifetime of the entire program. The singleton can be activated and deactivated to
 * temporarily stop the tracking as desired. The singleton manages a mirror copy of the Python stack
 * so it can be accessed synchronized by its the allocation tracking interfaces.
 * */
class Tracker
{
  public:
    // Constructors
    ~Tracker();

    Tracker(Tracker& other) = delete;
    Tracker(Tracker&& other) = delete;
    void operator=(const Tracker&) = delete;
    void operator=(Tracker&&) = delete;

    // Interface to get the tracker instance
    static PyObject* createTracker(
            std::unique_ptr<RecordWriter> record_writer,
            bool native_traces,
            unsigned int memory_interval,
            bool follow_fork);
    static PyObject* destroyTracker();
    static Tracker* getTracker();

    // Allocation tracking interface
    __attribute__((always_inline)) inline static void
    trackAllocation(void* ptr, size_t size, hooks::Allocator func)
    {
        Tracker* tracker = getTracker();
        if (tracker) {
            tracker->trackAllocationImpl(ptr, size, func);
        }
    }

    __attribute__((always_inline)) inline static void
    trackDeallocation(void* ptr, size_t size, hooks::Allocator func)
    {
        Tracker* tracker = getTracker();
        if (tracker) {
            tracker->trackDeallocationImpl(ptr, size, func);
        }
    }

    __attribute__((always_inline)) inline static void invalidate_module_cache()
    {
        Tracker* tracker = getTracker();
        if (tracker) {
            tracker->invalidate_module_cache_impl();
        }
    }

    __attribute__((always_inline)) inline static void updateModuleCache()
    {
        Tracker* tracker = getTracker();
        if (tracker) {
            tracker->updateModuleCacheImpl();
        }
    }

    __attribute__((always_inline)) inline static void registerThreadName(const char* name)
    {
        Tracker* tracker = getTracker();
        if (tracker) {
            tracker->registerThreadNameImpl(name);
        }
    }

    // RawFrame stack interface
    bool pushFrame(const RawFrame& frame);
    bool popFrames(uint32_t count);

    // Interface to activate/deactivate the tracking
    static const std::atomic<bool>& isActive();
    static void activate();
    static void deactivate();

  private:
    class BackgroundThread
    {
      public:
        // Constructors
        BackgroundThread(std::shared_ptr<RecordWriter> record_writer, unsigned int memory_interval);

        // Methods
        void start();
        void stop();

      private:
        // Data members
        std::shared_ptr<RecordWriter> d_writer;
        bool d_stop{false};
        unsigned int d_memory_interval;
        std::mutex d_mutex;
        std::condition_variable d_cv;
        std::thread d_thread;
        mutable std::ifstream d_procs_statm;

        // Methods
        size_t getRSS() const;
        static unsigned long int timeElapsed();
    };

    // Data members
    FrameCollection<RawFrame> d_frames{0, 2};
    static std::atomic<bool> d_active;
    static std::unique_ptr<Tracker> d_instance_owner;
    static std::atomic<Tracker*> d_instance;

    std::shared_ptr<RecordWriter> d_writer;
    FrameTree d_native_trace_tree;
    bool d_unwind_native_frames;
    unsigned int d_memory_interval;
    bool d_follow_fork;
    elf::SymbolPatcher d_patcher;
    std::unique_ptr<BackgroundThread> d_background_thread;

    // Methods
    frame_id_t registerFrame(const RawFrame& frame);

    void trackAllocationImpl(void* ptr, size_t size, hooks::Allocator func);
    void trackDeallocationImpl(void* ptr, size_t size, hooks::Allocator func);
    void invalidate_module_cache_impl();
    void updateModuleCacheImpl();
    void registerThreadNameImpl(const char* name);

    explicit Tracker(
            std::unique_ptr<RecordWriter> record_writer,
            bool native_traces,
            unsigned int memory_interval,
            bool follow_fork);

    static void prepareFork();
    static void parentFork();
    static void childFork();
};

}  // namespace memray::tracking_api
