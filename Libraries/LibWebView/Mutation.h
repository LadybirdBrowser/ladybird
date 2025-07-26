/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibIPC/Forward.h>
#include <LibWeb/Forward.h>
#include <LibWebView/Forward.h>

namespace WebView {

struct WEBVIEW_API AttributeMutation {
    String attribute_name;
    Optional<String> new_value;
};

struct WEBVIEW_API CharacterDataMutation {
    String new_value;
};

struct WEBVIEW_API ChildListMutation {
    Vector<Web::UniqueNodeID> added;
    Vector<Web::UniqueNodeID> removed;
    size_t target_child_count { 0 };
};

struct WEBVIEW_API Mutation {
    using Type = Variant<AttributeMutation, CharacterDataMutation, ChildListMutation>;

    String type;
    Web::UniqueNodeID target { 0 };
    String serialized_target;
    Type mutation;
};

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder&, WebView::AttributeMutation const&);

template<>
ErrorOr<WebView::AttributeMutation> decode(Decoder&);

template<>
ErrorOr<void> encode(Encoder&, WebView::CharacterDataMutation const&);

template<>
ErrorOr<WebView::CharacterDataMutation> decode(Decoder&);

template<>
ErrorOr<void> encode(Encoder&, WebView::ChildListMutation const&);

template<>
ErrorOr<WebView::ChildListMutation> decode(Decoder&);

template<>
WEBVIEW_API ErrorOr<void> encode(Encoder&, WebView::Mutation const&);

template<>
WEBVIEW_API ErrorOr<WebView::Mutation> decode(Decoder&);

}
