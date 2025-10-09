/*
 * Copyright (c) 2024-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2024-2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <LibGC/Ptr.h>
#include <LibGfx/Bitmap.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebGL/WebGLRenderingContextBase.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::WebGL {

using namespace Web::HTML;

class WebGLRenderingContextImpl : public WebGLRenderingContextBase {
public:
    WebGLRenderingContextImpl(JS::Realm&, NonnullOwnPtr<OpenGLContext>);

    virtual OpenGLContext& context() override { return *m_context; }

    virtual void present() = 0;
    virtual void needs_to_present() = 0;
    virtual void set_error(GLenum) = 0;
    void buffer_data(WebIDL::UnsignedLong target, WebIDL::LongLong size, WebIDL::UnsignedLong usage);
    void buffer_data(WebIDL::UnsignedLong target, GC::Root<WebIDL::BufferSource> const& data, WebIDL::UnsignedLong usage);
    void buffer_sub_data(WebIDL::UnsignedLong target, WebIDL::LongLong offset, GC::Root<WebIDL::BufferSource> const& data);
    void compressed_tex_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::UnsignedLong internalformat, WebIDL::Long width, WebIDL::Long height, WebIDL::Long border, GC::Root<WebIDL::ArrayBufferView> const& data);
    void compressed_tex_sub_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long xoffset, WebIDL::Long yoffset, WebIDL::Long width, WebIDL::Long height, WebIDL::UnsignedLong format, GC::Root<WebIDL::ArrayBufferView> const& data);
    void read_pixels(WebIDL::Long x, WebIDL::Long y, WebIDL::Long width, WebIDL::Long height, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, GC::Root<WebIDL::ArrayBufferView> const& pixels);
    void tex_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long internalformat, WebIDL::Long width, WebIDL::Long height, WebIDL::Long border, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, GC::Root<WebIDL::ArrayBufferView> const& pixels);
    void tex_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long internalformat, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, TexImageSource source);
    void tex_sub_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long xoffset, WebIDL::Long yoffset, WebIDL::Long width, WebIDL::Long height, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, GC::Root<WebIDL::ArrayBufferView> const& pixels);
    void tex_sub_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long xoffset, WebIDL::Long yoffset, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, TexImageSource source);
    void uniform1fv(GC::Root<WebGLUniformLocation> const& location, Float32List v);
    void uniform2fv(GC::Root<WebGLUniformLocation> const& location, Float32List v);
    void uniform3fv(GC::Root<WebGLUniformLocation> const& location, Float32List v);
    void uniform4fv(GC::Root<WebGLUniformLocation> const& location, Float32List v);
    void uniform1iv(GC::Root<WebGLUniformLocation> const& location, Int32List v);
    void uniform2iv(GC::Root<WebGLUniformLocation> const& location, Int32List v);
    void uniform3iv(GC::Root<WebGLUniformLocation> const& location, Int32List v);
    void uniform4iv(GC::Root<WebGLUniformLocation> const& location, Int32List v);
    void uniform_matrix2fv(GC::Root<WebGLUniformLocation> const& location, bool transpose, Float32List value);
    void uniform_matrix3fv(GC::Root<WebGLUniformLocation> const& location, bool transpose, Float32List value);
    void uniform_matrix4fv(GC::Root<WebGLUniformLocation> const& location, bool transpose, Float32List value);
    void active_texture(WebIDL::UnsignedLong texture);
    void attach_shader(GC::Root<WebGLProgram> const& program, GC::Root<WebGLShader> const& shader);
    void bind_attrib_location(GC::Root<WebGLProgram> const& program, WebIDL::UnsignedLong index, String const& name);
    void bind_buffer(WebIDL::UnsignedLong target, GC::Root<WebGLBuffer> const& buffer);
    void bind_framebuffer(WebIDL::UnsignedLong target, GC::Root<WebGLFramebuffer> const& framebuffer);
    void bind_renderbuffer(WebIDL::UnsignedLong target, GC::Root<WebGLRenderbuffer> const& renderbuffer);
    void bind_texture(WebIDL::UnsignedLong target, GC::Root<WebGLTexture> const& texture);
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
    void compile_shader(GC::Root<WebGLShader> const& shader);
    void copy_tex_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::UnsignedLong internalformat, WebIDL::Long x, WebIDL::Long y, WebIDL::Long width, WebIDL::Long height, WebIDL::Long border);
    void copy_tex_sub_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long xoffset, WebIDL::Long yoffset, WebIDL::Long x, WebIDL::Long y, WebIDL::Long width, WebIDL::Long height);
    GC::Root<WebGLBuffer> create_buffer();
    GC::Root<WebGLFramebuffer> create_framebuffer();
    GC::Root<WebGLProgram> create_program();
    GC::Root<WebGLRenderbuffer> create_renderbuffer();
    GC::Root<WebGLShader> create_shader(WebIDL::UnsignedLong type);
    GC::Root<WebGLTexture> create_texture();
    void cull_face(WebIDL::UnsignedLong mode);
    void delete_buffer(GC::Root<WebGLBuffer> const& buffer);
    void delete_framebuffer(GC::Root<WebGLFramebuffer> const& framebuffer);
    void delete_program(GC::Root<WebGLProgram> const& program);
    void delete_renderbuffer(GC::Root<WebGLRenderbuffer> const& renderbuffer);
    void delete_shader(GC::Root<WebGLShader> const& shader);
    void delete_texture(GC::Root<WebGLTexture> const& texture);
    void depth_func(WebIDL::UnsignedLong func);
    void depth_mask(bool flag);
    void depth_range(float z_near, float z_far);
    void detach_shader(GC::Root<WebGLProgram> const& program, GC::Root<WebGLShader> const& shader);
    void disable(WebIDL::UnsignedLong cap);
    void disable_vertex_attrib_array(WebIDL::UnsignedLong index);
    void draw_arrays(WebIDL::UnsignedLong mode, WebIDL::Long first, WebIDL::Long count);
    void draw_elements(WebIDL::UnsignedLong mode, WebIDL::Long count, WebIDL::UnsignedLong type, WebIDL::LongLong offset);
    void enable(WebIDL::UnsignedLong cap);
    void enable_vertex_attrib_array(WebIDL::UnsignedLong index);
    void finish();
    void flush();
    void framebuffer_renderbuffer(WebIDL::UnsignedLong target, WebIDL::UnsignedLong attachment, WebIDL::UnsignedLong renderbuffertarget, GC::Root<WebGLRenderbuffer> const& renderbuffer);
    void framebuffer_texture2d(WebIDL::UnsignedLong target, WebIDL::UnsignedLong attachment, WebIDL::UnsignedLong textarget, GC::Root<WebGLTexture> const& texture, WebIDL::Long level);
    void front_face(WebIDL::UnsignedLong mode);
    void generate_mipmap(WebIDL::UnsignedLong target);
    GC::Root<WebGLActiveInfo> get_active_attrib(GC::Root<WebGLProgram> const& program, WebIDL::UnsignedLong index);
    GC::Root<WebGLActiveInfo> get_active_uniform(GC::Root<WebGLProgram> const& program, WebIDL::UnsignedLong index);
    Optional<Vector<GC::Root<WebGLShader>>> get_attached_shaders(GC::Root<WebGLProgram> const& program);
    WebIDL::Long get_attrib_location(GC::Root<WebGLProgram> const& program, String const& name);
    JS::Value get_buffer_parameter(WebIDL::UnsignedLong target, WebIDL::UnsignedLong pname);
    JS::Value get_parameter(WebIDL::UnsignedLong pname);
    WebIDL::UnsignedLong get_error();
    JS::Value get_program_parameter(GC::Root<WebGLProgram> const& program, WebIDL::UnsignedLong pname);
    Optional<String> get_program_info_log(GC::Root<WebGLProgram> const& program);
    JS::Value get_shader_parameter(GC::Root<WebGLShader> const& shader, WebIDL::UnsignedLong pname);
    GC::Root<WebGLShaderPrecisionFormat> get_shader_precision_format(WebIDL::UnsignedLong shadertype, WebIDL::UnsignedLong precisiontype);
    Optional<String> get_shader_info_log(GC::Root<WebGLShader> const& shader);
    GC::Root<WebGLUniformLocation> get_uniform_location(GC::Root<WebGLProgram> const& program, String const& name);
    void hint(WebIDL::UnsignedLong target, WebIDL::UnsignedLong mode);
    bool is_buffer(GC::Root<WebGLBuffer> const& buffer);
    bool is_enabled(WebIDL::UnsignedLong cap);
    bool is_framebuffer(GC::Root<WebGLFramebuffer> const& framebuffer);
    bool is_program(GC::Root<WebGLProgram> const& program);
    bool is_renderbuffer(GC::Root<WebGLRenderbuffer> const& renderbuffer);
    bool is_shader(GC::Root<WebGLShader> const& shader);
    bool is_texture(GC::Root<WebGLTexture> const& texture);
    void line_width(float width);
    void link_program(GC::Root<WebGLProgram> const& program);
    void pixel_storei(WebIDL::UnsignedLong pname, WebIDL::Long param);
    void polygon_offset(float factor, float units);
    void renderbuffer_storage(WebIDL::UnsignedLong target, WebIDL::UnsignedLong internalformat, WebIDL::Long width, WebIDL::Long height);
    void sample_coverage(float value, bool invert);
    void scissor(WebIDL::Long x, WebIDL::Long y, WebIDL::Long width, WebIDL::Long height);
    void shader_source(GC::Root<WebGLShader> const& shader, String const& source);
    void stencil_func(WebIDL::UnsignedLong func, WebIDL::Long ref, WebIDL::UnsignedLong mask);
    void stencil_func_separate(WebIDL::UnsignedLong face, WebIDL::UnsignedLong func, WebIDL::Long ref, WebIDL::UnsignedLong mask);
    void stencil_mask(WebIDL::UnsignedLong mask);
    void stencil_mask_separate(WebIDL::UnsignedLong face, WebIDL::UnsignedLong mask);
    void stencil_op(WebIDL::UnsignedLong fail, WebIDL::UnsignedLong zfail, WebIDL::UnsignedLong zpass);
    void stencil_op_separate(WebIDL::UnsignedLong face, WebIDL::UnsignedLong fail, WebIDL::UnsignedLong zfail, WebIDL::UnsignedLong zpass);
    void tex_parameterf(WebIDL::UnsignedLong target, WebIDL::UnsignedLong pname, float param);
    void tex_parameteri(WebIDL::UnsignedLong target, WebIDL::UnsignedLong pname, WebIDL::Long param);
    void uniform1f(GC::Root<WebGLUniformLocation> const& location, float x);
    void uniform2f(GC::Root<WebGLUniformLocation> const& location, float x, float y);
    void uniform3f(GC::Root<WebGLUniformLocation> const& location, float x, float y, float z);
    void uniform4f(GC::Root<WebGLUniformLocation> const& location, float x, float y, float z, float w);
    void uniform1i(GC::Root<WebGLUniformLocation> const& location, WebIDL::Long x);
    void uniform2i(GC::Root<WebGLUniformLocation> const& location, WebIDL::Long x, WebIDL::Long y);
    void uniform3i(GC::Root<WebGLUniformLocation> const& location, WebIDL::Long x, WebIDL::Long y, WebIDL::Long z);
    void uniform4i(GC::Root<WebGLUniformLocation> const& location, WebIDL::Long x, WebIDL::Long y, WebIDL::Long z, WebIDL::Long w);
    void use_program(GC::Root<WebGLProgram> const& program);
    void validate_program(GC::Root<WebGLProgram> const& program);
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

private:
    GC::Ref<JS::Realm> m_realm;
    GC::Ptr<WebGLBuffer> m_array_buffer_binding;
    GC::Ptr<WebGLBuffer> m_element_array_buffer_binding;
    GC::Ptr<WebGLProgram> m_current_program;
    GC::Ptr<WebGLFramebuffer> m_framebuffer_binding;
    GC::Ptr<WebGLRenderbuffer> m_renderbuffer_binding;
    GC::Ptr<WebGLTexture> m_texture_binding_2d;
    GC::Ptr<WebGLTexture> m_texture_binding_cube_map;

    NonnullOwnPtr<OpenGLContext> m_context;
};

}
