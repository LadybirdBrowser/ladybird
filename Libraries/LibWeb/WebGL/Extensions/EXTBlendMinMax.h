/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/EXTBlendMinMax.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Forward.h>

namespace Web::WebGL {

class EXTBlendMinMax : public Bindings::Wrappable {
    WEB_WRAPPABLE(EXTBlendMinMax, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(EXTBlendMinMax);

public:
    static JS::ThrowCompletionOr<GC::Ref<Bindings::Wrappable>> create(JS::Realm&, GC::Ref<WebGLRenderingContextBase>);

protected:
    void visit_edges(GC::Cell::Visitor&) override;

private:
    EXTBlendMinMax(JS::Realm&, GC::Ref<WebGLRenderingContextBase>);

    GC::Ref<WebGLRenderingContextBase> m_context;
};

}
