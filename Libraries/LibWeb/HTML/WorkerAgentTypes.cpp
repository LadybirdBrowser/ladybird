/*
 * Copyright (c) 2026, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWeb/HTML/WorkerAgentTypes.h>

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Web::HTML::WorkerAgentStartRequest const& request)
{
    TRY(encoder.encode(request.url));
    TRY(encoder.encode(request.agent_type));
    TRY(encoder.encode(request.type));
    TRY(encoder.encode(request.credentials));
    TRY(encoder.encode(request.name));
    TRY(encoder.encode(request.extended_lifetime));
    TRY(encoder.encode(request.outside_port));
    TRY(encoder.encode(request.outside_settings));
    TRY(encoder.encode(request.storage_key));
    TRY(encoder.encode(request.caller_is_secure_context));
    TRY(encoder.encode(request.owner_token));
    return {};
}

template<>
ErrorOr<Web::HTML::WorkerAgentStartRequest> decode(Decoder& decoder)
{
    return Web::HTML::WorkerAgentStartRequest {
        .url = TRY(decoder.decode<URL::URL>()),
        .agent_type = TRY(decoder.decode<Web::Bindings::AgentType>()),
        .type = TRY(decoder.decode<Web::Bindings::WorkerType>()),
        .credentials = TRY(decoder.decode<Web::Bindings::RequestCredentials>()),
        .name = TRY(decoder.decode<String>()),
        .extended_lifetime = TRY(decoder.decode<bool>()),
        .outside_port = TRY(decoder.decode<Web::HTML::TransferDataEncoder>()),
        .outside_settings = TRY(decoder.decode<Web::HTML::SerializedEnvironmentSettingsObject>()),
        .storage_key = TRY(decoder.decode<Web::StorageAPI::StorageKey>()),
        .caller_is_secure_context = TRY(decoder.decode<bool>()),
        .owner_token = TRY(decoder.decode<Web::HTML::WorkerAgentOwnerToken>()),
    };
}

}
