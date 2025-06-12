/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWeb/HTML/Numbers.h>
#include <LibWeb/HTML/WebViewHints.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/PixelUnits.h>

namespace Web::HTML {

static void set_up_browsing_context_features(WebViewHints& target, TokenizedFeature::Map const& tokenized_features, Page const& page);

WebViewHints WebViewHints::from_tokenised_features(TokenizedFeature::Map const& tokenized_features, Page const& page)
{
    WebViewHints hints;
    hints.popup = check_if_a_popup_window_is_requested(tokenized_features) == TokenizedFeature::Popup::Yes;
    set_up_browsing_context_features(hints, tokenized_features, page);
    return hints;
}

// https://drafts.csswg.org/cssom-view/#set-up-browsing-context-features
static void set_up_browsing_context_features(WebViewHints& target, TokenizedFeature::Map const& tokenized_features, Page const& page)
{
    // 1. Let x be null.
    Optional<CSSPixels> x;

    // 2. Let y be null.
    Optional<CSSPixels> y;

    // 3. Let width be null.
    Optional<CSSPixels> width;

    // 4. Let height be null.
    Optional<CSSPixels> height;

    auto screen_rect = page.web_exposed_screen_area();

    // 5. If tokenizedFeatures["left"] exists:
    if (auto left = tokenized_features.get("left"sv); left.has_value()) {
        // 1. Set x to the result of invoking the rules for parsing integers on tokenizedFeatures["left"].
        // 2. If x is an error, set x to 0.
        x = parse_integer(*left).value_or(0);

        // 3. Optionally, clamp x in a user-agent-defined manner so that the window does not move outside the Web-exposed available screen area.
        x = min(*x, screen_rect.x());

        // 4. Optionally, move target’s window such that the window’s left edge is at the horizontal coordinate x relative
        //    to the left edge of the Web-exposed screen area, measured in CSS pixels of target. The positive axis is rightward.
        // Note: Handled in the UI process when creating the traversable navigable.
    }

    // 6. If tokenizedFeatures["top"] exists:
    if (auto top = tokenized_features.get("top"sv); top.has_value()) {
        // 1. Set y to the result of invoking the rules for parsing integers on tokenizedFeatures["top"].
        // 2. If y is an error, set y to 0.
        y = parse_integer(*top).value_or(0);

        // 3. Optionally, clamp y in a user-agent-defined manner so that the window does not move outside the Web-exposed available screen area.
        y = min(*y, screen_rect.y());

        // 4. Optionally, move target’s window such that the window’s top edge is at the vertical coordinate y relative
        //    to the top edge of the Web-exposed screen area, measured in CSS pixels of target. The positive axis is downward.
        // Note: Handled in the UI process when creating the traversable navigable.
    }

    // 7. If tokenizedFeatures["width"] exists:
    if (auto width_token = tokenized_features.get("width"sv); width_token.has_value()) {
        // 1. Set width to the result of invoking the rules for parsing integers on tokenizedFeatures["width"].
        // 2. If width is an error, set width to 0.
        width = parse_integer(*width_token).value_or(0);

        // 3. If width is not 0:
        if (width != 0) {
            // 1. Optionally, clamp width in a user-agent-defined manner so that the window does not get too small or bigger than the Web-exposed available screen area.
            width = clamp(*width, 100, screen_rect.width());

            // 2. Optionally, size target’s window by moving its right edge such that the distance between the left and right edges of the viewport are width CSS pixels of target.
            // 3. Optionally, move target’s window in a user-agent-defined manner so that it does not grow outside the Web-exposed available screen area.
            // Note: Handled in the UI process when creating the traversable navigable.
        }
    }

    // 8. If tokenizedFeatures["height"] exists:
    if (auto height_token = tokenized_features.get("height"sv); height_token.has_value()) {
        // 1. Set height to the result of invoking the rules for parsing integers on tokenizedFeatures["height"].
        // 2. If height is an error, set height to 0.
        height = parse_integer(*height_token).value_or(0);

        // 3. If height is not 0:
        if (height != 0) {
            // 1. Optionally, clamp height in a user-agent-defined manner so that the window does not get too small or bigger than the Web-exposed available screen area.
            height = clamp(*height, 100, screen_rect.height());

            // 2. Optionally, size target’s window by moving its bottom edge such that the distance between the top and bottom edges of the viewport are height CSS pixels of target.
            // 3. Optionally, move target’s window in a user-agent-defined manner so that it does not grow outside the Web-exposed available screen area.
            // Note: Handled in the UI process when creating the traversable navigable.
        }
    }

    auto scale = page.client().device_pixels_per_css_pixel();

    if (x.has_value()) {
        // Make sure we don't fly off the screen to the right
        if (x.value() + width.value_or(0) > screen_rect.width())
            x = screen_rect.width() - width.value_or(0);
        target.screen_x = x.value() / scale;
    }

    if (y.has_value()) {
        // Make sure we don't fly off the screen to the bottom
        if (y.value() + height.value_or(0) > screen_rect.height())
            y = screen_rect.height() - height.value_or(0);
        target.screen_y = y.value() / scale;
    }

    if (width.has_value()) {
        target.width = width.value() / scale;
    }

    if (height.has_value()) {
        target.height = height.value() / scale;
    }
}

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, ::Web::HTML::WebViewHints const& data_holder)
{
    TRY(encoder.encode(data_holder.popup));
    TRY(encoder.encode(data_holder.width));
    TRY(encoder.encode(data_holder.height));
    TRY(encoder.encode(data_holder.screen_x));
    TRY(encoder.encode(data_holder.screen_y));

    return {};
}

template<>
ErrorOr<::Web::HTML::WebViewHints> decode(Decoder& decoder)
{
    auto popup = TRY(decoder.decode<bool>());
    auto width = TRY(decoder.decode<Optional<Web::DevicePixels>>());
    auto height = TRY(decoder.decode<Optional<Web::DevicePixels>>());
    auto screen_x = TRY(decoder.decode<Optional<Web::DevicePixels>>());
    auto screen_y = TRY(decoder.decode<Optional<Web::DevicePixels>>());

    return ::Web::HTML::WebViewHints {
        .popup = popup,
        .width = width,
        .height = height,
        .screen_x = screen_x,
        .screen_y = screen_y,
    };
}

}
