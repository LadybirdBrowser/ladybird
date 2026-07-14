/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/EnumBits.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <AK/Utf16FlyString.h>
#include <AK/WeakPtr.h>
#include <AK/Weakable.h>
#include <LibGC/Cell.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/Forward.h>
#include <LibWeb/PixelUnits.h>

namespace Web {

struct ChromeMetrics;

}

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

    virtual bool contains(CSSPixelPoint, ChromeMetrics const&) const = 0;
    virtual MouseAction handle_pointer_event(Utf16FlyString const& type, unsigned button, CSSPixelPoint visual_viewport_position) = 0;
    virtual void mouse_enter() = 0;
    virtual void mouse_leave() = 0;

    virtual Optional<CSS::CursorPredefined> cursor() const { return {}; }

protected:
    explicit ChromeWidget(Paintable&);

    RefPtr<Paintable> paintable() const;

private:
    friend class Paintable;

    void detach_from_paintable(Badge<Paintable>);
    virtual void did_detach_from_paintable() { }

    WeakPtr<Paintable> m_paintable;
};

}
