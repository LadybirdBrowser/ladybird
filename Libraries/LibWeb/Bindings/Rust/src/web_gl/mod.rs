/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The WebContent half of WebGL, which validates what script asks for, keeps track of the state that it can read
//! back, and records everything else as commands for the Compositor, which runs GL.

pub(crate) mod buffer_kind;
pub(crate) mod byte_ranges;
pub(crate) mod constants;
pub(crate) mod proxy;

// What follows runs on the JS runtime, which the tests of the crate do not link.
#[cfg(not(test))]
pub(crate) mod angle_instanced_arrays;
#[cfg(not(test))]
pub(crate) mod buffer_source;
#[cfg(not(test))]
pub(crate) mod canvas;
#[cfg(not(test))]
pub(crate) mod exports;
#[cfg(not(test))]
pub(crate) mod extensions;
#[cfg(not(test))]
pub(crate) mod fingerprint;
#[cfg(not(test))]
pub(crate) mod gl_string;
#[cfg(not(test))]
pub(crate) mod host;
#[cfg(not(test))]
pub(crate) mod oes_vertex_array_object;
#[cfg(not(test))]
pub(crate) mod parameters;
#[cfg(not(test))]
pub(crate) mod state;
#[cfg(not(test))]
pub(crate) mod types;
#[cfg(not(test))]
pub(crate) mod values;
#[cfg(not(test))]
pub(crate) mod web_gl2_rendering_context;
#[cfg(not(test))]
pub(crate) mod web_gl2_rendering_context_base;
#[cfg(not(test))]
pub(crate) mod web_gl2_rendering_context_overloads;
#[cfg(not(test))]
pub(crate) mod web_gl_active_info;
#[cfg(not(test))]
pub(crate) mod web_gl_buffer;
#[cfg(not(test))]
pub(crate) mod web_gl_draw_buffers;
#[cfg(not(test))]
pub(crate) mod web_gl_framebuffer;
#[cfg(not(test))]
pub(crate) mod web_gl_object;
#[cfg(not(test))]
pub(crate) mod web_gl_program;
#[cfg(not(test))]
pub(crate) mod web_gl_rendering_context;
#[cfg(not(test))]
pub(crate) mod web_gl_rendering_context_base;
#[cfg(not(test))]
pub(crate) mod web_gl_rendering_context_overloads;
#[cfg(not(test))]
pub(crate) mod web_gl_shader;
#[cfg(not(test))]
pub(crate) mod web_gl_shader_precision_format;
#[cfg(not(test))]
pub(crate) mod web_gl_sync;
#[cfg(not(test))]
pub(crate) mod web_gl_uniform_location;

// The interfaces of the two lists below differ only in their entries, so they have no file: an entry expands to the
// module that the generated bindings look up for its interface.

// An entry expands to
//
//     pub(crate) mod ext_blend_min_max {
//         pub(crate) struct EXTBlendMinMax { context: ContextRef }
//         impl Wrappable for EXTBlendMinMax { .. }
//         impl EXTBlendMinMax { pub(crate) fn create(vm: &Vm, context: &ContextState) -> Ref<Self> { .. } }
//     }
#[cfg(not(test))]
extensions::modules_of_extension_interfaces_without_operations! {
    /// https://registry.khronos.org/webgl/extensions/EXT_blend_minmax/
    ext_blend_min_max::EXTBlendMinMax,
    /// https://registry.khronos.org/webgl/extensions/EXT_color_buffer_float/
    ext_color_buffer_float::EXTColorBufferFloat,
    /// https://registry.khronos.org/webgl/extensions/EXT_render_snorm/
    ext_render_snorm::EXTRenderSnorm,
    /// https://registry.khronos.org/webgl/extensions/EXT_texture_filter_anisotropic/
    ext_texture_filter_anisotropic::EXTTextureFilterAnisotropic,
    /// https://registry.khronos.org/webgl/extensions/EXT_texture_norm16/
    ext_texture_norm16::EXTTextureNorm16,
    /// https://registry.khronos.org/webgl/extensions/OES_element_index_uint/
    oes_element_index_uint::OESElementIndexUint,
    /// https://registry.khronos.org/webgl/extensions/OES_standard_derivatives/
    oes_standard_derivatives::OESStandardDerivatives,
    /// https://registry.khronos.org/webgl/extensions/WEBGL_compressed_texture_s3tc/
    web_gl_compressed_texture_s3tc::WebGLCompressedTextureS3tc {
        compressed_texture_formats_enabled_when_created: [
            GL_COMPRESSED_RGB_S3TC_DXT1_EXT,
            GL_COMPRESSED_RGBA_S3TC_DXT1_EXT,
            GL_COMPRESSED_RGBA_S3TC_DXT3_EXT,
            GL_COMPRESSED_RGBA_S3TC_DXT5_EXT,
        ],
    },
    /// https://registry.khronos.org/webgl/extensions/WEBGL_compressed_texture_s3tc_srgb/
    web_gl_compressed_texture_s3tc_srgb::WebGLCompressedTextureS3tcSrgb {
        compressed_texture_formats_enabled_when_created: [
            GL_COMPRESSED_SRGB_S3TC_DXT1_EXT,
            GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT,
            GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT,
            GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT,
        ],
    },
    /// https://registry.khronos.org/webgl/extensions/WEBGL_debug_renderer_info/
    web_gl_debug_renderer_info::WebGLDebugRendererInfo,
}

// An entry expands to
//
//     pub(crate) mod web_gl_query {
//         pub(crate) struct WebGLQuery { object: WebGLObject }
//         impl Wrappable for WebGLQuery { .. }
//         impl Inherits<WebGLObject> for WebGLQuery { .. }
//         impl WebGLQuery { pub(crate) fn create(vm: &Vm, context: &ContextState, handle: u32) -> Ref<Self> { .. } }
//     }
#[cfg(not(test))]
web_gl_object::modules_of_interfaces_that_only_inherit_from_web_gl_object! {
    /// https://registry.khronos.org/webgl/specs/latest/2.0/#3.2
    web_gl_query::WebGLQuery,
    /// https://registry.khronos.org/webgl/specs/latest/1.0/#5.7
    web_gl_renderbuffer::WebGLRenderbuffer,
    /// https://registry.khronos.org/webgl/specs/latest/2.0/#3.3
    web_gl_sampler::WebGLSampler,
    /// https://registry.khronos.org/webgl/specs/latest/1.0/#5.9
    web_gl_texture::WebGLTexture,
    /// https://registry.khronos.org/webgl/specs/latest/2.0/#3.5
    web_gl_transform_feedback::WebGLTransformFeedback,
    /// https://registry.khronos.org/webgl/specs/latest/2.0/#3.6
    web_gl_vertex_array_object::WebGLVertexArrayObject,
    /// https://registry.khronos.org/webgl/extensions/OES_vertex_array_object/
    web_gl_vertex_array_object_oes::WebGLVertexArrayObjectOES,
}
