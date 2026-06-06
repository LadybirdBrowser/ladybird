/*
 * Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/WorkerLocation.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Export.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/workers.html#worker-locations
class WEB_API WorkerLocation : public Bindings::Wrappable {
    WEB_WRAPPABLE(WorkerLocation, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(WorkerLocation);

public:
    virtual ~WorkerLocation() override;

    String href() const;
    String origin() const;
    String protocol() const;
    String host() const;
    String hostname() const;
    String port() const;
    String pathname() const;
    String search() const;
    String hash() const;

private:
    explicit WorkerLocation(WorkerGlobalScope&);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    GC::Ref<WorkerGlobalScope> m_global_scope;
};

}
