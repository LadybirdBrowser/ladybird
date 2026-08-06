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
    TRY(encoder.encode(parameters.initiator_to_check));
    TRY(encoder.encode(parameters.user_involvement));
    return {};
}

template<>
ErrorOr<Web::TraverseByDeltaHistoryOperationParameters> IPC::decode(Decoder& decoder)
{
    return Web::TraverseByDeltaHistoryOperationParameters {
        .traversable_id = TRY(decoder.decode<Web::HTML::CrossProcessId>()),
        .delta = TRY(decoder.decode<i32>()),
        .initiator_to_check = TRY(decoder.decode<Optional<Web::HTML::CrossProcessId>>()),
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

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::NavigableDestructionHistoryOperationParameters const& parameters)
{
    return encoder.encode(parameters.traversable_id);
}

template<>
ErrorOr<Web::NavigableDestructionHistoryOperationParameters> IPC::decode(Decoder& decoder)
{
    return Web::NavigableDestructionHistoryOperationParameters {
        .traversable_id = TRY(decoder.decode<Web::HTML::CrossProcessId>()),
    };
}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::FinalizeSameDocumentNavigationHistoryOperationParameters const& parameters)
{
    TRY(encoder.encode(parameters.navigable_id));
    TRY(encoder.encode(parameters.target_entry));
    TRY(encoder.encode(parameters.replaces_current_entry));
    TRY(encoder.encode(parameters.history_handling));
    TRY(encoder.encode(parameters.user_involvement));
    return {};
}

template<>
ErrorOr<Web::FinalizeSameDocumentNavigationHistoryOperationParameters> IPC::decode(Decoder& decoder)
{
    return Web::FinalizeSameDocumentNavigationHistoryOperationParameters {
        .navigable_id = TRY(decoder.decode<Web::HTML::CrossProcessId>()),
        .target_entry = TRY(decoder.decode<Web::HTML::SameDocumentNavigationEntry>()),
        .replaces_current_entry = TRY(decoder.decode<bool>()),
        .history_handling = TRY(decoder.decode<Web::HTML::HistoryHandlingBehavior>()),
        .user_involvement = TRY(decoder.decode<Web::HTML::UserNavigationInvolvement>()),
    };
}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::CloseTopLevelTraversableHistoryOperationParameters const& parameters)
{
    return encoder.encode(parameters.traversable_id);
}

template<>
ErrorOr<Web::CloseTopLevelTraversableHistoryOperationParameters> IPC::decode(Decoder& decoder)
{
    return Web::CloseTopLevelTraversableHistoryOperationParameters {
        .traversable_id = TRY(decoder.decode<Web::HTML::CrossProcessId>()),
    };
}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::ResetSessionHistoryForTestingOperationParameters const& parameters)
{
    return encoder.encode(parameters.traversable_id);
}

template<>
ErrorOr<Web::ResetSessionHistoryForTestingOperationParameters> IPC::decode(Decoder& decoder)
{
    return Web::ResetSessionHistoryForTestingOperationParameters {
        .traversable_id = TRY(decoder.decode<Web::HTML::CrossProcessId>()),
    };
}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::FlushSessionHistoryTraversalQueueOperationParameters const& parameters)
{
    return encoder.encode(parameters.traversable_id);
}

template<>
ErrorOr<Web::FlushSessionHistoryTraversalQueueOperationParameters> IPC::decode(Decoder& decoder)
{
    return Web::FlushSessionHistoryTraversalQueueOperationParameters {
        .traversable_id = TRY(decoder.decode<Web::HTML::CrossProcessId>()),
    };
}
