/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use ak::Utf16String;
use libjs_rust::api::Value;

use super::byte_ranges::gl_sizei_of_array_length;
use super::constants::{
    GL_ACTIVE_ATTRIBUTES, GL_ACTIVE_TEXTURE, GL_ACTIVE_UNIFORM_BLOCKS, GL_ACTIVE_UNIFORMS, GL_ALIASED_LINE_WIDTH_RANGE,
    GL_ALIASED_POINT_SIZE_RANGE, GL_ALPHA_BITS, GL_ARRAY_BUFFER_BINDING, GL_ATTACHED_SHADERS, GL_BLEND, GL_BLEND_COLOR,
    GL_BLEND_DST_ALPHA, GL_BLEND_DST_RGB, GL_BLEND_EQUATION_ALPHA, GL_BLEND_EQUATION_RGB, GL_BLEND_SRC_ALPHA,
    GL_BLEND_SRC_RGB, GL_BLUE_BITS, GL_BUFFER_SIZE, GL_BUFFER_USAGE, GL_COLOR_CLEAR_VALUE, GL_COLOR_WRITEMASK,
    GL_COMPILE_STATUS, GL_COMPRESSED_TEXTURE_FORMATS, GL_COPY_READ_BUFFER_BINDING, GL_COPY_WRITE_BUFFER_BINDING,
    GL_CULL_FACE, GL_CULL_FACE_MODE, GL_CURRENT_PROGRAM, GL_CURRENT_VERTEX_ATTRIB, GL_DELETE_STATUS, GL_DEPTH_BITS,
    GL_DEPTH_CLEAR_VALUE, GL_DEPTH_FUNC, GL_DEPTH_RANGE, GL_DEPTH_TEST, GL_DEPTH_WRITEMASK, GL_DITHER,
    GL_ELEMENT_ARRAY_BUFFER_BINDING, GL_FRAGMENT_SHADER_DERIVATIVE_HINT, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME,
    GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_CUBE_MAP_FACE,
    GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL, GL_FRAMEBUFFER_BINDING, GL_FRONT_FACE, GL_GENERATE_MIPMAP_HINT,
    GL_GREEN_BITS, GL_IMPLEMENTATION_COLOR_READ_FORMAT, GL_IMPLEMENTATION_COLOR_READ_TYPE, GL_INVALID_ENUM,
    GL_INVALID_OPERATION, GL_LINE_WIDTH, GL_LINK_STATUS, GL_MAX_3D_TEXTURE_SIZE, GL_MAX_ARRAY_TEXTURE_LAYERS,
    GL_MAX_CLIENT_WAIT_TIMEOUT_WEBGL, GL_MAX_COLOR_ATTACHMENTS, GL_MAX_COMBINED_FRAGMENT_UNIFORM_COMPONENTS,
    GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, GL_MAX_COMBINED_UNIFORM_BLOCKS, GL_MAX_COMBINED_VERTEX_UNIFORM_COMPONENTS,
    GL_MAX_CUBE_MAP_TEXTURE_SIZE, GL_MAX_DRAW_BUFFERS, GL_MAX_ELEMENT_INDEX, GL_MAX_ELEMENTS_INDICES,
    GL_MAX_ELEMENTS_VERTICES, GL_MAX_FRAGMENT_INPUT_COMPONENTS, GL_MAX_FRAGMENT_UNIFORM_BLOCKS,
    GL_MAX_FRAGMENT_UNIFORM_COMPONENTS, GL_MAX_FRAGMENT_UNIFORM_VECTORS, GL_MAX_PROGRAM_TEXEL_OFFSET,
    GL_MAX_RENDERBUFFER_SIZE, GL_MAX_SAMPLES, GL_MAX_SERVER_WAIT_TIMEOUT, GL_MAX_TEXTURE_IMAGE_UNITS,
    GL_MAX_TEXTURE_LOD_BIAS, GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, GL_MAX_TEXTURE_SIZE,
    GL_MAX_TRANSFORM_FEEDBACK_INTERLEAVED_COMPONENTS, GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_ATTRIBS,
    GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_COMPONENTS, GL_MAX_UNIFORM_BLOCK_SIZE, GL_MAX_UNIFORM_BUFFER_BINDINGS,
    GL_MAX_VARYING_COMPONENTS, GL_MAX_VARYING_VECTORS, GL_MAX_VERTEX_ATTRIBS, GL_MAX_VERTEX_OUTPUT_COMPONENTS,
    GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS, GL_MAX_VERTEX_UNIFORM_BLOCKS, GL_MAX_VERTEX_UNIFORM_COMPONENTS,
    GL_MAX_VERTEX_UNIFORM_VECTORS, GL_MAX_VIEWPORT_DIMS, GL_MIN_PROGRAM_TEXEL_OFFSET, GL_NONE, GL_PACK_ALIGNMENT,
    GL_PACK_ROW_LENGTH, GL_PACK_SKIP_PIXELS, GL_PACK_SKIP_ROWS, GL_PIXEL_PACK_BUFFER_BINDING,
    GL_PIXEL_UNPACK_BUFFER_BINDING, GL_POLYGON_OFFSET_FACTOR, GL_POLYGON_OFFSET_FILL, GL_POLYGON_OFFSET_UNITS,
    GL_RASTERIZER_DISCARD, GL_RED_BITS, GL_RENDERBUFFER, GL_RENDERBUFFER_ALPHA_SIZE, GL_RENDERBUFFER_BINDING,
    GL_RENDERBUFFER_BLUE_SIZE, GL_RENDERBUFFER_DEPTH_SIZE, GL_RENDERBUFFER_GREEN_SIZE, GL_RENDERBUFFER_HEIGHT,
    GL_RENDERBUFFER_INTERNAL_FORMAT, GL_RENDERBUFFER_RED_SIZE, GL_RENDERBUFFER_SAMPLES, GL_RENDERBUFFER_STENCIL_SIZE,
    GL_RENDERBUFFER_WIDTH, GL_RENDERER, GL_SAMPLE_ALPHA_TO_COVERAGE, GL_SAMPLE_BUFFERS, GL_SAMPLE_COVERAGE,
    GL_SAMPLE_COVERAGE_INVERT, GL_SAMPLE_COVERAGE_VALUE, GL_SAMPLER_BINDING, GL_SAMPLES, GL_SCISSOR_BOX,
    GL_SCISSOR_TEST, GL_SHADER_TYPE, GL_SHADING_LANGUAGE_VERSION, GL_STENCIL_BACK_FAIL, GL_STENCIL_BACK_FUNC,
    GL_STENCIL_BACK_PASS_DEPTH_FAIL, GL_STENCIL_BACK_PASS_DEPTH_PASS, GL_STENCIL_BACK_REF, GL_STENCIL_BACK_VALUE_MASK,
    GL_STENCIL_BACK_WRITEMASK, GL_STENCIL_BITS, GL_STENCIL_CLEAR_VALUE, GL_STENCIL_FAIL, GL_STENCIL_FUNC,
    GL_STENCIL_PASS_DEPTH_FAIL, GL_STENCIL_PASS_DEPTH_PASS, GL_STENCIL_REF, GL_STENCIL_TEST, GL_STENCIL_VALUE_MASK,
    GL_STENCIL_WRITEMASK, GL_SUBPIXEL_BITS, GL_TEXTURE, GL_TEXTURE_BASE_LEVEL, GL_TEXTURE_BINDING_2D,
    GL_TEXTURE_BINDING_2D_ARRAY, GL_TEXTURE_BINDING_CUBE_MAP, GL_TEXTURE_COMPARE_FUNC, GL_TEXTURE_COMPARE_MODE,
    GL_TEXTURE_CUBE_MAP_NEGATIVE_Z, GL_TEXTURE_CUBE_MAP_POSITIVE_X, GL_TEXTURE_IMMUTABLE_FORMAT,
    GL_TEXTURE_IMMUTABLE_LEVELS, GL_TEXTURE_MAG_FILTER, GL_TEXTURE_MAX_ANISOTROPY_EXT, GL_TEXTURE_MAX_LEVEL,
    GL_TEXTURE_MAX_LOD, GL_TEXTURE_MIN_FILTER, GL_TEXTURE_MIN_LOD, GL_TEXTURE_WRAP_R, GL_TEXTURE_WRAP_S,
    GL_TEXTURE_WRAP_T, GL_TRANSFORM_FEEDBACK_ACTIVE, GL_TRANSFORM_FEEDBACK_BINDING,
    GL_TRANSFORM_FEEDBACK_BUFFER_BINDING, GL_TRANSFORM_FEEDBACK_BUFFER_MODE, GL_TRANSFORM_FEEDBACK_PAUSED,
    GL_TRANSFORM_FEEDBACK_VARYINGS, GL_TRUE, GL_UNIFORM_BUFFER_BINDING, GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT,
    GL_UNMASKED_RENDERER_WEBGL, GL_UNMASKED_VENDOR_WEBGL, GL_UNPACK_ALIGNMENT, GL_UNPACK_COLORSPACE_CONVERSION_WEBGL,
    GL_UNPACK_FLIP_Y_WEBGL, GL_UNPACK_IMAGE_HEIGHT, GL_UNPACK_PREMULTIPLY_ALPHA_WEBGL, GL_UNPACK_ROW_LENGTH,
    GL_UNPACK_SKIP_IMAGES, GL_UNPACK_SKIP_PIXELS, GL_UNPACK_SKIP_ROWS, GL_VALIDATE_STATUS, GL_VENDOR, GL_VERSION,
    GL_VERTEX_ARRAY_BINDING, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, GL_VERTEX_ATTRIB_ARRAY_DIVISOR,
    GL_VERTEX_ATTRIB_ARRAY_ENABLED, GL_VERTEX_ATTRIB_ARRAY_INTEGER, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED,
    GL_VERTEX_ATTRIB_ARRAY_SIZE, GL_VERTEX_ATTRIB_ARRAY_STRIDE, GL_VERTEX_ATTRIB_ARRAY_TYPE, GL_VIEWPORT,
};
use super::gl_string::utf16_string_from_gl_string;
use super::state::{ContextState, RenderingContext, Version, VertexArrayBinding};
use super::values::{object_or_null, string_value};
use super::web_gl_buffer::WebGLBuffer;
use super::web_gl_program::WebGLProgram;
use super::web_gl_rendering_context_base::{is_valid_framebuffer_attachment, is_valid_framebuffer_binding_target};
use super::web_gl_sampler::WebGLSampler;
use super::web_gl_shader::WebGLShader;
use super::web_gl_uniform_location::WebGLUniformLocation;

