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

#include <pthread.h>

#include <vector>
#include <string>

class SrsThreadPool;

// The thread mutex wrapper, without error.
class SrsThreadMutex
{
private:
    pthread_mutex_t lock_;
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

    SrsThreadEntry();
};

// Allocate a(or almost) fixed thread poll to execute tasks,
// so that we can take the advantage of multiple CPUs.
class SrsThreadPool
{
private:
    SrsThreadEntry* entry_;
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

#endif
