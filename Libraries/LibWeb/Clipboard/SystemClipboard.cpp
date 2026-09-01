/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWeb/Clipboard/SystemClipboard.h>

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::Clipboard::SystemClipboardRepresentation const& output)
{
    TRY(encoder.encode(output.name));
    TRY(encoder.encode(output.data));
    return {};
}

template<>
ErrorOr<Web::Clipboard::SystemClipboardRepresentation> IPC::decode(Decoder& decoder)
{
    auto name = TRY(decoder.decode<String>());
    auto data = TRY(decoder.decode<ByteString>());

    return Web::Clipboard::SystemClipboardRepresentation { move(name), move(data) };
}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::Clipboard::SystemClipboardItem const& output)
{
    TRY(encoder.encode(output.system_clipboard_representations));
    return {};
}

template<>
ErrorOr<Web::Clipboard::SystemClipboardItem> IPC::decode(Decoder& decoder)
{
    auto system_clipboard_representation = TRY(decoder.decode<Vector<Web::Clipboard::SystemClipboardRepresentation>>());

    return Web::Clipboard::SystemClipboardItem { move(system_clipboard_representation) };
}
