/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringBuilder.h>
#include <LibWebView/NotificationFreedesktopArguments.h>

namespace WebView {

static constexpr i32 expire_timeout_default = -1;
static constexpr i32 expire_timeout_never = 0;
static constexpr i32 expire_timeout_without_notification_center_ms = 25000;
static constexpr u8 urgency_normal = 1;
static constexpr u8 urgency_critical = 2;

static String escape_markup(String const& text)
{
    StringBuilder builder;
    for (auto code_point : text.code_points()) {
        switch (code_point) {
        case '&':
            builder.append("&amp;"sv);
            break;
        case '<':
            builder.append("&lt;"sv);
            break;
        case '>':
            builder.append("&gt;"sv);
            break;
        default:
            builder.append_code_point(code_point);
        }
    }
    return MUST(builder.to_string());
}

static FreedesktopImageData image_data_for(Gfx::Bitmap const& bitmap)
{
    FreedesktopImageData image;
    image.width = bitmap.width();
    image.height = bitmap.height();
    image.rowstride = bitmap.width() * 4;
    image.pixels = MUST(ByteBuffer::create_uninitialized(static_cast<size_t>(image.rowstride) * image.height));
    size_t offset = 0;
    for (int y = 0; y < bitmap.height(); ++y) {
        for (int x = 0; x < bitmap.width(); ++x) {
            // The daemon wants straight RGBA; a premultiplied bitmap's channels are scaled back up by their alpha.
            auto color = bitmap.get_pixel(x, y);
            auto alpha = color.alpha();
            auto straight = [&](u8 channel) -> u8 {
                if (bitmap.alpha_type() != Gfx::AlphaType::Premultiplied || alpha == 0 || alpha == 255)
                    return channel;
                return static_cast<u8>(min(255, (channel * 255 + alpha / 2) / alpha));
            };
            image.pixels[offset++] = straight(color.red());
            image.pixels[offset++] = straight(color.green());
            image.pixels[offset++] = straight(color.blue());
            image.pixels[offset++] = alpha;
        }
    }
    return image;
}

FreedesktopNotifyArguments build_notify_arguments(PlatformNotification const& notification, FreedesktopCapabilities const& capabilities, u32 replaces_daemon_id)
{
    FreedesktopNotifyArguments arguments;
    arguments.app_name = "Ladybird"_string;
    arguments.replaces_id = replaces_daemon_id;
    arguments.app_icon = "org.ladybird.Ladybird"_string;
    arguments.summary = notification.data.title.to_utf8();

    if (capabilities.body) {
        auto attribution = notification.attribution;
        auto body = notification.data.body.to_utf8();
        if (capabilities.body_markup) {
            attribution = escape_markup(attribution);
            body = escape_markup(body);
        }
        arguments.body = MUST(String::formatted("{}\n\n{}", attribution, body));
    }

    // "default" makes the whole notification clickable where the daemon shows actions as buttons.
    if (capabilities.actions)
        arguments.actions = { "default"_string, "Activate"_string };

    bool sticky = notification.data.require_interaction;
    arguments.hints.append({ "urgency"_string, sticky ? urgency_critical : urgency_normal });
    if (notification.data.silent == true)
        arguments.hints.append({ "suppress-sound"_string, true });
    arguments.hints.append({ "desktop-entry"_string, "org.ladybird.Ladybird"_string });
    if (notification.icon)
        arguments.hints.append({ "image-data"_string, image_data_for(*notification.icon) });

    if (sticky)
        arguments.expire_timeout = expire_timeout_never;
    else if (capabilities.persistence)
        arguments.expire_timeout = expire_timeout_default;
    else
        arguments.expire_timeout = expire_timeout_without_notification_center_ms;

    return arguments;
}

}
