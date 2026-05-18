#include "AddHostWindow.hpp"
#include "GameStreamClient.hpp"
#include "LoadingOverlay.hpp"
#include <cstring>

using namespace nanogui;

namespace {

/* The keyboard layout. Each row is a string of single-byte lowercase
 * characters; the corresponding shifted character is the uppercase form
 * for letters, or a shift-symbol for the punctuation row. This stays
 * compact and ASCII-only -- hostnames, IPs, and domain names don't need
 * anything outside ASCII. */
struct KeyRow {
    const char* lower;
    const char* upper;
};

const KeyRow kKeyRows[] = {
    { "1234567890", "1234567890" },
    { "qwertyuiop", "QWERTYUIOP" },
    { "asdfghjkl-", "ASDFGHJKL_" },
    { "zxcvbnm.:/", "ZXCVBNM.:/" },
};

constexpr int kKeyButtonSize = 80;
constexpr int kSpaceButtonWidth = 320;
constexpr int kControlButtonWidth = 160;

} /* anonymous namespace */

AddHostWindow::AddHostWindow(Widget *parent): ContentWindow(parent, "Add Host") {
    set_left_pop_button();
    set_box_layout(Orientation::Vertical, Alignment::Fill);

    m_text_box = container()->add<TextBox>("");
    m_text_box->set_placeholder("IP address or hostname (e.g. 192.168.1.50 or gaming-pc.local)");
    m_text_box->set_fixed_height(60);

    /* Build the four keyboard rows. Each row is its own Widget with a
     * horizontal layout, so the keys align even though the rows have
     * different counts of keys. */
    for (const auto& row : kKeyRows) {
        auto row_container = container()->add<Widget>();
        row_container->set_layout(new BoxLayout(Orientation::Horizontal, Alignment::Middle, 0, 8));

        size_t row_len = strlen(row.lower);
        for (size_t i = 0; i < row_len; ++i) {
            std::string lower(1, row.lower[i]);
            std::string upper(1, row.upper[i]);

            auto button = row_container->add<Button>(lower);
            button->set_fixed_size(Size(kKeyButtonSize, kKeyButtonSize));

            /* Capture this and the button so the callback can read the
             * current shift state and the button's *current* caption. */
            auto* btn_ptr = button;
            button->set_callback([this, btn_ptr] {
                m_text_box->set_value(m_text_box->value() + btn_ptr->caption());
            });

            m_letter_buttons.push_back({button, lower, upper});
        }
    }

    /* Bottom row: shift, space, backspace, connect. */
    auto bottom = container()->add<Widget>();
    bottom->set_layout(new BoxLayout(Orientation::Horizontal, Alignment::Middle, 0, 8));

    m_shift_button = bottom->add<Button>("SHIFT");
    m_shift_button->set_flags(Button::ToggleButton);
    m_shift_button->set_fixed_size(Size(kControlButtonWidth, kKeyButtonSize));
    m_shift_button->set_change_callback([this](bool pushed) {
        m_shift_on = pushed;
        rebuild_letter_captions();
    });

    auto space = bottom->add<Button>("");
    /* Use a small caption rather than empty so the button has visual
     * weight; nanogui's Button supports unicode just fine. */
    space->set_caption(" ");
    space->set_fixed_size(Size(kSpaceButtonWidth, kKeyButtonSize));
    space->set_callback([this] {
        m_text_box->set_value(m_text_box->value() + " ");
    });

    auto backspace = bottom->add<Button>("");
    backspace->set_icon(FA_BACKSPACE);
    backspace->set_icon_extra_scale(2);
    backspace->set_fixed_size(Size(kControlButtonWidth, kKeyButtonSize));
    backspace->set_callback([this] {
        auto v = m_text_box->value();
        if (!v.empty()) {
            v.pop_back();
            m_text_box->set_value(v);
        }
    });

    auto connect = bottom->add<Button>("Connect");
    connect->set_fixed_size(Size(kControlButtonWidth, kKeyButtonSize));
    connect->set_callback([this] {
        on_connect_pressed();
    });
}

void AddHostWindow::rebuild_letter_captions() {
    for (auto& lb : m_letter_buttons) {
        lb.button->set_caption(m_shift_on ? lb.upper : lb.lower);
    }
    perform_layout();
}

void AddHostWindow::on_connect_pressed() {
    auto value = m_text_box->value();
    if (value.empty()) return;

    /* Trim leading/trailing whitespace -- easy to add accidentally with
     * the space key and confuses libcurl. */
    auto first = value.find_first_not_of(" \t");
    if (first == std::string::npos) return;
    auto last = value.find_last_not_of(" \t");
    value = value.substr(first, last - first + 1);

    auto loader = add<LoadingOverlay>();
    GameStreamClient::client()->connect(value, [this, loader](auto result) {
        loader->dispose();
        if (result.isSuccess()) {
            this->pop();
        } else {
            screen()->add<MessageDialog>(
                MessageDialog::Type::Information, "Error", result.error());
        }
    });
}
