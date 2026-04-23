#include "hud/node.h"

namespace uldum::hud {

void Node::draw(Hud& hud) const {
    if (!visible) return;
    if (!is_owned_by(hud.local_player())) return;
    for (const auto& child : m_children) {
        child->draw(hud);
    }
}

void Panel::draw(Hud& hud) const {
    if (!visible) return;
    if (!is_owned_by(hud.local_player())) return;
    hud.draw_rect(rect, bg);
    Node::draw(hud);
}

void Label::draw(Hud& hud) const {
    if (!visible || text.empty()) return;
    if (!is_owned_by(hud.local_player())) return;

    // Baseline: centered vertically within rect. Ascent puts the top of
    // tall glyphs near the top of the rect; line height centers it.
    f32 ascent      = hud.text_ascent_px(px_size);
    f32 line_height = hud.text_line_height_px(px_size);
    f32 y_baseline  = rect.y + (rect.h - line_height) * 0.5f + ascent;

    f32 x_left = rect.x;
    if (align != Align::Left) {
        f32 w = hud.text_width_px(text, px_size);
        if (align == Align::Center) x_left = rect.x + (rect.w - w) * 0.5f;
        else                        x_left = rect.x + (rect.w - w);  // Right
    }

    hud.draw_text(x_left, y_baseline, text, color, px_size);
    Node::draw(hud);
}

void Bar::draw(Hud& hud) const {
    if (!visible) return;
    if (!is_owned_by(hud.local_player())) return;
    hud.draw_rect(rect, bg_color);

    f32 f = fill < 0.0f ? 0.0f : (fill > 1.0f ? 1.0f : fill);
    if (f > 0.0f) {
        Rect fr = rect;
        if (orientation == Orientation::Horizontal) {
            f32 fw = rect.w * f;
            if (direction == Direction::Rtl) {
                fr.x = rect.x + rect.w - fw;
            }
            fr.w = fw;
        } else {
            f32 fh = rect.h * f;
            if (direction == Direction::Btt) {
                fr.y = rect.y + rect.h - fh;
            }
            fr.h = fh;
        }
        hud.draw_rect(fr, fill_color);
    }
    Node::draw(hud);
}

void Image::draw(Hud& hud) const {
    if (!visible) return;
    if (!is_owned_by(hud.local_player())) return;
    if (!source.empty()) {
        hud.draw_image(rect, source, tint);
    }
    Node::draw(hud);
}

void Button::draw(Hud& hud) const {
    if (!visible) return;
    if (!is_owned_by(hud.local_player())) return;
    Color c = m_pressed ? bg_press : (m_hovered ? bg_hover : bg);
    hud.draw_rect(rect, c);
    Node::draw(hud);
}

bool Button::on_release(bool over) {
    bool clicked = m_pressed && over && enabled;
    m_pressed = false;
    if (clicked && on_click_cb) on_click_cb();
    return clicked;
}

} // namespace uldum::hud
