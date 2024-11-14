/*
 * Copyright (c) 2020-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefCounted.h>
#include <LibGC/Root.h>
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
    GC::Ptr<AbortSignal> signal;
};

class IDLEventListener final : public JS::Object {
    JS_OBJECT(IDLEventListener, JS::Object);
    GC_DECLARE_ALLOCATOR(IDLEventListener);

public:
    [[nodiscard]] static GC::Ref<IDLEventListener> create(JS::Realm&, GC::Ref<WebIDL::CallbackType>);
    IDLEventListener(JS::Realm&, GC::Ref<WebIDL::CallbackType>);

    virtual ~IDLEventListener() = default;

    WebIDL::CallbackType& callback() { return *m_callback; }

private:
    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ref<WebIDL::CallbackType> m_callback;
};

}
