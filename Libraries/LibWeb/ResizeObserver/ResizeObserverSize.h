/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Bindings/ResizeObserverPrototype.h>

namespace Web::ResizeObserver {

// https://drafts.csswg.org/resize-observer-1/#resizeobserversize
class ResizeObserverSize : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(ResizeObserverSize, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(ResizeObserverSize);

public:
    static JS::NonnullGCPtr<ResizeObserverSize> calculate_box_size(JS::Realm& realm, DOM::Element& target, Bindings::ResizeObserverBoxOptions observed_box);

    double inline_size() const { return m_inline_size; }
    void set_inline_size(double inline_size) { m_inline_size = inline_size; }

    double block_size() const { return m_block_size; }
    void set_block_size(double block_size) { m_block_size = block_size; }

    bool equals(ResizeObserverSize const& other) const;

private:
    explicit ResizeObserverSize(JS::Realm& realm)
        : PlatformObject(realm)
    {
    }

    virtual void initialize(JS::Realm&) override;

    double m_inline_size { 0 };
    double m_block_size { 0 };
};

}
