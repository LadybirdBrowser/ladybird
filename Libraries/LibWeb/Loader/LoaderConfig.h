/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/NonnullRefPtr.h>
#include <LibWeb/Export.h>
#include <LibWebCommon/Loader/LoaderConfigSnapshot.h>

namespace Web {

// The process's loader configuration, read and changed on the main thread. A snapshot already taken never changes.
WEB_API NonnullRefPtr<LoaderConfigSnapshot const> current_loader_config();
WEB_API void update_loader_config(Function<void(LoaderConfig&)> const& change);
WEB_API void set_site_compatibility_data(SiteCompatibilityData);

}
