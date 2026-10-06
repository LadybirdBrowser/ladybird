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
#include <LibURL/URL.h>
#include <LibWebCommon/Export.h>
#include <LibWebCommon/Loader/SiteCompatibility.h>

namespace Web {

// Site compatibility data, whose URL patterns may only be matched on one thread.
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

    String user_agent_for_url(URL::URL const& url) const { return site_compatibility->data.user_agent_for_url(url, user_agent); }
    String user_agent_for_websocket_url(URL::URL const& url) const { return site_compatibility->data.user_agent_for_websocket_url(url, user_agent); }
    bool exposes_experimental_interface(URL::URL const& url, StringView name) const { return site_compatibility->data.exposes_experimental_interface(url, name); }
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
