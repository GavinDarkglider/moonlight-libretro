#include "GameStreamClient.hpp"
#include "Settings.hpp"
#include <thread>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <algorithm>
#include <vector>
#include <iostream>
#include <fstream>
#include <future>
#include <atomic>
#include <system_error>
#include <nanogui/nanogui.h>

#include <unistd.h>

/*
 * Background worker for GameStreamClient.
 *
 * History: an earlier version used a free-floating std::atomic<bool> +
 * std::thread global, and got abort()ed at exit when pthread_join
 * returned EINVAL ("Another thread is already waiting" or "thread is
 * not a joinable thread"). The crash was traced to std::thread::join()
 * throwing system_error out of the libretro shutdown path, hitting
 * std::terminate.
 *
 * This rewrite is paranoid about two things:
 *   (a) every state transition is taken under m_async_mutex; there is no
 *       window in which "accept_tasks=true but no thread exists", and no
 *       window in which two callers can both believe they own the join.
 *   (b) join() is wrapped in try/catch and falls back to detach() on
 *       failure. The worker also publishes a "done" flag we can wait on
 *       independently of join(), so we get a deterministic handoff even
 *       if join() blows up.
 *
 * task_loop() may be called repeatedly across core load/unload cycles;
 * each time it will start a fresh worker if the previous one was shut
 * down.
 */

namespace {

struct AsyncState {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<std::function<void()>> tasks;

    /* accept_tasks: the producer side will reject new submissions if this
     * is false. Set to false at the start of shutdown.
     * worker_present: a worker thread object is currently held in
     * `worker` and has not yet been joined or detached.
     * worker_done: flipped to true by the worker itself just before it
     * returns. Shutdown waits on this so it doesn't have to rely on
     * std::thread::join. */
    bool accept_tasks = false;
    bool worker_present = false;
    bool worker_done = false;

    std::thread worker;

    /* Last-line-of-defense destructor. Runs from __cxa_finalize when the
     * .so is being unloaded. If perform_async_shutdown() was called
     * before this (the normal path), worker is no longer joinable here
     * and this is a no-op. If it wasn't called (broken frontend, crash
     * path, etc.), make damn sure we don't leave a joinable std::thread
     * for ~thread() to terminate() on. */
    ~AsyncState() {
        if (worker.joinable()) {
            try { worker.detach(); } catch (...) {}
        }
    }
};

AsyncState& state() {
    static AsyncState s;
    return s;
}

void worker_main() {
    for (;;) {
        std::vector<std::function<void()>> batch;
        {
            std::unique_lock<std::mutex> lock(state().mutex);
            state().cv.wait(lock, [] {
                return !state().accept_tasks || !state().tasks.empty();
            });
            if (!state().accept_tasks && state().tasks.empty()) {
                break;
            }
            batch.swap(state().tasks);
        }

        for (auto& task : batch) {
            /* Don't let an exception from one task tear down the worker;
             * that would leave subsequent tasks unrun and shutdown
             * waiting forever. */
            try {
                task();
            } catch (...) {
                /* Swallow. Logging from this thread isn't safe to do
                 * through nanogui, and we can't recover anyway. */
            }
        }
    }

    /* Publish completion. Even if our caller can't join() us cleanly,
     * they can wait on this flag. */
    {
        std::lock_guard<std::mutex> lock(state().mutex);
        state().worker_done = true;
    }
    state().cv.notify_all();
}

} /* anonymous namespace */

void perform_async_startup() {
    std::lock_guard<std::mutex> lock(state().mutex);
    if (state().worker_present) {
        return; /* already running */
    }

    state().accept_tasks = true;
    state().worker_done = false;
    state().tasks.clear();

    state().worker = std::thread(&worker_main);
    state().worker_present = true;
}

void perform_async(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(state().mutex);
        if (!state().accept_tasks) {
            /* Shutdown is in progress (or already done). Drop the task;
             * its lambda captures probably point at objects we're about
             * to destroy anyway. */
            return;
        }
        state().tasks.push_back(std::move(task));
    }
    state().cv.notify_one();
}

void perform_async_shutdown() {
    std::thread worker_to_dispose;
    {
        std::unique_lock<std::mutex> lock(state().mutex);
        if (!state().worker_present) {
            return; /* already shut down, or never started */
        }
        state().accept_tasks = false;
        state().cv.notify_all();

        /* Wait for the worker to acknowledge completion via worker_done.
         * Use a timeout as a safety net: if the worker is wedged in a
         * task (shouldn't happen -- everything it does is bounded), we
         * still want to make progress on shutdown rather than hanging
         * the host process. */
        state().cv.wait_for(lock, std::chrono::seconds(5), [] {
            return state().worker_done;
        });

        /* Hand the thread object to a local that outlives the lock. */
        worker_to_dispose = std::move(state().worker);
        state().worker_present = false;
        state().tasks.clear();
    }

    /* Dispose of the thread object without ever letting an exception
     * escape this function. Try join first; if that throws (which is
     * what triggered the original abort()), detach as a last resort.
     * Either way, the std::thread is left non-joinable so its destructor
     * won't call std::terminate. */
    if (worker_to_dispose.joinable()) {
        try {
            worker_to_dispose.join();
        } catch (const std::system_error&) {
            try {
                worker_to_dispose.detach();
            } catch (...) {
                /* Both join and detach failed. Best we can do is leak
                 * the thread object; the worker already signalled done
                 * via worker_done, so the OS thread itself has exited
                 * cleanly. */
            }
        } catch (...) {
            try { worker_to_dispose.detach(); } catch (...) {}
        }
    }
}

