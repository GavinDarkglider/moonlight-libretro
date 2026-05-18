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
#include <nanogui/nanogui.h>

#include <unistd.h>

static std::mutex m_async_mutex;
static std::condition_variable m_async_cv;
static std::vector<std::function<void()>> m_tasks;
static std::atomic<bool> m_async_running{false};
static std::thread m_async_thread;

static void task_loop() {
    if (m_async_running.exchange(true)) {
        return; /* already running */
    }

    m_async_thread = std::thread([](){
        for (;;) {
            std::vector<std::function<void()>> m_tasks_copy;
            {
                std::unique_lock<std::mutex> lock(m_async_mutex);
                m_async_cv.wait(lock, [] {
                    return !m_async_running.load() || !m_tasks.empty();
                });
                if (!m_async_running.load() && m_tasks.empty()) {
                    break;
                }
                m_tasks_copy.swap(m_tasks);
            }

            for (auto &task: m_tasks_copy) {
                task();
            }
        }
    });
}

void perform_async(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> guard(m_async_mutex);
        if (!m_async_running.load()) {
            /* Drop tasks queued during shutdown; the worker has already
             * been joined and will not pick them up. */
            return;
        }
        m_tasks.push_back(std::move(task));
    }
    m_async_cv.notify_one();
}

void perform_async_shutdown() {
    if (!m_async_running.exchange(false)) {
        return; /* never started, or already shut down */
    }
    m_async_cv.notify_all();
    if (m_async_thread.joinable()) {
        m_async_thread.join();
    }
    /* Anything still queued at this point will never run; drop it so the
     * lambdas (and their captures) are destroyed deterministically before
     * the surrounding C++ objects go away. */
    std::lock_guard<std::mutex> guard(m_async_mutex);
    m_tasks.clear();
}

GameStreamClient::GameStreamClient() {
    task_loop();
}

void GameStreamClient::connect(const std::string &address, ServerCallback<SERVER_DATA> callback) {
    m_server_data[address] = SERVER_DATA();
    
    perform_async([this, address, callback] {
        // TODO: mem leak here :(
        std::string key_dir = Settings::settings()->working_dir() + "/key";
        int status = gs_init(&m_server_data[address], (char *)(new std::string(address))->c_str(), key_dir.c_str(), 0, false);
        
        nanogui::async([this, address, callback, status] {
            if (status == GS_OK) {
                Settings::settings()->add_host(address);
                callback(Result<SERVER_DATA>::success(m_server_data[address]));
            } else {
                callback(Result<SERVER_DATA>::failure(gs_error != NULL ? gs_error : "Unknown error..."));
            }
        });
    });
}

void GameStreamClient::pair(const std::string &address, const std::string &pin, ServerCallback<bool> callback) {
    if (m_server_data.count(address) == 0) {
        callback(Result<bool>::failure("Firstly call connect()..."));
        return;
    }
    
    perform_async([this, address, pin, callback] {
        int status = gs_pair(&m_server_data[address], (char *)pin.c_str());
        
        nanogui::async([callback, status] {
            if (status == GS_OK) {
                callback(Result<bool>::success(true));
            } else {
                callback(Result<bool>::failure(gs_error != NULL ? gs_error : "Unknown error..."));
            }
        });
    });
}

void GameStreamClient::applist(const std::string &address, ServerCallback<PAPP_LIST> callback) {
    if (m_server_data.count(address) == 0) {
        callback(Result<PAPP_LIST>::failure("Firstly call connect() & pair()..."));
        return;
    }
    
    m_app_list[address] = PAPP_LIST();
    
    perform_async([this, address, callback] {
        int status = gs_applist(&m_server_data[address], &m_app_list[address]);
        
        nanogui::async([this, address, callback, status] {
            if (status == GS_OK) {
                callback(Result<PAPP_LIST>::success(m_app_list[address]));
            } else {
                callback(Result<PAPP_LIST>::failure(gs_error != NULL ? gs_error : "Unknown error..."));
            }
        });
    });
}

void GameStreamClient::app_boxart(const std::string &address, int app_id, ServerCallback<std::pair<char*, size_t>> callback) {
    if (m_server_data.count(address) == 0) {
        callback(Result<std::pair<char*, size_t>>::failure("Firstly call connect() & pair()..."));
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
                callback(Result<std::pair<char*, size_t>>::failure(gs_error != NULL ? gs_error : "Unknown error..."));
            }
        });
    });
}

void GameStreamClient::start(const std::string &address, STREAM_CONFIGURATION config, int app_id, ServerCallback<STREAM_CONFIGURATION> callback) {
    if (m_server_data.count(address) == 0) {
        callback(Result<STREAM_CONFIGURATION>::failure("Firstly call connect() & pair()..."));
        return;
    }
    
    m_config = config;
    
    perform_async([this, address, app_id, callback] {
        int status = gs_start_app(&m_server_data[address], &m_config, app_id, false, false, 0);
        
        nanogui::async([this, callback, status] {
            if (status == GS_OK) {
                callback(Result<STREAM_CONFIGURATION>::success(m_config));
            } else {
                callback(Result<STREAM_CONFIGURATION>::failure(gs_error != NULL ? gs_error : "Unknown error..."));
            }
        });
    });
}

void GameStreamClient::quit(const std::string &address, ServerCallback<bool> callback) {
    if (m_server_data.count(address) == 0) {
        callback(Result<bool>::failure("Firstly call connect() & pair()..."));
        return;
    }
    
    perform_async([this, address, callback] {
        int status = gs_quit_app(&m_server_data[address]);
        
        nanogui::async([this, callback, status] {
            if (status == GS_OK) {
                callback(Result<bool>::success(true));
            } else {
                callback(Result<bool>::failure(gs_error != NULL ? gs_error : "Unknown error..."));
            }
        });
    });
}