unsafe extern "C" {
    safe fn web_navigator_compatibility_mode() -> u8;
}

/// The navigator compatibility mode, numbered as Web::NavigatorCompatibilityMode is.
const MODE_CHROME: u8 = 0;
const MODE_GECKO: u8 = 1;
const MODE_WEBKIT: u8 = 2;

fn navigator_compatibility_mode() -> u8 {
    web_navigator_compatibility_mode()
}

/// VENDOR, RENDERER, VERSION, and SHADING_LANGUAGE_VERSION name the browser's WebGL implementation rather than the
/// GPU, whose own strings are only behind WEBGL_debug_renderer_info. Each engine words them its own way, so the answer
/// follows the navigator compatibility mode.
fn masked_string_parameter(context: &ContextState, cx: &CallContext, pname: u32) -> Value {
    let mode = navigator_compatibility_mode();
    let webgl2 = context.is_webgl2();
    let text: &str = match (pname, mode) {
        (GL_VENDOR, MODE_GECKO) => "Mozilla",
        (GL_VENDOR, _) => "WebKit",
        (GL_RENDERER, MODE_GECKO) => return string_value(cx, sanitized_renderer(context)),
        (GL_RENDERER, _) => "WebKit WebGL",
        (GL_VERSION, MODE_CHROME) if webgl2 => "WebGL 2.0 (OpenGL ES 3.0 Chromium)",
        (GL_VERSION, MODE_CHROME) => "WebGL 1.0 (OpenGL ES 2.0 Chromium)",
        (GL_VERSION, _) if webgl2 => "WebGL 2.0",
        (GL_VERSION, _) => "WebGL 1.0",
        (GL_SHADING_LANGUAGE_VERSION, MODE_CHROME) if webgl2 => "WebGL GLSL ES 3.00 (OpenGL ES GLSL ES 3.0 Chromium)",
        (GL_SHADING_LANGUAGE_VERSION, MODE_CHROME) => "WebGL GLSL ES 1.0 (OpenGL ES GLSL ES 1.0 Chromium)",
        (GL_SHADING_LANGUAGE_VERSION, _) if webgl2 => "WebGL GLSL ES 3.00",
        (GL_SHADING_LANGUAGE_VERSION, MODE_WEBKIT) => "WebGL GLSL ES 1.0 (1.0)",
        (GL_SHADING_LANGUAGE_VERSION, _) => "WebGL GLSL ES 1.0",
        _ => return string_parameter(context, cx, pname),
    };
    string_value(cx, Utf16String::from_ascii(text.as_bytes()))
}

