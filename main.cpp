#include <iostream>
#include <queue>
#include <thread>
#include <vector>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <random>

class TaskQueue {
public:
    TaskQueue() = default;
    ~TaskQueue() { clear(); }

    bool empty() const {
        std::lock_guard<std::mutex> lock(mtx);
        return tasks.empty();
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mtx);
        return tasks.size();
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mtx);
        while (!tasks.empty()) tasks.pop();
    }

    bool pop(std::function<void()>& task) {
        std::lock_guard<std::mutex> lock(mtx);
        if (tasks.empty()) return false;
        task = std::move(tasks.front());
        tasks.pop();
        return true;
    }

    void push(std::function<void()> task) {
        std::lock_guard<std::mutex> lock(mtx);
        tasks.push(task);
    }

private:
    std::queue<std::function<void()>> tasks;
    mutable std::mutex mtx;
};

class ThreadPool {
public:
    ThreadPool() {
        for (int i = 0; i < 2; ++i) {
            workers1.emplace_back(&ThreadPool::workerFunction, this, std::ref(queue1), std::ref(total_time1));
            workers2.emplace_back(&ThreadPool::workerFunction, this, std::ref(queue2), std::ref(total_time2));
        }
    }

    ~ThreadPool() {
        terminate();
    }

    void addTask(std::function<void()> task, int duration) {
        std::lock_guard<std::mutex> lock(mtx);
        if (total_time1.load() <= total_time2.load()) {
            queue1.push(task);
            total_time1 += duration;
        } else {
            queue2.push(task);
            total_time2 += duration;
        }
        cv.notify_one();
    }

    void terminate() {
        {
            std::lock_guard<std::mutex> lock(mtx);
            stop = true;
        }
        cv.notify_all();

        for (auto& w : workers1) w.join();
        for (auto& w : workers2) w.join();
    }

private:
    std::vector<std::thread> workers1;
    std::vector<std::thread> workers2;

    TaskQueue queue1;
    TaskQueue queue2;

    std::atomic<int> total_time1{0};
    std::atomic<int> total_time2{0};

    std::mutex mtx;
    std::condition_variable cv;
    std::atomic<bool> stop{false};

    void workerFunction(TaskQueue& queue, std::atomic<int>& totalTime) {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mtx);
                cv.wait(lock, [this, &queue] { return stop || !queue.empty(); });

                if (stop && queue.empty()) break;

                if (!queue.pop(task)) continue;
            }

            auto start = std::chrono::high_resolution_clock::now();
            task();
            auto end = std::chrono::high_resolution_clock::now();

            int duration = std::chrono::duration_cast<std::chrono::seconds>(end - start).count();
            totalTime -= duration;
        }
    }
};

void generateRandomTask(ThreadPool& pool) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> distr(2, 15);

    int duration = distr(gen);
    pool.addTask([duration]() {
        std::cout << "Task started, duration: " << duration << " seconds\n";
        std::this_thread::sleep_for(std::chrono::seconds(duration));
        std::cout << "Task completed, duration: " << duration << " seconds\n";
    }, duration);
}

int main() {
    ThreadPool pool;

    std::vector<std::thread> taskGenerators;
    for (int i = 0; i < 5; ++i) {
        taskGenerators.emplace_back([&pool]() {
            for (int j = 0; j < 10; ++j) {
                generateRandomTask(pool);
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        });
    }

    for (auto& t : taskGenerators) t.join();

    std::this_thread::sleep_for(std::chrono::seconds(10));
    pool.terminate();

    return 0;
}