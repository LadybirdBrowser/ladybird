/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>

namespace Web::IndexedDB {

// https://w3c.github.io/IndexedDB/#idbfactory
class IDBFactory : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(IDBFactory, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(IDBFactory);

public:
    virtual ~IDBFactory() override;

protected:
    explicit IDBFactory(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
};

}
