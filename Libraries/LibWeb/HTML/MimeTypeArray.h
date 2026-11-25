/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/MimeTypeArray.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/system-state.html#mimetypearray
class MimeTypeArray final : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(MimeTypeArray, mime_type_array)

public:
    [[nodiscard]] static GC::Ref<MimeTypeArray> create(Window&);
};

}