/// The unmasked vendor: the GPU's, except that a navigator claiming WebKit on macOS answers as that engine does there,
/// and one claiming Gecko answers with the vendor's bare name.
fn unmasked_vendor(context: &ContextState, cx: &CallContext) -> Value {
    let vendor = context.proxy.get_string(GL_VENDOR);
    let text: Option<&str> = match navigator_compatibility_mode() {
        MODE_WEBKIT if cfg!(target_os = "macos") => Some("Apple Inc."),
        MODE_GECKO => gpu_vendor_name(&vendor, &context.proxy.get_string(GL_RENDERER)),
        _ => None,
    };
    match text {
        Some(text) => string_value(cx, Utf16String::from_ascii(text.as_bytes())),
        None => string_value(cx, utf16_string_from_gl_string(&vendor)),
    }
}

/// The unmasked renderer: the GPU's, except that a navigator claiming WebKit on macOS answers as that engine does
/// there, and one claiming Gecko answers with a representative device of the GPU's family.
fn unmasked_renderer(context: &ContextState, cx: &CallContext) -> Value {
    match navigator_compatibility_mode() {
        MODE_WEBKIT if cfg!(target_os = "macos") => string_value(cx, Utf16String::from_ascii(b"Apple GPU")),
        MODE_GECKO => string_value(cx, sanitized_renderer(context)),
        _ => string_parameter(context, cx, GL_RENDERER),
    }
}

/// The GPU vendor's bare name, found in either GL string (an ANGLE renderer string names the vendor in its own
/// way), or None for a vendor without one.
fn gpu_vendor_name(vendor: &[u8], renderer: &[u8]) -> Option<&'static str> {
    let contains = |needle: &str| {
        let needle = needle.as_bytes();
        [vendor, renderer]
            .iter()
            .any(|haystack| haystack.windows(needle.len()).any(|window| window == needle))
    };
    if contains("Apple") {
        Some("Apple")
    } else if contains("NVIDIA") {
        Some("NVIDIA Corporation")
    } else if contains("Intel") {
        Some("Intel")
    } else if contains("AMD") || contains("ATI") {
        Some("AMD")
    } else if contains("Qualcomm") {
        Some("Qualcomm")
    } else if contains("ARM") {
        Some("ARM")
    } else if contains("Mesa") || contains("X.Org") {
        Some("Mesa")
    } else {
        None
    }
}

