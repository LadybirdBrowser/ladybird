/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/SiteIsolation.h>

namespace WebView {

static SiteIsolationMode s_site_isolation_mode = SiteIsolationMode::TopLevel;

Optional<SiteIsolationMode> site_isolation_mode_from_string(StringView mode)
{
    if (mode.equals_ignoring_ascii_case("disable"sv) || mode.equals_ignoring_ascii_case("disabled"sv))
        return SiteIsolationMode::Disabled;
    if (mode.equals_ignoring_ascii_case("top-level"sv))
        return SiteIsolationMode::TopLevel;
    if (mode.equals_ignoring_ascii_case("iframe"sv) || mode.equals_ignoring_ascii_case("iframes"sv))
        return SiteIsolationMode::IFrame;
    return {};
}

SiteIsolationMode site_isolation_mode()
{
    return s_site_isolation_mode;
}

void set_site_isolation_mode(SiteIsolationMode mode)
{
    s_site_isolation_mode = mode;
}

}
