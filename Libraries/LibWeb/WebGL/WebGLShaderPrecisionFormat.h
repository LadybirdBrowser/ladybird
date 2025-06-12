/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/WebGL/Types.h>

namespace Web::WebGL {

class WebGLShaderPrecisionFormat final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(WebGLShaderPrecisionFormat, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(WebGLShaderPrecisionFormat);

public:
    static GC::Ref<WebGLShaderPrecisionFormat> create(JS::Realm& realm, GLint range_min, GLint range_max, GLint precision);

    virtual ~WebGLShaderPrecisionFormat() override;

    GLint range_min() const { return m_range_min; }
    GLint range_max() const { return m_range_max; }
    GLint precision() const { return m_precision; }

protected:
    explicit WebGLShaderPrecisionFormat(JS::Realm&, GLint range_min, GLint range_max, GLint precision);

    virtual void initialize(JS::Realm&) override;

private:
    GLint m_range_min { 0 };
    GLint m_range_max { 0 };
    GLint m_precision { 0 };
};

}