/// A Gecko-shaped renderer string: a representative device for the GPU's family, marked ", or similar". An Apple GPU
/// of any generation reads "Apple M1, or similar"; another GPU keeps its own string with the same marker.
fn sanitized_renderer(context: &ContextState) -> Utf16String {
    let renderer = context.proxy.get_string(GL_RENDERER);
    if gpu_vendor_name(&context.proxy.get_string(GL_VENDOR), &renderer) == Some("Apple") {
        return Utf16String::from_ascii(b"Apple M1, or similar");
    }
    let mut text = String::from_utf8_lossy(&renderer).into_owned();
    text.push_str(", or similar");
    Utf16String::from_utf8(&text)
}
use crate::bindings::interface::{Ref, wrap};
use crate::bindings::{CallContext, WebIdlResult};

fn is_cube_map_face(target: u32) -> bool {
    (GL_TEXTURE_CUBE_MAP_POSITIVE_X..=GL_TEXTURE_CUBE_MAP_NEGATIVE_Z).contains(&target)
}

fn is_gl_true(integer_that_gl_returned_for_a_boolean: i32) -> bool {
    u32::try_from(integer_that_gl_returned_for_a_boolean) == Ok(GL_TRUE)
}

fn number_value(unsigned_integer_or_float: impl Into<f64>) -> Value {
    Value::from_f64(unsigned_integer_or_float.into())
}

fn null_with_invalid_enum_error(context: &ContextState) -> Value {
    context.set_error(GL_INVALID_ENUM);
    Value::NULL
}

fn integer_parameter(context: &ContextState, pname: u32) -> i32 {
    let mut result = [0];
    context.proxy.get_integerv_robust_angle(pname, 1, &mut result);
    result[0]
}

fn integer64_parameter(context: &ContextState, pname: u32) -> i64 {
    let mut result = [0];
    context.proxy.get_integer64v_robust_angle(pname, 1, &mut result);
    result[0]
}

fn float_parameter(context: &ContextState, pname: u32) -> f32 {
    let mut result = [0.0];
    context.proxy.get_floatv_robust_angle(pname, 1, &mut result);
    result[0]
}

fn boolean_parameter(context: &ContextState, pname: u32) -> bool {
    let mut result = [0];
    context.proxy.get_booleanv_robust_angle(pname, 1, &mut result);
    u32::from(result[0]) == GL_TRUE
}

fn int32_array_parameter<const LENGTH: usize>(context: &ContextState, cx: &CallContext, pname: u32) -> Value {
    let mut result = [0; LENGTH];
    context
        .proxy
        .get_integerv_robust_angle(pname, const { gl_sizei_of_array_length(LENGTH) }, &mut result);
    context.int32_array(cx, &result)
}

fn float32_array_parameter<const LENGTH: usize>(context: &ContextState, cx: &CallContext, pname: u32) -> Value {
    let mut result = [0.0; LENGTH];
    context
        .proxy
        .get_floatv_robust_angle(pname, const { gl_sizei_of_array_length(LENGTH) }, &mut result);
    context.float32_array(cx, &result)
}

fn string_parameter(context: &ContextState, cx: &CallContext, name: u32) -> Value {
    string_value(cx, utf16_string_from_gl_string(&context.proxy.get_string(name)))
}

fn integer_tex_parameter(context: &ContextState, target: u32, pname: u32) -> i32 {
    let mut result = [0];
    context
        .proxy
        .get_tex_parameteriv_robust_angle(target, pname, 1, &mut result);
    result[0]
}

fn integer_vertex_attrib(context: &ContextState, index: u32, pname: u32) -> i32 {
    let mut result = [0];
    context
        .proxy
        .get_vertex_attribiv_robust_angle(index, pname, 1, &mut result);
    result[0]
}

