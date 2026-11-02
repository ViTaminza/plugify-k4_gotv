// k4_gotv — the background file-I/O worker (see k4_worker.hpp).

#include "k4_worker.hpp"

#include <utility>

namespace k4 {

struct Worker::Impl {
	std::mutex mutex;
	std::condition_variable condition;
	std::deque<std::function<std::function<void()>()>> queue;
	std::mutex completedMutex;
	std::deque<std::function<void()>> completed;
	std::thread thread;
	bool running = false;
	bool stopping = false;

	void Loop() {
		for (;;) {
			std::function<std::function<void()>()> work;
			{
				std::unique_lock<std::mutex> lock(mutex);
				condition.wait(lock, [this] { return stopping || !queue.empty(); });
				if (stopping && queue.empty()) {
					return;
				}
				work = std::move(queue.front());
				queue.pop_front();
			}
			std::function<void()> done;
			try {
				done = work();
			} catch (...) {
				done = {}; // a failed file operation reports through its captured result instead
			}
			if (done) {
				std::lock_guard<std::mutex> lock(completedMutex);
				completed.push_back(std::move(done));
			}
		}
	}
};

Worker::Worker() : m_impl(std::make_unique<Impl>()) {}

Worker::~Worker() {
	Stop();
}

void Worker::Start() {
	if (m_impl->running) {
		return;
	}
	m_impl->stopping = false;
	m_impl->running = true;
	m_impl->thread = std::thread([impl = m_impl.get()] { impl->Loop(); });
}

void Worker::Stop() {
	if (!m_impl->running) {
		return;
	}
	{
		std::lock_guard<std::mutex> lock(m_impl->mutex);
		m_impl->stopping = true;
	}
	m_impl->condition.notify_all();
	if (m_impl->thread.joinable()) {
		m_impl->thread.join();
	}
	m_impl->running = false;
	{
		std::lock_guard<std::mutex> lock(m_impl->completedMutex);
		m_impl->completed.clear(); // completions after unload must not run
	}
}

void Worker::Post(std::function<std::function<void()>()> work) {
	if (!m_impl->running) {
		Start();
	}
	{
		std::lock_guard<std::mutex> lock(m_impl->mutex);
		m_impl->queue.push_back(std::move(work));
	}
	m_impl->condition.notify_one();
}

std::size_t Worker::Poll() {
	std::deque<std::function<void()>> ready;
	{
		std::lock_guard<std::mutex> lock(m_impl->completedMutex);
		ready.swap(m_impl->completed);
	}
	std::size_t ran = 0;
	for (auto& completion : ready) {
		try {
			completion();
		} catch (...) {
			// a completion must never unwind into the plugin update loop
		}
		++ran;
	}
	return ran;
}

std::size_t Worker::Pending() const {
	std::size_t pending = 0;
	{
		std::lock_guard<std::mutex> lock(m_impl->mutex);
		pending += m_impl->queue.size();
	}
	{
		std::lock_guard<std::mutex> lock(m_impl->completedMutex);
		pending += m_impl->completed.size();
	}
	return pending;
}

} // namespace k4
