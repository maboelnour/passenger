/*
 *  Phusion Passenger - https://www.phusionpassenger.com/
 *  Copyright (c) 2010-2014 Phusion
 *
 *  "Phusion Passenger" is a trademark of Hongli Lai & Ninh Bui.
 *
 *  See LICENSE file for license information.
 */
#ifndef _PASSENGER_SAFE_LIBEV_H_
#define _PASSENGER_SAFE_LIBEV_H_

#include <ev++.h>
#include <vector>
#include <list>
#include <memory>
#include <boost/thread.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/function.hpp>
#include <boost/bind.hpp>
#include <oxt/thread.hpp>

namespace Passenger {

using namespace std;
using namespace boost;


/**
 * Class for thread-safely using libev.
 */
class SafeLibev {
private:
	// 2^28-1. Command IDs are 28-bit so that we can pack DataSource's state and
	// its planId in 32-bits total.
	static const unsigned int MAX_COMMAND_ID = 268435455;

	typedef boost::function<void ()> Callback;

	struct Command {
		Callback callback;
		unsigned int id: 31;
		bool canceled: 1;

		Command(unsigned int _id, const Callback &_callback)
			: callback(_callback),
			  id(_id),
			  canceled(false)
			{ }
	};

	struct ev_loop *loop;
	pthread_t loopThread;
	ev_async async;

	boost::mutex syncher;
	boost::condition_variable cond;
	vector<Command> commands;
	unsigned int nextCommandId;

	static void asyncHandler(EV_P_ ev_async *w, int revents) {
		SafeLibev *self = (SafeLibev *) w->data;
		self->runCommands();
	}

	static void timeoutHandler(int revents, void *arg) {
		auto_ptr<Callback> callback((Callback *) arg);
		(*callback)();
	}

	void runCommands() {
		boost::unique_lock<boost::mutex> l(syncher);
		vector<Command> commands = this->commands;
		this->commands.clear();
		l.unlock();

		vector<Command>::const_iterator it, end = commands.end();
		for (it = commands.begin(); it != end; it++) {
			if (!it->canceled) {
				it->callback();
			}
		}
	}

	template<typename Watcher>
	void startWatcherAndNotify(Watcher *watcher, bool *done) {
		watcher->set(loop);
		watcher->start();
		boost::unique_lock<boost::mutex> l(syncher);
		*done = true;
		cond.notify_all();
	}

	template<typename Watcher>
	void stopWatcherAndNotify(Watcher *watcher, bool *done) {
		watcher->stop();
		boost::unique_lock<boost::mutex> l(syncher);
		*done = true;
		cond.notify_all();
	}

	void runAndNotify(const Callback *callback, bool *done) {
		(*callback)();
		boost::unique_lock<boost::mutex> l(syncher);
		*done = true;
		cond.notify_all();
	}

	void incNextCommandId() {
		if (nextCommandId == MAX_COMMAND_ID) {
			nextCommandId = 1;
		} else {
			nextCommandId++;
		}
	}

public:
	/** SafeLibev takes over ownership of the loop object. */
	SafeLibev(struct ev_loop *loop) {
		this->loop = loop;
		loopThread = pthread_self();
		nextCommandId = 1;

		ev_async_init(&async, asyncHandler);
		ev_set_priority(&async, EV_MAXPRI);
		async.data = this;
		ev_async_start(loop, &async);
	}

	~SafeLibev() {
		destroy();
		ev_loop_destroy(loop);
	}

	void destroy() {
		ev_async_stop(loop, &async);
	}

	struct ev_loop *getLoop() const {
		return loop;
	}

	void setCurrentThread() {
		loopThread = pthread_self();
		#ifdef OXT_THREAD_LOCAL_KEYWORD_SUPPORTED
			oxt::thread_signature = this;
		#endif
	}

	pthread_t getCurrentThread() const {
		return loopThread;
	}

