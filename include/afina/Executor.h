#ifndef AFINA_THREADPOOL_H
#define AFINA_THREADPOOL_H

#include <condition_variable>
#include <functional>
#include <unordered_set>
#include <iostream>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <chrono>
#include <ctime>
#include <atomic>

namespace Afina {

// Forward declaration
class Executor;

bool initiate_thread(Executor *executor, void *(*function)(void *), bool use_lock);
void * perform(void *executor_void);
void delete_self(Executor *executor);

/**
 * # Thread pool
 */

class Executor {
public:
    enum class State {
        // Threadpool is fully operational, tasks could be added and get executed
                kRun,

        // Threadpool is on the way to be shutdown, no ned task could be added, but existing will be
        // completed as requested
                kStopping,

        // Threadppol is stopped
                kStopped
    };

    Executor() : workers_perf{0} {};

    ~Executor() = default;

    void Start(std::size_t low_watermark = 3, std::size_t hight_watermark = 3, std::size_t max_queue_size = 5,
               std::chrono::milliseconds idle_time = std::chrono::milliseconds{1000});

    /**
     * Signal thread pool to stop, it will stop accepting new jobs and close threads just after each become
     * free. All enqueued jobs will be complete.
     *
     * In case if await flag is true, call won't return until all background jobs are done and all threads are stopped
     */
    void Stop(bool await = false);

    void Join();

    /**
     * Add function to be executed on the threadpool. Method returns true in case if task has been placed
     * onto execution queue, i.e scheduled for execution and false otherwise.
     *
     * That function doesn't wait for function result. Function could always be written in a way to notify caller about
     * execution finished by itself
     */
    template <typename F, typename... Types> bool Execute(F &&func, Types... args) {
        // Prepare "task"
        std::cout << "pool: " << __PRETTY_FUNCTION__ << std::endl;

        auto exec = std::bind(std::forward<F>(func), std::forward<Types>(args)...);

        if (state.load(std::memory_order_seq_cst) != State::kRun) {
            return false;
        }

        // Enqueue new task
        std::unique_lock<std::mutex> lock(sh_res_mutex);
        tasks.push_back(exec);
        if(workers_perf == threads.size() && threads.size() < hight_watermark){
            lock.unlock();
            if(!initiate_thread(this, perform, true)) {
                Stop(true);
                //TODO: throw error futher (make more error msgs)
                return false;
            }
        }
        empty_condition.notify_one();
        return true;
    }

    // No copy/move/assign allowed
    Executor(const Executor &) = delete;

    Executor(Executor &&) = delete;

    Executor &operator=(const Executor &) = delete;

    Executor &operator=(Executor &&) = delete;

private:

    friend bool initiate_thread(Executor *executor, void *(*function)(void *), bool use_lock);

    friend void * perform(void *executor_void);

    friend void delete_self(Executor *executor);

    /**
     * Mutex to protect state below from concurrent modification
    */
    std::mutex sh_res_mutex;

    /**
     * Conditional variable to await new data in case of empty queue
     */
    std::condition_variable empty_condition;

    std::condition_variable term_condition;

    /**
     * Set of all existing threads
     */
    std::unordered_set<pthread_t> threads;

    /**
     * Task queue
     */
    std::deque<std::function<void()>> tasks;

    /**
     * Flag to stop bg threads
     */
    std::atomic<State> state;
    //State state;

    std::size_t low_watermark, hight_watermark, max_queue_size, workers_perf;
    std::chrono::milliseconds idle_time;
};

}
#endif // AFINA_THREADPOOL_H
