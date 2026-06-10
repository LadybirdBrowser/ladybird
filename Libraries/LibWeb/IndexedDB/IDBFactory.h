/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2024-2025, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/PromiseCapability.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Forward.h>
#include <LibWeb/IndexedDB/IDBOpenDBRequest.h>

namespace Web::IndexedDB {

// https://w3c.github.io/IndexedDB/#idbfactory
class IDBFactory : public Bindings::Wrappable {
    WEB_WRAPPABLE(IDBFactory, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(IDBFactory);

public:
    virtual ~IDBFactory() override;

    WebIDL::ExceptionOr<GC::Ref<IDBOpenDBRequest>> open(String const& name, Optional<u64> version);
    WebIDL::ExceptionOr<GC::Ref<IDBOpenDBRequest>> delete_database(String const& name);
    GC::Ref<WebIDL::Promise> databases(JS::Realm&);
    void databases(GC::Ref<WebIDL::Promise>);

    WebIDL::ExceptionOr<i8> cmp(JS::Value first, JS::Value second);

protected:
    explicit IDBFactory(HTML::WindowOrWorkerGlobalScopeMixin&);

    virtual void visit_edges(GC::Cell::Visitor&) override;

private:
    HTML::WindowOrWorkerGlobalScopeMixin& relevant_global_scope() const;
    JS::Object& relevant_global_object() const;
    HTML::EnvironmentSettingsObject& relevant_settings_object() const;

    GC::Ref<DOM::EventTarget> m_global;
};

}
