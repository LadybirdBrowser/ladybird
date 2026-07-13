/*
 * Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <AK/Utf16String.h>

namespace Web::HTML {

class NavigatorIDMixin {
public:
    // WARNING: Any information in this API that varies from user to user can be used to profile the user. In fact, if
    // enough such information is available, a user can actually be uniquely identified. For this reason, user agent
    // implementers are strongly urged to include as little information in this API as possible.

    // https://html.spec.whatwg.org/multipage/system-state.html#dom-navigator-appcodename
    Utf16FlyString app_code_name() const { return "Mozilla"_utf16_fly_string; }

    // https://html.spec.whatwg.org/multipage/system-state.html#dom-navigator-appcodename
    Utf16FlyString app_name() const { return "Netscape"_utf16_fly_string; }

    // https://html.spec.whatwg.org/multipage/system-state.html#dom-navigator-appversion
    Utf16String app_version() const;

    // https://html.spec.whatwg.org/multipage/system-state.html#dom-navigator-platform
    Utf16String platform() const;

    // https://html.spec.whatwg.org/multipage/system-state.html#dom-navigator-product
    Utf16FlyString product() const { return "Gecko"_utf16_fly_string; }

    // https://html.spec.whatwg.org/multipage/system-state.html#dom-navigator-productsub
    Utf16FlyString product_sub() const;

    // https://html.spec.whatwg.org/multipage/system-state.html#dom-navigator-useragent
    Utf16String user_agent() const;

    // https://html.spec.whatwg.org/multipage/system-state.html#dom-navigator-vendor
    Utf16FlyString vendor() const;

    // https://html.spec.whatwg.org/multipage/system-state.html#dom-navigator-vendorsub
    Utf16FlyString vendor_sub() const { return ""_utf16_fly_string; }

    // FIXME: If the navigator compatibility mode is Gecko, then the user agent must also support the following partial interface:
    //       bool taint_enabled()
    //       ByteString oscpu()
};

}
