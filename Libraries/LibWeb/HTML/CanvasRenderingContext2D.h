/*
 * Copyright (c) 2020-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/CanvasRenderingContext2D.h>
#include <LibWeb/HTML/Canvas/Canvas2DContextBase.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/canvas.html#canvasrenderingcontext2d
class WEB_API CanvasRenderingContext2D final : public Canvas2DContextBase {
    WEB_RUST_INTERFACE(CanvasRenderingContext2D, canvas_rendering_context2d)

public:
    static GC::Ref<CanvasRenderingContext2D> create(HTMLCanvasElement&, CanvasRenderingContext2DSettings const&);
};

}
