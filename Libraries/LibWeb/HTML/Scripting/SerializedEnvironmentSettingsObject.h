/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <LibIPC/Forward.h>
#include <LibURL/Origin.h>
#include <LibURL/URL.h>
#include <LibWeb/Export.h>
#include <LibWeb/HTML/SerializedPolicyContainer.h>

namespace Web::HTML {

enum class CanUseCrossOriginIsolatedAPIs : u8 {
    No,
    Yes,
};

struct SerializedDocument {
    URL::URL url;
    bool relevant_settings_object_is_secure_context { false };
};

struct SerializedWindow {
    SerializedDocument associated_document;
};

struct SerializedWorkerGlobalScope {
    bool relevant_settings_object_is_secure_context { false };
};

using SerializedGlobal = Variant<SerializedWindow, SerializedWorkerGlobalScope>;

struct SerializedEnvironmentSettingsObject {
    String id;
    URL::URL creation_url;
    Optional<URL::URL> top_level_creation_url;
    Optional<URL::Origin> top_level_origin;

    URL::URL api_base_url;
    URL::Origin origin;
    bool has_cross_site_ancestor;
    SerializedPolicyContainer policy_container;
    CanUseCrossOriginIsolatedAPIs cross_origin_isolated_capability;
    double time_origin;
    SerializedGlobal global;
};

}

namespace IPC {

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::HTML::SerializedWindow const&);

template<>
WEB_API ErrorOr<Web::HTML::SerializedWindow> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::HTML::SerializedWorkerGlobalScope const&);

template<>
WEB_API ErrorOr<Web::HTML::SerializedWorkerGlobalScope> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::HTML::SerializedEnvironmentSettingsObject const&);

template<>
WEB_API ErrorOr<Web::HTML::SerializedEnvironmentSettingsObject> decode(Decoder&);

}
