/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <LibIPC/Forward.h>
#include <LibURL/Origin.h>
#include <LibURL/URL.h>
#include <LibWeb/Export.h>

namespace Web::HTML {

struct ReplicatedNavigableState {
    Utf16String target_name;
    URL::URL active_document_url;
    URL::Origin active_document_origin;
    bool active_document_is_fully_active { false };
};

}

namespace IPC {

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::HTML::ReplicatedNavigableState const&);
template<>
WEB_API ErrorOr<Web::HTML::ReplicatedNavigableState> decode(Decoder&);

}
