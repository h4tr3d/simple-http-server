#ifndef WORKER_H
#define WORKER_H

#include <vector>
#include <memory>
#include <mutex>
#include <thread>
#include <atomic>

#include <ev++.h>

/**
 * @brief The DummyMutex struct
 */
struct DummyMutex
{
    void lock(){}
    void unlock(){}
    bool try_lock(){return true;}
};

/**
 * @brief The Worker struct
 *
 * Event loop with associated thread and locking protection holder
 */
struct Worker
{
    using Mutex = std::mutex;
    //using Mutex = DummyMutex;

    Worker();

    // Dummy async cb
    void onAsync(ev::async &watcher, int revents);

    // Mutex interface to use with std::lock_guard<> and so on
    void lock();

    void unlock();

    bool try_lock();

    // Own interface
    /**
     * @brief notify loop about changes from another thread
     */
    void notify();

    /**
     * @brief modify loop in thread-safe way
     *
     * This method wrap modify callback with appropriate code. It lock mutex only in case when
     * runs from another thred then execute this loop. It also notify loop about changes if needed.
     *
     * Example:
     * @code
     * size_t threads = g_workerPool->m_workers.size();
     * m_acceptors.reserve(threads);
     * for (int i = 0; i < threads; ++i)
     * {
     *     cout << "Add acceptor: " << (i+1) << endl;
     *     Worker &worker = *g_workerPool->m_workers[i];
     *
     *     worker.modify([this,&worker](){
     *         m_acceptors.emplace_back( new ev::io(worker.m_loop) );
     *         ev::io &io = *m_acceptors.back();
     *         io.set<TcpServer, &TcpServer::onAccept>(this);
     *         io.start(m_socket, ev::READ);
     *     });
     * }
     * @endcode
     *
     */
    template<typename T>
    void modify(const T &modifyRoutine)
    {
        bool needLock = std::this_thread::get_id() != m_thread.get_id();

        {
            std::unique_lock<Mutex> guard(m_mutex, std::defer_lock);
            if (needLock)
                guard.lock();

            modifyRoutine();
        }

        if (needLock)
            m_async.send();
    }

    void stop()
    {
        m_stop = true;
        notify();
    }

    // libev loop lock interface
    static void releaseLoop(struct ev_loop *loop);
    static void acquireLoop(struct ev_loop *loop);

    Mutex                   m_mutex;
    std::thread             m_thread;
    std::atomic<bool>       m_stop{false};
    ev::dynamic_loop        m_loop;
    ev::async               m_async;
};

template<typename Loop>
inline
Worker *get_loop_worker(Loop &loop)
{
    return static_cast<Worker*>(ev_userdata(loop));
}


/**
 * @brief The WorkerPool struct
 *
 * Declares workers pool
 *
 */
struct WorkerPool
{
    WorkerPool(size_t threads = 0);

    std::vector<std::unique_ptr<Worker>> m_workers;
};


#endif // WORKER_H
