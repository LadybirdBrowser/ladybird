/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/EXTRenderSnorm.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Forward.h>

namespace Web::WebGL {

class EXTRenderSnorm : public Bindings::Wrappable {
    WEB_WRAPPABLE(EXTRenderSnorm, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(EXTRenderSnorm);

public:
    static JS::ThrowCompletionOr<GC::Ref<Bindings::Wrappable>> create(GC::Ref<WebGLRenderingContextBase>);

protected:
    void visit_edges(GC::Cell::Visitor&) override;

private:
    EXTRenderSnorm(GC::Ref<WebGLRenderingContextBase>);

    GC::Ref<WebGLRenderingContextBase> m_context;
};

}
