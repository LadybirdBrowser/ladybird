/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2024-2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebGL/Types.h>
#include <LibWeb/WebGL/WebGLRenderingContextImpl.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::WebGL {

using namespace Web::HTML;

class WebGL2RenderingContextImpl : public WebGLRenderingContextImpl {
    WEB_NON_IDL_PLATFORM_OBJECT(WebGL2RenderingContextImpl, WebGLRenderingContextImpl);

public:
    WebGL2RenderingContextImpl(JS::Realm&, NonnullOwnPtr<OpenGLContext>);

    void copy_buffer_sub_data(WebIDL::UnsignedLong read_target, WebIDL::UnsignedLong write_target, WebIDL::LongLong read_offset, WebIDL::LongLong write_offset, WebIDL::LongLong size);
    void get_buffer_sub_data(WebIDL::UnsignedLong target, WebIDL::LongLong src_byte_offset, GC::Root<WebIDL::ArrayBufferView> dst_buffer, WebIDL::UnsignedLongLong dst_offset, WebIDL::UnsignedLong length);
    void blit_framebuffer(WebIDL::Long src_x0, WebIDL::Long src_y0, WebIDL::Long src_x1, WebIDL::Long src_y1, WebIDL::Long dst_x0, WebIDL::Long dst_y0, WebIDL::Long dst_x1, WebIDL::Long dst_y1, WebIDL::UnsignedLong mask, WebIDL::UnsignedLong filter);
    void framebuffer_texture_layer(WebIDL::UnsignedLong target, WebIDL::UnsignedLong attachment, GC::Root<WebGLTexture> texture, WebIDL::Long level, WebIDL::Long layer);
    void invalidate_framebuffer(WebIDL::UnsignedLong target, Vector<WebIDL::UnsignedLong> attachments);
    void invalidate_sub_framebuffer(WebIDL::UnsignedLong target, Vector<WebIDL::UnsignedLong> attachments, WebIDL::Long x, WebIDL::Long y, WebIDL::Long width, WebIDL::Long height);
    void read_buffer(WebIDL::UnsignedLong src);
    JS::Value get_internalformat_parameter(WebIDL::UnsignedLong target, WebIDL::UnsignedLong internalformat, WebIDL::UnsignedLong pname);
    void renderbuffer_storage_multisample(WebIDL::UnsignedLong target, WebIDL::Long samples, WebIDL::UnsignedLong internalformat, WebIDL::Long width, WebIDL::Long height);
    void tex_storage2d(WebIDL::UnsignedLong target, WebIDL::Long levels, WebIDL::UnsignedLong internalformat, WebIDL::Long width, WebIDL::Long height);
    void tex_storage3d(WebIDL::UnsignedLong target, WebIDL::Long levels, WebIDL::UnsignedLong internalformat, WebIDL::Long width, WebIDL::Long height, WebIDL::Long depth);
    void tex_image3d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long internalformat, WebIDL::Long width, WebIDL::Long height, WebIDL::Long depth, WebIDL::Long border, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, GC::Root<WebIDL::ArrayBufferView> src_data);
    void tex_image3d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long internalformat, WebIDL::Long width, WebIDL::Long height, WebIDL::Long depth, WebIDL::Long border, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, GC::Root<WebIDL::ArrayBufferView> src_data, WebIDL::UnsignedLongLong src_offset);
    void tex_sub_image3d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long xoffset, WebIDL::Long yoffset, WebIDL::Long zoffset, WebIDL::Long width, WebIDL::Long height, WebIDL::Long depth, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, GC::Root<WebIDL::ArrayBufferView> src_data, WebIDL::UnsignedLongLong src_offset);
    void uniform1ui(GC::Root<WebGLUniformLocation> location, WebIDL::UnsignedLong v0);
    void uniform2ui(GC::Root<WebGLUniformLocation> location, WebIDL::UnsignedLong v0, WebIDL::UnsignedLong v1);
    void uniform3ui(GC::Root<WebGLUniformLocation> location, WebIDL::UnsignedLong v0, WebIDL::UnsignedLong v1, WebIDL::UnsignedLong v2);
    void uniform4ui(GC::Root<WebGLUniformLocation> location, WebIDL::UnsignedLong v0, WebIDL::UnsignedLong v1, WebIDL::UnsignedLong v2, WebIDL::UnsignedLong v3);
    void uniform1uiv(GC::Root<WebGLUniformLocation> location, Uint32List v, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length);
    void uniform2uiv(GC::Root<WebGLUniformLocation> location, Uint32List v, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length);
    void uniform3uiv(GC::Root<WebGLUniformLocation> location, Uint32List v, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length);
    void uniform4uiv(GC::Root<WebGLUniformLocation> location, Uint32List v, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length);
    void uniform_matrix3x2fv(GC::Root<WebGLUniformLocation> location, bool transpose, Float32List data, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length);
    void uniform_matrix4x2fv(GC::Root<WebGLUniformLocation> location, bool transpose, Float32List data, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length);
    void uniform_matrix2x3fv(GC::Root<WebGLUniformLocation> location, bool transpose, Float32List data, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length);
    void uniform_matrix4x3fv(GC::Root<WebGLUniformLocation> location, bool transpose, Float32List data, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length);
    void uniform_matrix2x4fv(GC::Root<WebGLUniformLocation> location, bool transpose, Float32List data, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length);
    void uniform_matrix3x4fv(GC::Root<WebGLUniformLocation> location, bool transpose, Float32List data, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length);
    void vertex_attrib_i4i(WebIDL::UnsignedLong index, WebIDL::Long x, WebIDL::Long y, WebIDL::Long z, WebIDL::Long w);
    void vertex_attrib_i4iv(WebIDL::UnsignedLong index, Int32List values);
    void vertex_attrib_i4ui(WebIDL::UnsignedLong index, WebIDL::UnsignedLong x, WebIDL::UnsignedLong y, WebIDL::UnsignedLong z, WebIDL::UnsignedLong w);
    void vertex_attrib_i4uiv(WebIDL::UnsignedLong index, Uint32List values);
    void vertex_attrib_i_pointer(WebIDL::UnsignedLong index, WebIDL::Long size, WebIDL::UnsignedLong type, WebIDL::Long stride, WebIDL::LongLong offset);
    void vertex_attrib_divisor(WebIDL::UnsignedLong index, WebIDL::UnsignedLong divisor);
    void draw_arrays_instanced(WebIDL::UnsignedLong mode, WebIDL::Long first, WebIDL::Long count, WebIDL::Long instance_count);
    void draw_elements_instanced(WebIDL::UnsignedLong mode, WebIDL::Long count, WebIDL::UnsignedLong type, WebIDL::LongLong offset, WebIDL::Long instance_count);
    void draw_range_elements(WebIDL::UnsignedLong mode, WebIDL::UnsignedLong start, WebIDL::UnsignedLong end, WebIDL::Long count, WebIDL::UnsignedLong type, WebIDL::LongLong offset);
    void draw_buffers(Vector<WebIDL::UnsignedLong> buffers);
    void clear_bufferfv(WebIDL::UnsignedLong buffer, WebIDL::Long drawbuffer, Float32List values, WebIDL::UnsignedLongLong src_offset);
    void clear_bufferiv(WebIDL::UnsignedLong buffer, WebIDL::Long drawbuffer, Int32List values, WebIDL::UnsignedLongLong src_offset);
    void clear_bufferuiv(WebIDL::UnsignedLong buffer, WebIDL::Long drawbuffer, Uint32List values, WebIDL::UnsignedLongLong src_offset);
    void clear_bufferfi(WebIDL::UnsignedLong buffer, WebIDL::Long drawbuffer, float depth, WebIDL::Long stencil);
    GC::Root<WebGLQuery> create_query();
    void delete_query(GC::Root<WebGLQuery> query);
    void begin_query(WebIDL::UnsignedLong target, GC::Root<WebGLQuery> query);
    void end_query(WebIDL::UnsignedLong target);
    GC::Root<WebGLQuery> get_query(WebIDL::UnsignedLong target, WebIDL::UnsignedLong pname);
    JS::Value get_query_parameter(GC::Root<WebGLQuery> query, WebIDL::UnsignedLong pname);
    GC::Root<WebGLSampler> create_sampler();
    void delete_sampler(GC::Root<WebGLSampler> sampler);
    void bind_sampler(WebIDL::UnsignedLong unit, GC::Root<WebGLSampler> sampler);
    void sampler_parameteri(GC::Root<WebGLSampler> sampler, WebIDL::UnsignedLong pname, WebIDL::Long param);
    void sampler_parameterf(GC::Root<WebGLSampler> sampler, WebIDL::UnsignedLong pname, float param);
    GC::Root<WebGLSync> fence_sync(WebIDL::UnsignedLong condition, WebIDL::UnsignedLong flags);
    void delete_sync(GC::Root<WebGLSync> sync);
    WebIDL::UnsignedLong client_wait_sync(GC::Root<WebGLSync> sync, WebIDL::UnsignedLong flags, WebIDL::UnsignedLongLong timeout);
    void wait_sync(GC::Root<WebGLSync> sync, WebIDL::UnsignedLong flags, WebIDL::UnsignedLongLong timeout);
    JS::Value get_sync_parameter(GC::Root<WebGLSync> sync, WebIDL::UnsignedLong pname);
    GC::Root<WebGLTransformFeedback> create_transform_feedback();
    void delete_transform_feedback(GC::Root<WebGLTransformFeedback> transform_feedback);
    void bind_transform_feedback(WebIDL::UnsignedLong target, GC::Root<WebGLTransformFeedback> transform_feedback);
    void begin_transform_feedback(WebIDL::UnsignedLong primitive_mode);
    void end_transform_feedback();
    void transform_feedback_varyings(GC::Root<WebGLProgram> program, Vector<String> const& varyings, WebIDL::UnsignedLong buffer_mode);
    void pause_transform_feedback();
    void resume_transform_feedback();
    void bind_buffer_base(WebIDL::UnsignedLong target, WebIDL::UnsignedLong index, GC::Root<WebGLBuffer> buffer);
    void bind_buffer_range(WebIDL::UnsignedLong target, WebIDL::UnsignedLong index, GC::Root<WebGLBuffer> buffer, WebIDL::LongLong offset, WebIDL::LongLong size);
    Optional<Vector<WebIDL::UnsignedLong>> get_uniform_indices(GC::Root<WebGLProgram> program, Vector<String> const& uniform_names);
    JS::Value get_active_uniforms(GC::Root<WebGLProgram> program, Vector<WebIDL::UnsignedLong> uniform_indices, WebIDL::UnsignedLong pname);
    WebIDL::UnsignedLong get_uniform_block_index(GC::Root<WebGLProgram> program, String uniform_block_name);
    JS::Value get_active_uniform_block_parameter(GC::Root<WebGLProgram> program, WebIDL::UnsignedLong uniform_block_index, WebIDL::UnsignedLong pname);
    Optional<String> get_active_uniform_block_name(GC::Root<WebGLProgram> program, WebIDL::UnsignedLong uniform_block_index);
    void uniform_block_binding(GC::Root<WebGLProgram> program, WebIDL::UnsignedLong uniform_block_index, WebIDL::UnsignedLong uniform_block_binding);
    GC::Root<WebGLVertexArrayObject> create_vertex_array();
    void delete_vertex_array(GC::Root<WebGLVertexArrayObject> vertex_array);
    bool is_vertex_array(GC::Root<WebGLVertexArrayObject> vertex_array);
    void bind_vertex_array(GC::Root<WebGLVertexArrayObject> array);
    void compressed_tex_image3d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::UnsignedLong internalformat, WebIDL::Long width, WebIDL::Long height, WebIDL::Long depth, WebIDL::Long border, GC::Root<WebIDL::ArrayBufferView> src_data, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length_override);
    void compressed_tex_sub_image3d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long xoffset, WebIDL::Long yoffset, WebIDL::Long zoffset, WebIDL::Long width, WebIDL::Long height, WebIDL::Long depth, WebIDL::UnsignedLong format, GC::Root<WebIDL::ArrayBufferView> src_data, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length_override);
};

}