	bool onEventLoopThread() const {
		#ifdef OXT_THREAD_LOCAL_KEYWORD_SUPPORTED
			// Avoid double reads of the thread-local variable.
			const void *sig = oxt::thread_signature;
			if (OXT_UNLIKELY(sig == NULL)) {
				return pthread_equal(pthread_self(), loopThread);
			} else {
				return sig == this;
			}
		#else
			return pthread_equal(pthread_self(), loopThread);
		#endif
	}

	template<typename Watcher>
	void start(Watcher &watcher) {
		if (onEventLoopThread()) {
			watcher.set(loop);
			watcher.start();
		} else {
			boost::unique_lock<boost::mutex> l(syncher);
			bool done = false;
			commands.push_back(Command(nextCommandId,
				boost::bind(&SafeLibev::startWatcherAndNotify<Watcher>,
					this, &watcher, &done)));
			incNextCommandId();
			ev_async_send(loop, &async);
			while (!done) {
				cond.wait(l);
			}
		}
	}

	template<typename Watcher>
	void stop(Watcher &watcher) {
		if (onEventLoopThread()) {
			watcher.stop();
		} else {
			boost::unique_lock<boost::mutex> l(syncher);
			bool done = false;
			commands.push_back(Command(nextCommandId,
				boost::bind(&SafeLibev::stopWatcherAndNotify<Watcher>,
					this, &watcher, &done)));
			incNextCommandId();
			ev_async_send(loop, &async);
			while (!done) {
				cond.wait(l);
			}
		}
	}

	void run(const Callback &callback) {
		assert(callback != NULL);
		if (onEventLoopThread()) {
			callback();
		} else {
			runSync(callback);
		}
	}

	void runSync(const Callback &callback) {
		assert(callback != NULL);
		boost::unique_lock<boost::mutex> l(syncher);
		bool done = false;
		commands.push_back(Command(nextCommandId,
			boost::bind(&SafeLibev::runAndNotify, this,
				&callback, &done)));
		incNextCommandId();
		ev_async_send(loop, &async);
		while (!done) {
			cond.wait(l);
		}
	}

	/** Run a callback after a certain timeout. */
	void runAfter(unsigned int timeout, const Callback &callback) {
		assert(callback != NULL);
		ev_once(loop, -1, 0, timeout / 1000.0, timeoutHandler, new Callback(callback));
	}

	/** Thread-safe version of runAfter(). */
	void runAfterTS(unsigned int timeout, const Callback &callback) {
		assert(callback != NULL);
		if (onEventLoopThread()) {
			runAfter(timeout, callback);
		} else {
			runLater(boost::bind(&SafeLibev::runAfter, this, timeout, callback));
		}
	}

	unsigned int runLater(const Callback &callback) {
		assert(callback != NULL);
		unsigned int result;
		{
			boost::unique_lock<boost::mutex> l(syncher);
			commands.push_back(Command(nextCommandId, callback));
			result = nextCommandId;
			incNextCommandId();
		}
		ev_async_send(loop, &async);
		return result;
	}

	/**
	 * Cancels a callback that was scheduled to be run by runLater().
	 * Returns whether the command has been successfully cancelled or not.
	 * That is, a return value of true guarantees that the callback will not be called
	 * in the future, while a return value of false means that the callback has already
	 * been called or is currently being called.
	 */
	bool cancelCommand(unsigned int id) {
		if (id == 0) {
			return false;
		}

		boost::unique_lock<boost::mutex> l(syncher);
		// TODO: we can do a binary search because the command ID
		// is monotically increasing except on overflow.
		vector<Command>::iterator it, end = commands.end();
		for (it = commands.begin(); it != end; it++) {
			if (it->id == id) {
				it->canceled = true;
				return true;
			}
		}
		return false;
	}
};

typedef boost::shared_ptr<SafeLibev> SafeLibevPtr;


} // namespace Passenger

#endif /* _PASSENGER_SAFE_LIBEV_H_ */
