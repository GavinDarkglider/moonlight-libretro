#include "ServerDiscovery.hpp"
#include "Log.h"

#include <nanogui/nanogui.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

#include <chrono>
#include <map>
#include <memory>

extern "C" {
#include "mdns.h"
}

namespace {

/* The mDNS callback can only pass one void* of user data, so we bundle
 * everything the parser needs to share with the main loop into a single
 * struct. */
struct ScanContext {
    /* Per-service-name accumulator. The mDNS protocol may return PTR,
     * SRV, and A records across one or more packets; we collect them
     * here until we have enough to emit a DiscoveredHost. */
    struct Accumulator {
        std::string ptr_name;       /* e.g. "MYPC._nvstream._tcp.local." */
        std::string hostname;       /* from SRV target,  e.g. "MYPC.local." */
        std::string ipv4_address;   /* from A record */
        unsigned short port = 0;    /* from SRV port */
        bool emitted = false;
    };
    std::map<std::string, Accumulator> by_service;

    ServerDiscovery::HostCallback callback;

    /* Strip a trailing dot from a DNS name if present, then if it ends
     * in ".local" leave it alone (libcurl/getaddrinfo handles .local
     * via mDNS NSS when configured). Suitable for showing in the UI. */
    static std::string clean_hostname(const std::string& s) {
        if (s.empty()) return s;
        if (s.back() == '.') return s.substr(0, s.size() - 1);
        return s;
    }

    /* Once an accumulator has at least a hostname or IPv4 + a port,
     * emit a DiscoveredHost on the main thread and mark it emitted so
     * we don't fire duplicates if more records arrive later. */
    void maybe_emit(Accumulator& acc) {
        if (acc.emitted) return;
        if (acc.port == 0) return;
        if (acc.hostname.empty() && acc.ipv4_address.empty()) return;

        DiscoveredHost host;
        host.hostname = clean_hostname(acc.hostname);
        host.ipv4_address = acc.ipv4_address;
        host.port = acc.port;
        acc.emitted = true;

        /* Hop to the main thread so the UI can safely build widgets. */
        auto cb = callback;
        nanogui::async([cb, host] {
            cb(host);
        });
    }
};

/* Convert an mdns_string_t (offset + length into the packet buffer)
 * into a std::string. The library's mdns_string_extract() writes a
 * null-terminated string into a caller-provided buffer; we wrap that. */
std::string extract_string(mdns_string_t s) {
    return std::string(s.str, s.length);
}

int mdns_callback(int sock, const struct sockaddr* from, size_t addrlen,
                  mdns_entry_type_t entry, uint16_t query_id, uint16_t rtype,
                  uint16_t rclass, uint32_t ttl, const void* data, size_t size,
                  size_t name_offset, size_t name_length, size_t record_offset,
                  size_t record_length, void* user_data)
{
    (void)sock; (void)from; (void)addrlen; (void)entry; (void)query_id;
    (void)rclass; (void)ttl; (void)name_length;

    auto* ctx = static_cast<ScanContext*>(user_data);

    /* mdns_string_extract needs scratch space to write a NUL-terminated
     * decoded name. We use stack buffers since we never need more than
     * one decode in flight at a time. */
    char name_buf[256];
    char data_buf[256];

    /* Decode the record's *owner name* -- for PTR, this is the service
     * type ("_nvstream._tcp.local."); for SRV / A / TXT, it's the
     * instance-specific name ("MYPC._nvstream._tcp.local."). */
    size_t off = name_offset;
    mdns_string_t owner = mdns_string_extract(data, size, &off, name_buf, sizeof(name_buf));
    std::string owner_str = extract_string(owner);

    switch (rtype) {
    case MDNS_RECORDTYPE_PTR: {
        /* The PTR's *data* points at the instance name. Use that as our
         * accumulator key. */
        mdns_string_t ptr = mdns_record_parse_ptr(
            data, size, record_offset, record_length,
            data_buf, sizeof(data_buf));
        std::string instance = extract_string(ptr);
        if (instance.empty()) break;

        auto& acc = ctx->by_service[instance];
        acc.ptr_name = instance;
        break;
    }
    case MDNS_RECORDTYPE_SRV: {
        /* For SRV the owner is the instance name. */
        mdns_record_srv_t srv = mdns_record_parse_srv(
            data, size, record_offset, record_length,
            data_buf, sizeof(data_buf));
        if (srv.name.length == 0) break;

        auto& acc = ctx->by_service[owner_str];
        acc.hostname = extract_string(srv.name);
        acc.port = srv.port;
        ctx->maybe_emit(acc);
        break;
    }
    case MDNS_RECORDTYPE_A: {
        struct sockaddr_in addr;
        mdns_record_parse_a(data, size, record_offset, record_length, &addr);
        if (addr.sin_addr.s_addr == 0) break;

        char ipstr[INET_ADDRSTRLEN];
        if (!inet_ntop(AF_INET, &addr.sin_addr, ipstr, sizeof(ipstr))) break;

        /* A records' owner names are hostnames, e.g. "MYPC.local."
         * We don't have a direct service-name correlation, so walk the
         * accumulators and fill in any whose SRV target matches this
         * hostname. */
        for (auto& kv : ctx->by_service) {
            if (kv.second.hostname == owner_str &&
                kv.second.ipv4_address.empty()) {
                kv.second.ipv4_address = ipstr;
                ctx->maybe_emit(kv.second);
            }
        }
        break;
    }
    default:
        break;
    }
    return 0;
}

} /* anonymous namespace */

