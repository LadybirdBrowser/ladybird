/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Forward.h>

namespace Web::WebGL::Extensions {

class EXTBlendMinMax : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(EXTBlendMinMax, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(EXTBlendMinMax);

public:
    static JS::ThrowCompletionOr<GC::Ptr<EXTBlendMinMax>> create(JS::Realm&, GC::Ref<WebGLRenderingContext>);

protected:
    void initialize(JS::Realm&) override;
    void visit_edges(Visitor&) override;

private:
    EXTBlendMinMax(JS::Realm&, GC::Ref<WebGLRenderingContext>);

    GC::Ref<WebGLRenderingContext> m_context;
};

}
