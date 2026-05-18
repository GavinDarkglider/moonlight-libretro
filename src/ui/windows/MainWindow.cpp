#include "MainWindow.hpp"
#include "AddHostWindow.hpp"
#include "AddHostButton.hpp"
#include "DiscoveryWindow.hpp"
#include "HostButton.hpp"
#include "GameStreamClient.hpp"
#include "LoadingOverlay.hpp"
#include "AppListWindow.hpp"
#include "SettingsWindow.hpp"
#include "Settings.hpp"
#include "nanovg.h"

using namespace nanogui;

MainWindow::MainWindow(Widget *parent): ContentWindow(parent, "Moonlight ") {
    set_box_layout(Orientation::Horizontal, Alignment::Minimum);
    
    /* Left title button: search the LAN for GameStream/Sunshine hosts
     * via mDNS. */
    set_left_title_button(FA_SEARCH, [this] {
        push<DiscoveryWindow>();
    });
    
    set_right_title_button(FA_COG, [this] {
        push<SettingsWindow>();
    });
}

void MainWindow::window_appear() {
    reload();
}

void MainWindow::reload() {
    clean_container();
    
    for (auto host: Settings::settings()->hosts()) {
        auto button = container()->add<HostButton>(host);
        button->set_fixed_size(Size(200, 200));
        button->set_callback([this, button] {
            if (button->is_active()) {
                if (button->is_paired()) {
                    push<AppListWindow>(button->address());
                } else {
                    auto loader = add<LoadingOverlay>("Pairing... (Enter 0000)");
                    
                    GameStreamClient::client()->pair(button->address(), "0000", [this, loader](auto result){
                        loader->dispose();
                        
                        if (result.isSuccess()) {
                            reload();
                        } else {
                            screen()->add<MessageDialog>(MessageDialog::Type::Information, "Error", result.error());
                        }
                    });
                }
            } else {
                screen()->add<MessageDialog>(MessageDialog::Type::Information, "Error", "Innactive host...");
            }
        });
    }
    
    auto button = container()->add<AddHostButton>();
    button->set_fixed_size(Size(200, 200));
    button->set_callback([this] {
        push<AddHostWindow>();
    });
    
    perform_layout();
}

void MainWindow::draw(NVGcontext *ctx) {
    ContentWindow::draw(ctx);
    
    nvgSave(ctx);
    
    NVGcolor bgColor;
    bgColor.r = 255 / 255.0;
    bgColor.g = 255 / 255.0;
    bgColor.b = 255 / 255.0;
    bgColor.a = 200 / 255.0;
    nvgFillColor(ctx, bgColor);

    nvgFontSize(ctx, 20);
    nvgFontFace(ctx, "sans");
    nvgText(ctx, width() - 40, height() - 8, MOONLIGHT_LIBRETRO_VERSION, NULL);
    
    nvgRestore(ctx);
}