ServerDiscovery::ServerDiscovery() = default;

ServerDiscovery::~ServerDiscovery() {
    stop();
}

void ServerDiscovery::start(HostCallback on_host, CompletionCallback on_done) {
    std::lock_guard<std::mutex> lock(m_thread_mutex);
    if (m_running.load()) {
        return;
    }
    m_running = true;
    m_stop_requested = false;
    m_thread = std::thread(&ServerDiscovery::run, this,
                           std::move(on_host), std::move(on_done));
}

void ServerDiscovery::stop() {
    std::lock_guard<std::mutex> lock(m_thread_mutex);
    m_stop_requested = true;
    if (m_thread.joinable()) {
        try {
            m_thread.join();
        } catch (...) {
            try { m_thread.detach(); } catch (...) {}
        }
    }
    m_running = false;
}

void ServerDiscovery::run(HostCallback on_host, CompletionCallback on_done) {
    /* Open an mDNS socket on an ephemeral port. Per RFC 6762, using
     * ephemeral source ports yields unicast responses (only we see
     * them) rather than multicast (every host on the LAN sees them).
     * For one-shot discovery from a client that's exactly what we
     * want. */
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = 0;
    int sock = mdns_socket_open_ipv4(&addr);
    if (sock < 0) {
        LOG_FMT("ServerDiscovery: failed to open mDNS socket (errno=%d)\n", errno);
        m_running = false;
        if (on_done) {
            auto cb = std::move(on_done);
            nanogui::async([cb] { cb(); });
        }
        return;
    }

    /* Send the service query. _nvstream._tcp.local. is what both GFE
     * and Sunshine advertise as. */
    static const char kServiceName[] = "_nvstream._tcp.local.";
    char query_buffer[2048];
    int query_id = mdns_query_send(
        sock,
        MDNS_RECORDTYPE_PTR,
        kServiceName, sizeof(kServiceName) - 1,
        query_buffer, sizeof(query_buffer),
        0);
    if (query_id < 0) {
        LOG("ServerDiscovery: mdns_query_send failed\n");
        mdns_socket_close(sock);
        m_running = false;
        if (on_done) {
            auto cb = std::move(on_done);
            nanogui::async([cb] { cb(); });
        }
        return;
    }

    ScanContext ctx;
    ctx.callback = std::move(on_host);

    /* Receive responses until either kScanDurationSeconds has elapsed
     * or stop() was called. select() with a small timeout lets us check
     * the stop flag responsively without burning CPU. */
    char recv_buffer[8192];
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::seconds(kScanDurationSeconds);

    while (!m_stop_requested.load()) {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) break;

        struct timeval tv;
        auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(
            deadline - now);
        tv.tv_sec = remaining.count() / 1000000;
        tv.tv_usec = remaining.count() % 1000000;
        /* Cap the per-iteration wait at 100ms so the stop flag is
         * checked responsively. */
        if (tv.tv_sec > 0 || tv.tv_usec > 100'000) {
            tv.tv_sec = 0;
            tv.tv_usec = 100'000;
        }

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(sock, &readfds);
        int n = select(sock + 1, &readfds, nullptr, nullptr, &tv);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) continue;

        mdns_query_recv(sock, recv_buffer, sizeof(recv_buffer),
                        mdns_callback, &ctx, query_id);
    }

    mdns_socket_close(sock);
    m_running = false;

    /* Notify the main thread that the scan is complete. */
    if (on_done) {
        auto cb = std::move(on_done);
        nanogui::async([cb] { cb(); });
    }
}
