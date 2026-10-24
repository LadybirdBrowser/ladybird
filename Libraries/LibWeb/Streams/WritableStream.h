/*
 * Copyright (c) 2023, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/Bindings/WritableStream.h>
#include <LibWeb/Export.h>

namespace Web::Streams {

// https://streams.spec.whatwg.org/#writablestream
// The C++ side of a WritableStream, which is implemented in Rust (Libraries/LibWeb/Bindings/Rust/src/streams).
class WEB_API WritableStream : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(WritableStream, writable_stream)
};

}
