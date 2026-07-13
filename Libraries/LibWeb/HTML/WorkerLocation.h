/*
 * Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Export.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/workers.html#worker-locations
class WEB_API WorkerLocation : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(WorkerLocation, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(WorkerLocation);

public:
    virtual ~WorkerLocation() override;

    Utf16String href() const;
    Utf16String origin() const;
    Utf16String protocol() const;
    Utf16String host() const;
    Utf16String hostname() const;
    Utf16String port() const;
    Utf16String pathname() const;
    Utf16String search() const;
    Utf16String hash() const;

private:
    explicit WorkerLocation(WorkerGlobalScope&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ref<WorkerGlobalScope> m_global_scope;
};

}