fn parameter_of_either_version(context: &ContextState, cx: &CallContext, pname: u32) -> Option<Value> {
    Some(match pname {
        GL_ACTIVE_TEXTURE
        | GL_ALPHA_BITS
        | GL_BLEND_DST_ALPHA
        | GL_BLEND_DST_RGB
        | GL_BLEND_EQUATION_ALPHA
        | GL_BLEND_EQUATION_RGB
        | GL_BLEND_SRC_ALPHA
        | GL_BLEND_SRC_RGB
        | GL_BLUE_BITS
        | GL_CULL_FACE_MODE
        | GL_DEPTH_BITS
        | GL_DEPTH_FUNC
        | GL_FRONT_FACE
        | GL_GENERATE_MIPMAP_HINT
        | GL_GREEN_BITS
        | GL_IMPLEMENTATION_COLOR_READ_FORMAT
        | GL_IMPLEMENTATION_COLOR_READ_TYPE
        | GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS
        | GL_MAX_CUBE_MAP_TEXTURE_SIZE
        | GL_MAX_FRAGMENT_UNIFORM_VECTORS
        | GL_MAX_RENDERBUFFER_SIZE
        | GL_MAX_TEXTURE_IMAGE_UNITS
        | GL_MAX_TEXTURE_SIZE
        | GL_MAX_VARYING_VECTORS
        | GL_MAX_VERTEX_ATTRIBS
        | GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS
        | GL_MAX_VERTEX_UNIFORM_VECTORS
        | GL_PACK_ALIGNMENT
        | GL_RED_BITS
        | GL_SAMPLE_BUFFERS
        | GL_SAMPLES
        | GL_STENCIL_BACK_FAIL
        | GL_STENCIL_BACK_FUNC
        | GL_STENCIL_BACK_PASS_DEPTH_FAIL
        | GL_STENCIL_BACK_PASS_DEPTH_PASS
        | GL_STENCIL_BACK_REF
        | GL_STENCIL_BACK_VALUE_MASK
        | GL_STENCIL_BACK_WRITEMASK
        | GL_STENCIL_BITS
        | GL_STENCIL_CLEAR_VALUE
        | GL_STENCIL_FAIL
        | GL_STENCIL_FUNC
        | GL_STENCIL_PASS_DEPTH_FAIL
        | GL_STENCIL_PASS_DEPTH_PASS
        | GL_STENCIL_REF
        | GL_STENCIL_VALUE_MASK
        | GL_STENCIL_WRITEMASK
        | GL_SUBPIXEL_BITS
        | GL_UNPACK_ALIGNMENT => Value::from_i32(integer_parameter(context, pname)),

        GL_DEPTH_CLEAR_VALUE
        | GL_LINE_WIDTH
        | GL_POLYGON_OFFSET_FACTOR
        | GL_POLYGON_OFFSET_UNITS
        | GL_SAMPLE_COVERAGE_VALUE => number_value(float_parameter(context, pname)),

        GL_BLEND
        | GL_CULL_FACE
        | GL_DEPTH_TEST
        | GL_DEPTH_WRITEMASK
        | GL_DITHER
        | GL_POLYGON_OFFSET_FILL
        | GL_SAMPLE_ALPHA_TO_COVERAGE
        | GL_SAMPLE_COVERAGE
        | GL_SAMPLE_COVERAGE_INVERT
        | GL_SCISSOR_TEST
        | GL_STENCIL_TEST => Value::from_bool(boolean_parameter(context, pname)),

        GL_ALIASED_LINE_WIDTH_RANGE | GL_ALIASED_POINT_SIZE_RANGE | GL_DEPTH_RANGE => {
            float32_array_parameter::<2>(context, cx, pname)
        }
        GL_BLEND_COLOR | GL_COLOR_CLEAR_VALUE => float32_array_parameter::<4>(context, cx, pname),
        GL_MAX_VIEWPORT_DIMS => int32_array_parameter::<2>(context, cx, pname),
        GL_SCISSOR_BOX | GL_VIEWPORT => int32_array_parameter::<4>(context, cx, pname),
        GL_COLOR_WRITEMASK => {
            let mut result = [0; 4];
            context
                .proxy
                .get_booleanv_robust_angle(GL_COLOR_WRITEMASK, 4, &mut result);
            context.boolean_array(cx, &result.map(|is_written| is_written != 0))
        }

        GL_RENDERER | GL_SHADING_LANGUAGE_VERSION | GL_VENDOR | GL_VERSION => {
            masked_string_parameter(context, cx, pname)
        }

        GL_ARRAY_BUFFER_BINDING => object_or_null(cx, context.array_buffer_binding.get()),
        GL_CURRENT_PROGRAM => object_or_null(cx, context.current_program.get()),
        GL_ELEMENT_ARRAY_BUFFER_BINDING => object_or_null(cx, context.element_array_buffer_binding.get()),
        GL_FRAMEBUFFER_BINDING => object_or_null(cx, context.framebuffer_binding.get()),
        GL_RENDERBUFFER_BINDING => object_or_null(cx, context.renderbuffer_binding.get()),
        GL_TEXTURE_BINDING_2D => object_or_null(cx, context.texture_binding_2d.get()),
        GL_TEXTURE_BINDING_CUBE_MAP => object_or_null(cx, context.texture_binding_cube_map.get()),

        GL_UNMASKED_VENDOR_WEBGL => {
            if context.extension_enabled("WEBGL_debug_renderer_info") {
                unmasked_vendor(context, cx)
            } else {
                null_with_invalid_enum_error(context)
            }
        }
        GL_UNMASKED_RENDERER_WEBGL => {
            if context.extension_enabled("WEBGL_debug_renderer_info") {
                unmasked_renderer(context, cx)
            } else {
                null_with_invalid_enum_error(context)
            }
        }

        // NB: This has the same value as FRAGMENT_SHADER_DERIVATIVE_HINT_OES.
        GL_FRAGMENT_SHADER_DERIVATIVE_HINT => {
            if context.extension_enabled("OES_standard_derivatives") || context.is_webgl2() {
                Value::from_i32(integer_parameter(context, pname))
            } else {
                null_with_invalid_enum_error(context)
            }
        }
        // NB: This has the same value as MAX_COLOR_ATTACHMENTS_WEBGL.
        GL_MAX_COLOR_ATTACHMENTS => {
            if context.extension_enabled("WEBGL_draw_buffers") || context.is_webgl2() {
                Value::from_i32(integer_parameter(context, pname))
            } else {
                null_with_invalid_enum_error(context)
            }
        }
        GL_MAX_DRAW_BUFFERS => {
            // FIXME: Allow this code path for MAX_DRAW_BUFFERS_WEBGL
            if context.is_webgl2() {
                Value::from_i32(integer_parameter(context, pname))
            } else {
                null_with_invalid_enum_error(context)
            }
        }
        GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT => {
            if context.extension_enabled("EXT_texture_filter_anisotropic") {
                number_value(float_parameter(context, pname))
            } else {
                null_with_invalid_enum_error(context)
            }
        }
        // NB: This has the same value as VERTEX_ARRAY_BINDING_OES.
        GL_VERTEX_ARRAY_BINDING => {
            if context.extension_enabled("OES_vertex_array_object") || context.is_webgl2() {
                match context.current_vertex_array.get() {
                    Some(VertexArrayBinding::WebGL2(vertex_array)) => Value::from_object(wrap(cx, vertex_array)),
                    Some(VertexArrayBinding::Oes(vertex_array)) => Value::from_object(wrap(cx, vertex_array)),
                    None => Value::NULL,
                }
            } else {
                null_with_invalid_enum_error(context)
            }
        }

        GL_COMPRESSED_TEXTURE_FORMATS => {
            let formats = context.enabled_compressed_texture_formats.borrow().clone();
            context.uint32_array(cx, &formats)
        }
        GL_UNPACK_FLIP_Y_WEBGL => Value::from_bool(context.unpack_flip_y.get()),
        GL_UNPACK_PREMULTIPLY_ALPHA_WEBGL => Value::from_bool(context.unpack_premultiply_alpha.get()),
        GL_UNPACK_COLORSPACE_CONVERSION_WEBGL => number_value(context.unpack_colorspace_conversion.get()),

        _ => return None,
    })
}

