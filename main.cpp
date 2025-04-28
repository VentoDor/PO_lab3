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
#include <map>
#include <limits>

using namespace std::chrono;

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

    bool pop(std::function<void()>& task, int& id) {
        std::lock_guard<std::mutex> lock(mtx);
        if (tasks.empty()) return false;
        task = std::move(tasks.front().second);
        id = tasks.front().first;
        tasks.pop();
        return true;
    }

    void push(int id, std::function<void()> task) {
        std::lock_guard<std::mutex> lock(mtx);
        tasks.push({id, task});
    }

private:
    std::queue<std::pair<int, std::function<void()>>> tasks;
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
        printStatistics();
    }

    void addTask(std::function<void()> task, int duration) {
        std::lock_guard<std::mutex> lock(mtx);

        int task_id = nextTaskId++;
        enqueue_times[task_id] = high_resolution_clock::now();

        // Наприклад, нехай max total_time на чергу = 60
        if (total_time1.load() <= total_time2.load()) {
            if (total_time1.load() + duration > 60) {
                ++rejected_tasks;
                return;
            }
            queue1.push(task_id, task);
            total_time1 += duration;
        } else {
            if (total_time2.load() + duration > 60) {
                ++rejected_tasks;
                return;
            }
            queue2.push(task_id, task);
            total_time2 += duration;
        }

        cv.notify_one();
    }

    void terminate() {
        {
            std::lock_guard<std::mutex> lock(mtx);
            if (stop) return;
            stop = true;
        }
        cv.notify_all();

        for (auto& w : workers1) {
            if (w.joinable()) w.join();
        }
        for (auto& w : workers2) {
            if (w.joinable()) w.join();
        }
    }

private:
    std::vector<std::thread> workers1;
    std::vector<std::thread> workers2;

    TaskQueue queue1;
    TaskQueue queue2;

    std::atomic<int> total_time1{0};
    std::atomic<int> total_time2{0};

    std::atomic<int> rejected_tasks{0};
    std::mutex mtx;
    std::condition_variable cv;
    std::atomic<bool> stop{false};

    std::atomic<int> nextTaskId{0};

    std::map<int, high_resolution_clock::time_point> enqueue_times;
    std::mutex stats_mtx;

    double min_wait_time = std::numeric_limits<double>::max();
    double max_wait_time = 0;
    double total_wait_time = 0;
    int completed_tasks = 0;

    void workerFunction(TaskQueue& queue, std::atomic<int>& totalTime) {
        while (true) {
            std::function<void()> task;
            int task_id;
            {
                std::unique_lock<std::mutex> lock(mtx);
                cv.wait(lock, [this, &queue] { return stop || !queue.empty(); });

                if (stop && queue.empty()) break;

                if (!queue.pop(task, task_id)) continue;
            }

            auto now = high_resolution_clock::now();
            double wait_time = duration<double>(now - enqueue_times[task_id]).count();

            {
                std::lock_guard<std::mutex> lock(stats_mtx);
                if (wait_time < min_wait_time) min_wait_time = wait_time;
                if (wait_time > max_wait_time) max_wait_time = wait_time;
                total_wait_time += wait_time;
                ++completed_tasks;
            }

            auto start = high_resolution_clock::now();
            task();
            auto end = high_resolution_clock::now();

            int duration_secs = duration_cast<seconds>(end - start).count();
            totalTime -= duration_secs;
        }
    }

    void printStatistics() {
        std::lock_guard<std::mutex> lock(stats_mtx);
        std::cout << "\n====== Статистика ======\n";
        std::cout << "Відхилено задач: " << rejected_tasks << "\n";
        std::cout << "Виконано задач: " << completed_tasks << "\n";
        if (completed_tasks > 0) {
            std::cout << "Мінімальний час очікування: " << min_wait_time << " сек\n";
            std::cout << "Максимальний час очікування: " << max_wait_time << " сек\n";
            std::cout << "Середній час очікування: " << (total_wait_time / completed_tasks) << " сек\n";
        } else {
            std::cout << "Жодної задачі не виконано.\n";
        }
        std::cout << "========================\n";
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
    return 0;
}
