/*
 * Copyright (c) 2025, Undefine <undefine@undefine.pl>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/OESElementIndexUint.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Forward.h>

namespace Web::WebGL {

class OESElementIndexUint : public Bindings::Wrappable {
    WEB_WRAPPABLE(OESElementIndexUint, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(OESElementIndexUint);

public:
    static JS::ThrowCompletionOr<GC::Ref<Bindings::Wrappable>> create(GC::Ref<WebGLRenderingContextBase>);

protected:
    void visit_edges(GC::Cell::Visitor&) override;

private:
    OESElementIndexUint(GC::Ref<WebGLRenderingContextBase>);

    GC::Ref<WebGLRenderingContextBase> m_context;
};

}
