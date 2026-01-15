/*
 * Copyright (c) 2025, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Color.h>
#include <LibGfx/PaintingSurface.h>
#include <LibGfx/WindingRule.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/Canvas/CanvasSettings.h>

#pragma once

namespace Web::HTML {

class AbstractCanvasRenderingContext2DBase {
protected:
    virtual DrawingState& drawing_state() = 0;
    virtual DrawingState const& drawing_state() const = 0;

    [[nodiscard]] virtual Gfx::Painter* painter() = 0;
    virtual void did_draw(Gfx::FloatRect const&) = 0;

    virtual Gfx::Path& path() = 0;

    virtual void stroke_internal(Gfx::Path const& path) = 0;
    virtual void fill_internal(Gfx::Path const& path, Gfx::WindingRule winding_rule) = 0;
    virtual void clip_internal(Gfx::Path& path, Gfx::WindingRule winding_rule) = 0;

    virtual void set_font(StringView font) = 0;

    virtual CanvasRenderingContext2DSettings context_attributes() = 0;
    virtual bool origin_clean() const = 0;

    virtual Gfx::Color clear_color() const = 0;

    virtual JS::Realm& realm() const = 0;

    virtual RefPtr<Gfx::PaintingSurface> surface() const = 0;
};

}
