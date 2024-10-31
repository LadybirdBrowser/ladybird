/*
 * Copyright (c) 2022, the SerenityOS developers.
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <LibJS/Heap/GCPtr.h>
#include <LibJS/Heap/HeapFunction.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::HTML {

class AnimationFrameCallbackDriver final : public JS::Cell {
    JS_CELL(AnimationFrameCallbackDriver, JS::Cell);
    JS_DECLARE_ALLOCATOR(AnimationFrameCallbackDriver);

    using Callback = JS::NonnullGCPtr<JS::HeapFunction<void(double)>>;

public:
    [[nodiscard]] WebIDL::UnsignedLong add(Callback handler);
    bool remove(WebIDL::UnsignedLong);
    bool has_callbacks() const;
    void run(double now);

private:
    virtual void visit_edges(Cell::Visitor&) override;

    // https://html.spec.whatwg.org/multipage/imagebitmap-and-animations.html#animation-frame-callback-identifier
    WebIDL::UnsignedLong m_animation_frame_callback_identifier { 0 };

    OrderedHashMap<WebIDL::UnsignedLong, Callback> m_callbacks;
    OrderedHashMap<WebIDL::UnsignedLong, Callback> m_executing_callbacks;
};

}