fn parameter_of_webgl2(context: &ContextState, cx: &CallContext, pname: u32) -> Option<Value> {
    Some(match pname {
        GL_MAX_SAMPLES
        | GL_MAX_3D_TEXTURE_SIZE
        | GL_MAX_ARRAY_TEXTURE_LAYERS
        | GL_MAX_VERTEX_UNIFORM_COMPONENTS
        | GL_MAX_UNIFORM_BUFFER_BINDINGS
        | GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT
        | GL_MAX_VERTEX_UNIFORM_BLOCKS
        | GL_MAX_FRAGMENT_INPUT_COMPONENTS
        | GL_MAX_FRAGMENT_UNIFORM_COMPONENTS
        | GL_MAX_COMBINED_UNIFORM_BLOCKS
        | GL_MAX_FRAGMENT_UNIFORM_BLOCKS
        | GL_MAX_VARYING_COMPONENTS
        | GL_MAX_ELEMENTS_INDICES
        | GL_MAX_ELEMENTS_VERTICES
        | GL_MAX_TRANSFORM_FEEDBACK_INTERLEAVED_COMPONENTS
        | GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_ATTRIBS
        | GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_COMPONENTS
        | GL_MIN_PROGRAM_TEXEL_OFFSET
        | GL_MAX_PROGRAM_TEXEL_OFFSET
        | GL_MAX_VERTEX_OUTPUT_COMPONENTS
        | GL_PACK_ROW_LENGTH
        | GL_PACK_SKIP_ROWS
        | GL_PACK_SKIP_PIXELS
        | GL_UNPACK_IMAGE_HEIGHT
        | GL_UNPACK_ROW_LENGTH
        | GL_UNPACK_SKIP_IMAGES
        | GL_UNPACK_SKIP_PIXELS
        | GL_UNPACK_SKIP_ROWS => Value::from_i32(integer_parameter(context, pname)),

        GL_MAX_UNIFORM_BLOCK_SIZE
        | GL_MAX_COMBINED_VERTEX_UNIFORM_COMPONENTS
        | GL_MAX_COMBINED_FRAGMENT_UNIFORM_COMPONENTS
        | GL_MAX_ELEMENT_INDEX
        | GL_MAX_SERVER_WAIT_TIMEOUT => Value::from_f64(integer64_parameter(context, pname) as f64),

        GL_MAX_TEXTURE_LOD_BIAS => number_value(float_parameter(context, pname)),

        GL_TRANSFORM_FEEDBACK_ACTIVE | GL_TRANSFORM_FEEDBACK_PAUSED | GL_RASTERIZER_DISCARD => {
            Value::from_bool(boolean_parameter(context, pname))
        }

        GL_COPY_READ_BUFFER_BINDING => object_or_null(cx, context.copy_read_buffer_binding.get()),
        GL_COPY_WRITE_BUFFER_BINDING => object_or_null(cx, context.copy_write_buffer_binding.get()),
        GL_PIXEL_PACK_BUFFER_BINDING => object_or_null(cx, context.pixel_pack_buffer_binding.get()),
        GL_PIXEL_UNPACK_BUFFER_BINDING => object_or_null(cx, context.pixel_unpack_buffer_binding.get()),
        GL_TEXTURE_BINDING_2D_ARRAY => object_or_null(cx, context.texture_binding_2d_array.get()),
        GL_TRANSFORM_FEEDBACK_BINDING => object_or_null(cx, context.transform_feedback_binding.get()),
        GL_TRANSFORM_FEEDBACK_BUFFER_BINDING => object_or_null(cx, context.transform_feedback_buffer_binding.get()),
        GL_UNIFORM_BUFFER_BINDING => object_or_null(cx, context.uniform_buffer_binding.get()),
        // FIXME: Return the sampler that was bound, or null without one, instead of a new object for the name that GL
        //        has bound.
        GL_SAMPLER_BINDING => {
            let handle = integer_parameter(context, pname).cast_unsigned();
            Value::from_object(wrap(cx, WebGLSampler::create(cx.vm(), context, handle)))
        }

        // A page must never be able to block the compositor, so clientWaitSync never waits; the host clamps the
        // timeout to zero to match.
        GL_MAX_CLIENT_WAIT_TIMEOUT_WEBGL => Value::from_i32(0),

        _ => return None,
    })
}

