/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use ak::Utf16String;
use libjs_rust::api::{Gc, GcWeak, Object, Trace, Vm};

use super::canvas::expose_experimental_interfaces;
use super::gl_string::ascii_bytes;
use super::state::{ContextState, WebGLVersion};
use crate::bindings::CallContext;
use crate::bindings::interface::{Ref, create_dictionary_object, wrap};
use crate::bindings::realm_data::{WrapperWorld, wrapper_world_of};

macro_rules! enums_of_extension_interfaces {
    ($($module:ident :: $interface:ident),+ $(,)?) => {
        #[derive(Clone, Copy, Debug, PartialEq, Eq)]
        enum ExtensionInterface {
            $($interface,)+
        }

        impl ExtensionInterface {
            fn create(self, vm: &Vm, context: &ContextState) -> ExtensionObject {
                match self {
                    $(Self::$interface => {
                        ExtensionObject::$interface(super::$module::$interface::create(vm, context))
                    })+
                }
            }
        }

        #[derive(Clone, Copy, Trace)]
        #[gc(crate = libjs_rust)]
        pub(crate) enum ExtensionObject {
            $($interface(Ref<super::$module::$interface>),)+
        }

        impl ExtensionObject {
            fn wrap(self, cx: &CallContext) -> Gc<Object> {
                match self {
                    $(Self::$interface(extension) => wrap(cx, extension),)+
                }
            }
        }
    };
}

// Every extension interface that a row of AVAILABLE_WEBGL_EXTENSIONS can name, as `module::Interface`. Expands to
//
//     enum ExtensionInterface { ANGLEInstancedArrays, .. }
//     impl ExtensionInterface { fn create(self, vm: &Vm, context: &ContextState) -> ExtensionObject { .. } }
//     pub(crate) enum ExtensionObject { ANGLEInstancedArrays(Ref<ANGLEInstancedArrays>), .. }
//     impl ExtensionObject { fn wrap(self, cx: &CallContext) -> Gc<Object> { .. } }
enums_of_extension_interfaces! {
    angle_instanced_arrays::ANGLEInstancedArrays,
    ext_blend_min_max::EXTBlendMinMax,
    ext_color_buffer_float::EXTColorBufferFloat,
    ext_render_snorm::EXTRenderSnorm,
    ext_texture_filter_anisotropic::EXTTextureFilterAnisotropic,
    ext_texture_norm16::EXTTextureNorm16,
    oes_element_index_uint::OESElementIndexUint,
    oes_standard_derivatives::OESStandardDerivatives,
    oes_vertex_array_object::OESVertexArrayObject,
    web_gl_compressed_texture_s3tc::WebGLCompressedTextureS3tc,
    web_gl_compressed_texture_s3tc_srgb::WebGLCompressedTextureS3tcSrgb,
    web_gl_debug_renderer_info::WebGLDebugRendererInfo,
    web_gl_draw_buffers::WebGLDrawBuffers,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Kind {
    Interface(ExtensionInterface),
    EmptyObject,
    /// Offered only while the experimental interfaces are exposed.
    NotImplemented,
}

struct Extension {
    name: &'static str,
    required_angle_extensions: &'static [&'static str],
    kind: Kind,
    only_for_webgl_version: Option<WebGLVersion>,
}

