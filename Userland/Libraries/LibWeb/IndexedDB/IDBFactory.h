/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2024, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/IndexedDB/IDBOpenDBRequest.h>

namespace Web::IndexedDB {

// https://w3c.github.io/IndexedDB/#idbfactory
class IDBFactory : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(IDBFactory, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(IDBFactory);

public:
    virtual ~IDBFactory() override;

    WebIDL::ExceptionOr<JS::NonnullGCPtr<IDBOpenDBRequest>> open(String const& name, Optional<u64> version);

protected:
    explicit IDBFactory(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
};

}
