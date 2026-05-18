#include "DiscoveryWindow.hpp"
#include "HostButton.hpp"
#include "AppListWindow.hpp"
#include "GameStreamClient.hpp"
#include "LoadingOverlay.hpp"
#include "Settings.hpp"
#include "Log.h"

using namespace nanogui;

DiscoveryWindow::DiscoveryWindow(Widget* parent)
    : ContentWindow(parent, "Discover Hosts") {
    set_left_pop_button();
    set_box_layout(Orientation::Vertical, Alignment::Fill);

    /* Status row at the top: text label that flips between
     * "Searching..." and "Found N host(s)" or "No hosts found." */
    m_status_widget = container()->add<Widget>();
    m_status_widget->set_layout(new BoxLayout(Orientation::Horizontal, Alignment::Middle, 0, 10));
    m_status_widget->set_fixed_height(60);
    m_status_label = m_status_widget->add<Label>("");
    m_status_label->set_font_size(28);

    /* Results grid below. */
    m_results_widget = container()->add<Widget>();
    m_results_widget->set_layout(new GridLayout(Orientation::Horizontal, 4, Alignment::Middle, 0, 20));
}

DiscoveryWindow::~DiscoveryWindow() {
    /* m_discovery's destructor joins the worker thread if it's still
     * running. unique_ptr handles it. */
}

void DiscoveryWindow::window_appear() {
    start_scan();
}

void DiscoveryWindow::window_disappear() {
    /* Tear down the scanner if the user backs out before it finishes.
     * The worker thread joins; any in-flight nanogui::async callbacks
     * are safe because they capture by value and don't deref `this`. */
    if (m_discovery) {
        m_discovery->stop();
        m_discovery.reset();
    }
    m_scanning = false;
}

void DiscoveryWindow::start_scan() {
    if (m_scanning) return;

    /* Reset state for a fresh scan. */
    m_seen_addresses.clear();
    for (auto child : m_results_widget->children()) {
        if (m_results_widget->child_index(child) != -1) {
            m_results_widget->remove_child(child);
        }
    }
    m_status_label->set_caption("Searching for GameStream and Sunshine hosts...");
    perform_layout();

    m_scanning = true;
    m_discovery = std::make_unique<ServerDiscovery>();

    /* Callbacks fire on the main thread (nanogui::async marshals them).
     * The discovery is stopped from window_disappear() and from our
     * destructor, so a callback firing after `this` is gone shouldn't
     * happen. Defensively check m_scanning anyway in case nanogui has
     * a pending callback that escapes. */
    m_discovery->start(
        /* on_host */
        [this](const DiscoveredHost& host) {
            if (!m_scanning) return;
            add_host_card(host);
        },
        /* on_done */
        [this] {
            if (!m_scanning) return;
            m_scanning = false;
            if (m_seen_addresses.empty()) {
                m_status_label->set_caption(
                    "No hosts found. Make sure GeForce Experience or Sunshine "
                    "is running on the same network, or add the host manually.");
            } else {
                m_status_label->set_caption(
                    "Found " + std::to_string(m_seen_addresses.size()) +
                    " host(s). Tap one to pair.");
            }
            perform_layout();
        });
}

void DiscoveryWindow::add_host_card(const DiscoveredHost& host) {
    /* Prefer the hostname for storage (it's stable across DHCP lease
     * changes; libcurl resolves it). Fall back to IPv4 if no hostname. */
    const std::string& address = !host.hostname.empty()
        ? host.hostname
        : host.ipv4_address;
    if (address.empty()) return;

    if (m_seen_addresses.count(address)) return;
    m_seen_addresses.insert(address);

    auto button = m_results_widget->add<HostButton>(address);
    button->set_fixed_size(Size(200, 200));
    button->set_callback([this, button, address] {
        /* Mirror MainWindow's behaviour: persist the host, then branch
         * on its current state. The HostButton's constructor kicks off
         * a connect() asynchronously; by the time the user taps the
         * button, is_active()/is_paired() should reflect the host's
         * current state. */
        Settings::settings()->add_host(address);

        if (!button->is_active()) {
            screen()->add<MessageDialog>(
                MessageDialog::Type::Information,
                "Host not responding",
                "Couldn't reach " + address +
                ". It may have gone offline; try again or add manually.");
            return;
        }

        if (button->is_paired()) {
            push<AppListWindow>(address);
            return;
        }

        auto loader = add<LoadingOverlay>("Pairing... (Enter 0000)");
        GameStreamClient::client()->pair(address, "0000",
            [this, loader](auto result) {
                loader->dispose();
                if (result.isSuccess()) {
                    this->pop();
                } else {
                    screen()->add<MessageDialog>(
                        MessageDialog::Type::Information,
                        "Pairing failed",
                        result.error());
                }
            });
    });

    perform_layout();
}

void DiscoveryWindow::draw(NVGcontext* ctx) {
    ContentWindow::draw(ctx);
}
