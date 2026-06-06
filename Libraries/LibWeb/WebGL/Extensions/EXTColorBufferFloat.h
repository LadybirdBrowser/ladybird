/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/EXTColorBufferFloat.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Forward.h>

namespace Web::WebGL {

class EXTColorBufferFloat : public Bindings::Wrappable {
    WEB_WRAPPABLE(EXTColorBufferFloat, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(EXTColorBufferFloat);

public:
    static JS::ThrowCompletionOr<GC::Ref<Bindings::Wrappable>> create(GC::Ref<WebGLRenderingContextBase>);

protected:
    void visit_edges(GC::Cell::Visitor&) override;

private:
    EXTColorBufferFloat(GC::Ref<WebGLRenderingContextBase>);

    GC::Ref<WebGLRenderingContextBase> m_context;
};

}
