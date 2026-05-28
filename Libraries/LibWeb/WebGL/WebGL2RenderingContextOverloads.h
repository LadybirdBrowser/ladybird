/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2024-2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebGL/WebGL2RenderingContextImpl.h>
#include <LibWeb/WebIDL/Buffers.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::WebGL {

using namespace Web::HTML;

class WebGL2RenderingContextOverloads : public WebGL2RenderingContextImpl {
    WEB_NON_IDL_PLATFORM_OBJECT(WebGL2RenderingContextOverloads, WebGL2RenderingContextImpl);

public:
    WebGL2RenderingContextOverloads(JS::Realm&, NonnullOwnPtr<OpenGLContext>);

    void buffer_data(WebIDL::UnsignedLong target, WebIDL::LongLong size, WebIDL::UnsignedLong usage);
    void buffer_data(WebIDL::UnsignedLong target, WebIDL::NullableBufferSourceVariant src_data, WebIDL::UnsignedLong usage);
    void buffer_sub_data(WebIDL::UnsignedLong target, WebIDL::LongLong dst_byte_offset, WebIDL::BufferSource src_data);
    void buffer_data(WebIDL::UnsignedLong target, WebIDL::ArrayBufferView src_data, WebIDL::UnsignedLong usage, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong length);
    void buffer_sub_data(WebIDL::UnsignedLong target, WebIDL::LongLong dst_byte_offset, WebIDL::ArrayBufferView src_data, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong length);
    void tex_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long internalformat, WebIDL::Long width, WebIDL::Long height, WebIDL::Long border, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, WebIDL::NullableArrayBufferViewVariant pixels);
    void tex_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long internalformat, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, TexImageSource source);
    void tex_sub_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long xoffset, WebIDL::Long yoffset, WebIDL::Long width, WebIDL::Long height, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, WebIDL::NullableArrayBufferViewVariant pixels);
    void tex_sub_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long xoffset, WebIDL::Long yoffset, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, TexImageSource source);
    void tex_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long internalformat, WebIDL::Long width, WebIDL::Long height, WebIDL::Long border, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, TexImageSource source);
    void tex_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long internalformat, WebIDL::Long width, WebIDL::Long height, WebIDL::Long border, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, WebIDL::ArrayBufferView src_data, WebIDL::UnsignedLongLong src_offset);
    void tex_sub_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long xoffset, WebIDL::Long yoffset, WebIDL::Long width, WebIDL::Long height, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, TexImageSource source);
    void tex_sub_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long xoffset, WebIDL::Long yoffset, WebIDL::Long width, WebIDL::Long height, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, WebIDL::ArrayBufferView src_data, WebIDL::UnsignedLongLong src_offset);
    void compressed_tex_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::UnsignedLong internalformat, WebIDL::Long width, WebIDL::Long height, WebIDL::Long border, WebIDL::ArrayBufferView src_data, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length_override);
    void compressed_tex_sub_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long xoffset, WebIDL::Long yoffset, WebIDL::Long width, WebIDL::Long height, WebIDL::UnsignedLong format, WebIDL::ArrayBufferView src_data, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length_override);
    void uniform1fv(GC::Ptr<WebGLUniformLocation> location, Float32List v, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length);
    void uniform2fv(GC::Ptr<WebGLUniformLocation> location, Float32List v, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length);
    void uniform3fv(GC::Ptr<WebGLUniformLocation> location, Float32List v, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length);
    void uniform4fv(GC::Ptr<WebGLUniformLocation> location, Float32List v, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length);
    void uniform1iv(GC::Ptr<WebGLUniformLocation> location, Int32List v, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length);
    void uniform2iv(GC::Ptr<WebGLUniformLocation> location, Int32List v, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length);
    void uniform3iv(GC::Ptr<WebGLUniformLocation> location, Int32List v, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length);
    void uniform4iv(GC::Ptr<WebGLUniformLocation> location, Int32List v, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length);
    void uniform_matrix2fv(GC::Ptr<WebGLUniformLocation> location, bool transpose, Float32List data, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length);
    void uniform_matrix3fv(GC::Ptr<WebGLUniformLocation> location, bool transpose, Float32List data, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length);
    void uniform_matrix4fv(GC::Ptr<WebGLUniformLocation> location, bool transpose, Float32List data, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length);
    void read_pixels(WebIDL::Long x, WebIDL::Long y, WebIDL::Long width, WebIDL::Long height, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, WebIDL::NullableArrayBufferViewVariant pixels);
    void read_pixels(WebIDL::Long x, WebIDL::Long y, WebIDL::Long width, WebIDL::Long height, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, WebIDL::LongLong offset);
};

}
