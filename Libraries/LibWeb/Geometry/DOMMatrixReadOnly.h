/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2023, Bastiaan van der Plaat <bastiaan.v.d.plaat@gmail.com>
 * Copyright (c) 2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/Utf16String.h>
#include <LibWeb/Bindings/DOMMatrixReadOnly.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/Export.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Geometry {

// https://drafts.fxtf.org/geometry/#dommatrixreadonly
// The C++ side of a DOMMatrixReadOnly, which is implemented in Rust (Libraries/LibWeb/Bindings/Rust/src/geometry):
// what CSS Typed OM reads. A DOMMatrixReadOnly that C++ receives from script can be a DOMMatrix.
class WEB_API DOMMatrixReadOnly : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(DOMMatrixReadOnly, dom_matrix_read_only)

public:
    // The elements m11, m12, m13, m14, m21, ..., m44, in that order.
    Array<double, 16> elements() const;

    bool is2d() const;

    // https://drafts.fxtf.org/geometry/#dommatrixreadonly-stringification-behavior
    WebIDL::ExceptionOr<Utf16String> to_string() const;
};

}
