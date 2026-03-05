/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Forward.h>

namespace Web::WebGL::Extensions {

class EXTTextureNorm16 : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(EXTTextureNorm16, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(EXTTextureNorm16);

public:
    static JS::ThrowCompletionOr<GC::Ref<JS::Object>> create(JS::Realm&, GC::Ref<WebGLRenderingContextBase>);

protected:
    void initialize(JS::Realm&) override;
    void visit_edges(Visitor&) override;

private:
    EXTTextureNorm16(JS::Realm&, GC::Ref<WebGLRenderingContextBase>);

    GC::Ref<WebGLRenderingContextBase> m_context;
};

}
