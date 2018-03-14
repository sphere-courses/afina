#include "../../include/afina/Executor.h"
#include <iostream>
#include <functional>

namespace Afina {

bool initiate_thread(Executor *executor, void *(*function)(void *), bool use_lock) {
    std::cout << "pool: " << __PRETTY_FUNCTION__ << std::endl;

    pthread_t new_thread;

    if (pthread_create(&new_thread, nullptr, function, executor) != 0) {
        std::cout << "Could not initiate thread" << std::endl;
        return false;
    }
    pthread_detach(new_thread);

    if(use_lock) {
        std::unique_lock<std::mutex> sh_res_lock(executor->sh_res_mutex);
    }
    executor->threads.insert(new_thread);
    return true;
}

void *perform(void *executor_void) {
    std::cout << "pool: " << __PRETTY_FUNCTION__ << std::endl;

    Executor *executor = reinterpret_cast<Executor *>(executor_void);

    //TODO: use something else to create lock objects without automatical locking

    std::unique_lock<std::mutex> sh_res_lock(executor->sh_res_mutex);

    std::cv_status wait_res;

    while(true){
        if(executor->state.load() == Executor::State::kRun){
            while(executor->tasks.empty() && executor->state.load() == Executor::State::kRun) {
                wait_res = executor->empty_condition.wait_for(sh_res_lock, executor->idle_time);
            }

            if(executor->state.load() != Executor::State::kRun) {
                std::cout << "Worker is going to terminate (because of server stopping)" << std::endl;

                executor->threads.erase(pthread_self());
                if (executor->threads.empty()) {
                    executor->state = Executor::State::kStopped;
                }
                executor->term_condition.notify_all();
                return nullptr;
            }

            if ((wait_res == std::cv_status::timeout && executor->workers_perf > executor->low_watermark)) {
                std::cout << "Worker is going to terminate (because of waiting time)" << std::endl;

                executor->threads.erase(pthread_self());
                executor->term_condition.notify_all();
                return nullptr;
            }

            executor->workers_perf++;
            std::function<void()> task = executor->tasks.front();
            executor->tasks.pop_front();
            sh_res_lock.unlock();

            try {
                task();
            } catch (std::exception &exp) {
                std::cout << exp.what() << std::endl;

                sh_res_lock.lock();

                executor->workers_perf--;
                executor->threads.erase(pthread_self());
                if(executor->threads.empty()){
                    executor->state = Executor::State::kStopped;
                }
                executor->term_condition.notify_all();
                return nullptr;
            }

            sh_res_lock.lock();
            executor->workers_perf--;
        } else {
            std::cout << "Worker is going to terminate (because of server stopping)" << std::endl;

            executor->threads.erase(pthread_self());
            if (executor->threads.empty()) {
                executor->state = Executor::State::kStopped;
            }
            executor->term_condition.notify_all();
            return nullptr;
        }
    }
}

void Executor::Start(size_t low, size_t hight, size_t max, std::chrono::milliseconds idle) {
    std::cout << "pool: " << __PRETTY_FUNCTION__ << std::endl;

    low_watermark = low;
    hight_watermark = hight;
    max_queue_size = max;
    idle_time = idle;

    std::unique_lock<std::mutex> start_lock(state_mutex);

    // Initiate workers threads
    for (size_t i = 0; i < low_watermark; ++i) {
        if (!initiate_thread(this, perform, false)) {
            start_lock.unlock();
            this->Stop(true);
            // No lock is need due to single thread
            state = Executor::State::kStopped;
            //TODO: throw error futher (make more error msgs)
            return;
        }
    }

    state = Executor::State::kRun;
}

void Executor::Stop(bool await) {
    std::cout << "pool: " << __PRETTY_FUNCTION__ << std::endl;

    std::unique_lock<std::mutex> sh_res_lock(sh_res_mutex);

    state = Executor::State::kStopping;
    tasks.clear();

    sh_res_lock.unlock();

    empty_condition.notify_all();

    if (await) {
        Join();
    }
}

void Executor::Join() {
    std::cout << "pool: " << __PRETTY_FUNCTION__ << std::endl;

    std::unique_lock<std::mutex> sh_res_lock(sh_res_mutex);
    empty_condition.notify_all();

    while (!threads.empty()) {
        empty_condition.notify_all();
        term_condition.wait(sh_res_lock);
    }
    state = Executor::State::kStopped;
}

}