// NB: The order is that of getSupportedExtensions(), which pages can observe, and it is not alphabetical.
#[rustfmt::skip]
static AVAILABLE_WEBGL_EXTENSIONS: [Extension; 53] = [
    Extension { name: "OES_fbo_render_mipmap", required_angle_extensions: &["GL_OES_fbo_render_mipmap"], kind: Kind::NotImplemented, only_for_webgl_version: Some(WebGLVersion::WebGL1) },
    Extension { name: "OES_texture_float", required_angle_extensions: &["GL_OES_texture_float"], kind: Kind::NotImplemented, only_for_webgl_version: Some(WebGLVersion::WebGL1) },
    Extension { name: "EXT_texture_mirror_clamp_to_edge", required_angle_extensions: &["GL_EXT_texture_mirror_clamp_to_edge"], kind: Kind::NotImplemented, only_for_webgl_version: None },
    Extension { name: "WEBGL_lose_context", required_angle_extensions: &[], kind: Kind::NotImplemented, only_for_webgl_version: None },
    Extension { name: "EXT_texture_norm16", required_angle_extensions: &["GL_EXT_texture_norm16"], kind: Kind::Interface(ExtensionInterface::EXTTextureNorm16), only_for_webgl_version: Some(WebGLVersion::WebGL2) },
    Extension { name: "NV_shader_noperspective_interpolation", required_angle_extensions: &["GL_NV_shader_noperspective_interpolation"], kind: Kind::NotImplemented, only_for_webgl_version: Some(WebGLVersion::WebGL2) },
    Extension { name: "OES_element_index_uint", required_angle_extensions: &["GL_OES_element_index_uint"], kind: Kind::Interface(ExtensionInterface::OESElementIndexUint), only_for_webgl_version: Some(WebGLVersion::WebGL1) },
    Extension { name: "OES_standard_derivatives", required_angle_extensions: &["GL_OES_standard_derivatives"], kind: Kind::Interface(ExtensionInterface::OESStandardDerivatives), only_for_webgl_version: Some(WebGLVersion::WebGL1) },
    Extension { name: "EXT_float_blend", required_angle_extensions: &["GL_EXT_float_blend"], kind: Kind::EmptyObject, only_for_webgl_version: None },
    Extension { name: "EXT_frag_depth", required_angle_extensions: &["GL_EXT_frag_depth"], kind: Kind::NotImplemented, only_for_webgl_version: Some(WebGLVersion::WebGL1) },
    Extension { name: "EXT_color_buffer_half_float", required_angle_extensions: &["GL_EXT_color_buffer_half_float"], kind: Kind::NotImplemented, only_for_webgl_version: None },
    Extension { name: "EXT_sRGB", required_angle_extensions: &["GL_EXT_sRGB"], kind: Kind::NotImplemented, only_for_webgl_version: Some(WebGLVersion::WebGL1) },
    Extension { name: "WEBGL_compressed_texture_astc", required_angle_extensions: &["KHR_texture_compression_astc_hdr", "KHR_texture_compression_astc_ldr"], kind: Kind::NotImplemented, only_for_webgl_version: None },
    Extension { name: "WEBGL_depth_texture", required_angle_extensions: &["GL_ANGLE_depth_texture"], kind: Kind::NotImplemented, only_for_webgl_version: Some(WebGLVersion::WebGL1) },
    Extension { name: "WEBGL_color_buffer_float", required_angle_extensions: &["EXT_color_buffer_half_float", "OES_texture_float"], kind: Kind::NotImplemented, only_for_webgl_version: Some(WebGLVersion::WebGL1) },
    Extension { name: "EXT_disjoint_timer_query", required_angle_extensions: &["GL_EXT_disjoint_timer_query"], kind: Kind::NotImplemented, only_for_webgl_version: Some(WebGLVersion::WebGL1) },
    Extension { name: "WEBGL_compressed_texture_s3tc_srgb", required_angle_extensions: &["GL_EXT_texture_compression_s3tc_srgb"], kind: Kind::Interface(ExtensionInterface::WebGLCompressedTextureS3tcSrgb), only_for_webgl_version: None },
    Extension { name: "EXT_texture_filter_anisotropic", required_angle_extensions: &["GL_EXT_texture_filter_anisotropic"], kind: Kind::Interface(ExtensionInterface::EXTTextureFilterAnisotropic), only_for_webgl_version: None },
    Extension { name: "EXT_disjoint_timer_query_webgl2", required_angle_extensions: &["GL_EXT_disjoint_timer_query"], kind: Kind::NotImplemented, only_for_webgl_version: Some(WebGLVersion::WebGL2) },
    Extension { name: "OES_texture_float_linear", required_angle_extensions: &["GL_OES_texture_float_linear"], kind: Kind::EmptyObject, only_for_webgl_version: None },
    Extension { name: "OES_vertex_array_object", required_angle_extensions: &["GL_OES_vertex_array_object"], kind: Kind::Interface(ExtensionInterface::OESVertexArrayObject), only_for_webgl_version: Some(WebGLVersion::WebGL1) },
    Extension { name: "WEBGL_clip_cull_distance", required_angle_extensions: &["GL_EXT_clip_cull_distance"], kind: Kind::NotImplemented, only_for_webgl_version: Some(WebGLVersion::WebGL2) },
    Extension { name: "ANGLE_instanced_arrays", required_angle_extensions: &["GL_ANGLE_instanced_arrays"], kind: Kind::Interface(ExtensionInterface::ANGLEInstancedArrays), only_for_webgl_version: Some(WebGLVersion::WebGL1) },
    Extension { name: "EXT_shader_texture_lod", required_angle_extensions: &["GL_EXT_shader_texture_lod"], kind: Kind::NotImplemented, only_for_webgl_version: Some(WebGLVersion::WebGL1) },
    Extension { name: "OES_texture_half_float", required_angle_extensions: &["GL_OES_texture_half_float"], kind: Kind::NotImplemented, only_for_webgl_version: Some(WebGLVersion::WebGL1) },
    Extension { name: "KHR_parallel_shader_compile", required_angle_extensions: &["GL_KHR_parallel_shader_compile"], kind: Kind::NotImplemented, only_for_webgl_version: None },
    Extension { name: "WEBGL_compressed_texture_pvrtc", required_angle_extensions: &["GL_IMG_texture_compression_pvrtc"], kind: Kind::NotImplemented, only_for_webgl_version: None },
    Extension { name: "EXT_clip_control", required_angle_extensions: &["GL_EXT_clip_control"], kind: Kind::NotImplemented, only_for_webgl_version: None },
    Extension { name: "EXT_blend_minmax", required_angle_extensions: &["GL_EXT_blend_minmax"], kind: Kind::Interface(ExtensionInterface::EXTBlendMinMax), only_for_webgl_version: Some(WebGLVersion::WebGL1) },
    Extension { name: "WEBGL_debug_renderer_info", required_angle_extensions: &[], kind: Kind::Interface(ExtensionInterface::WebGLDebugRendererInfo), only_for_webgl_version: None },
    Extension { name: "OES_sample_variables", required_angle_extensions: &["GL_OES_sample_variables"], kind: Kind::NotImplemented, only_for_webgl_version: Some(WebGLVersion::WebGL2) },
    Extension { name: "WEBGL_render_shared_exponent", required_angle_extensions: &["GL_QCOM_render_shared_exponent"], kind: Kind::NotImplemented, only_for_webgl_version: Some(WebGLVersion::WebGL2) },
    Extension { name: "WEBGL_debug_shaders", required_angle_extensions: &[], kind: Kind::NotImplemented, only_for_webgl_version: None },
    Extension { name: "OES_shader_multisample_interpolation", required_angle_extensions: &["GL_OES_shader_multisample_interpolation"], kind: Kind::NotImplemented, only_for_webgl_version: Some(WebGLVersion::WebGL2) },
    Extension { name: "EXT_color_buffer_float", required_angle_extensions: &["GL_EXT_color_buffer_float"], kind: Kind::Interface(ExtensionInterface::EXTColorBufferFloat), only_for_webgl_version: Some(WebGLVersion::WebGL2) },
    Extension { name: "OES_texture_half_float_linear", required_angle_extensions: &["GL_OES_texture_half_float_linear"], kind: Kind::NotImplemented, only_for_webgl_version: Some(WebGLVersion::WebGL1) },
    Extension { name: "EXT_texture_compression_rgtc", required_angle_extensions: &["GL_EXT_texture_compression_rgtc"], kind: Kind::NotImplemented, only_for_webgl_version: None },
    Extension { name: "EXT_render_snorm", required_angle_extensions: &["GL_EXT_render_snorm"], kind: Kind::Interface(ExtensionInterface::EXTRenderSnorm), only_for_webgl_version: Some(WebGLVersion::WebGL2) },
    Extension { name: "WEBGL_blend_func_extended", required_angle_extensions: &["GL_EXT_blend_func_extended"], kind: Kind::NotImplemented, only_for_webgl_version: None },
    Extension { name: "EXT_polygon_offset_clamp", required_angle_extensions: &["GL_EXT_polygon_offset_clamp"], kind: Kind::NotImplemented, only_for_webgl_version: None },
    Extension { name: "EXT_texture_compression_bptc", required_angle_extensions: &["GL_EXT_texture_compression_bptc"], kind: Kind::NotImplemented, only_for_webgl_version: None },
    Extension { name: "WEBGL_polygon_mode", required_angle_extensions: &["GL_ANGLE_polygon_mode"], kind: Kind::NotImplemented, only_for_webgl_version: None },
    Extension { name: "EXT_depth_clamp", required_angle_extensions: &["GL_EXT_depth_clamp"], kind: Kind::NotImplemented, only_for_webgl_version: None },
    Extension { name: "WEBGL_compressed_texture_etc", required_angle_extensions: &["GL_ANGLE_compressed_texture_etc"], kind: Kind::NotImplemented, only_for_webgl_version: None },
    Extension { name: "WEBGL_provoking_vertex", required_angle_extensions: &["GL_ANGLE_provoking_vertex"], kind: Kind::NotImplemented, only_for_webgl_version: Some(WebGLVersion::WebGL2) },
    Extension { name: "WEBGL_draw_buffers", required_angle_extensions: &["GL_EXT_draw_buffers"], kind: Kind::Interface(ExtensionInterface::WebGLDrawBuffers), only_for_webgl_version: Some(WebGLVersion::WebGL1) },
    Extension { name: "OVR_multiview2", required_angle_extensions: &["GL_OVR_multiview2"], kind: Kind::NotImplemented, only_for_webgl_version: Some(WebGLVersion::WebGL2) },
    Extension { name: "WEBGL_compressed_texture_etc1", required_angle_extensions: &["GL_OES_compressed_ETC1_RGB8_texture"], kind: Kind::NotImplemented, only_for_webgl_version: None },
    Extension { name: "WEBGL_compressed_texture_s3tc", required_angle_extensions: &["GL_EXT_texture_compression_dxt1", "GL_ANGLE_texture_compression_dxt3", "GL_ANGLE_texture_compression_dxt5"], kind: Kind::Interface(ExtensionInterface::WebGLCompressedTextureS3tc), only_for_webgl_version: None },
    Extension { name: "WEBGL_multi_draw", required_angle_extensions: &["GL_ANGLE_multi_draw"], kind: Kind::NotImplemented, only_for_webgl_version: None },
    Extension { name: "EXT_conservative_depth", required_angle_extensions: &["GL_EXT_conservative_depth"], kind: Kind::NotImplemented, only_for_webgl_version: Some(WebGLVersion::WebGL2) },
    Extension { name: "WEBGL_stencil_texturing", required_angle_extensions: &["GL_ANGLE_stencil_texturing"], kind: Kind::NotImplemented, only_for_webgl_version: Some(WebGLVersion::WebGL2) },
    Extension { name: "OES_draw_buffers_indexed", required_angle_extensions: &["GL_OES_draw_buffers_indexed"], kind: Kind::NotImplemented, only_for_webgl_version: None },
];

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct EnabledExtension {
    #[gc(untraced)]
    name: &'static str,
    object: ExtensionObject,
}

