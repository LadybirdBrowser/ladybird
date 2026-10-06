/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

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

static RefPtr<LoaderConfigSnapshot const>& loader_config()
{
    static auto& loader_config = *new RefPtr<LoaderConfigSnapshot const>(default_loader_config());
    return loader_config;
}

NonnullRefPtr<LoaderConfigSnapshot const> current_loader_config()
{
    return *loader_config();
}

void update_loader_config(Function<void(LoaderConfig&)> const& change)
{
    auto config = loader_config()->config;
    change(config);
    VERIFY(!config.preferred_languages.is_empty());
    loader_config() = make_ref_counted<LoaderConfigSnapshot>(move(config));
}

void set_site_compatibility_data(SiteCompatibilityData data)
{
    update_loader_config([&](auto& config) {
        config.site_compatibility = make_ref_counted<SharedSiteCompatibilityData>(move(data));
    });
}

}
