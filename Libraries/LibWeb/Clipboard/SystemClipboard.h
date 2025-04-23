#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Vector.h>
#include <AK/String.h>


namespace Web::Clipboard {


// https://w3c.github.io/clipboard-apis/#model

struct SystemClipboardRepresentation {
    String     name;   // e.g. "text/plain"
    ByteBuffer data;   // raw bytes
};

struct SystemClipboardItem {
    Vector<SystemClipboardRepresentation> representations;
};

Vector<SystemClipboardItem> const& system_clipboard_data();

void set_system_clipboard_data(Vector<SystemClipboardItem>);

} // namespace Web::Clipboard