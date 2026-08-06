/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWeb/HTML/HistoryOperation.h>

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::PushHistoryOperationParameters const& parameters)
{
    TRY(encoder.encode(parameters.navigable_id));
    TRY(encoder.encode(parameters.user_involvement));
    return {};
}

template<>
ErrorOr<Web::PushHistoryOperationParameters> IPC::decode(Decoder& decoder)
{
    return Web::PushHistoryOperationParameters {
        .navigable_id = TRY(decoder.decode<Web::HTML::CrossProcessId>()),
        .user_involvement = TRY(decoder.decode<Web::HTML::UserNavigationInvolvement>()),
    };
}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::ReplaceHistoryOperationParameters const& parameters)
{
    TRY(encoder.encode(parameters.navigable_id));
    TRY(encoder.encode(parameters.user_involvement));
    return {};
}

template<>
ErrorOr<Web::ReplaceHistoryOperationParameters> IPC::decode(Decoder& decoder)
{
    return Web::ReplaceHistoryOperationParameters {
        .navigable_id = TRY(decoder.decode<Web::HTML::CrossProcessId>()),
        .user_involvement = TRY(decoder.decode<Web::HTML::UserNavigationInvolvement>()),
    };
}
