/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

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

// A WebContent-initiated operation with value-shaped parameters. The request boundary retains any process-local
// state under a private initiation ID; the queue and coordinator remain local until the later ownership switch.
using HistoryOperationParameters = Variant<PushHistoryOperationParameters, ReplaceHistoryOperationParameters, ReloadHistoryOperationParameters>;

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

}
