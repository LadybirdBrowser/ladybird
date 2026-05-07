/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/EnumBits.h>
#include <AK/RefCounted.h>
#include <AK/Weakable.h>
#include <LibGC/Cell.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/Forward.h>
#include <LibWeb/PixelUnits.h>

namespace Web::Painting {

enum class MouseAction : u8 {
    None = 0,
    CaptureInput = 1 << 0,
    SwallowEvent = 1 << 1,
};

AK_ENUM_BITWISE_OPERATORS(MouseAction);

class ChromeWidget
    : public RefCounted<ChromeWidget>
    , public Weakable<ChromeWidget> {
public:
    virtual ~ChromeWidget() = default;

    virtual MouseAction handle_pointer_event(FlyString const& type, unsigned button, CSSPixelPoint visual_viewport_position) = 0;
    virtual void mouse_enter() = 0;
    virtual void mouse_leave() = 0;

    virtual Optional<CSS::CursorPredefined> cursor() const { return {}; }
};

}
