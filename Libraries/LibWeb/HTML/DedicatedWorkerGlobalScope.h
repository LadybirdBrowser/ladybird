/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/DedicatedWorkerGlobalScopeGlobalMixin.h>
#include <LibWeb/Bindings/WorkerGlobalScopePrototype.h>
#include <LibWeb/HTML/WorkerGlobalScope.h>

#define ENUMERATE_DEDICATED_WORKER_GLOBAL_SCOPE_EVENT_HANDLERS(E) \
    E(onmessage, HTML::EventNames::message)                       \
    E(onmessageerror, HTML::EventNames::messageerror)

namespace Web::HTML {

class DedicatedWorkerGlobalScope
    : public WorkerGlobalScope
    , public Bindings::DedicatedWorkerGlobalScopeGlobalMixin {
    WEB_PLATFORM_OBJECT(DedicatedWorkerGlobalScope, WorkerGlobalScope);
    JS_DECLARE_ALLOCATOR(DedicatedWorkerGlobalScope);

public:
    virtual ~DedicatedWorkerGlobalScope() override;

    WebIDL::ExceptionOr<void> post_message(JS::Value message, StructuredSerializeOptions const&);
    WebIDL::ExceptionOr<void> post_message(JS::Value message, Vector<JS::Handle<JS::Object>> const& transfer);

    void set_name(String name) { m_name = move(name); }
    String name() const { return m_name; }

    void close();

#undef __ENUMERATE
#define __ENUMERATE(attribute_name, event_name)       \
    void set_##attribute_name(WebIDL::CallbackType*); \
    WebIDL::CallbackType* attribute_name();
    ENUMERATE_DEDICATED_WORKER_GLOBAL_SCOPE_EVENT_HANDLERS(__ENUMERATE)
#undef __ENUMERATE

    virtual void finalize() override;

private:
    DedicatedWorkerGlobalScope(JS::Realm&, JS::NonnullGCPtr<Web::Page>);

    virtual void initialize_web_interfaces_impl() override;

    String m_name;
};

}
