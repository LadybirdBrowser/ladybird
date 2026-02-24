/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWeb/HTML/Scripting/SerializedEnvironmentSettingsObject.h>

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Web::HTML::SerializedWindow const& window)
{
    TRY(encoder.encode(window.associated_document.url));
    TRY(encoder.encode(window.associated_document.relevant_settings_object_is_secure_context));

    return {};
}

template<>
ErrorOr<Web::HTML::SerializedWindow> decode(Decoder& decoder)
{
    return Web::HTML::SerializedWindow {
        .associated_document {
            .url = TRY(decoder.decode<URL::URL>()),
            .relevant_settings_object_is_secure_context = TRY(decoder.decode<bool>()),
        },
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::HTML::SerializedWorkerGlobalScope const& worker_global_scope)
{
    TRY(encoder.encode(worker_global_scope.relevant_settings_object_is_secure_context));
    return {};
}

template<>
ErrorOr<Web::HTML::SerializedWorkerGlobalScope> decode(Decoder& decoder)
{
    return Web::HTML::SerializedWorkerGlobalScope {
        .relevant_settings_object_is_secure_context = TRY(decoder.decode<bool>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::HTML::SerializedEnvironmentSettingsObject const& object)
{
    TRY(encoder.encode(object.id));
    TRY(encoder.encode(object.creation_url));
    TRY(encoder.encode(object.top_level_creation_url));
    TRY(encoder.encode(object.top_level_origin));
    TRY(encoder.encode(object.api_base_url));
    TRY(encoder.encode(object.origin));
    TRY(encoder.encode(object.has_cross_site_ancestor));
    TRY(encoder.encode(object.policy_container));
    TRY(encoder.encode(object.cross_origin_isolated_capability));
    TRY(encoder.encode(object.time_origin));
    TRY(encoder.encode(object.global));

    return {};
}

template<>
ErrorOr<Web::HTML::SerializedEnvironmentSettingsObject> decode(Decoder& decoder)
{
    return Web::HTML::SerializedEnvironmentSettingsObject {
        .id = TRY(decoder.decode<String>()),
        .creation_url = TRY(decoder.decode<URL::URL>()),
        .top_level_creation_url = TRY(decoder.decode<Optional<URL::URL>>()),
        .top_level_origin = TRY(decoder.decode<Optional<URL::Origin>>()),
        .api_base_url = TRY(decoder.decode<URL::URL>()),
        .origin = TRY(decoder.decode<URL::Origin>()),
        .has_cross_site_ancestor = TRY(decoder.decode<bool>()),
        .policy_container = TRY(decoder.decode<Web::HTML::SerializedPolicyContainer>()),
        .cross_origin_isolated_capability = TRY(decoder.decode<Web::HTML::CanUseCrossOriginIsolatedAPIs>()),
        .time_origin = TRY(decoder.decode<double>()),
        .global = TRY(decoder.decode<Web::HTML::SerializedGlobal>()),
    };
}

}
