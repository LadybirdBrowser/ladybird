/*
 * Copyright (c) 2023, Bastiaan van der Plaat <bastiaan.v.d.plaat@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "SelectItem.h"
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::HTML::SelectItemOption const& item)
{
    TRY(encoder.encode(item.id));
    TRY(encoder.encode(item.label));
    TRY(encoder.encode(item.value));
    TRY(encoder.encode(item.selected));
    TRY(encoder.encode(item.disabled));
    return {};
}

template<>
ErrorOr<Web::HTML::SelectItemOption> IPC::decode(Decoder& decoder)
{
    auto id = TRY(decoder.decode<u32>());
    auto label = TRY(decoder.decode<String>());
    auto value = TRY(decoder.decode<String>());
    auto selected = TRY(decoder.decode<bool>());
    auto disabled = TRY(decoder.decode<bool>());
    return Web::HTML::SelectItemOption { id, move(label), move(value), selected, disabled };
}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::HTML::SelectItemOptionGroup const& item)
{
    TRY(encoder.encode(item.label));
    TRY(encoder.encode(item.items));
    return {};
}

template<>
ErrorOr<Web::HTML::SelectItemOptionGroup> IPC::decode(Decoder& decoder)
{
    auto label = TRY(decoder.decode<String>());
    auto items = TRY(decoder.decode<Vector<Web::HTML::SelectItemOption>>());
    return Web::HTML::SelectItemOptionGroup { move(label), move(items) };
}

template<>
ErrorOr<void> IPC::encode(Encoder&, Web::HTML::SelectItemSeparator const&)
{
    return {};
}

template<>
ErrorOr<Web::HTML::SelectItemSeparator> IPC::decode(Decoder&)
{
    return Web::HTML::SelectItemSeparator {};
}
