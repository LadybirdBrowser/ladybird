/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWebCommon/HTML/EmbedderPolicy.h>

namespace Web::HTML {

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Web::HTML::EmbedderPolicy const& embedder_policy)
{
    TRY(encoder.encode(embedder_policy.value));
    TRY(encoder.encode(embedder_policy.reporting_endpoint));
    TRY(encoder.encode(embedder_policy.report_only_value));
    TRY(encoder.encode(embedder_policy.report_only_reporting_endpoint));

    return {};
}

template<>
ErrorOr<Web::HTML::EmbedderPolicy> decode(Decoder& decoder)
{
    Web::HTML::EmbedderPolicy embedder_policy {};

    embedder_policy.value = TRY(decoder.decode<Web::HTML::EmbedderPolicyValue>());
    embedder_policy.reporting_endpoint = TRY(decoder.decode<Utf16String>());
    embedder_policy.report_only_value = TRY(decoder.decode<Web::HTML::EmbedderPolicyValue>());
    embedder_policy.report_only_reporting_endpoint = TRY(decoder.decode<Utf16String>());

    return embedder_policy;
}

}
