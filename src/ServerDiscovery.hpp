#include <string>
#include <vector>
#include <functional>
#include <thread>
#include <mutex>
#include <atomic>

#pragma once

/*
 * mDNS-based discovery of GameStream/Sunshine hosts on the local network.
 *
 * Both NVIDIA GeForce Experience and Sunshine advertise themselves with
 * the same service type: _nvstream._tcp.local.
 *
 * Usage:
 *
 *   auto discovery = new ServerDiscovery();
 *   discovery->start([](DiscoveredHost host) {
 *       // Called on the main thread (via nanogui::async) when a new
 *       // host is found. Safe to mutate UI state from here.
 *   });
 *
 *   // ... later, e.g. when the user dismisses the discovery window:
 *   discovery->stop();
 *   delete discovery;
 *
 * The discovery thread runs for up to ServerDiscovery::kScanDurationSeconds
 * and then exits on its own. stop() is safe to call before, during, or
 * after that.
 */

struct DiscoveredHost {
    /* The hostname the host advertises, e.g. "GAMING-RIG.local". Suitable
     * for passing to GameStreamClient::connect() -- libcurl will resolve
     * the hostname via the system resolver, which routes .local through
     * any running mDNS responder (nss-mdns / systemd-resolved). */
    std::string hostname;
    /* The IPv4 address from the A record, dotted-decimal form. Used as a
     * fallback if hostname resolution doesn't work on a system without
     * mDNS NSS configured. */
    std::string ipv4_address;
    /* The TCP port from the SRV record. GameStream is typically on 47989
     * (HTTP) but we read what the host advertises. */
    unsigned short port = 47989;
};

class ServerDiscovery {
public:
    using HostCallback = std::function<void(const DiscoveredHost&)>;
    using CompletionCallback = std::function<void()>;

    ServerDiscovery();
    ~ServerDiscovery();

    /* Spawn the discovery thread. on_host fires on the main thread (via
     * nanogui::async) once per host discovered. on_done fires on the
     * main thread when the scan finishes (either by timeout or by stop()).
     * Safe to call only once per ServerDiscovery instance; subsequent
     * calls are no-ops. on_done is optional. */
    void start(HostCallback on_host, CompletionCallback on_done = {});

    /* Request the discovery thread to exit and join it. Idempotent and
     * safe to call from any thread. Blocks until the thread is gone,
     * but is bounded by kScanDurationSeconds. */
    void stop();

    bool is_running() const { return m_running.load(); }

    static constexpr int kScanDurationSeconds = 5;

private:
    void run(HostCallback on_host, CompletionCallback on_done);

    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stop_requested{false};
    std::mutex m_thread_mutex;
};
