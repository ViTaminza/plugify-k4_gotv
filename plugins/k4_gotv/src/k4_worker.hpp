// k4_gotv — one background thread for file I/O (zip, part reads, deletes).
//
// The game thread must never block on a demo-sized read: Work runs on the worker and returns a
// completion closure; Poll() (OnPluginUpdate, game thread) runs that closure with the result.
// Nothing from s2sdk or the plugin state may be touched on the worker thread - work captures
// values and returns values.

#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace k4 {

class Worker {
public:
	Worker();
	~Worker();
	Worker(const Worker&) = delete;
	Worker& operator=(const Worker&) = delete;

	void Start();
	// Stops the thread after the queued work (does not run the completions).
	void Stop();

	// Thread-safe. `work` runs on the worker thread; the returned callable runs in Poll().
	// An exception thrown by `work` is swallowed (no completion runs).
	void Post(std::function<std::function<void()>()> work);

	// Runs the completed closures on the caller's thread (game thread). Returns how many ran.
	std::size_t Poll();

	std::size_t Pending() const;

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace k4
