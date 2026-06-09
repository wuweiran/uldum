#include "hud/node.h"
#include "hud/hud.h"

#include "i18n/locale.h"

namespace uldum::hud {

// All Node-subclass draw methods virtually-dispatch through
// HudRenderInterface; no concrete renderer types are referenced here,
// which is what lets `uldum_hud` compile and link without any Vulkan /
// rhi dependency. The vtables for Node, Panel, Label, Bar, Image, and
// Button live in this translation unit (uldum_hud) so consumers don't
// drag the renderer in at link time.

void Node::draw(HudRenderInterface& r) const {
    if (!visible) return;
    if (!is_owned_by(r.hud().local_player())) return;
    for (const auto& child : m_children) {
        child->draw(r);
    }
}

void Panel::draw(HudRenderInterface& r) const {
    if (!visible) return;
    if (!is_owned_by(r.hud().local_player())) return;
    r.draw_rect(rect, bg);
    Node::draw(r);
}

void Label::draw(HudRenderInterface& r) const {
    if (!visible || text.empty()) return;
    if (!is_owned_by(r.hud().local_player())) return;

    auto* lm = r.hud().locale_manager();
    std::string rendered = lm
        ? lm->resolve(i18n::Pool::Map, text)
        : text.key;
    if (rendered.empty()) return;

    f32 ascent      = r.text_ascent_px(px_size);
    f32 line_height = r.text_line_height_px(px_size);

    // Split on '\n' and render each line on its own baseline. The
    // low-level draw_text / text_width_px primitives are single-line
    // (draw_text drops '\n' with no Y advance; text_width_px stops at
    // the first '\n'), so multi-line layout has to happen here — a
    // label whose localized string contains newlines would otherwise
    // draw every line overlapping on one baseline and mis-align.
    //
    // Count lines first to vertically center the whole block in the
    // rect (matches the single-line centering when there's one line).
    auto count_lines = [](std::string_view s) {
        u32 n = 1;
        for (char c : s) if (c == '\n') ++n;
        return n;
    };
    const u32 line_count = count_lines(rendered);
    const f32 block_h = static_cast<f32>(line_count) * line_height;
    f32 line_top = rect.y + (rect.h - block_h) * 0.5f;

    std::string_view rest{rendered};
    for (;;) {
        const size_t nl = rest.find('\n');
        std::string_view line = (nl == std::string_view::npos)
            ? rest : rest.substr(0, nl);

        f32 x_left = rect.x;
        if (align != Align::Left) {
            f32 w = r.text_width_px(line, px_size);
            if (align == Align::Center) x_left = rect.x + (rect.w - w) * 0.5f;
            else                        x_left = rect.x + (rect.w - w);  // Right
        }
        r.draw_text(x_left, line_top + ascent, line, color, px_size);
        line_top += line_height;

        if (nl == std::string_view::npos) break;
        rest.remove_prefix(nl + 1);
    }

    Node::draw(r);
}

void Bar::draw(HudRenderInterface& r) const {
    if (!visible) return;
    if (!is_owned_by(r.hud().local_player())) return;
    r.draw_rect(rect, bg_color);

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
        r.draw_rect(fr, fill_color);
    }
    Node::draw(r);
}

void Image::draw(HudRenderInterface& r) const {
    if (!visible) return;
    if (!is_owned_by(r.hud().local_player())) return;
    if (!source.empty()) {
        r.draw_image(rect, source, tint);
    }
    Node::draw(r);
}

void Button::draw(HudRenderInterface& r) const {
    if (!visible) return;
    if (!is_owned_by(r.hud().local_player())) return;
    Color c = m_pressed ? bg_press : (m_hovered ? bg_hover : bg);
    r.draw_rect(rect, c);
    Node::draw(r);
}

bool Button::on_release(bool over) {
    bool clicked = m_pressed && over && enabled;
    m_pressed = false;
    if (clicked && on_click_cb) on_click_cb();
    return clicked;
}

} // namespace uldum::hud
