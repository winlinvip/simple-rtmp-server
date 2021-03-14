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
#include <srs_kernel_utility.hpp>

#include <unistd.h>

using namespace std;

SrsThreadMutex::SrsThreadMutex()
{
    // https://man7.org/linux/man-pages/man3/pthread_mutexattr_init.3.html
    int r0 = pthread_mutexattr_init(&attr_);
    srs_assert(!r0);

    // https://man7.org/linux/man-pages/man3/pthread_mutexattr_gettype.3p.html
    r0 = pthread_mutexattr_settype(&attr_, PTHREAD_MUTEX_ERRORCHECK);
    srs_assert(!r0);

    // https://michaelkerrisk.com/linux/man-pages/man3/pthread_mutex_init.3p.html
    r0 = pthread_mutex_init(&lock_, &attr_);
    srs_assert(!r0);
}

SrsThreadMutex::~SrsThreadMutex()
{
    int r0 = pthread_mutex_destroy(&lock_);
    srs_assert(!r0);

    r0 = pthread_mutexattr_destroy(&attr_);
    srs_assert(!r0);
}

void SrsThreadMutex::lock()
{
    // https://man7.org/linux/man-pages/man3/pthread_mutex_lock.3p.html
    //        EDEADLK
    //                 The mutex type is PTHREAD_MUTEX_ERRORCHECK and the current
    //                 thread already owns the mutex.
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

SrsThreadEntry::~SrsThreadEntry()
{
}

SrsThreadPool::SrsThreadPool()
{
    entry_ = NULL;
    lock_ = new SrsThreadMutex();

    // Add primordial thread, current thread itself.
    SrsThreadEntry* entry = new SrsThreadEntry();
    threads_.push_back(entry);
    entry_ = entry;

    entry->pool = this;
    entry->label = "primordial";
    entry->start = NULL;
    entry->arg = NULL;
    entry->num = 1;
}

// TODO: FIMXE: If free the pool, we should stop all threads.
SrsThreadPool::~SrsThreadPool()
{
    srs_freep(lock_);
}

// @remark Note that we should never write logs, because log is not ready not.
srs_error_t SrsThreadPool::initialize()
{
    srs_error_t err = srs_success;

    // TODO: FIXME: Should init ST for each thread.
    if ((err = srs_st_init()) != srs_success) {
        return srs_error_wrap(err, "initialize st failed");
    }

    interval_ = _srs_config->get_threads_interval();

    return err;
}

srs_error_t SrsThreadPool::execute(string label, srs_error_t (*start)(void* arg), void* arg)
{
    srs_error_t err = srs_success;

    SrsThreadEntry* entry = new SrsThreadEntry();

    // To protect the threads_ for executing thread-safe.
    if (true) {
        SrsThreadLocker(lock_);
        threads_.push_back(entry);
    }

    entry->pool = this;
    entry->label = label;
    entry->start = start;
    entry->arg = arg;

    // The id of thread, should equal to the debugger thread id.
    // For gdb, it's: info threads
    // For lldb, it's: thread list
    static int num = entry_->num + 1;
    entry->num = num++;

    // https://man7.org/linux/man-pages/man3/pthread_create.3.html
    pthread_t trd;
    int r0 = pthread_create(&trd, NULL, SrsThreadPool::start, entry);
    if (r0 != 0) {
        entry->err = srs_error_new(ERROR_THREAD_CREATE, "create thread %s, r0=%d", label.c_str(), r0);
        return srs_error_copy(entry->err);
    }

    entry->trd = trd;

    return err;
}

srs_error_t SrsThreadPool::run()
{
    srs_error_t err = srs_success;

    // Write the init log here.
    srs_trace("Thread #%d(%s): init interval=%dms", entry_->num, entry_->label.c_str(), srsu2msi(interval_));

    while (true) {
        sleep(interval_ / SRS_UTIME_SECONDS);

        string async_logs = _srs_async_log->description();
        srs_trace("Thread #%d(%s): cycle threads=%d%s", entry_->num, entry_->label.c_str(), (int)threads_.size(),
            async_logs.c_str());
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
    srs_trace("Thread #%d(%s): run", entry->num, entry->label.c_str());

    if ((err = entry->start(entry->arg)) != srs_success) {
        entry->err = err;
    }

    // We do not use the return value, the err has been set to entry->err.
    return NULL;
}

SrsThreadPool* _srs_thread_pool = new SrsThreadPool();

SrsAsyncFileWriter::SrsAsyncFileWriter(std::string p, srs_utime_t interval)
{
    filename_ = p;
    writer_ = new SrsFileWriter();
    queue_ = new SrsThreadQueue<SrsSharedPtrMessage>();
    co_queue_ = new SrsCoroutineQueue<SrsSharedPtrMessage>();
    interval_ = interval;
    last_flush_time_ = srs_get_system_time();
}

// TODO: FIXME: Before free the writer, we must remove it from the manager.
SrsAsyncFileWriter::~SrsAsyncFileWriter()
{
    // TODO: FIXME: Should we flush dirty logs?
    srs_freep(writer_);
    srs_freep(queue_);
    srs_freep(co_queue_);
}

srs_error_t SrsAsyncFileWriter::open()
{
    return writer_->open(filename_);
}

srs_error_t SrsAsyncFileWriter::open_append()
{
    return writer_->open_append(filename_);
}

void SrsAsyncFileWriter::close()
{
    writer_->close();
}

srs_error_t SrsAsyncFileWriter::write(void* buf, size_t count, ssize_t* pnwrite)
{
    srs_error_t err = srs_success;

    if (count <= 0) {
        return err;
    }

    char* cp = new char[count];
    memcpy(cp, buf, count);

    SrsSharedPtrMessage* msg = new SrsSharedPtrMessage();
    msg->wrap(cp, count);

    co_queue_->push_back(msg);

    // Whether flush to thread-queue.
    if (srs_get_system_time() - last_flush_time_ >= interval_) {
        last_flush_time_ = srs_get_system_time();

        vector<SrsSharedPtrMessage*> flying;
        co_queue_->swap(flying);
        queue_->push_back(flying);
    }

    if (pnwrite) {
        *pnwrite = count;
    }

    return err;
}

srs_error_t SrsAsyncFileWriter::writev(const iovec* iov, int iovcnt, ssize_t* pnwrite)
{
    srs_error_t err = srs_success;

    for (int i = 0; i < iovcnt; i++) {
        const iovec* p = iov + i;

        ssize_t nn = 0;
        if ((err = write(p->iov_base, p->iov_len, &nn)) != srs_success) {
            return srs_error_wrap(err, "write %d iov %d bytes", i, p->iov_len);
        }

        if (pnwrite) {
            *pnwrite += nn;
        }
    }

    return err;
}

srs_error_t SrsAsyncFileWriter::flush()
{
    srs_error_t err = srs_success;

    vector<SrsSharedPtrMessage*> flying;
    queue_->swap(flying);

    for (int i = 0; i < (int)flying.size(); i++) {
        SrsSharedPtrMessage* msg = flying.at(i);

        srs_error_t r0 = writer_->write(msg->payload, msg->size, NULL);

        // Choose a random error to return.
        if (err == srs_success) {
            err = r0;
        } else {
            srs_freep(r0);
        }

        srs_freep(msg);
    }

    return err;
}

SrsAsyncLogManager::SrsAsyncLogManager()
{
    interval_ = 0;

    reopen_ = false;
    lock_ = new SrsThreadMutex();
}

// TODO: FIXME: We should stop the thread first, then free the manager.
SrsAsyncLogManager::~SrsAsyncLogManager()
{
    srs_freep(lock_);

    for (int i = 0; i < (int)writers_.size(); i++) {
        SrsAsyncFileWriter* writer = writers_.at(i);
        srs_freep(writer);
    }
}

// @remark Note that we should never write logs, because log is not ready not.
srs_error_t SrsAsyncLogManager::initialize()
{
    srs_error_t err =  srs_success;

    interval_ = _srs_config->srs_log_flush_interval();
    if (interval_ <= 0) {
        return srs_error_new(ERROR_SYSTEM_LOGFILE, "invalid interval=%dms", srsu2msi(interval_));
    }

    return err;
}

// @remark Now, log is ready, and we can print logs.
srs_error_t SrsAsyncLogManager::run()
{
    srs_error_t err =  srs_success;

    if ((err = _srs_thread_pool->execute("log", SrsAsyncLogManager::start, this)) != srs_success) {
        return srs_error_wrap(err, "run async log");
    }

    srs_trace("AsyncLogs: Init flush_interval=%dms", srsu2msi(interval_));

    return err;
}

srs_error_t SrsAsyncLogManager::create_writer(std::string filename, SrsAsyncFileWriter** ppwriter)
{
    srs_error_t err = srs_success;

    SrsAsyncFileWriter* writer = new SrsAsyncFileWriter(filename, interval_);
    writers_.push_back(writer);

    if ((err = writer->open()) != srs_success) {
        return srs_error_wrap(err, "open file %s fail", filename.c_str());
    }

    *ppwriter = writer;
    return err;
}

void SrsAsyncLogManager::reopen()
{
    SrsThreadLocker(lock_);
    reopen_ = true;
}

std::string SrsAsyncLogManager::description()
{
    SrsThreadLocker(lock_);

    int nn_logs = 0;
    int max_logs = 0;
    for (int i = 0; i < (int)writers_.size(); i++) {
        SrsAsyncFileWriter* writer = writers_.at(i);

        int nn = (int)writer->queue_->size();
        nn_logs += nn;
        max_logs = srs_max(max_logs, nn);
    }

    int nn_co_logs = 0;
    int max_co_logs = 0;
    for (int i = 0; i < (int)writers_.size(); i++) {
        SrsAsyncFileWriter* writer = writers_.at(i);

        int nn = (int)writer->co_queue_->size();
        nn_co_logs += nn;
        max_co_logs = srs_max(max_co_logs, nn);
    }

    static char buf[128];
    snprintf(buf, sizeof(buf), ", files=%d, queue=%d/%d, coq=%d/%d",
        (int)writers_.size(), nn_logs, max_logs, nn_co_logs, max_co_logs);

    return buf;
}

srs_error_t SrsAsyncLogManager::start(void* arg)
{
    SrsAsyncLogManager* log = (SrsAsyncLogManager*)arg;
    return log->do_start();
}

srs_error_t SrsAsyncLogManager::do_start()
{
    srs_error_t err = srs_success;

    // Never quit for this thread.
    while (true) {
        // Reopen all log files.
        if (reopen_) {
            SrsThreadLocker(lock_);
            reopen_ = false;

            for (int i = 0; i < (int)writers_.size(); i++) {
                SrsAsyncFileWriter* writer = writers_.at(i);

                writer->close();
                if ((err = writer->open()) != srs_success) {
                    srs_error_reset(err); // Ignore any error for reopen logs.
                }
            }
        }

        // Flush all logs from cache to disk.
        if (true) {
            SrsThreadLocker(lock_);

            for (int i = 0; i < (int)writers_.size(); i++) {
                SrsAsyncFileWriter* writer = writers_.at(i);

                if ((err = writer->flush()) != srs_success) {
                    srs_error_reset(err); // Ignore any error for flushing logs.
                }
            }
        }

        // We use the system primordial sleep, not the ST sleep, because
        // this is a system thread, not a coroutine.
        timespec tv = {0};
        tv.tv_sec = interval_ / SRS_UTIME_SECONDS;
        tv.tv_nsec = (interval_ % SRS_UTIME_MILLISECONDS) * 1000;
        nanosleep(&tv, NULL);
    }

    return err;
}

SrsAsyncLogManager* _srs_async_log = new SrsAsyncLogManager();
