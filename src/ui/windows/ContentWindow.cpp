#include "ContentWindow.hpp"
#include "Application.hpp"
#include "nanovg.h"

using namespace nanogui;

ContentWindow::ContentWindow(Widget *parent, const std::string& title): Widget(parent) {
    set_layout(new BoxLayout(Orientation::Vertical, Alignment::Middle, 0, 0));
    set_size(parent->size());
    set_fixed_size(parent->size());
    
    auto title_container = add<Widget>();
    title_container->set_fixed_size(Size(parent->width() - 40, 80));
    title_container->set_layout(new BoxLayout(Orientation::Horizontal, Alignment::Middle, 0, 10));
    
    m_left_title_button_container = title_container->add<Widget>();
    m_left_title_button_container->set_layout(new BoxLayout(Orientation::Horizontal));
    
    m_title_label = title_container->add<Label>(title);
    /* Reserve 80px on each side for an optional title button (60px) plus
     * BoxLayout spacing, plus the container's own 40px outer margins.
     * The original 140 only accounted for a right button; once MainWindow
     * grew a left "discover" button, the right "settings" button got
     * pushed off-screen. */
    m_title_label->set_fixed_width(parent->width() - 220);
    m_title_label->set_font_size(40);
    
    m_right_title_button_container = title_container->add<Widget>();
    m_right_title_button_container->set_layout(new BoxLayout(Orientation::Horizontal));
    
    m_scroll = add<VScrollPanel>();
    m_scroll->set_fixed_size(Size(parent->width() - 60, parent->height() - 80));
    m_container = m_scroll->add<Widget>();
}

void ContentWindow::draw(NVGcontext *ctx) {
    nvgSave(ctx);
    
    // Draw bg
    NVGcolor bgColor;
    bgColor.r = 48 / 255.0;
    bgColor.g = 48 / 255.0;
    bgColor.b = 48 / 255.0;
    bgColor.a = 255 / 255.0;
    nvgFillColor(ctx, bgColor);

    nvgBeginPath(ctx);
    nvgRect(ctx, 0, 0, width(), height());
    nvgFill(ctx);
    
    // Draw header
    NVGcolor headerColor;
    headerColor.r = 62 / 255.0;
    headerColor.g = 78 / 255.0;
    headerColor.b = 184 / 255.0;
    headerColor.a = 255 / 255.0;
    nvgFillColor(ctx, headerColor);

    nvgBeginPath(ctx);
    nvgRect(ctx, 0, 0, width(), 80);
    nvgFill(ctx);
    
    // Draw separator
    NVGcolor greyColor;
    headerColor.r = 0 / 255.0;
    headerColor.g = 0 / 255.0;
    headerColor.b = 0 / 255.0;
    headerColor.a = 100 / 255.0;

    NVGcolor blackColor;
    blackColor.r = 0 / 255.0;
    blackColor.g = 0 / 255.0;
    blackColor.b = 0 / 255.0;
    blackColor.a = 0 / 255.0;

    nvgBeginPath(ctx);
    NVGpaint gradient = nvgLinearGradient(ctx, 0, 80, 0, 84, greyColor, blackColor);
    nvgFillPaint(ctx, gradient);
    nvgRect(ctx, 0, 80, width(), 4);
    nvgFill(ctx);
    
    nvgRestore(ctx);
    
    Widget::draw(ctx);
}

void ContentWindow::set_left_title_button(int icon, const std::function<void()> &callback) {
    auto button = m_left_title_button_container->add<Button>("", icon);
    button->set_fixed_size(Size(60, 60));
    button->set_icon_extra_scale(3);
    button->set_callback([callback] {
        callback();
    });
    perform_layout();
}

void ContentWindow::set_right_title_button(int icon, const std::function<void()> &callback) {
    auto button = m_right_title_button_container->add<Button>("", icon);
    button->set_fixed_size(Size(60, 60));
    button->set_icon_extra_scale(3);
    button->set_callback([callback] {
        callback();
    });
    perform_layout();
}