impl<V: Version> RenderingContext<V> {
    pub(crate) fn get_buffer_parameter(&self, _: &mut CallContext, target: u32, pname: u32) -> WebIdlResult<Value> {
        match pname {
            GL_BUFFER_SIZE | GL_BUFFER_USAGE => {
                let mut result = [0];
                self.proxy
                    .get_buffer_parameteriv_robust_angle(target, pname, 1, &mut result);
                Ok(Value::from_i32(result[0]))
            }
            _ => Ok(null_with_invalid_enum_error(self)),
        }
    }

    pub(crate) fn get_parameter(&self, cx: &mut CallContext, pname: u32) -> WebIdlResult<Value> {
        if let Some(value) = parameter_of_either_version(self, cx, pname) {
            return Ok(value);
        }
        if self.is_webgl2()
            && let Some(value) = parameter_of_webgl2(self, cx, pname)
        {
            return Ok(value);
        }
        Ok(null_with_invalid_enum_error(self))
    }

    pub(crate) fn get_framebuffer_attachment_parameter(
        &self,
        cx: &mut CallContext,
        target: u32,
        attachment: u32,
        pname: u32,
    ) -> WebIdlResult<Value> {
        if !is_valid_framebuffer_binding_target(self.version(), target) {
            return Ok(null_with_invalid_enum_error(self));
        }

        let Some(framebuffer) = self.framebuffer_binding.get() else {
            self.set_error(GL_INVALID_OPERATION);
            return Ok(Value::NULL);
        };

        if !is_valid_framebuffer_attachment(self.version(), self.extension_enabled("WEBGL_draw_buffers"), attachment) {
            return Ok(null_with_invalid_enum_error(self));
        }

        let Some(framebuffer_attachment) = framebuffer.attachment(attachment) else {
            if pname == GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE {
                return Ok(number_value(GL_NONE));
            }
            if pname == GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME && self.is_webgl2() {
                return Ok(Value::NULL);
            }
            self.set_error(if self.is_webgl2() {
                GL_INVALID_OPERATION
            } else {
                GL_INVALID_ENUM
            });
            return Ok(Value::NULL);
        };

        let is_texture_attached = framebuffer_attachment.texture.is_some();
        match pname {
            GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE => Ok(number_value(if is_texture_attached {
                GL_TEXTURE
            } else {
                GL_RENDERBUFFER
            })),
            GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME => match framebuffer_attachment.texture {
                Some(texture) => Ok(Value::from_object(wrap(cx, texture))),
                None => Ok(object_or_null(cx, framebuffer_attachment.renderbuffer)),
            },
            GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL if is_texture_attached => {
                Ok(Value::from_i32(framebuffer_attachment.texture_level))
            }
            GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_CUBE_MAP_FACE if is_texture_attached => {
                let texture_target = framebuffer_attachment.texture_target;
                Ok(number_value(if is_cube_map_face(texture_target) {
                    texture_target
                } else {
                    0
                }))
            }
            _ => Ok(null_with_invalid_enum_error(self)),
        }
    }

    pub(crate) fn get_program_parameter(
        &self,
        _: &mut CallContext,
        program: Ref<WebGLProgram>,
        pname: u32,
    ) -> WebIdlResult<Value> {
        let Some(program_handle) = self.handle_of(program) else {
            return Ok(Value::NULL);
        };

        let mut result = [0];
        self.proxy
            .get_programiv_robust_angle(program_handle, pname, 1, &mut result);
        let result = result[0];
        match pname {
            GL_ATTACHED_SHADERS | GL_ACTIVE_ATTRIBUTES | GL_ACTIVE_UNIFORMS => Ok(Value::from_i32(result)),

            GL_TRANSFORM_FEEDBACK_BUFFER_MODE | GL_TRANSFORM_FEEDBACK_VARYINGS | GL_ACTIVE_UNIFORM_BLOCKS => {
                if self.is_webgl2() {
                    Ok(Value::from_i32(result))
                } else {
                    Ok(null_with_invalid_enum_error(self))
                }
            }

            GL_DELETE_STATUS | GL_LINK_STATUS | GL_VALIDATE_STATUS => Ok(Value::from_bool(is_gl_true(result))),
            _ => Ok(null_with_invalid_enum_error(self)),
        }
    }

    pub(crate) fn get_renderbuffer_parameter(
        &self,
        _: &mut CallContext,
        target: u32,
        pname: u32,
    ) -> WebIdlResult<Value> {
        match pname {
            GL_RENDERBUFFER_WIDTH
            | GL_RENDERBUFFER_HEIGHT
            | GL_RENDERBUFFER_INTERNAL_FORMAT
            | GL_RENDERBUFFER_RED_SIZE
            | GL_RENDERBUFFER_GREEN_SIZE
            | GL_RENDERBUFFER_BLUE_SIZE
            | GL_RENDERBUFFER_ALPHA_SIZE
            | GL_RENDERBUFFER_DEPTH_SIZE
            | GL_RENDERBUFFER_SAMPLES
            | GL_RENDERBUFFER_STENCIL_SIZE => {
                let mut result = [0];
                self.proxy
                    .get_renderbuffer_parameteriv_robust_angle(target, pname, 1, &mut result);
                Ok(Value::from_i32(result[0]))
            }
            // If pname is not in the table above, generates an INVALID_ENUM error.
            _ => Ok(null_with_invalid_enum_error(self)),
        }
    }

