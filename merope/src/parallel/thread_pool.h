#pragma once

#include <condition_variable>
#include <cstddef>
#include <exception>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace merope {

std::size_t default_worker_count() noexcept;

class c_thread_pool {
public:
    explicit c_thread_pool(std::size_t workers = 0);
    ~c_thread_pool();

    c_thread_pool(const c_thread_pool&)            = delete;
    c_thread_pool& operator=(const c_thread_pool&) = delete;

    std::size_t worker_count() const noexcept { return m_workers.size(); }

    // Submits one task. Tasks must not submit further tasks and then wait for
    // them; this pool is used for a single flat fan out per query.
    void submit(std::function<void()> task);

    void wait_for_all();

private:
    void worker_loop();

    std::vector<std::thread>          m_workers;
    std::queue<std::function<void()>> m_queue;
    std::mutex                        m_mutex;
    std::condition_variable           m_work_available;
    std::condition_variable           m_all_done;
    std::size_t                       m_outstanding = 0;
    bool                              m_stopping    = false;
    std::exception_ptr                m_first_error;
};

}
