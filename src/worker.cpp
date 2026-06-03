#include <iostream>

#include <worker.h>
#include <misc.h>

WorkerPool::WorkerPool(int poolSize) : run(true), start_times(poolSize) {
	linfo << "Spawning pool with size " << poolSize << log::endl;
	for (size_t i = 0; i < poolSize; ++i) {
		start_times[i] = 0;
		workers.emplace_back([this, i] {
			while (true) {
				std::function<void()> task;
				{
					std::unique_lock<std::mutex> lock(this->queueMutex);
					this->condition.wait(lock, [this] {
						return !this->run || !this->tasks.empty();
					});
					if (!this->run && this->tasks.empty())
						return;
					task = std::move(this->tasks.front());
					this->tasks.pop();
				}
				start_times[i] = time(0);
				task();
				start_times[i] = 0;
			}
		});
	}
	std::thread([this] {
		while (run) {
			std::this_thread::sleep_for(std::chrono::seconds(5));
			int64_t now = time(0);
			for (size_t i = 0; i < start_times.size(); ++i) {
				int64_t s = start_times[i];
				if (s > 0 && (now - s) > 10) 
				lerr << "Worker " << i << " stuck!\n";
			}
		}
	}).detach();
}

WorkerPool::~WorkerPool() {
	run = false;
	condition.notify_all();

	for (auto &worker : workers) {
		if (worker.joinable())
			worker.join();
	}
}


void WorkerPool::enqueue(std::function<void()> task) {
	{
		std::lock_guard<std::mutex> lock(queueMutex);
		tasks.push(std::move(task));
	}
	condition.notify_one();
}
