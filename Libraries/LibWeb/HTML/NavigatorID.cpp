/*
 * Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <AK/Utf16StringBuilder.h>
#include <LibWeb/HTML/NavigatorID.h>
#include <LibWeb/Loader/ResourceLoader.h>
#include <LibWeb/Loader/UserAgent.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/system-state.html#dom-navigator-appversion
Utf16String NavigatorIDMixin::app_version() const
{
    // 1. Let userAgent be this's relevant settings object's environment default `User-Agent` value.
    // FIXME: Store that on the settings object?
    auto user_agent = ResourceLoader::the().user_agent();

    // 2. If userAgent does not start with `Mozilla/5.0 (`, then return the empty string.
    if (!user_agent.starts_with_bytes("Mozilla/5.0 ("sv))
        return {};

    // 3. Let trail be the substring of userAgent, isomorphic decoded, that follows the "Mozilla/" prefix.
    auto trail = MUST(user_agent.substring_from_byte_offset("Mozilla/"sv.length()));

    // 4. -> If the navigator compatibility mode is Chrome or WebKit
    auto navigator_compatibility_mode = ResourceLoader::the().navigator_compatibility_mode();
    if (navigator_compatibility_mode == NavigatorCompatibilityMode::Chrome || navigator_compatibility_mode == NavigatorCompatibilityMode::WebKit) {
        // Return trail.
        return Utf16String::from_ascii_without_validation(trail.bytes());
    }
    //    -> If the navigator compatibility mode is Gecko
    if (navigator_compatibility_mode == NavigatorCompatibilityMode::Gecko) {
        // If trail starts with "5.0 (Windows", then return "5.0 (Windows)".
        if (trail.starts_with_bytes("5.0 (Windows"sv))
            return "5.0 (Windows)"_utf16;

        // Otherwise, return the prefix of trail up to but not including the first U+003B (;), concatenated with the
        // character U+0029 RIGHT PARENTHESIS. For example, "5.0 (Macintosh)", "5.0 (Android 10)", or "5.0 (X11)".
        if (auto index = trail.find_byte_offset(';'); index.has_value()) {
            Utf16StringBuilder output;
            output.append_ascii(MUST(trail.substring_from_byte_offset(0, *index)));
            output.append_ascii(')');
            return output.to_string();
        }
        return Utf16String::from_ascii_without_validation(trail.bytes());
    }

    VERIFY_NOT_REACHED();
}

// https://html.spec.whatwg.org/multipage/system-state.html#dom-navigator-platform
Utf16String NavigatorIDMixin::platform() const
{
    // Must return a string representing the platform on which the browser is executing (e.g. "MacIntel", "Win32",
    // "Linux x86_64", "Linux armv81") or, for privacy and compatibility, a string that is commonly returned on another
    // platform.

    // FIXME: Use some portion of the user agent string to make spoofing work 100%
    return Utf16String::from_ascii_without_validation(ResourceLoader::the().platform().bytes());
}

// https://html.spec.whatwg.org/multipage/system-state.html#dom-navigator-productsub
Utf16FlyString NavigatorIDMixin::product_sub() const
{
    auto navigator_compatibility_mode = ResourceLoader::the().navigator_compatibility_mode();

    // Must return the appropriate string from the following list:

    // If the navigator compatibility mode is Chrome or WebKit
    if (navigator_compatibility_mode == NavigatorCompatibilityMode::Chrome || navigator_compatibility_mode == NavigatorCompatibilityMode::WebKit) {
        // The string "20030107".
        return "20030107"_utf16_fly_string;
    }

    // If the navigator compatibility mode is Gecko
    if (navigator_compatibility_mode == NavigatorCompatibilityMode::Gecko) {
        // The string "20100101".
        return "20100101"_utf16_fly_string;
    }

    VERIFY_NOT_REACHED();
}

// https://html.spec.whatwg.org/multipage/system-state.html#dom-navigator-useragent
Utf16String NavigatorIDMixin::user_agent() const
{
    // Must return the default `User-Agent` value.
    return Utf16String::from_ascii_without_validation(ResourceLoader::the().user_agent().bytes());
}

// https://html.spec.whatwg.org/multipage/system-state.html#dom-navigator-vendor
Utf16FlyString NavigatorIDMixin::vendor() const
{
    auto navigator_compatibility_mode = ResourceLoader::the().navigator_compatibility_mode();

    // Must return the appropriate string from the following list:

    // If the navigator compatibility mode is Chrome
    if (navigator_compatibility_mode == NavigatorCompatibilityMode::Chrome) {
        // The string "Google Inc.".
        return "Google Inc."_utf16_fly_string;
    }

    // If the navigator compatibility mode is Gecko
    if (navigator_compatibility_mode == NavigatorCompatibilityMode::Gecko) {
        // The empty string.
        return ""_utf16_fly_string;
    }

    // If the navigator compatibility mode is WebKit
    if (navigator_compatibility_mode == NavigatorCompatibilityMode::WebKit) {
        // The string "Apple Computer, Inc.".
        return "Apple Computer, Inc."_utf16_fly_string;
    }

    VERIFY_NOT_REACHED();
}

}
