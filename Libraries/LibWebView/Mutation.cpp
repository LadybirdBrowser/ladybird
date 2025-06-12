/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWebView/Mutation.h>

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, WebView::AttributeMutation const& mutation)
{
    TRY(encoder.encode(mutation.attribute_name));
    TRY(encoder.encode(mutation.new_value));
    return {};
}

template<>
ErrorOr<WebView::AttributeMutation> IPC::decode(Decoder& decoder)
{
    auto attribute_name = TRY(decoder.decode<String>());
    auto new_value = TRY(decoder.decode<Optional<String>>());

    return WebView::AttributeMutation { move(attribute_name), move(new_value) };
}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, WebView::CharacterDataMutation const& mutation)
{
    TRY(encoder.encode(mutation.new_value));
    return {};
}

template<>
ErrorOr<WebView::CharacterDataMutation> IPC::decode(Decoder& decoder)
{
    auto new_value = TRY(decoder.decode<String>());

    return WebView::CharacterDataMutation { move(new_value) };
}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, WebView::ChildListMutation const& mutation)
{
    TRY(encoder.encode(mutation.added));
    TRY(encoder.encode(mutation.removed));
    TRY(encoder.encode(mutation.target_child_count));
    return {};
}

template<>
ErrorOr<WebView::ChildListMutation> IPC::decode(Decoder& decoder)
{
    auto added = TRY(decoder.decode<Vector<Web::UniqueNodeID>>());
    auto removed = TRY(decoder.decode<Vector<Web::UniqueNodeID>>());
    auto target_child_count = TRY(decoder.decode<size_t>());

    return WebView::ChildListMutation { move(added), move(removed), target_child_count };
}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, WebView::Mutation const& mutation)
{
    TRY(encoder.encode(mutation.type));
    TRY(encoder.encode(mutation.target));
    TRY(encoder.encode(mutation.serialized_target));
    TRY(encoder.encode(mutation.mutation));
    return {};
}

template<>
ErrorOr<WebView::Mutation> IPC::decode(Decoder& decoder)
{
    auto type = TRY(decoder.decode<String>());
    auto target = TRY(decoder.decode<Web::UniqueNodeID>());
    auto serialized_target = TRY(decoder.decode<String>());
    auto mutation = TRY(decoder.decode<WebView::Mutation::Type>());

    return WebView::Mutation { move(type), target, move(serialized_target), move(mutation) };
}
