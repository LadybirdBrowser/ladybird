/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWebCommon/HTML/SessionHistoryEntryIdentity.h>

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::HTML::SessionHistoryEntryIdentity const& identity)
{
    TRY(encoder.encode(identity.document_state_id));
    TRY(encoder.encode(identity.navigation_api_id));
    return {};
}

template<>
ErrorOr<Web::HTML::SessionHistoryEntryIdentity> IPC::decode(Decoder& decoder)
{
    return Web::HTML::SessionHistoryEntryIdentity {
        .document_state_id = TRY(decoder.decode<Web::HTML::CrossProcessId>()),
        .navigation_api_id = TRY(decoder.decode<Utf16String>()),
    };
}
