/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibIPC/Forward.h>

namespace Web::Clipboard {

// https://w3c.github.io/clipboard-apis/#system-clipboard-representation
struct SystemClipboardRepresentation {
    ByteString data;
    String mime_type;
};

// https://w3c.github.io/clipboard-apis/#system-clipboard-item
struct SystemClipboardItem {
    Vector<SystemClipboardRepresentation> system_clipboard_representations;
};

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder&, Web::Clipboard::SystemClipboardRepresentation const&);

template<>
ErrorOr<Web::Clipboard::SystemClipboardRepresentation> decode(Decoder&);

template<>
ErrorOr<void> encode(Encoder&, Web::Clipboard::SystemClipboardItem const&);

template<>
ErrorOr<Web::Clipboard::SystemClipboardItem> decode(Decoder&);

}