    pub(crate) fn get_shader_parameter(
        &self,
        _: &mut CallContext,
        shader: Ref<WebGLShader>,
        pname: u32,
    ) -> WebIdlResult<Value> {
        let Some(shader_handle) = self.handle_of(shader) else {
            return Ok(Value::NULL);
        };

        let mut result = [0];
        self.proxy
            .get_shaderiv_robust_angle(shader_handle, pname, 1, &mut result);
        let result = result[0];
        match pname {
            GL_SHADER_TYPE => Ok(Value::from_i32(result)),
            GL_DELETE_STATUS | GL_COMPILE_STATUS => Ok(Value::from_bool(is_gl_true(result))),
            _ => Ok(null_with_invalid_enum_error(self)),
        }
    }

    pub(crate) fn get_tex_parameter(&self, _: &mut CallContext, target: u32, pname: u32) -> WebIdlResult<Value> {
        match pname {
            GL_TEXTURE_MAG_FILTER | GL_TEXTURE_MIN_FILTER | GL_TEXTURE_WRAP_S | GL_TEXTURE_WRAP_T => {
                return Ok(Value::from_i32(integer_tex_parameter(self, target, pname)));
            }
            GL_TEXTURE_MAX_ANISOTROPY_EXT => {
                if self.extension_enabled("EXT_texture_filter_anisotropic") {
                    return Ok(Value::from_i32(integer_tex_parameter(self, target, pname)));
                }
                return Ok(null_with_invalid_enum_error(self));
            }
            _ => {}
        }

        if self.is_webgl2() {
            match pname {
                GL_TEXTURE_BASE_LEVEL
                | GL_TEXTURE_COMPARE_FUNC
                | GL_TEXTURE_COMPARE_MODE
                | GL_TEXTURE_IMMUTABLE_LEVELS
                | GL_TEXTURE_MAX_LEVEL
                | GL_TEXTURE_WRAP_R => {
                    return Ok(Value::from_i32(integer_tex_parameter(self, target, pname)));
                }
                GL_TEXTURE_IMMUTABLE_FORMAT => {
                    return Ok(Value::from_bool(is_gl_true(integer_tex_parameter(self, target, pname))));
                }
                // FIXME: Return the level of detail, instead of whether TEXTURE_IMMUTABLE_FORMAT is set.
                GL_TEXTURE_MAX_LOD | GL_TEXTURE_MIN_LOD => {
                    let mut result = [0.0];
                    self.proxy
                        .get_tex_parameterfv_robust_angle(target, GL_TEXTURE_IMMUTABLE_FORMAT, 1, &mut result);
                    return Ok(Value::from_bool(result[0] == 1.0));
                }
                _ => {}
            }
        }

        Ok(null_with_invalid_enum_error(self))
    }

    pub(crate) fn get_uniform(
        &self,
        _: &mut CallContext,
        _: Ref<WebGLProgram>,
        _: Ref<WebGLUniformLocation>,
    ) -> WebIdlResult<Value> {
        // FIXME: Implement this.
        Ok(Value::from_i32(0))
    }

    pub(crate) fn get_vertex_attrib(&self, cx: &mut CallContext, index: u32, pname: u32) -> WebIdlResult<Value> {
        match pname {
            GL_CURRENT_VERTEX_ATTRIB => {
                let mut result = [0.0; 4];
                self.proxy
                    .get_vertex_attribfv_robust_angle(index, GL_CURRENT_VERTEX_ATTRIB, 4, &mut result);
                Ok(self.float32_array(cx, &result))
            }
            // FIXME: Return the buffer that was bound, or null without one, instead of a new object for the name that
            //        GL has bound.
            GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING => {
                let handle = integer_vertex_attrib(self, index, pname).cast_unsigned();
                Ok(Value::from_object(wrap(cx, WebGLBuffer::create(cx.vm(), self, handle))))
            }
            // NB: This has the same value as VERTEX_ATTRIB_ARRAY_DIVISOR_ANGLE.
            GL_VERTEX_ATTRIB_ARRAY_DIVISOR => {
                if self.extension_enabled("ANGLE_instanced_arrays") || self.is_webgl2() {
                    Ok(Value::from_i32(integer_vertex_attrib(self, index, pname)))
                } else {
                    Ok(null_with_invalid_enum_error(self))
                }
            }
            GL_VERTEX_ATTRIB_ARRAY_ENABLED | GL_VERTEX_ATTRIB_ARRAY_NORMALIZED => {
                Ok(Value::from_bool(is_gl_true(integer_vertex_attrib(self, index, pname))))
            }
            GL_VERTEX_ATTRIB_ARRAY_INTEGER => {
                if self.is_webgl2() {
                    Ok(Value::from_bool(is_gl_true(integer_vertex_attrib(self, index, pname))))
                } else {
                    Ok(null_with_invalid_enum_error(self))
                }
            }
            GL_VERTEX_ATTRIB_ARRAY_SIZE | GL_VERTEX_ATTRIB_ARRAY_STRIDE | GL_VERTEX_ATTRIB_ARRAY_TYPE => {
                Ok(Value::from_i32(integer_vertex_attrib(self, index, pname)))
            }
            _ => Ok(null_with_invalid_enum_error(self)),
        }
    }
}
