/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2024-2025, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/PromiseCapability.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/IndexedDB/IDBOpenDBRequest.h>

namespace Web::IndexedDB {

// https://w3c.github.io/IndexedDB/#idbfactory
class IDBFactory : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(IDBFactory, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(IDBFactory);

public:
    virtual ~IDBFactory() override;

    WebIDL::ExceptionOr<GC::Ref<IDBOpenDBRequest>> open(String const& name, Optional<u64> version);
    WebIDL::ExceptionOr<GC::Ref<IDBOpenDBRequest>> delete_database(String const& name);
    GC::Ref<WebIDL::Promise> databases();

    WebIDL::ExceptionOr<i8> cmp(JS::Value first, JS::Value second);

protected:
    explicit IDBFactory(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
};

}