pub(crate) struct EnabledEmptyExtension {
    name: &'static str,
    objects: Vec<(WrapperWorld, GcWeak<Object>)>,
}

fn is_supported(context: &ContextState, extension: &Extension) -> bool {
    if extension
        .only_for_webgl_version
        .is_some_and(|version| version != context.version())
    {
        return false;
    }
    if extension.kind == Kind::NotImplemented && !expose_experimental_interfaces() {
        return false;
    }
    let opengl_extensions = context.supported_opengl_extensions.borrow();
    extension.required_angle_extensions.iter().all(|required| {
        opengl_extensions
            .iter()
            .any(|supported| supported == required.as_bytes())
    })
}

impl ContextState {
    pub(crate) fn supported_extensions(&self) -> Vec<Utf16String> {
        AVAILABLE_WEBGL_EXTENSIONS
            .iter()
            .filter(|extension| is_supported(self, extension))
            .map(|extension| Utf16String::from_ascii(extension.name.as_bytes()))
            .collect()
    }

    /// https://registry.khronos.org/webgl/specs/latest/1.0/#5.14.14
    pub(crate) fn extension(&self, cx: &CallContext, name: &Utf16String) -> Option<Gc<Object>> {
        // Returns an object if, and only if, name is an ASCII case-insensitive match [HTML] for one of the names
        // returned from getSupportedExtensions; otherwise, returns null. The object returned from getExtension
        // contains any constants or functions provided by the extension. A returned object may have no constants or
        // functions if the extension does not define any, but a unique object must still be returned. That object is
        // used to indicate that the extension has been enabled.
        let name = ascii_bytes(name)?;
        let extension = AVAILABLE_WEBGL_EXTENSIONS
            .iter()
            .find(|extension| extension.name.as_bytes().eq_ignore_ascii_case(&name) && is_supported(self, extension))?;

        // "Multiple calls to getExtension with the same extension string, taking into account case-insensitive
        // comparison, must return the same object as long as the extension is enabled."
        let enabled_object = self
            .enabled_extensions
            .borrow()
            .iter()
            .find(|enabled| enabled.name == extension.name)
            .map(|enabled| enabled.object);
        if let Some(object) = enabled_object {
            return Some(object.wrap(cx));
        }
        let wrapper_world = wrapper_world_of(cx.vm(), cx.realm());
        let enabled_empty_object = self
            .enabled_empty_extensions
            .borrow()
            .iter()
            .find(|enabled| enabled.name == extension.name)
            .and_then(|enabled| enabled.objects.iter().find(|(world, _)| *world == wrapper_world))
            .and_then(|(_, object)| object.get());
        if let Some(object) = enabled_empty_object {
            return Some(object);
        }

        if extension.kind == Kind::NotImplemented {
            return None;
        }
        for required_extension in extension.required_angle_extensions {
            self.proxy.request_extension_angle(required_extension.as_bytes());
        }

        match extension.kind {
            Kind::Interface(interface) => {
                let object = interface.create(cx.vm(), self);
                self.enabled_extensions.borrow_mut(cx.vm()).push(EnabledExtension {
                    name: extension.name,
                    object,
                });
                Some(object.wrap(cx))
            }
            Kind::EmptyObject => {
                let object = create_dictionary_object(cx);
                let weak_object = GcWeak::new(cx.vm().heap(), object);
                let mut enabled_empty_extensions = self.enabled_empty_extensions.borrow_mut();
                let index = match enabled_empty_extensions
                    .iter()
                    .position(|enabled| enabled.name == extension.name)
                {
                    Some(index) => index,
                    None => {
                        enabled_empty_extensions.push(EnabledEmptyExtension {
                            name: extension.name,
                            objects: Vec::new(),
                        });
                        enabled_empty_extensions.len() - 1
                    }
                };
                let objects = &mut enabled_empty_extensions[index].objects;
                objects.retain(|(world, _)| *world != wrapper_world);
                objects.push((wrapper_world, weak_object));
                Some(object)
            }
            Kind::NotImplemented => unreachable!("returned above"),
        }
    }

