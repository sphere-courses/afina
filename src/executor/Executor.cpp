#include <afina/Executor.h>

#include <iostream>
#include <functional>
#include <unordered_set>
#include <pthread.h>


namespace Afina {

bool initiate_thread(Executor *executor, void *(*function)(void *), bool use_lock) {
    std::cout << "pool: " << __PRETTY_FUNCTION__ << std::endl;

    pthread_t new_thread;

    if (pthread_create(&new_thread, nullptr, function, executor) != 0) {
        std::cout << "Could not initiate thread" << std::endl;
        return false;
    }
    pthread_detach(new_thread);

    std::unique_lock<std::mutex> sh_res_lock(executor->sh_res_mutex, std::defer_lock);
    if (use_lock) {
        sh_res_lock.lock();
    }
    executor->threads.insert(new_thread);
    return true;
}

void *perform(void *executor_void) {
    std::cout << "pool: " << __PRETTY_FUNCTION__ << std::endl;

    auto executor = reinterpret_cast<Executor *>(executor_void);
    std::unique_lock<std::mutex> sh_res_lock(executor->sh_res_mutex);
    std::cv_status wait_res;

    while (executor->state.load(std::memory_order_acquire) == Executor::State::kRun) {

        while (executor->tasks.empty() && executor->state.load(std::memory_order_acquire) == Executor::State::kRun) {
            wait_res = executor->empty_condition.wait_for(sh_res_lock, executor->idle_time);

            if ((wait_res == std::cv_status::timeout && executor->workers_perf > executor->low_watermark)) {
                std::cout << "Worker is going to terminate (because of waiting time)" << std::endl;

                delete_self(executor);
                return nullptr;
            }
        }

        if (executor->state.load(std::memory_order_acquire) != Executor::State::kRun) {
            std::cout << "Worker is going to terminate (because of server stopping)" << std::endl;

            delete_self(executor);
            return nullptr;
        }

        ++executor->workers_perf;
        std::function<void()> task = executor->tasks.front();
        executor->tasks.pop_front();
        sh_res_lock.unlock();

        try {
            task();
        } catch (std::exception &exp) {
            std::cout << exp.what() << std::endl;
        }

        sh_res_lock.lock();
        executor->workers_perf--;
    }
    std::cout << "Worker is going to terminate (because of server stopping)" << std::endl;

    delete_self(executor);
    return nullptr;
}

void delete_self(Executor *executor) {
    executor->threads.erase(pthread_self());
    if (executor->threads.empty()) {
        executor->term_condition.notify_all();
    }
}


void Executor::Start(size_t low, size_t hight, size_t max, std::chrono::milliseconds idle) {
    std::cout << "pool: " << __PRETTY_FUNCTION__ << std::endl;

    low_watermark = low;
    hight_watermark = hight;
    max_queue_size = max;
    idle_time = idle;

    std::unique_lock<std::mutex> start_lock(sh_res_mutex);

    // Initiate workers threads
    for (size_t i = 0; i < low_watermark; ++i) {
        if (!initiate_thread(this, perform, false)) {
            start_lock.unlock();
            this->Stop(true);
            //TODO: throw error futher (make more error msgs)
            return;
        }
    }

    state.store(Executor::State::kRun, std::memory_order_relaxed);
}

void Executor::Stop(bool await) {
    std::cout << "pool: " << __PRETTY_FUNCTION__ << std::endl;

    std::unique_lock<std::mutex> sh_res_lock(sh_res_mutex);

    state.store(Executor::State::kStopping, std::memory_order_release);
    tasks.clear();

    sh_res_lock.unlock();

    if (await) {
        Join();
    }
}

void Executor::Join() {
    std::cout << "pool: " << __PRETTY_FUNCTION__ << std::endl;

    std::unique_lock<std::mutex> sh_res_lock(sh_res_mutex);

    while (!threads.empty()) {
        empty_condition.notify_all();
        term_condition.wait(sh_res_lock);
    }
    state.store(Executor::State::kStopped, std::memory_order_relaxed);
}


}