/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/MutexProtected.h>
#include <AK/NeverDestroyed.h>
#include <AK/RefPtr.h>
#include <LibWeb/Loader/LoaderConfig.h>
#include <LibWebCommon/Loader/UserAgent.h>

namespace Web {

static NonnullRefPtr<LoaderConfigSnapshot const> default_loader_config()
{
    LoaderConfig config {
        .user_agent = MUST(String::from_utf8(default_user_agent)),
        .site_compatibility = make_ref_counted<SharedSiteCompatibilityData>(SiteCompatibilityData {}),
        .preferred_languages = default_preferred_languages(),
    };
    return make_ref_counted<LoaderConfigSnapshot>(move(config));
}

static MutexProtected<NonnullRefPtr<LoaderConfigSnapshot const>>& loader_config()
{
    static NeverDestroyed<MutexProtected<NonnullRefPtr<LoaderConfigSnapshot const>>> loader_config(default_loader_config());
    return *loader_config;
}

NonnullRefPtr<LoaderConfigSnapshot const> current_loader_config()
{
    return loader_config().with_locked([](auto const& snapshot) { return snapshot; });
}

void update_loader_config(Function<void(LoaderConfig&)> const& change)
{
    loader_config().with_locked([&](auto& snapshot) {
        auto config = snapshot->config;
        change(config);
        VERIFY(!config.preferred_languages.is_empty());
        snapshot = make_ref_counted<LoaderConfigSnapshot>(move(config));
    });
}

void set_site_compatibility_data(SiteCompatibilityData data)
{
    update_loader_config([&](auto& config) {
        config.site_compatibility = make_ref_counted<SharedSiteCompatibilityData>(move(data));
    });
}

}
