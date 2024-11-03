/*
 * Copyright (c) 2020-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefCounted.h>
#include <LibJS/Heap/Handle.h>
#include <LibWeb/DOM/AbortSignal.h>
#include <LibWeb/WebIDL/CallbackType.h>

namespace Web::DOM {

// NOTE: Even though these dictionaries are defined in EventTarget.idl, they are here to prevent a circular include between EventTarget.h and AbortSignal.h.
struct EventListenerOptions {
    bool capture { false };
};

struct AddEventListenerOptions : public EventListenerOptions {
    bool passive { false };
    bool once { false };
    JS::GCPtr<AbortSignal> signal;
};

class IDLEventListener final : public JS::Object {
    JS_OBJECT(IDLEventListener, JS::Object);
    JS_DECLARE_ALLOCATOR(IDLEventListener);

public:
    [[nodiscard]] static JS::NonnullGCPtr<IDLEventListener> create(JS::Realm&, JS::NonnullGCPtr<WebIDL::CallbackType>);
    IDLEventListener(JS::Realm&, JS::NonnullGCPtr<WebIDL::CallbackType>);

    virtual ~IDLEventListener() = default;

    WebIDL::CallbackType& callback() { return *m_callback; }

private:
    virtual void visit_edges(Cell::Visitor&) override;

    JS::NonnullGCPtr<WebIDL::CallbackType> m_callback;
};

}
