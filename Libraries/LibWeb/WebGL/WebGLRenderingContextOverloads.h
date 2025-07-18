/*
 * Copyright (c) 2024-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2024-2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebGL/WebGLRenderingContextImpl.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::WebGL {

using namespace Web::HTML;

class WebGLRenderingContextOverloads : public WebGLRenderingContextImpl {
    WEB_NON_IDL_PLATFORM_OBJECT(WebGLRenderingContextOverloads, WebGLRenderingContextImpl);

public:
    WebGLRenderingContextOverloads(JS::Realm&, NonnullOwnPtr<OpenGLContext>);

    void buffer_data(WebIDL::UnsignedLong target, WebIDL::LongLong size, WebIDL::UnsignedLong usage);
    void buffer_data(WebIDL::UnsignedLong target, GC::Root<WebIDL::BufferSource> data, WebIDL::UnsignedLong usage);
    void buffer_sub_data(WebIDL::UnsignedLong target, WebIDL::LongLong offset, GC::Root<WebIDL::BufferSource> data);
    void compressed_tex_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::UnsignedLong internalformat, WebIDL::Long width, WebIDL::Long height, WebIDL::Long border, GC::Root<WebIDL::ArrayBufferView> data);
    void compressed_tex_sub_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long xoffset, WebIDL::Long yoffset, WebIDL::Long width, WebIDL::Long height, WebIDL::UnsignedLong format, GC::Root<WebIDL::ArrayBufferView> data);
    void read_pixels(WebIDL::Long x, WebIDL::Long y, WebIDL::Long width, WebIDL::Long height, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, GC::Root<WebIDL::ArrayBufferView> pixels);
    void tex_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long internalformat, WebIDL::Long width, WebIDL::Long height, WebIDL::Long border, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, GC::Root<WebIDL::ArrayBufferView> pixels);
    void tex_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long internalformat, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, TexImageSource source);
    void tex_sub_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long xoffset, WebIDL::Long yoffset, WebIDL::Long width, WebIDL::Long height, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, GC::Root<WebIDL::ArrayBufferView> pixels);
    void tex_sub_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long xoffset, WebIDL::Long yoffset, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, TexImageSource source);
    void uniform1fv(GC::Root<WebGLUniformLocation> location, Float32List v);
    void uniform2fv(GC::Root<WebGLUniformLocation> location, Float32List v);
    void uniform3fv(GC::Root<WebGLUniformLocation> location, Float32List v);
    void uniform4fv(GC::Root<WebGLUniformLocation> location, Float32List v);
    void uniform1iv(GC::Root<WebGLUniformLocation> location, Int32List v);
    void uniform2iv(GC::Root<WebGLUniformLocation> location, Int32List v);
    void uniform3iv(GC::Root<WebGLUniformLocation> location, Int32List v);
    void uniform4iv(GC::Root<WebGLUniformLocation> location, Int32List v);
    void uniform_matrix2fv(GC::Root<WebGLUniformLocation> location, bool transpose, Float32List value);
    void uniform_matrix3fv(GC::Root<WebGLUniformLocation> location, bool transpose, Float32List value);
    void uniform_matrix4fv(GC::Root<WebGLUniformLocation> location, bool transpose, Float32List value);
};

}
