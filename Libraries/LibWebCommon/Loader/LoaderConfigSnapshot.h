/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/NonnullRefPtr.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibURL/Origin.h>
#include <LibWebCommon/Export.h>
#include <LibWebCommon/Loader/SiteCompatibility.h>

namespace Web {

// Site compatibility data, which the agents of a process share across their threads.
struct WEBCOMMON_API SharedSiteCompatibilityData final : public AtomicRefCounted<SharedSiteCompatibilityData> {
    explicit SharedSiteCompatibilityData(SiteCompatibilityData data)
        : data(move(data))
    {
    }
    ~SharedSiteCompatibilityData();

    SiteCompatibilityData const data;
};

// The user agent's settings that a fetch reads.
struct LoaderConfig {
    String user_agent;
    NonnullRefPtr<SharedSiteCompatibilityData const> site_compatibility;
    Vector<String> preferred_languages;
    bool enable_global_privacy_control { false };

    String user_agent_for(URL::Origin const& top_level_origin, URL::Origin const& origin) const { return site_compatibility->data.user_agent_for(top_level_origin, origin, user_agent); }
    bool exposes_experimental_interface(URL::Origin const& top_level_origin, URL::Origin const& origin, StringView name) const { return site_compatibility->data.exposes_experimental_interface(top_level_origin, origin, name); }
};

// A loader configuration as it was at one moment, which a fetch keeps for its lifetime.
struct LoaderConfigSnapshot final : public AtomicRefCounted<LoaderConfigSnapshot> {
    explicit LoaderConfigSnapshot(LoaderConfig config)
        : config(move(config))
    {
    }

    LoaderConfig const config;
};

}
