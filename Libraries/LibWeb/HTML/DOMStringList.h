/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <LibWeb/Bindings/DOMStringList.h>
#include <LibWeb/Bindings/RustInterface.h>

namespace Web::HTML {

class DOMStringList final : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(DOMStringList, dom_string_list)

public:
    static GC::Ref<DOMStringList> create(Vector<Utf16String>);
};

}
