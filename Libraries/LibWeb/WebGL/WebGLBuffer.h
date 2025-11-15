/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 * Copyright (c) 2025, Undefine <undefine@undefine.pl>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/WebGL/Types.h>
#include <LibWeb/WebGL/WebGLObject.h>

namespace Web::WebGL {

class WebGLBuffer final : public WebGLObject {
    WEB_PLATFORM_OBJECT(WebGLBuffer, WebGLObject);
    GC_DECLARE_ALLOCATOR(WebGLBuffer);

public:
    static GC::Ref<WebGLBuffer> create(JS::Realm& realm, WebGLRenderingContextBase& context, GLuint handle);

    virtual ~WebGLBuffer();

    void mark_as_index_buffer(bool value) { m_is_index_buffer = value ? TriState::True : TriState::False; }
    TriState is_index_buffer() const { return m_is_index_buffer; }

protected:
    explicit WebGLBuffer(JS::Realm&, WebGLRenderingContextBase&, GLuint handle);

    virtual void initialize(JS::Realm&) override;

private:
    TriState m_is_index_buffer { TriState::Unknown };
};

}
