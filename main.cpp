#include <iostream>
#include <queue>
#include <thread>
#include <shared_mutex>
#include <vector>
#include <functional>
#include <atomic>
#include <random>
#include <chrono>
#include <map>
#include <limits>
#include <mutex>
#include <condition_variable>

using namespace std;
using read_write_lock = shared_mutex;
using read_lock = shared_lock<read_write_lock>;
using write_lock = unique_lock<read_write_lock>;
mutex output_mutex;

template <typename task_type_t>
class task_queue {
    using task_queue_impl = queue<task_type_t>;
public:
    task_queue() = default;
    ~task_queue() { clear(); }
    bool empty() const {
        read_lock _(m_rw_lock);
        return m_tasks.empty();
    }
    size_t size() const {
        read_lock _(m_rw_lock);
        return m_tasks.size();
    }
    void clear() {
        write_lock _(m_rw_lock);
        while (!m_tasks.empty()) m_tasks.pop();
    }
    bool pop(task_type_t& task) {
        write_lock _(m_rw_lock);
        if (m_tasks.empty())
            return false;
        task = std::move(m_tasks.front());
        m_tasks.pop();
        return true;
    }
    template <typename... arguments>
    void emplace(arguments&&... params) {
        write_lock _(m_rw_lock);
        m_tasks.emplace(std::forward<arguments>(params)...);
    }
private:
    mutable read_write_lock m_rw_lock;
    task_queue_impl m_tasks;
};

struct Task {
    function<void()> func;
    int duration;
    size_t id;
};

class thread_pool {
public:
    thread_pool() {
        for (int i = 0; i < 2; ++i) {
            m_workers1.emplace_back(&thread_pool::worker, this, ref(m_queue1), ref(m_sum_time1), ref(m_queue1_stats));
            m_workers2.emplace_back(&thread_pool::worker, this, ref(m_queue2), ref(m_sum_time2), ref(m_queue2_stats));
        }
    }
    ~thread_pool() {
        terminate();
        print_statistics();
    }
    void add_task(function<void()> func, int duration) {
        size_t id = m_next_id++;
        {
            lock_guard<mutex> lock(m_stat_mtx);
            m_enqueue_time[id] = chrono::high_resolution_clock::now();
        }
        if (m_sum_time1.load() <= m_sum_time2.load()) {
            m_queue1.emplace(Task{func, duration, id});
            m_sum_time1 += duration;
        } else {
            m_queue2.emplace(Task{func, duration, id});
            m_sum_time2 += duration;
        }
        m_queue_cv.notify_all();
    }
    void terminate() {
        m_terminate = true;
        m_queue_cv.notify_all();
        for (auto& w : m_workers1)
            if (w.joinable()) w.join();
        for (auto& w : m_workers2)
            if (w.joinable()) w.join();
    }
    void print_statistics() const {
        cout << "\n====== Статистика ======\n";
        cout << "Кількість створених потоків: " << (m_workers1.size() + m_workers2.size()) << endl;
        cout << "Виконано задач: " << m_completed << endl;
        cout << "Середній час знаходження воркера у стані очікування: ";
        if (m_wait_samples > 0) cout << (m_total_wait_worker / m_wait_samples) << " сек\n";
        else cout << "н/д\n";
        cout << "Середній час виконання задачі: ";
        if (m_completed > 0) cout << (m_total_exec_time / m_completed) << " сек\n";
        else cout << "н/д\n";
        cout << "Середня довжина черги 1: ";
        if (m_queue1_stats.samples > 0) cout << (double)m_queue1_stats.sum_len / m_queue1_stats.samples << endl;
        else cout << "н/д\n";
        cout << "Середня довжина черги 2: ";
        if (m_queue2_stats.samples > 0) cout << (double)m_queue2_stats.sum_len / m_queue2_stats.samples << endl;
        else cout << "н/д\n";
        cout << "========================\n";
    }
private:
    struct QueueStats {
        atomic<size_t> sum_len{0};
        atomic<size_t> samples{0};
    };
    void worker(task_queue<Task>& queue, atomic<int>& sum_time, QueueStats& stats) {
        using namespace chrono;
        while (!m_terminate.load()) {
            Task task;
            stats.sum_len += queue.size();
            stats.samples++;
            {
                unique_lock<mutex> lock(m_queue_mtx);
                m_queue_cv.wait(lock, [this, &queue] {
                    return m_terminate || !queue.empty();
                });
                if (m_terminate) return;
                if (!queue.pop(task)) continue;
            }
            auto exec_start = high_resolution_clock::now();
            double task_queue_wait = 0;
            {
                lock_guard<mutex> lock(m_stat_mtx);
                task_queue_wait = duration<double>(exec_start - m_enqueue_time[task.id]).count();
                m_total_wait_task += task_queue_wait;
            }
            auto t_start = high_resolution_clock::now();
            task.func();
            auto t_end = high_resolution_clock::now();
            double exec_secs = duration<double>(t_end - t_start).count();
            {
                lock_guard<mutex> lock(m_stat_mtx);
                m_total_exec_time += exec_secs;
                m_completed++;
            }
            sum_time -= task.duration;
        }
    }
    vector<thread> m_workers1;
    vector<thread> m_workers2;
    task_queue<Task> m_queue1;
    task_queue<Task> m_queue2;
    atomic<int> m_sum_time1{0};
    atomic<int> m_sum_time2{0};
    atomic<bool> m_terminate{false};
    atomic<size_t> m_next_id{0};
    condition_variable_any m_queue_cv;
    mutable mutex m_queue_mtx;
    atomic<int> m_completed{0};
    mutable mutex m_stat_mtx;
    map<size_t, chrono::high_resolution_clock::time_point> m_enqueue_time;
    double m_total_wait_worker = 0;
    size_t m_wait_samples = 0;
    double m_total_exec_time = 0;
    double m_total_wait_task = 0;
    QueueStats m_queue1_stats;
    QueueStats m_queue2_stats;
};

void generate_task(thread_pool& pool) {
    static random_device rd;
    static mt19937 gen(rd());
    static uniform_int_distribution<> distr(2, 15);
    int duration = distr(gen);
    pool.add_task([duration]() { 
      {
        lock_guard<mutex> lock(output_mutex);
        cout << "Task started (" << duration << " s)\n";
      }
      this_thread::sleep_for(chrono::seconds(duration));
      {
        lock_guard<mutex> lock(output_mutex);
        cout << "Task finished (" << duration << " s)\n";
      }
    }, duration);
}

int main() {
    thread_pool pool;
    vector<thread> generators;
    for (int i = 0; i < 5; ++i) {
        generators.emplace_back([&pool]() {
            for (int j = 0; j < 10; ++j) {
                generate_task(pool);
                this_thread::sleep_for(chrono::milliseconds(500));
            }
        });
    }
    for (auto& t : generators) t.join();
    this_thread::sleep_for(chrono::seconds(20));
    return 0;
}
