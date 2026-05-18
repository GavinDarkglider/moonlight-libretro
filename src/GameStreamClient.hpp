#include <string>
#include <vector>
#include <functional>
#include <map>

extern "C" {
    #include "client.h"
    #include "errors.h"
}

#pragma once

extern void perform_async(std::function<void()> task);

/* Bring the background worker thread up. Safe to call multiple times --
 * the second call is a no-op. moonlight_init() invokes this so that a
 * reload cycle (shutdown -> init) restarts the worker that the previous
 * shutdown joined. */
extern void perform_async_startup();

/* Signal the background worker thread to drain its queue, exit, and be
 * disposed of (join, or detach as a last resort). Must be called before
 * the .so is dlclose()d, otherwise the static-destructor path tries to
 * destroy a still-joinable std::thread and calls std::terminate(). */
extern void perform_async_shutdown();

template <typename T>
struct Result {
public:
    static Result success(T value) {
        return result(value, "", true);
    }
    
    static Result failure(std::string error) {
        return result(T(), error, false);
    }
    
    bool isSuccess() const {
        return _isSuccess;
    }
    
    T value() const {
        return _value;
    }
    
    std::string error() const {
        return _error;
    }
    
private:
    static Result result(T value, std::string error, bool isSuccess) {
        Result result;
        result._value = value;
        result._error = error;
        result._isSuccess = isSuccess;
        return result;
    }
    
    T _value;
    std::string _error = "";
    bool _isSuccess = false;
};

template<class T> using ServerCallback = const std::function<void(Result<T>)>;

class GameStreamClient {
public:
    static GameStreamClient* client() {
        static GameStreamClient client;
        return &client;
    }
    
    SERVER_DATA server_data(const std::string &address) {
        return m_server_data[address];
    }
    
    void connect(const std::string &address, ServerCallback<SERVER_DATA> callback);
    void pair(const std::string &address, const std::string &pin, ServerCallback<bool> callback);
    void applist(const std::string &address, ServerCallback<PAPP_LIST> callback);
    void app_boxart(const std::string &address, int app_id, ServerCallback<std::pair<char*, size_t>> callback);
    void start(const std::string &address, STREAM_CONFIGURATION config, int app_id, ServerCallback<STREAM_CONFIGURATION> callback);
    void quit(const std::string &address, ServerCallback<bool> callback);
    
private:
    GameStreamClient();
    
    std::map<std::string, SERVER_DATA> m_server_data;
    std::map<std::string, PAPP_LIST> m_app_list;
    STREAM_CONFIGURATION m_config;
};
