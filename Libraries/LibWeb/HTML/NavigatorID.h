/*
 * Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>

namespace Web::HTML {

class NavigatorIDMixin {
public:
    // WARNING: Any information in this API that varies from user to user can be used to profile the user. In fact, if
    // enough such information is available, a user can actually be uniquely identified. For this reason, user agent
    // implementers are strongly urged to include as little information in this API as possible.

    // https://html.spec.whatwg.org/multipage/system-state.html#dom-navigator-appcodename
    String app_code_name() const { return "Mozilla"_string; }

    // https://html.spec.whatwg.org/multipage/system-state.html#dom-navigator-appcodename
    String app_name() const { return "Netscape"_string; }

    // https://html.spec.whatwg.org/multipage/system-state.html#dom-navigator-appversion
    String app_version() const;

    // https://html.spec.whatwg.org/multipage/system-state.html#dom-navigator-platform
    String platform() const;

    // https://html.spec.whatwg.org/multipage/system-state.html#dom-navigator-product
    String product() const { return "Gecko"_string; }

    // https://html.spec.whatwg.org/multipage/system-state.html#dom-navigator-productsub
    String product_sub() const;

    // https://html.spec.whatwg.org/multipage/system-state.html#dom-navigator-useragent
    String user_agent() const;

    // https://html.spec.whatwg.org/multipage/system-state.html#dom-navigator-vendor
    String vendor() const;

    // https://html.spec.whatwg.org/multipage/system-state.html#dom-navigator-vendorsub
    String vendor_sub() const { return String {}; }

    // FIXME: If the navigator compatibility mode is Gecko, then the user agent must also support the following partial interface:
    //       bool taint_enabled()
    //       ByteString oscpu()
};

}
