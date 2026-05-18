#include "ContentWindow.hpp"
#include "GameStreamClient.hpp"
#include <vector>
#pragma once

class AddHostWindow: public ContentWindow {
public:
    AddHostWindow(Widget *parent);

private:
    /* When the shift toggle flips, every letter button's caption is
     * re-rendered. The buttons live here for the callback to reach. */
    struct LetterButton {
        nanogui::Button* button;
        std::string lower;
        std::string upper;
    };
    std::vector<LetterButton> m_letter_buttons;
    nanogui::Button* m_shift_button = nullptr;
    bool m_shift_on = false;

    nanogui::TextBox* m_text_box = nullptr;

    void rebuild_letter_captions();
    void on_connect_pressed();
};
