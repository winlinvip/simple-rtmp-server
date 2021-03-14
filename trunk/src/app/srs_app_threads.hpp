/**
 * The MIT License (MIT)
 *
 * Copyright (c) 2013-2020 Winlin
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef SRS_APP_THREADS_HPP
#define SRS_APP_THREADS_HPP

#include <srs_core.hpp>

#include <srs_kernel_file.hpp>
#include <srs_kernel_flv.hpp>

#include <pthread.h>

#include <vector>
#include <string>

class SrsThreadPool;

// The thread mutex wrapper, without error.
class SrsThreadMutex
{
private:
    pthread_mutex_t lock_;
    pthread_mutexattr_t attr_;
public:
    SrsThreadMutex();
    virtual ~SrsThreadMutex();
public:
    void lock();
    void unlock();
};

// The thread mutex locker.
#define SrsThreadLocker(instance) \
    impl__SrsThreadLocker _SRS_free_##instance(instance)

class impl__SrsThreadLocker
{
private:
    SrsThreadMutex* lock;
public:
    impl__SrsThreadLocker(SrsThreadMutex* l) {
        lock = l;
        lock->lock();
    }
    virtual ~impl__SrsThreadLocker() {
        lock->unlock();
    }
};

// The information for a thread.
class SrsThreadEntry
{
public:
    SrsThreadPool* pool;
    std::string label;
    srs_error_t (*start)(void* arg);
    void* arg;
    int num;
public:
    // The thread object.
    pthread_t trd;
    // The exit error of thread.
    srs_error_t err;
public:
    SrsThreadEntry();
    virtual ~SrsThreadEntry();
};

// Allocate a(or almost) fixed thread poll to execute tasks,
// so that we can take the advantage of multiple CPUs.
class SrsThreadPool
{
private:
    SrsThreadEntry* entry_;
    srs_utime_t interval_;
private:
    SrsThreadMutex* lock_;
    std::vector<SrsThreadEntry*> threads_;
public:
    SrsThreadPool();
    virtual ~SrsThreadPool();
public:
    // Initialize the thread pool.
    srs_error_t initialize();
    // Execute start function with label in thread.
    srs_error_t execute(std::string label, srs_error_t (*start)(void* arg), void* arg);
    // Run in the primordial thread, util stop or quit.
    srs_error_t run();
    // Stop the thread pool and quit the primordial thread.
    void stop();
private:
    static void* start(void* arg);
};

// The global thread pool.
extern SrsThreadPool* _srs_thread_pool;

// We use coroutine queue to collect messages from different coroutines,
// then swap to the SrsThreadQueue and process by another thread.
template<typename T>
class SrsCoroutineQueue
{
private:
    std::vector<T*> dirty_;
public:
    SrsCoroutineQueue() {
    }
    virtual ~SrsCoroutineQueue() {
        for (int i = 0; i < (int)dirty_.size(); i++) {
            T* msg = dirty_.at(i);
            srs_freep(msg);
        }
    }
public:
    // SrsCoroutineQueue::push_back
    void push_back(T* msg) {
        dirty_.push_back(msg);
    }
    // SrsCoroutineQueue::swap
    void swap(std::vector<T*>& flying) {
        dirty_.swap(flying);
    }
    // SrsCoroutineQueue::size
    size_t size() {
        return dirty_.size();
    }
};

// Thread-safe queue.
template<typename T>
class SrsThreadQueue
{
private:
    std::vector<T*> dirty_;
    SrsThreadMutex* lock_;
public:
    // SrsThreadQueue::SrsThreadQueue
    SrsThreadQueue() {
        lock_ = new SrsThreadMutex();
    }
    // SrsThreadQueue::~SrsThreadQueue
    virtual ~SrsThreadQueue() {
        srs_freep(lock_);
        for (int i = 0; i < (int)dirty_.size(); i++) {
            T* msg = dirty_.at(i);
            srs_freep(msg);
        }
    }
public:
    // SrsThreadQueue::push_back
    void push_back(T* msg) {
        SrsThreadLocker(lock_);
        dirty_.push_back(msg);
    }
    // SrsThreadQueue::push_back
    void push_back(std::vector<T*>& flying) {
        SrsThreadLocker(lock_);
        dirty_.insert(dirty_.end(), flying.begin(), flying.end());
    }
    // SrsThreadQueue::swap
    void swap(std::vector<T*>& flying) {
        SrsThreadLocker(lock_);
        dirty_.swap(flying);
    }
    // SrsThreadQueue::size
    size_t size() {
        SrsThreadLocker(lock_);
        return dirty_.size();
    }
};

// Async file writer.
class SrsAsyncFileWriter : public ISrsWriter
{
    friend class SrsAsyncLogManager;
private:
    std::string filename_;
    SrsFileWriter* writer_;
private:
    // The thread-queue, to flush to disk by dedicated thread.
    SrsThreadQueue<SrsSharedPtrMessage>* queue_;
private:
    // The coroutine-queue, to avoid requires lock for each log.
    SrsCoroutineQueue<SrsSharedPtrMessage>* co_queue_;
private:
    SrsAsyncFileWriter(std::string p);
    virtual ~SrsAsyncFileWriter();
public:
    // Open file writer, in truncate mode.
    virtual srs_error_t open();
    // Open file writer, in append mode.
    virtual srs_error_t open_append();
    // Close current writer.
    virtual void close();
// Interface ISrsWriteSeeker
public:
    virtual srs_error_t write(void* buf, size_t count, ssize_t* pnwrite);
    virtual srs_error_t writev(const iovec* iov, int iovcnt, ssize_t* pnwrite);
public:
    // Flush coroutine-queue to thread-queue, avoid requiring lock for each message.
    void flush_co_queue();
    // Flush thread-queue to disk, generally by dedicated thread.
    srs_error_t flush();
};

// The async log file writer manager, use a thread to flush multiple writers,
// and reopen all log files when got LOGROTATE signal.
class SrsAsyncLogManager
{
private:
    // The async flush interval.
    srs_utime_t interval_;
private:
    // The async reopen event.
    bool reopen_;
private:
    SrsThreadMutex* lock_;
    std::vector<SrsAsyncFileWriter*> writers_;
public:
    SrsAsyncLogManager();
    virtual ~SrsAsyncLogManager();
public:
    // Initialize the async log manager.
    srs_error_t initialize();
    // Run the async log manager thread.
    srs_error_t run();
    // Create a managed writer, user should never free it.
    srs_error_t create_writer(std::string filename, SrsAsyncFileWriter** ppwriter);
    // Reopen all log files, asynchronously.
    virtual void reopen();
public:
    // Get the summary of this manager.
    std::string description();
private:
    static srs_error_t start(void* arg);
    srs_error_t do_start();
};

// The global async log manager.
extern SrsAsyncLogManager* _srs_async_log;

#endif
