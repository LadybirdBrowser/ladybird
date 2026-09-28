/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/Utf16FlyString.h>
#include <AK/Utf16String.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibIPC/Forward.h>
#include <LibWebCommon/Export.h>
#include <LibWebCommon/Forward.h>

namespace WebView {

struct WEBCOMMON_API AttributeMutation {
    Utf16FlyString attribute_name;
    Optional<Utf16String> new_value;
};

struct WEBCOMMON_API CharacterDataMutation {
    String new_value;
};

struct WEBCOMMON_API ChildListMutation {
    Vector<Compositing::UniqueNodeID> added;
    Vector<Compositing::UniqueNodeID> removed;
    size_t target_child_count { 0 };
};

struct WEBCOMMON_API Mutation {
    using Type = Variant<AttributeMutation, CharacterDataMutation, ChildListMutation>;

    String type;
    Compositing::UniqueNodeID target { 0 };
    String serialized_target;
    Type mutation;
};

}

namespace IPC {

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, WebView::AttributeMutation const&);

template<>
WEBCOMMON_API ErrorOr<WebView::AttributeMutation> decode(Decoder&);

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, WebView::CharacterDataMutation const&);

template<>
WEBCOMMON_API ErrorOr<WebView::CharacterDataMutation> decode(Decoder&);

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, WebView::ChildListMutation const&);

template<>
WEBCOMMON_API ErrorOr<WebView::ChildListMutation> decode(Decoder&);

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, WebView::Mutation const&);

template<>
WEBCOMMON_API ErrorOr<WebView::Mutation> decode(Decoder&);

}
