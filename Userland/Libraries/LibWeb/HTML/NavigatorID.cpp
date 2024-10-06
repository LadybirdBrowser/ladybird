/*
 * Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <LibWeb/HTML/NavigatorID.h>
#include <LibWeb/Loader/ResourceLoader.h>
#include <LibWeb/Loader/UserAgent.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/system-state.html#dom-navigator-appversion
String NavigatorIDMixin::app_version() const
{
    auto navigator_compatibility_mode = ResourceLoader::the().navigator_compatibility_mode();

    // Must return the appropriate string that starts with "5.0 (", as follows:

    // Let trail be the substring of default `User-Agent` value that follows the "Mozilla/" prefix.
    auto user_agent_string = ResourceLoader::the().user_agent();
    auto trail = MUST(user_agent_string.substring_from_byte_offset(strlen("Mozilla/"), user_agent_string.bytes().size() - strlen("Mozilla/")));

    // If the navigator compatibility mode is Chrome or WebKit
    if (navigator_compatibility_mode == NavigatorCompatibilityMode::Chrome || navigator_compatibility_mode == NavigatorCompatibilityMode::WebKit) {
        // Return trail.
        return trail;
    }

    // If the navigator compatibility mode is Gecko
    if (navigator_compatibility_mode == NavigatorCompatibilityMode::Gecko) {
        // If trail starts with "5.0 (Windows", then return "5.0 (Windows)".
        if (trail.starts_with_bytes("5.0 (Windows"sv, CaseSensitivity::CaseSensitive))
            return "5.0 (Windows)"_string;

        // Otherwise, return the prefix of trail up to but not including the first U+003B (;), concatenated with the
        // character U+0029 RIGHT PARENTHESIS. For example, "5.0 (Macintosh)", "5.0 (Android 10)", or "5.0 (X11)".
        if (auto index = trail.find_byte_offset(';'); index.has_value()) {
            StringBuilder output;
            output.append(MUST(trail.substring_from_byte_offset(0, *index)));
            output.append(')');
            return MUST(output.to_string());
        }
        return trail;
    }

    VERIFY_NOT_REACHED();
}

// https://html.spec.whatwg.org/multipage/system-state.html#dom-navigator-platform
String NavigatorIDMixin::platform() const
{
    // Must return a string representing the platform on which the browser is executing (e.g. "MacIntel", "Win32",
    // "Linux x86_64", "Linux armv81") or, for privacy and compatibility, a string that is commonly returned on another
    // platform.

    // FIXME: Use some portion of the user agent string to make spoofing work 100%
    return ResourceLoader::the().platform();
}

// https://html.spec.whatwg.org/multipage/system-state.html#dom-navigator-productsub
String NavigatorIDMixin::product_sub() const
{
    auto navigator_compatibility_mode = ResourceLoader::the().navigator_compatibility_mode();

    // Must return the appropriate string from the following list:

    // If the navigator compatibility mode is Chrome or WebKit
    if (navigator_compatibility_mode == NavigatorCompatibilityMode::Chrome || navigator_compatibility_mode == NavigatorCompatibilityMode::WebKit) {
        // The string "20030107".
        return "20030107"_string;
    }

    // If the navigator compatibility mode is Gecko
    if (navigator_compatibility_mode == NavigatorCompatibilityMode::Gecko) {
        // The string "20100101".
        return "20100101"_string;
    }

    VERIFY_NOT_REACHED();
}

// https://html.spec.whatwg.org/multipage/system-state.html#dom-navigator-useragent
String NavigatorIDMixin::user_agent() const
{
    // Must return the default `User-Agent` value.
    return ResourceLoader::the().user_agent();
}

// https://html.spec.whatwg.org/multipage/system-state.html#dom-navigator-vendor
String NavigatorIDMixin::vendor() const
{
    auto navigator_compatibility_mode = ResourceLoader::the().navigator_compatibility_mode();

    // Must return the appropriate string from the following list:

    // If the navigator compatibility mode is Chrome
    if (navigator_compatibility_mode == NavigatorCompatibilityMode::Chrome) {
        // The string "Google Inc.".
        return "Google Inc."_string;
    }

    // If the navigator compatibility mode is Gecko
    if (navigator_compatibility_mode == NavigatorCompatibilityMode::Gecko) {
        // The empty string.
        return ""_string;
    }

    // If the navigator compatibility mode is WebKit
    if (navigator_compatibility_mode == NavigatorCompatibilityMode::WebKit) {
        // The string "Apple Computer, Inc.".
        return "Apple Computer, Inc."_string;
    }

    VERIFY_NOT_REACHED();
}

}
