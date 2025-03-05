/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Forward.h>

namespace Web::WebGL::Extensions {

class EXTColorBufferFloat : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(EXTColorBufferFloat, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(EXTColorBufferFloat);

public:
    static JS::ThrowCompletionOr<GC::Ptr<EXTColorBufferFloat>> create(JS::Realm&, GC::Ref<WebGL2RenderingContext>);

protected:
    void initialize(JS::Realm&) override;
    void visit_edges(Visitor&) override;

private:
    EXTColorBufferFloat(JS::Realm&, GC::Ref<WebGL2RenderingContext>);

    GC::Ref<WebGL2RenderingContext> m_context;
};

}