    // FIXME: Make this and any other instance of extension names an interned string, as the names of HTML tags are.
    pub(crate) fn extension_enabled(&self, name: &str) -> bool {
        let is_enabled = self
            .enabled_extensions
            .borrow()
            .iter()
            .any(|enabled| enabled.name.eq_ignore_ascii_case(name));
        is_enabled
            || self
                .enabled_empty_extensions
                .borrow()
                .iter()
                .any(|enabled| enabled.name.eq_ignore_ascii_case(name))
    }

    pub(crate) fn enable_compressed_texture_format(&self, format: u32) {
        self.enabled_compressed_texture_formats.borrow_mut().push(format);
    }
}

/// An entry is `module::Interface`, or `module::Interface { compressed_texture_formats_enabled_when_created: [..] }`
/// with the formats in the order that getParameter(COMPRESSED_TEXTURE_FORMATS) reports them in.
macro_rules! modules_of_extension_interfaces_without_operations {
    ($(
        $(#[$specification_link:meta])*
        $module:ident :: $interface:ident
        $({ compressed_texture_formats_enabled_when_created: [$($format:ident),+ $(,)?] $(,)? })?
    ),+ $(,)?) => {$(
        $(#[$specification_link])*
        pub(crate) mod $module {
            use libjs_rust::api::{Gc, Realm, Trace, Vm};

            use $crate::bindings::interface::{Ref, Wrappable, allocate};
            use $crate::web_gl::state::{ContextRef, ContextState};

            #[derive(Trace)]
            #[gc(crate = libjs_rust)]
            pub(crate) struct $interface {
                context: ContextRef,
            }

            impl Wrappable for $interface {
                fn relevant_realm(&self, _vm: &Vm) -> Option<Gc<Realm>> {
                    self.context.state().canvas().relevant_realm()
                }
            }

            impl $interface {
                pub(crate) fn create(vm: &Vm, context: &ContextState) -> Ref<Self> {
                    $($(context.enable_compressed_texture_format($crate::web_gl::constants::$format);)+)?
                    allocate(
                        vm,
                        |_| Self {
                            context: context.context_ref(),
                        },
                    )
                }
            }
        }
    )+};
}
pub(crate) use modules_of_extension_interfaces_without_operations;