GameStreamClient::GameStreamClient() {
    perform_async_startup();
}

void GameStreamClient::connect(const std::string &address, ServerCallback<SERVER_DATA> callback) {
    m_server_data[address] = SERVER_DATA();
    
    perform_async([this, address, callback] {
        // TODO: mem leak here :(
        std::string key_dir = Settings::settings()->working_dir() + "/key";
        int status = gs_init(&m_server_data[address], (char *)(new std::string(address))->c_str(), key_dir.c_str(), 0, false);
        
        nanogui::async([this, address, callback, status] {
            if (status == GS_OK) {
                callback(Result<SERVER_DATA>::success(m_server_data[address]));
            } else {
                callback(Result<SERVER_DATA>::failure(std::string(gs_error)));
            }
        });
    });
}

void GameStreamClient::pair(const std::string &address, const std::string &pin, ServerCallback<bool> callback) {
    if (m_server_data.count(address) == 0) {
        callback(Result<bool>::failure("Firstly call connect"));
        return;
    }
    
    perform_async([this, address, pin, callback] {
        int status = gs_pair(&m_server_data[address], (char *)pin.c_str());
        
        nanogui::async([callback, status] {
            if (status == GS_OK) {
                callback(Result<bool>::success(true));
            } else {
                callback(Result<bool>::failure(std::string(gs_error)));
            }
        });
    });
}

void GameStreamClient::applist(const std::string &address, ServerCallback<PAPP_LIST> callback) {
    if (m_server_data.count(address) == 0) {
        callback(Result<PAPP_LIST>::failure("Firstly call connect"));
        return;
    }
    
    perform_async([this, address, callback] {
        int status = gs_applist(&m_server_data[address], &m_app_list[address]);
        
        nanogui::async([this, address, callback, status] {
            if (status == GS_OK) {
                callback(Result<PAPP_LIST>::success(m_app_list[address]));
            } else {
                callback(Result<PAPP_LIST>::failure(std::string(gs_error)));
            }
        });
    });
}

void GameStreamClient::app_boxart(const std::string &address, int app_id, ServerCallback<std::pair<char*, size_t>> callback) {
    if (m_server_data.count(address) == 0) {
        callback(Result<std::pair<char*, size_t>>::failure("Firstly call connect"));
        return;
    }
    
    perform_async([this, address, app_id, callback] {
        char* data;
        size_t size;
        int status = gs_app_boxart(&m_server_data[address], app_id, &data, &size);
        
        nanogui::async([this, callback, data, size, status] {
            if (status == GS_OK) {
                callback(Result<std::pair<char*, size_t>>::success(std::make_pair(data, size)));
            } else {
                callback(Result<std::pair<char*, size_t>>::failure(std::string(gs_error)));
            }
        });
    });
}

void GameStreamClient::start(const std::string &address, STREAM_CONFIGURATION config, int app_id, ServerCallback<STREAM_CONFIGURATION> callback) {
    if (m_server_data.count(address) == 0) {
        callback(Result<STREAM_CONFIGURATION>::failure("Firstly call connect"));
        return;
    }
    
    m_config = config;
    
    perform_async([this, address, app_id, callback] {
        int status = gs_start_app(&m_server_data[address], &m_config, app_id, true, true, 0x1);
        
        nanogui::async([this, callback, status] {
            if (status == GS_OK) {
                callback(Result<STREAM_CONFIGURATION>::success(m_config));
            } else {
                callback(Result<STREAM_CONFIGURATION>::failure(std::string(gs_error)));
            }
        });
    });
}

void GameStreamClient::quit(const std::string &address, ServerCallback<bool> callback) {
    if (m_server_data.count(address) == 0) {
        callback(Result<bool>::failure("Firstly call connect"));
        return;
    }
    
    perform_async([this, address, callback] {
        int status = gs_quit_app(&m_server_data[address]);
        
        nanogui::async([this, callback, status] {
            if (status == GS_OK) {
                callback(Result<bool>::success(true));
            } else {
                callback(Result<bool>::failure(std::string(gs_error)));
            }
        });
    });
}
