#include "thread_pool.h"

std::size_t merope::default_worker_count() noexcept {
    const unsigned int reported = std::thread::hardware_concurrency();
    return reported == 0 ? 4u : static_cast<std::size_t>(reported);
}

merope::c_thread_pool::c_thread_pool(std::size_t workers) {
    const std::size_t count = workers == 0 ? default_worker_count() : workers;
    m_workers.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        m_workers.emplace_back([this] { worker_loop(); });
    }
}

merope::c_thread_pool::~c_thread_pool() {
    {
        std::lock_guard<std::mutex> guard(m_mutex);
        m_stopping = true;
    }
    m_work_available.notify_all();
    for (std::thread& worker : m_workers) {
        if (worker.joinable()) worker.join();
    }
}

void merope::c_thread_pool::submit(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> guard(m_mutex);
        m_queue.push(std::move(task));
        ++m_outstanding;
    }
    m_work_available.notify_one();
}

void merope::c_thread_pool::wait_for_all() {
    std::unique_lock<std::mutex> guard(m_mutex);
    m_all_done.wait(guard, [this] { return m_outstanding == 0; });

    if (m_first_error) {
        // Hand the failure over exactly once, so a retry starts from a clean pool.
        const std::exception_ptr error = m_first_error;
        m_first_error = nullptr;
        std::rethrow_exception(error);
    }
}

void merope::c_thread_pool::worker_loop() {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> guard(m_mutex);
            m_work_available.wait(guard, [this] { return m_stopping || !m_queue.empty(); });
            if (m_stopping && m_queue.empty()) return;
            task = std::move(m_queue.front());
            m_queue.pop();
        }

        try {
            task();
        } catch (...) {
            std::lock_guard<std::mutex> guard(m_mutex);
            if (!m_first_error) m_first_error = std::current_exception();
        }

        {
            std::lock_guard<std::mutex> guard(m_mutex);
            --m_outstanding;
            if (m_outstanding == 0) m_all_done.notify_all();
        }
    }
}

