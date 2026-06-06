/*
 * Copyright (c) 2025, Undefine <undefine@undefine.pl>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/OESStandardDerivatives.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Forward.h>

namespace Web::WebGL {

class OESStandardDerivatives : public Bindings::Wrappable {
    WEB_WRAPPABLE(OESStandardDerivatives, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(OESStandardDerivatives);

public:
    static JS::ThrowCompletionOr<GC::Ref<Bindings::Wrappable>> create(GC::Ref<WebGLRenderingContextBase>);

protected:
    void visit_edges(GC::Cell::Visitor&) override;

private:
    OESStandardDerivatives(GC::Ref<WebGLRenderingContextBase>);

    GC::Ref<WebGLRenderingContextBase> m_context;
};

}
