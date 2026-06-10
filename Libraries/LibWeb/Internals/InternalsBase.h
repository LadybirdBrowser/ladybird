/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::Internals {

class WEB_API InternalsBase : public Bindings::Wrappable {
    WEB_NON_IDL_WRAPPABLE(InternalsBase, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(InternalsBase);

public:
    virtual ~InternalsBase() override;

protected:
    explicit InternalsBase(HTML::Window&);

    HTML::Window& window() const;
    Page& page() const;

    virtual void visit_edges(GC::Cell::Visitor&) override;

private:
    GC::Ref<HTML::Window> m_window;
};

}
