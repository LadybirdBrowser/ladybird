/*
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::ServiceWorker {

// https://w3c.github.io/ServiceWorker/#cachestorage-interface
class CacheStorage : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(CacheStorage, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(CacheStorage);

public:
    GC::Ref<WebIDL::Promise> open(String const& cache_name);

private:
    explicit CacheStorage(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
};

}
