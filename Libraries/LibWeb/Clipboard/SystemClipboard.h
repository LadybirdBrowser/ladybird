/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/String.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibIPC/Forward.h>
#include <LibWeb/Export.h>
#include <LibWeb/HTML/SelectedFile.h>

namespace Web::Clipboard {

// https://w3c.github.io/clipboard-apis/#system-clipboard-representation
struct SystemClipboardRepresentation {
    String name;

    // AD-HOC: We store clipboard entries as a ByteString if the original source of the entry was either a text-based
    //         string or any entry written via navigator.clipboard.write.
    //
    //         We store them as a SelectedFile only if the user themself copied a file on their desktop. That file is
    //         never exposed to navigator.clipboard APIs, which is not specified, but generally matches the behavior of
    //         other browsers. Users may still explicitly paste files themselves.
    Variant<ByteString, HTML::SelectedFile> data;
};

// https://w3c.github.io/clipboard-apis/#system-clipboard-item
struct SystemClipboardItem {
    Vector<SystemClipboardRepresentation> system_clipboard_representations;
};

}

namespace IPC {

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::Clipboard::SystemClipboardRepresentation const&);

template<>
WEB_API ErrorOr<Web::Clipboard::SystemClipboardRepresentation> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::Clipboard::SystemClipboardItem const&);

template<>
WEB_API ErrorOr<Web::Clipboard::SystemClipboardItem> decode(Decoder&);

}
