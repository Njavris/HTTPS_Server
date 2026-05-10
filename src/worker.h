#ifndef __WORKER_H__
#define __WORKER_H__

#include <vector>
#include <mutex>
#include <queue>
#include <thread>
#include <condition_variable>
#include <functional>
#include <atomic>

class WorkerPool {
	std::vector<std::thread> workers;
	std::queue<std::function<void()>> tasks;
	std::mutex queueMutex;
	std::condition_variable condition;
	std::atomic<bool> run {true};
	std::vector<std::atomic<int64_t>> start_times; // 0 - Idle
public:
	WorkerPool(int poolSize);
	~WorkerPool();
	void enqueue(std::function<void()> work);
};

#endif // __WORKER_H__
