/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <AK/Variant.h>
#include <LibIPC/Forward.h>
#include <LibWeb/Export.h>
#include <LibWeb/HTML/CrossProcessId.h>
#include <LibWeb/HTML/UserNavigationInvolvement.h>

namespace Web {

enum class HistoryTraversalPrecheck : u8 {
    Needed,
    AlreadyDone,
};

struct PushHistoryOperationParameters {
    HTML::CrossProcessId navigable_id;
    HTML::UserNavigationInvolvement user_involvement;
};

struct ReplaceHistoryOperationParameters {
    HTML::CrossProcessId navigable_id;
    HTML::UserNavigationInvolvement user_involvement;
};

struct ReloadHistoryOperationParameters {
    HTML::CrossProcessId navigable_id;
    HTML::UserNavigationInvolvement user_involvement;
};

struct TraverseByDeltaHistoryOperationParameters {
    HTML::CrossProcessId traversable_id;
    i32 delta;
    HTML::UserNavigationInvolvement user_involvement;
};

struct TraverseToStepHistoryOperationParameters {
    HTML::CrossProcessId traversable_id;
    i32 target_step;
    HTML::UserNavigationInvolvement user_involvement;
};

struct NavigationAPITraverseHistoryOperationParameters {
    HTML::CrossProcessId navigable_id;
    Utf16String key;
    HTML::UserNavigationInvolvement user_involvement;
};

struct ResumeTraverseHistoryOperationParameters {
    HTML::CrossProcessId navigable_id;
    i32 target_step;
    HTML::UserNavigationInvolvement user_involvement;
};

struct NavigableCreationHistoryOperationParameters {
    HTML::CrossProcessId navigable_id;
};

// A WebContent-initiated operation with value-shaped parameters. The request boundary retains any process-local
// state under a private initiation ID; the queue and coordinator remain local until the later ownership switch.
using HistoryOperationParameters = Variant<
    PushHistoryOperationParameters,
    ReplaceHistoryOperationParameters,
    ReloadHistoryOperationParameters,
    TraverseByDeltaHistoryOperationParameters,
    TraverseToStepHistoryOperationParameters,
    NavigationAPITraverseHistoryOperationParameters,
    ResumeTraverseHistoryOperationParameters,
    NavigableCreationHistoryOperationParameters>;

}

namespace IPC {

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::PushHistoryOperationParameters const&);
template<>
WEB_API ErrorOr<Web::PushHistoryOperationParameters> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::ReplaceHistoryOperationParameters const&);
template<>
WEB_API ErrorOr<Web::ReplaceHistoryOperationParameters> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::ReloadHistoryOperationParameters const&);
template<>
WEB_API ErrorOr<Web::ReloadHistoryOperationParameters> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::TraverseByDeltaHistoryOperationParameters const&);
template<>
WEB_API ErrorOr<Web::TraverseByDeltaHistoryOperationParameters> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::TraverseToStepHistoryOperationParameters const&);
template<>
WEB_API ErrorOr<Web::TraverseToStepHistoryOperationParameters> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::NavigationAPITraverseHistoryOperationParameters const&);
template<>
WEB_API ErrorOr<Web::NavigationAPITraverseHistoryOperationParameters> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::ResumeTraverseHistoryOperationParameters const&);
template<>
WEB_API ErrorOr<Web::ResumeTraverseHistoryOperationParameters> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::NavigableCreationHistoryOperationParameters const&);
template<>
WEB_API ErrorOr<Web::NavigableCreationHistoryOperationParameters> decode(Decoder&);

}
