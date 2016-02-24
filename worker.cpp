#include <iostream>

#include "worker.h"

using namespace std;

Worker::Worker()
    : m_loop(ev::AUTO),
      m_async(m_loop)
{
    m_async.set<Worker, &Worker::onAsync>(this);
    m_async.start();
    ev_set_userdata(m_loop, this);
    ev_set_loop_release_cb(m_loop, releaseLoop, acquireLoop);

    m_thread = std::thread([this]()
    {
        m_mutex.lock();
        m_loop.run();
        m_mutex.unlock();
    });
}

void Worker::onAsync(ev::async &watcher, int revents)
{
    static_cast<void>(watcher);
    static_cast<void>(revents);
    if (m_stop)
    {
        m_loop.break_loop();
    }
}

void Worker::lock()
{
    m_mutex.lock();
}

void Worker::unlock()
{
    m_mutex.unlock();
}

bool Worker::try_lock()
{
    return m_mutex.try_lock();
}

void Worker::notify()
{
    if (std::this_thread::get_id() != m_thread.get_id())
        m_async.send();
}

void Worker::releaseLoop(struct ev_loop *loop)
{
    //cout << "Release loop: 0x" << (void*)loop << endl;
    auto *self = static_cast<Worker*>(ev_userdata(loop));
    self->unlock();
}

void Worker::acquireLoop(struct ev_loop *loop)
{
    //cout << "Acquire loop: 0x" << (void*)loop << endl;
    auto *self = static_cast<Worker*>(ev_userdata(loop));
    self->lock();
}

WorkerPool::WorkerPool(size_t threads)
{
    // Multithreading
    if (threads == 0)
    {
        threads = std::thread::hardware_concurrency();
        cout << "Adjust threads count to: " << threads << endl;
    }

    m_workers.reserve(threads);
    for (size_t i = 0; i < threads; ++i)
    {
        m_workers.emplace_back(new Worker);
    }
}
