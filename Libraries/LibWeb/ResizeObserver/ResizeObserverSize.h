/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/ResizeObserver.h>
#include <LibWeb/Bindings/ResizeObserverSize.h>
#include <LibWeb/Bindings/Wrappable.h>

namespace Web::ResizeObserver {

// https://drafts.csswg.org/resize-observer-1/#resizeobserversize
class ResizeObserverSize : public Bindings::Wrappable {
    WEB_WRAPPABLE(ResizeObserverSize, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(ResizeObserverSize);

public:
    struct RawSize {
        double inline_size { 0 };
        double block_size { 0 };
    };

    static RawSize compute_box_size(DOM::Element& target, Bindings::ResizeObserverBoxOptions observed_box);
    static GC::Ref<ResizeObserverSize> calculate_box_size(DOM::Element& target, Bindings::ResizeObserverBoxOptions observed_box);

    double inline_size() const { return m_inline_size; }
    void set_inline_size(double inline_size) { m_inline_size = inline_size; }

    double block_size() const { return m_block_size; }
    void set_block_size(double block_size) { m_block_size = block_size; }

    bool equals(RawSize const& other) const;
    bool equals(ResizeObserverSize const& other) const;

private:
    ResizeObserverSize()
        : Bindings::Wrappable()
    {
    }

    double m_inline_size { 0 };
    double m_block_size { 0 };
};

}
