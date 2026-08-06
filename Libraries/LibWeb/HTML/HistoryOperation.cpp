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

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::ReloadHistoryOperationParameters const& parameters)
{
    TRY(encoder.encode(parameters.navigable_id));
    TRY(encoder.encode(parameters.user_involvement));
    return {};
}

template<>
ErrorOr<Web::ReloadHistoryOperationParameters> IPC::decode(Decoder& decoder)
{
    return Web::ReloadHistoryOperationParameters {
        .navigable_id = TRY(decoder.decode<Web::HTML::CrossProcessId>()),
        .user_involvement = TRY(decoder.decode<Web::HTML::UserNavigationInvolvement>()),
    };
}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::TraverseByDeltaHistoryOperationParameters const& parameters)
{
    TRY(encoder.encode(parameters.traversable_id));
    TRY(encoder.encode(parameters.delta));
    TRY(encoder.encode(parameters.user_involvement));
    return {};
}

template<>
ErrorOr<Web::TraverseByDeltaHistoryOperationParameters> IPC::decode(Decoder& decoder)
{
    return Web::TraverseByDeltaHistoryOperationParameters {
        .traversable_id = TRY(decoder.decode<Web::HTML::CrossProcessId>()),
        .delta = TRY(decoder.decode<i32>()),
        .user_involvement = TRY(decoder.decode<Web::HTML::UserNavigationInvolvement>()),
    };
}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::TraverseToStepHistoryOperationParameters const& parameters)
{
    TRY(encoder.encode(parameters.traversable_id));
    TRY(encoder.encode(parameters.target_step));
    TRY(encoder.encode(parameters.user_involvement));
    return {};
}

template<>
ErrorOr<Web::TraverseToStepHistoryOperationParameters> IPC::decode(Decoder& decoder)
{
    return Web::TraverseToStepHistoryOperationParameters {
        .traversable_id = TRY(decoder.decode<Web::HTML::CrossProcessId>()),
        .target_step = TRY(decoder.decode<i32>()),
        .user_involvement = TRY(decoder.decode<Web::HTML::UserNavigationInvolvement>()),
    };
}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::NavigationAPITraverseHistoryOperationParameters const& parameters)
{
    TRY(encoder.encode(parameters.navigable_id));
    TRY(encoder.encode(parameters.key));
    TRY(encoder.encode(parameters.user_involvement));
    return {};
}

template<>
ErrorOr<Web::NavigationAPITraverseHistoryOperationParameters> IPC::decode(Decoder& decoder)
{
    return Web::NavigationAPITraverseHistoryOperationParameters {
        .navigable_id = TRY(decoder.decode<Web::HTML::CrossProcessId>()),
        .key = TRY(decoder.decode<Utf16String>()),
        .user_involvement = TRY(decoder.decode<Web::HTML::UserNavigationInvolvement>()),
    };
}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::ResumeTraverseHistoryOperationParameters const& parameters)
{
    TRY(encoder.encode(parameters.navigable_id));
    TRY(encoder.encode(parameters.target_step));
    TRY(encoder.encode(parameters.user_involvement));
    return {};
}

template<>
ErrorOr<Web::ResumeTraverseHistoryOperationParameters> IPC::decode(Decoder& decoder)
{
    return Web::ResumeTraverseHistoryOperationParameters {
        .navigable_id = TRY(decoder.decode<Web::HTML::CrossProcessId>()),
        .target_step = TRY(decoder.decode<i32>()),
        .user_involvement = TRY(decoder.decode<Web::HTML::UserNavigationInvolvement>()),
    };
}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::NavigableCreationHistoryOperationParameters const& parameters)
{
    return encoder.encode(parameters.navigable_id);
}

template<>
ErrorOr<Web::NavigableCreationHistoryOperationParameters> IPC::decode(Decoder& decoder)
{
    return Web::NavigableCreationHistoryOperationParameters {
        .navigable_id = TRY(decoder.decode<Web::HTML::CrossProcessId>()),
    };
}
