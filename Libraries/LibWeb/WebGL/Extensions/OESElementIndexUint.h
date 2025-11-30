/*
 * Copyright (c) 2025, Undefine <undefine@undefine.pl>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Forward.h>

namespace Web::WebGL::Extensions {

class OESElementIndexUint : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(OESElementIndexUint, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(OESElementIndexUint);

public:
    static JS::ThrowCompletionOr<GC::Ptr<OESElementIndexUint>> create(JS::Realm&, GC::Ref<WebGLRenderingContext>);

protected:
    void initialize(JS::Realm&) override;
    void visit_edges(Visitor&) override;

private:
    OESElementIndexUint(JS::Realm&, GC::Ref<WebGLRenderingContext>);

    GC::Ref<WebGLRenderingContext> m_context;
};

}
