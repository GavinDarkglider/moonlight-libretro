#include <set>
#include <memory>
#include "ContentWindow.hpp"
#include "ServerDiscovery.hpp"

#pragma once

/*
 * UI for discovering GameStream/Sunshine hosts on the local network via
 * mDNS. Shows a spinner while scanning and a grid of HostButtons as hosts
 * arrive. Tapping a host adds it to Settings and pushes the existing
 * AppListWindow / pairing flow.
 */

class DiscoveryWindow: public ContentWindow {
public:
    DiscoveryWindow(nanogui::Widget* parent);
    ~DiscoveryWindow();

    void window_appear() override;
    void window_disappear() override;

    void draw(NVGcontext* ctx) override;

private:
    void start_scan();
    void add_host_card(const DiscoveredHost& host);

    std::unique_ptr<ServerDiscovery> m_discovery;
    /* Dedup: a single host can advertise both an IPv4 A record and a
     * hostname, sometimes from multiple network interfaces. Key on
     * whichever connectable identifier we end up using. */
    std::set<std::string> m_seen_addresses;
    nanogui::Widget* m_status_widget = nullptr;
    nanogui::Label* m_status_label = nullptr;
    nanogui::Widget* m_results_widget = nullptr;
    bool m_scanning = false;
};
