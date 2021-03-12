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

#include <srs_app_threads.hpp>

#include <srs_kernel_error.hpp>
#include <srs_app_config.hpp>
#include <srs_app_log.hpp>
#include <srs_core_autofree.hpp>

#include <unistd.h>

using namespace std;

SrsThreadMutex::SrsThreadMutex()
{
    // https://michaelkerrisk.com/linux/man-pages/man3/pthread_mutex_init.3p.html
    int r0 = pthread_mutex_init(&lock_, NULL);
    srs_assert(!r0);
}

SrsThreadMutex::~SrsThreadMutex()
{
    int r0 = pthread_mutex_destroy(&lock_);
    srs_assert(!r0);
}

void SrsThreadMutex::lock()
{
    // https://man7.org/linux/man-pages/man3/pthread_mutex_lock.3p.html
    int r0 = pthread_mutex_lock(&lock_);
    srs_assert(!r0);
}

void SrsThreadMutex::unlock()
{
    int r0 = pthread_mutex_unlock(&lock_);
    srs_assert(!r0);
}

SrsThreadEntry::SrsThreadEntry()
{
    pool = NULL;
    start = NULL;
    arg = NULL;
    num = 0;

    err = srs_success;
}

SrsThreadPool::SrsThreadPool()
{
    entry_ = NULL;
    lock_ = new SrsThreadMutex();
}

SrsThreadPool::~SrsThreadPool()
{
    srs_freep(lock_);
}

srs_error_t SrsThreadPool::initialize()
{
    srs_error_t err = srs_success;

    // TODO: FIXME: Should init ST for each thread.
    if ((err = srs_st_init()) != srs_success) {
        return srs_error_wrap(err, "initialize st failed");
    }

    // Add primordial thread, current thread itself.
    SrsThreadEntry* entry = new SrsThreadEntry();
    threads_.push_back(entry);
    entry_ = entry;

    entry->pool = this;
    entry->label = "primordial";
    entry->start = NULL;
    entry->arg = NULL;
    entry->num = 1;

    srs_trace("Thread #%d: %s init", entry_->num, entry_->label.c_str());

    return err;
}

srs_error_t SrsThreadPool::execute(string label, srs_error_t (*start)(void* arg), void* arg)
{
    srs_error_t err = srs_success;

    static int num = entry_->num + 1;

    SrsThreadEntry* entry = new SrsThreadEntry();

    if (true) {
        SrsThreadLocker(lock_);
        threads_.push_back(entry);
    }

    entry->pool = this;
    entry->label = label;
    entry->start = start;
    entry->arg = arg;
    entry->num = num++;

    // https://man7.org/linux/man-pages/man3/pthread_create.3.html
    pthread_t trd;
    int r0 = pthread_create(&trd, NULL, SrsThreadPool::start, entry);
    if (r0 != 0) {
        entry->err = srs_error_new(ERROR_THREAD_CREATE, "create thread %s", label.c_str());
        return srs_error_copy(entry->err);
    }

    entry->trd = trd;

    return err;
}

srs_error_t SrsThreadPool::run()
{
    srs_error_t err = srs_success;

    while (true) {
        srs_trace("Thread #%d: %s run, threads=%d", entry_->num, entry_->label.c_str(),
            (int)threads_.size());
        sleep(60);
    }

    return err;
}

void SrsThreadPool::stop()
{
    // TODO: FIXME: Implements it.
}

void* SrsThreadPool::start(void* arg)
{
    srs_error_t err = srs_success;

    SrsThreadEntry* entry = (SrsThreadEntry*)arg;
    srs_trace("Thread #%d: %s run", entry->num, entry->label.c_str());

    if ((err = entry->start(entry->arg)) != srs_success) {
        entry->err = err;
    }

    // We do not use the return value, the err has been set to entry->err.
    return NULL;
}

SrsThreadPool* _srs_thread_pool = new SrsThreadPool();
