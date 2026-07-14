#include "hud/hud_network.h"
#include "hud/hud.h"
#include "hud/text_tag.h"
#include "i18n/locale.h"
#include "network/protocol.h"

namespace uldum::hud {

bool apply_network_message(Hud& hud, std::span<const u8> data) {
    if (data.empty()) return false;
    auto type = network::peek_type(data);

    switch (type) {
    case network::MsgType::S_HUD_CREATE_NODE: {
        network::ByteReader r(data);
        r.read_u8();
        std::string template_id = r.read_string();
        std::string anchor      = r.read_string();
        f32 x = r.read_f32(), y = r.read_f32(), w = r.read_f32(), h = r.read_f32();
        Hud::Placement pl{};
        pl.anchor       = anchor;
        pl.x = x; pl.y = y; pl.w = w; pl.h = h;
        // Server already routed by player mask — anything that arrives is
        // for us. Mark broadcast so the render filter accepts it
        // regardless of which slot this client is on.
        pl.players_mask = UINT32_MAX;
        hud.instantiate_template(template_id, pl);
        return true;
    }
    case network::MsgType::S_HUD_DESTROY_NODE: {
        network::ByteReader r(data);
        r.read_u8();
        std::string id = r.read_string();
        hud.remove_node_by_id(id);
        return true;
    }
    case network::MsgType::S_HUD_SET_LABEL_TEXT: {
        network::ByteReader r(data);
        r.read_u8();
        std::string id = r.read_string();
        i18n::LocalizedString loc;
        loc.key = r.read_string();
        u8 n = r.read_u8();
        loc.args.reserve(n);
        for (u8 i = 0; i < n; ++i) {
            std::string k = r.read_string();
            std::string v = r.read_string();
            loc.args.emplace_back(std::move(k), std::move(v));
        }
        hud.set_label_text(id, std::move(loc));
        return true;
    }
    case network::MsgType::S_HUD_SET_BAR_FILL: {
        network::ByteReader r(data);
        r.read_u8();
        std::string id = r.read_string();
        f32 fill       = r.read_f32();
        hud.set_bar_fill(id, fill);
        return true;
    }
    case network::MsgType::S_HUD_SET_NODE_VISIBLE: {
        network::ByteReader r(data);
        r.read_u8();
        std::string id = r.read_string();
        bool visible   = r.read_bool();
        hud.set_node_visible(id, visible);
        return true;
    }
    case network::MsgType::S_HUD_SET_IMAGE_SOURCE: {
        network::ByteReader r(data);
        r.read_u8();
        std::string id   = r.read_string();
        std::string path = r.read_string();
        hud.set_image_source(id, path);
        return true;
    }
    case network::MsgType::S_HUD_SET_BUTTON_ENABLED: {
        network::ByteReader r(data);
        r.read_u8();
        std::string id = r.read_string();
        bool enabled   = r.read_bool();
        hud.set_button_enabled(id, enabled);
        return true;
    }
    case network::MsgType::S_HUD_CREATE_TEXT_TAG: {
        network::ByteReader r(data);
        r.read_u8();
        TextTagCreateInfo info{};
        // See network::build_hud_create_text_tag for the wire layout:
        // localized key + args followed by the rest of the tag's physical state.
        info.text.key = r.read_string();
        u8 n = r.read_u8();
        info.text.args.reserve(n);
        for (u8 i = 0; i < n; ++i) {
            std::string k = r.read_string();
            std::string v = r.read_string();
            info.text.args.emplace_back(std::move(k), std::move(v));
        }
        info.px_size      = r.read_f32();
        info.pos.x        = r.read_f32();
        info.pos.y        = r.read_f32();
        info.pos.z        = r.read_f32();
        info.unit.id  = r.read_u32();
        info.z_offset = r.read_f32();
        info.color.rgba   = r.read_u32();
        info.velocity_x   = r.read_f32();
        info.velocity_y   = r.read_f32();
        info.lifespan     = r.read_f32();
        info.fadepoint    = r.read_f32();
        info.players_mask = UINT32_MAX;  // server routed here, so it's for us
        hud.create_text_tag(info);
        return true;
    }
    case network::MsgType::S_HUD_DISPLAY_MESSAGE: {
        network::ByteReader r(data);
        r.read_u8();
        i18n::LocalizedString loc;
        loc.key = r.read_string();
        u8 n = r.read_u8();
        loc.args.reserve(n);
        for (u8 i = 0; i < n; ++i) {
            std::string k = r.read_string();
            std::string v = r.read_string();
            loc.args.emplace_back(std::move(k), std::move(v));
        }
        f32 duration = r.read_f32();
        hud.display_message(std::move(loc), duration);
        return true;
    }
    default:
        return false;
    }
}

} // namespace uldum::hud
