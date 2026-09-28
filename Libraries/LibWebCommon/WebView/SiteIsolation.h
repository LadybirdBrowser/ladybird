/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/StringView.h>
#include <LibWebCommon/Export.h>
#include <LibWebCommon/Forward.h>

namespace WebView {

enum class SiteIsolationMode {
    Disabled,
    TopLevel,
    IFrame,
};

[[nodiscard]] WEBCOMMON_API Optional<SiteIsolationMode> site_isolation_mode_from_string(StringView);
[[nodiscard]] WEBCOMMON_API SiteIsolationMode site_isolation_mode();
WEBCOMMON_API void set_site_isolation_mode(SiteIsolationMode);

}
