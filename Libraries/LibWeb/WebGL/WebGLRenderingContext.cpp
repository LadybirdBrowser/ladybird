/*
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2023, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/WebGLRenderingContextPrototype.h>
#include <LibWeb/HTML/HTMLCanvasElement.h>
#include <LibWeb/HTML/TraversableNavigable.h>
#include <LibWeb/Infra/Strings.h>
#include <LibWeb/Painting/Paintable.h>
#include <LibWeb/WebGL/EventNames.h>
#include <LibWeb/WebGL/Extensions/ANGLEInstancedArrays.h>
#include <LibWeb/WebGL/Extensions/EXTBlendMinMax.h>
#include <LibWeb/WebGL/Extensions/EXTTextureFilterAnisotropic.h>
#include <LibWeb/WebGL/Extensions/OESElementIndexUint.h>
#include <LibWeb/WebGL/Extensions/OESStandardDerivatives.h>
#include <LibWeb/WebGL/Extensions/OESVertexArrayObject.h>
#include <LibWeb/WebGL/Extensions/WebGLCompressedTextureS3tc.h>
#include <LibWeb/WebGL/Extensions/WebGLCompressedTextureS3tcSrgb.h>
#include <LibWeb/WebGL/Extensions/WebGLDrawBuffers.h>
#include <LibWeb/WebGL/OpenGLContext.h>
#include <LibWeb/WebGL/WebGLContextEvent.h>
#include <LibWeb/WebGL/WebGLRenderingContext.h>
#include <LibWeb/WebGL/WebGLShader.h>
#include <LibWeb/WebIDL/Buffers.h>

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

namespace Web::WebGL {

GC_DEFINE_ALLOCATOR(WebGLRenderingContext);

// https://www.khronos.org/registry/webgl/specs/latest/1.0/#fire-a-webgl-context-event
void fire_webgl_context_event(HTML::HTMLCanvasElement& canvas_element, FlyString const& type)
{
    // To fire a WebGL context event named e means that an event using the WebGLContextEvent interface, with its type attribute [DOM4] initialized to e, its cancelable attribute initialized to true, and its isTrusted attribute [DOM4] initialized to true, is to be dispatched at the given object.
    // FIXME: Consider setting a status message.
    auto event = WebGLContextEvent::create(canvas_element.realm(), type, WebGLContextEventInit {});
    event->set_is_trusted(true);
    event->set_cancelable(true);
    canvas_element.dispatch_event(*event);
}

// https://www.khronos.org/registry/webgl/specs/latest/1.0/#fire-a-webgl-context-creation-error
void fire_webgl_context_creation_error(HTML::HTMLCanvasElement& canvas_element)
{
    // 1. Fire a WebGL context event named "webglcontextcreationerror" at canvas, optionally with its statusMessage attribute set to a platform dependent string about the nature of the failure.
    fire_webgl_context_event(canvas_element, EventNames::webglcontextcreationerror);
}

JS::ThrowCompletionOr<GC::Ptr<WebGLRenderingContext>> WebGLRenderingContext::create(JS::Realm& realm, HTML::HTMLCanvasElement& canvas_element, JS::Value options)
{
    // We should be coming here from getContext being called on a wrapped <canvas> element.
    auto context_attributes = TRY(convert_value_to_context_attributes_dictionary(canvas_element.vm(), options));

    auto skia_backend_context = canvas_element.navigable()->traversable_navigable()->skia_backend_context();
    if (!skia_backend_context) {
        fire_webgl_context_creation_error(canvas_element);
        return GC::Ptr<WebGLRenderingContext> { nullptr };
    }
    auto context = OpenGLContext::create(*skia_backend_context, OpenGLContext::WebGLVersion::WebGL1);
    if (!context) {
        fire_webgl_context_creation_error(canvas_element);
        return GC::Ptr<WebGLRenderingContext> { nullptr };
    }

    context->set_size(canvas_element.bitmap_size_for_canvas(1, 1));

    return realm.create<WebGLRenderingContext>(realm, canvas_element, context.release_nonnull(), context_attributes, context_attributes);
}

WebGLRenderingContext::WebGLRenderingContext(JS::Realm& realm, HTML::HTMLCanvasElement& canvas_element, NonnullOwnPtr<OpenGLContext> context, WebGLContextAttributes context_creation_parameters, WebGLContextAttributes actual_context_parameters)
    : WebGLRenderingContextOverloads(realm, move(context))
    , m_canvas_element(canvas_element)
    , m_context_creation_parameters(context_creation_parameters)
    , m_actual_context_parameters(actual_context_parameters)
{
}

WebGLRenderingContext::~WebGLRenderingContext() = default;

void WebGLRenderingContext::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(WebGLRenderingContext);
    Base::initialize(realm);
}

void WebGLRenderingContext::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    WebGLRenderingContextImpl::visit_edges(visitor);
    visitor.visit(m_canvas_element);
    visitor.visit(m_angle_instanced_arrays_extension);
    visitor.visit(m_ext_blend_min_max_extension);
    visitor.visit(m_ext_texture_filter_anisotropic);
    visitor.visit(m_oes_element_index_uint_object_extension);
    visitor.visit(m_oes_standard_derivatives_object_extension);
    visitor.visit(m_oes_vertex_array_object_extension);
    visitor.visit(m_webgl_compressed_texture_s3tc_extension);
    visitor.visit(m_webgl_compressed_texture_s3tc_srgb_extension);
    visitor.visit(m_webgl_draw_buffers_extension);
}

void WebGLRenderingContext::present()
{
    if (!m_should_present)
        return;

    m_should_present = false;
    context().present(m_context_creation_parameters.preserve_drawing_buffer);
}

GC::Ref<HTML::HTMLCanvasElement> WebGLRenderingContext::canvas_for_binding() const
{
    return *m_canvas_element;
}

void WebGLRenderingContext::needs_to_present()
{
    m_should_present = true;

    if (!m_canvas_element->paintable())
        return;
    m_canvas_element->paintable()->set_needs_display();
}

bool WebGLRenderingContext::is_context_lost() const
{
    dbgln_if(WEBGL_CONTEXT_DEBUG, "WebGLRenderingContext::is_context_lost()");
    return m_context_lost;
}

Optional<WebGLContextAttributes> WebGLRenderingContext::get_context_attributes()
{
    if (is_context_lost())
        return {};
    return m_actual_context_parameters;
}

void WebGLRenderingContext::set_size(Gfx::IntSize const& size)
{
    Gfx::IntSize final_size;
    final_size.set_width(max(size.width(), 1));
    final_size.set_height(max(size.height(), 1));
    context().set_size(final_size);
}

void WebGLRenderingContext::reset_to_default_state()
{
}

RefPtr<Gfx::PaintingSurface> WebGLRenderingContext::surface()
{
    return context().surface();
}

void WebGLRenderingContext::allocate_painting_surface_if_needed()
{
    context().allocate_painting_surface_if_needed();
}

Optional<Vector<String>> WebGLRenderingContext::get_supported_extensions()
{
    return context().get_supported_extensions();
}

JS::Object* WebGLRenderingContext::get_extension(String const& name)
{
    // Returns an object if, and only if, name is an ASCII case-insensitive match [HTML] for one of the names returned
    // from getSupportedExtensions; otherwise, returns null. The object returned from getExtension contains any constants
    // or functions provided by the extension. A returned object may have no constants or functions if the extension does
    // not define any, but a unique object must still be returned. That object is used to indicate that the extension has
    // been enabled.
    auto supported_extensions = get_supported_extensions();
    auto supported_extension_iterator = supported_extensions->find_if([&name](String const& supported_extension) {
        return supported_extension.equals_ignoring_ascii_case(name);
    });
    if (supported_extension_iterator == supported_extensions->end())
        return nullptr;

    if (name.equals_ignoring_ascii_case("ANGLE_instanced_arrays"sv)) {
        if (!m_angle_instanced_arrays_extension) {
            m_angle_instanced_arrays_extension = MUST(Extensions::ANGLEInstancedArrays::create(realm(), *this));
        }

        VERIFY(m_angle_instanced_arrays_extension);
        return m_angle_instanced_arrays_extension;
    }

    if (name.equals_ignoring_ascii_case("EXT_blend_minmax"sv)) {
        if (!m_ext_blend_min_max_extension) {
            m_ext_blend_min_max_extension = MUST(Extensions::EXTBlendMinMax::create(realm(), *this));
        }

        VERIFY(m_ext_blend_min_max_extension);
        return m_ext_blend_min_max_extension;
    }

    if (name.equals_ignoring_ascii_case("EXT_texture_filter_anisotropic"sv)) {
        if (!m_ext_texture_filter_anisotropic) {
            m_ext_texture_filter_anisotropic = MUST(Extensions::EXTTextureFilterAnisotropic::create(realm(), *this));
        }

        VERIFY(m_ext_texture_filter_anisotropic);
        return m_ext_texture_filter_anisotropic;
    }

    if (name.equals_ignoring_ascii_case("OES_element_index_uint"sv)) {
        if (!m_oes_element_index_uint_object_extension) {
            m_oes_element_index_uint_object_extension = MUST(Extensions::OESElementIndexUint::create(realm(), *this));
        }

        VERIFY(m_oes_element_index_uint_object_extension);
        return m_oes_element_index_uint_object_extension;
    }

    if (name.equals_ignoring_ascii_case("OES_standard_derivatives"sv)) {
        if (!m_oes_standard_derivatives_object_extension) {
            m_oes_standard_derivatives_object_extension = MUST(Extensions::OESStandardDerivatives::create(realm(), *this));
        }

        VERIFY(m_oes_standard_derivatives_object_extension);
        return m_oes_standard_derivatives_object_extension;
    }

    if (name.equals_ignoring_ascii_case("OES_vertex_array_object"sv)) {
        if (!m_oes_vertex_array_object_extension) {
            m_oes_vertex_array_object_extension = MUST(Extensions::OESVertexArrayObject::create(realm(), *this));
        }

        VERIFY(m_oes_vertex_array_object_extension);
        return m_oes_vertex_array_object_extension;
    }

    if (name.equals_ignoring_ascii_case("WEBGL_compressed_texture_s3tc"sv)) {
        if (!m_webgl_compressed_texture_s3tc_extension) {
            m_webgl_compressed_texture_s3tc_extension = MUST(Extensions::WebGLCompressedTextureS3tc::create(realm(), *this));

            m_enabled_compressed_texture_formats.append(GL_COMPRESSED_RGB_S3TC_DXT1_EXT);
            m_enabled_compressed_texture_formats.append(GL_COMPRESSED_RGBA_S3TC_DXT1_EXT);
            m_enabled_compressed_texture_formats.append(GL_COMPRESSED_RGBA_S3TC_DXT3_EXT);
            m_enabled_compressed_texture_formats.append(GL_COMPRESSED_RGBA_S3TC_DXT5_EXT);
        }

        VERIFY(m_webgl_compressed_texture_s3tc_extension);
        return m_webgl_compressed_texture_s3tc_extension;
    }

    if (name.equals_ignoring_ascii_case("WEBGL_compressed_texture_s3tc_srgb"sv)) {
        if (!m_webgl_compressed_texture_s3tc_srgb_extension) {
            m_webgl_compressed_texture_s3tc_srgb_extension = MUST(Extensions::WebGLCompressedTextureS3tcSrgb::create(realm(), *this));

            m_enabled_compressed_texture_formats.append(GL_COMPRESSED_SRGB_S3TC_DXT1_EXT);
            m_enabled_compressed_texture_formats.append(GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT);
            m_enabled_compressed_texture_formats.append(GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT);
            m_enabled_compressed_texture_formats.append(GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT);
        }

        VERIFY(m_webgl_compressed_texture_s3tc_srgb_extension);
        return m_webgl_compressed_texture_s3tc_srgb_extension;
    }

    if (name.equals_ignoring_ascii_case("WEBGL_draw_buffers"sv)) {
        if (!m_webgl_draw_buffers_extension) {
            m_webgl_draw_buffers_extension = MUST(Extensions::WebGLDrawBuffers::create(realm(), *this));
        }

        VERIFY(m_webgl_draw_buffers_extension);
        return m_webgl_draw_buffers_extension;
    }

    return nullptr;
}

WebIDL::Long WebGLRenderingContext::drawing_buffer_width() const
{
    auto size = canvas_for_binding()->bitmap_size_for_canvas();
    return size.width();
}

WebIDL::Long WebGLRenderingContext::drawing_buffer_height() const
{
    auto size = canvas_for_binding()->bitmap_size_for_canvas();
    return size.height();
}

bool WebGLRenderingContext::ext_texture_filter_anisotropic_extension_enabled() const
{
    return !!m_ext_texture_filter_anisotropic;
}

bool WebGLRenderingContext::angle_instanced_arrays_extension_enabled() const
{
    return !!m_angle_instanced_arrays_extension;
}

bool WebGLRenderingContext::oes_standard_derivatives_extension_enabled() const
{
    return !!m_oes_standard_derivatives_object_extension;
}

bool WebGLRenderingContext::webgl_draw_buffers_extension_enabled() const
{
    return !!m_webgl_draw_buffers_extension;
}

ReadonlySpan<WebIDL::UnsignedLong> WebGLRenderingContext::enabled_compressed_texture_formats() const
{
    return m_enabled_compressed_texture_formats;
}

}
