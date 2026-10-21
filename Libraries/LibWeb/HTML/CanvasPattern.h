/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <LibWeb/Bindings/CanvasPattern.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/Export.h>
#include <LibWeb/HTML/Canvas/CanvasDrawImage.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/canvas.html#canvaspattern
// The C++ side of a CanvasPattern, which is implemented in Rust (Libraries/LibWeb/Bindings/Rust/src/html): what the
// 2D contexts create and paint with.
class WEB_API CanvasPattern final : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(CanvasPattern, canvas_pattern)

public:
    static WebIDL::ExceptionOr<GC::Ptr<CanvasPattern>> create(CanvasImageSource const& image, Utf16FlyString const& repetition);
};

}
