/*
 * Copyright (c) 2024-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
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
#include <LibWeb/WebGL/WebGLRenderingContextBase.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::WebGL {

using namespace Web::HTML;

class WebGLRenderingContextImpl : public WebGLRenderingContextBase {
    WEB_NON_IDL_PLATFORM_OBJECT(WebGLRenderingContextImpl, WebGLRenderingContextBase);

public:
    WebGLRenderingContextImpl(JS::Realm&, NonnullOwnPtr<OpenGLContext>);

    virtual OpenGLContext& context() override { return *m_context; }

    virtual void present() = 0;
    virtual void needs_to_present() = 0;

    void active_texture(WebIDL::UnsignedLong texture);
    void attach_shader(GC::Root<WebGLProgram> program, GC::Root<WebGLShader> shader);
    void bind_attrib_location(GC::Root<WebGLProgram> program, WebIDL::UnsignedLong index, String name);
    void bind_buffer(WebIDL::UnsignedLong target, GC::Root<WebGLBuffer> buffer);
    void bind_framebuffer(WebIDL::UnsignedLong target, GC::Root<WebGLFramebuffer> framebuffer);
    void bind_renderbuffer(WebIDL::UnsignedLong target, GC::Root<WebGLRenderbuffer> renderbuffer);
    void bind_texture(WebIDL::UnsignedLong target, GC::Root<WebGLTexture> texture);
    void blend_color(float red, float green, float blue, float alpha);
    void blend_equation(WebIDL::UnsignedLong mode);
    void blend_equation_separate(WebIDL::UnsignedLong mode_rgb, WebIDL::UnsignedLong mode_alpha);
    void blend_func(WebIDL::UnsignedLong sfactor, WebIDL::UnsignedLong dfactor);
    void blend_func_separate(WebIDL::UnsignedLong src_rgb, WebIDL::UnsignedLong dst_rgb, WebIDL::UnsignedLong src_alpha, WebIDL::UnsignedLong dst_alpha);
    WebIDL::UnsignedLong check_framebuffer_status(WebIDL::UnsignedLong target);
    void clear(WebIDL::UnsignedLong mask);
    void clear_color(float red, float green, float blue, float alpha);
    void clear_depth(float depth);
    void clear_stencil(WebIDL::Long s);
    void color_mask(bool red, bool green, bool blue, bool alpha);
    void compile_shader(GC::Root<WebGLShader> shader);
    void copy_tex_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::UnsignedLong internalformat, WebIDL::Long x, WebIDL::Long y, WebIDL::Long width, WebIDL::Long height, WebIDL::Long border);
    void copy_tex_sub_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long xoffset, WebIDL::Long yoffset, WebIDL::Long x, WebIDL::Long y, WebIDL::Long width, WebIDL::Long height);
    GC::Root<WebGLBuffer> create_buffer();
    GC::Root<WebGLFramebuffer> create_framebuffer();
    GC::Root<WebGLProgram> create_program();
    GC::Root<WebGLRenderbuffer> create_renderbuffer();
    GC::Root<WebGLShader> create_shader(WebIDL::UnsignedLong type);
    GC::Root<WebGLTexture> create_texture();
    void cull_face(WebIDL::UnsignedLong mode);
    void delete_buffer(GC::Root<WebGLBuffer> buffer);
    void delete_framebuffer(GC::Root<WebGLFramebuffer> framebuffer);
    void delete_program(GC::Root<WebGLProgram> program);
    void delete_renderbuffer(GC::Root<WebGLRenderbuffer> renderbuffer);
    void delete_shader(GC::Root<WebGLShader> shader);
    void delete_texture(GC::Root<WebGLTexture> texture);
    void depth_func(WebIDL::UnsignedLong func);
    void depth_mask(bool flag);
    void depth_range(float z_near, float z_far);
    void detach_shader(GC::Root<WebGLProgram> program, GC::Root<WebGLShader> shader);
    void disable(WebIDL::UnsignedLong cap);
    void disable_vertex_attrib_array(WebIDL::UnsignedLong index);
    void draw_arrays(WebIDL::UnsignedLong mode, WebIDL::Long first, WebIDL::Long count);
    void draw_elements(WebIDL::UnsignedLong mode, WebIDL::Long count, WebIDL::UnsignedLong type, WebIDL::LongLong offset);
    void enable(WebIDL::UnsignedLong cap);
    void enable_vertex_attrib_array(WebIDL::UnsignedLong index);
    void finish();
    void flush();
    void framebuffer_renderbuffer(WebIDL::UnsignedLong target, WebIDL::UnsignedLong attachment, WebIDL::UnsignedLong renderbuffertarget, GC::Root<WebGLRenderbuffer> renderbuffer);
    void framebuffer_texture2d(WebIDL::UnsignedLong target, WebIDL::UnsignedLong attachment, WebIDL::UnsignedLong textarget, GC::Root<WebGLTexture> texture, WebIDL::Long level);
    void front_face(WebIDL::UnsignedLong mode);
    void generate_mipmap(WebIDL::UnsignedLong target);
    GC::Root<WebGLActiveInfo> get_active_attrib(GC::Root<WebGLProgram> program, WebIDL::UnsignedLong index);
    GC::Root<WebGLActiveInfo> get_active_uniform(GC::Root<WebGLProgram> program, WebIDL::UnsignedLong index);
    Optional<Vector<GC::Root<WebGLShader>>> get_attached_shaders(GC::Root<WebGLProgram> program);
    WebIDL::Long get_attrib_location(GC::Root<WebGLProgram> program, String name);
    JS::Value get_buffer_parameter(WebIDL::UnsignedLong target, WebIDL::UnsignedLong pname);
    WebIDL::ExceptionOr<JS::Value> get_parameter(WebIDL::UnsignedLong pname);
    WebIDL::UnsignedLong get_error();
    JS::Value get_program_parameter(GC::Root<WebGLProgram> program, WebIDL::UnsignedLong pname);
    Optional<String> get_program_info_log(GC::Root<WebGLProgram> program);
    JS::Value get_renderbuffer_parameter(WebIDL::UnsignedLong target, WebIDL::UnsignedLong pname);
    JS::Value get_shader_parameter(GC::Root<WebGLShader> shader, WebIDL::UnsignedLong pname);
    GC::Root<WebGLShaderPrecisionFormat> get_shader_precision_format(WebIDL::UnsignedLong shadertype, WebIDL::UnsignedLong precisiontype);
    Optional<String> get_shader_info_log(GC::Root<WebGLShader> shader);
    Optional<String> get_shader_source(GC::Root<WebGLShader> shader);
    JS::Value get_tex_parameter(WebIDL::UnsignedLong target, WebIDL::UnsignedLong pname);
    JS::Value get_uniform(GC::Root<WebGLProgram> program, GC::Root<WebGLUniformLocation> location);
    GC::Root<WebGLUniformLocation> get_uniform_location(GC::Root<WebGLProgram> program, String name);
    JS::Value get_vertex_attrib(WebIDL::UnsignedLong index, WebIDL::UnsignedLong pname);
    WebIDL::LongLong get_vertex_attrib_offset(WebIDL::UnsignedLong index, WebIDL::UnsignedLong pname);
    void hint(WebIDL::UnsignedLong target, WebIDL::UnsignedLong mode);
    bool is_buffer(GC::Root<WebGLBuffer> buffer);
    bool is_enabled(WebIDL::UnsignedLong cap);
    bool is_framebuffer(GC::Root<WebGLFramebuffer> framebuffer);
    bool is_program(GC::Root<WebGLProgram> program);
    bool is_renderbuffer(GC::Root<WebGLRenderbuffer> renderbuffer);
    bool is_shader(GC::Root<WebGLShader> shader);
    bool is_texture(GC::Root<WebGLTexture> texture);
    void line_width(float width);
    void link_program(GC::Root<WebGLProgram> program);
    void pixel_storei(WebIDL::UnsignedLong pname, WebIDL::Long param);
    void polygon_offset(float factor, float units);
    void renderbuffer_storage(WebIDL::UnsignedLong target, WebIDL::UnsignedLong internalformat, WebIDL::Long width, WebIDL::Long height);
    void sample_coverage(float value, bool invert);
    void scissor(WebIDL::Long x, WebIDL::Long y, WebIDL::Long width, WebIDL::Long height);
    void shader_source(GC::Root<WebGLShader> shader, String source);
    void stencil_func(WebIDL::UnsignedLong func, WebIDL::Long ref, WebIDL::UnsignedLong mask);
    void stencil_func_separate(WebIDL::UnsignedLong face, WebIDL::UnsignedLong func, WebIDL::Long ref, WebIDL::UnsignedLong mask);
    void stencil_mask(WebIDL::UnsignedLong mask);
    void stencil_mask_separate(WebIDL::UnsignedLong face, WebIDL::UnsignedLong mask);
    void stencil_op(WebIDL::UnsignedLong fail, WebIDL::UnsignedLong zfail, WebIDL::UnsignedLong zpass);
    void stencil_op_separate(WebIDL::UnsignedLong face, WebIDL::UnsignedLong fail, WebIDL::UnsignedLong zfail, WebIDL::UnsignedLong zpass);
    void tex_parameterf(WebIDL::UnsignedLong target, WebIDL::UnsignedLong pname, float param);
    void tex_parameteri(WebIDL::UnsignedLong target, WebIDL::UnsignedLong pname, WebIDL::Long param);
    void uniform1f(GC::Root<WebGLUniformLocation> location, float x);
    void uniform2f(GC::Root<WebGLUniformLocation> location, float x, float y);
    void uniform3f(GC::Root<WebGLUniformLocation> location, float x, float y, float z);
    void uniform4f(GC::Root<WebGLUniformLocation> location, float x, float y, float z, float w);
    void uniform1i(GC::Root<WebGLUniformLocation> location, WebIDL::Long x);
    void uniform2i(GC::Root<WebGLUniformLocation> location, WebIDL::Long x, WebIDL::Long y);
    void uniform3i(GC::Root<WebGLUniformLocation> location, WebIDL::Long x, WebIDL::Long y, WebIDL::Long z);
    void uniform4i(GC::Root<WebGLUniformLocation> location, WebIDL::Long x, WebIDL::Long y, WebIDL::Long z, WebIDL::Long w);
    void use_program(GC::Root<WebGLProgram> program);
    void validate_program(GC::Root<WebGLProgram> program);
    void vertex_attrib1f(WebIDL::UnsignedLong index, float x);
    void vertex_attrib2f(WebIDL::UnsignedLong index, float x, float y);
    void vertex_attrib3f(WebIDL::UnsignedLong index, float x, float y, float z);
    void vertex_attrib4f(WebIDL::UnsignedLong index, float x, float y, float z, float w);
    void vertex_attrib1fv(WebIDL::UnsignedLong index, Float32List values);
    void vertex_attrib2fv(WebIDL::UnsignedLong index, Float32List values);
    void vertex_attrib3fv(WebIDL::UnsignedLong index, Float32List values);
    void vertex_attrib4fv(WebIDL::UnsignedLong index, Float32List values);
    void vertex_attrib_pointer(WebIDL::UnsignedLong index, WebIDL::Long size, WebIDL::UnsignedLong type, bool normalized, WebIDL::Long stride, WebIDL::LongLong offset);
    void viewport(WebIDL::Long x, WebIDL::Long y, WebIDL::Long width, WebIDL::Long height);

protected:
    virtual void visit_edges(JS::Cell::Visitor&) override;

    GC::Ptr<WebGLBuffer> m_array_buffer_binding;
    GC::Ptr<WebGLBuffer> m_element_array_buffer_binding;
    GC::Ptr<WebGLProgram> m_current_program;
    GC::Ptr<WebGLFramebuffer> m_framebuffer_binding;
    GC::Ptr<WebGLRenderbuffer> m_renderbuffer_binding;
    GC::Ptr<WebGLTexture> m_texture_binding_2d;
    GC::Ptr<WebGLTexture> m_texture_binding_cube_map;

    // FIXME: Those are WebGL2 only but those need to be accessible from shared methods
    GC::Ptr<WebGLBuffer> m_uniform_buffer_binding;
    GC::Ptr<WebGLBuffer> m_copy_read_buffer_binding;
    GC::Ptr<WebGLBuffer> m_copy_write_buffer_binding;
    GC::Ptr<WebGLBuffer> m_transform_feedback_buffer_binding;
    GC::Ptr<WebGLBuffer> m_pixel_pack_buffer_binding;
    GC::Ptr<WebGLBuffer> m_pixel_unpack_buffer_binding;
    GC::Ptr<WebGLTexture> m_texture_binding_2d_array;
    GC::Ptr<WebGLTexture> m_texture_binding_3d;
    GC::Ptr<WebGLTransformFeedback> m_transform_feedback_binding;
    GC::Ptr<WebGLVertexArrayObject> m_current_vertex_array;
    GC::Ptr<WebGLQuery> m_any_samples_passed;
    GC::Ptr<WebGLQuery> m_any_samples_passed_conservative;
    GC::Ptr<WebGLQuery> m_transform_feedback_primitives_written;

    NonnullOwnPtr<OpenGLContext> m_context;
};

}
