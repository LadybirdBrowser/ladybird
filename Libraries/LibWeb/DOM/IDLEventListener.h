/*
 * Copyright (c) 2020-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024, Glenn Skrzypczak <glenn.skrzypczak@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefCounted.h>
#include <LibGC/Root.h>
#include <LibWeb/Bindings/EventTarget.h>
#include <LibWeb/DOM/AbortSignal.h>
#include <LibWeb/WebIDL/CallbackType.h>

namespace Web::DOM {

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
