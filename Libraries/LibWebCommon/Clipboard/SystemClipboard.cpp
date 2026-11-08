/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWebCommon/Clipboard/SystemClipboard.h>
#include <LibWebCommon/MimeSniff/MimeType.h>

namespace Web::Clipboard {

void apply_paste_mode(SystemClipboardItem& item, PasteMode paste_mode)
{
    if (paste_mode == PasteMode::AllMimeTypes)
        return;

    // INTEROP: A paste without formatting only exposes the plain text to the page, like the other engines, and so a
    //          paste event handler can't reinsert the formatted content. A copied file whose type is text/plain is
    //          still a file, so it is left out too.
    item.system_clipboard_representations.remove_all_matching([](SystemClipboardRepresentation const& representation) {
        auto mime_type = MimeSniff::MimeType::parse(representation.name);
        return !mime_type.has_value() || mime_type->essence() != "text/plain"sv || !representation.data.has<ByteString>();
    });
}

}

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
    auto data = TRY(decoder.decode<Variant<ByteString, Web::HTML::SelectedFile>>());

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
