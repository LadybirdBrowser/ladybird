/*
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Forward.h>
#include <LibWeb/Bindings/Path2D.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/Export.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/canvas.html#path2d
// The C++ side of a Path2D, which is implemented in Rust (Libraries/LibWeb/Bindings/Rust/src/html): what the 2D
// contexts fill, stroke and clip with.
class WEB_API Path2D final : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(Path2D, path2d)

public:
    // The path, which changes only when script calls a method of the Path2D.
    Gfx::Path const& path() const;
};

}
