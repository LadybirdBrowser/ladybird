/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/DedicatedWorkerGlobalScopeGlobalMixin.h>
#include <LibWeb/Bindings/WorkerGlobalScopePrototype.h>
#include <LibWeb/Export.h>
#include <LibWeb/HTML/WorkerGlobalScope.h>

namespace Web::HTML {

class WEB_API DedicatedWorkerGlobalScope
    : public WorkerGlobalScope
    , public Bindings::DedicatedWorkerGlobalScopeGlobalMixin {
    WEB_PLATFORM_OBJECT(DedicatedWorkerGlobalScope, WorkerGlobalScope);
    GC_DECLARE_ALLOCATOR(DedicatedWorkerGlobalScope);

public:
    virtual ~DedicatedWorkerGlobalScope() override;

    WebIDL::ExceptionOr<void> post_message(JS::Value message, StructuredSerializeOptions const&);
    WebIDL::ExceptionOr<void> post_message(JS::Value message, Vector<GC::Root<JS::Object>> const& transfer);

    void close();

    WebIDL::CallbackType* onmessage();
    void set_onmessage(WebIDL::CallbackType* callback);

    WebIDL::CallbackType* onmessageerror();
    void set_onmessageerror(WebIDL::CallbackType* callback);

    virtual void finalize() override;

private:
    DedicatedWorkerGlobalScope(JS::Realm&, GC::Ref<Web::Page>);

    virtual void initialize_web_interfaces_impl() override;
};

}
