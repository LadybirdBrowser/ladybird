/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "BindingsGenerator/IDLGenerators.h"

#include <AK/LexicalPath.h>
#include <AK/SourceGenerator.h>
#include <AK/StringBuilder.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/File.h>
#include <LibIDL/IDLParser.h>
#include <LibMain/Main.h>

static bool is_webgl_object_type(StringView type_name)
{
    return type_name == "WebGLBuffer"sv
        || type_name == "WebGLFramebuffer"sv
        || type_name == "WebGLProgram"sv
        || type_name == "WebGLRenderbuffer"sv
        || type_name == "WebGLShader"sv
        || type_name == "WebGLTexture"sv
        || type_name == "WebGLUniformLocation"sv
        || type_name == "WebGLVertexArrayObject"sv;
}

static bool gl_function_modifies_framebuffer(StringView function_name)
{
    return function_name == "clearColor"sv || function_name == "drawArrays"sv || function_name == "drawElements"sv;
}

static ByteString to_cpp_type(const IDL::Type& type, const IDL::Interface& interface)
{
    if (type.name() == "undefined"sv)
        return "void"sv;
    if (type.name() == "object"sv) {
        if (type.is_nullable())
            return "JS::Object*"sv;
        return "JS::Object&"sv;
    }
    if (type.name() == "DOMString"sv) {
        if (type.is_nullable())
            return "Optional<String>"sv;
        return "String"sv;
    }
    auto cpp_type = idl_type_name_to_cpp_type(type, interface);
    return cpp_type.name;
}

static ByteString idl_to_gl_function_name(StringView function_name)
{
    StringBuilder gl_function_name_builder;
    gl_function_name_builder.append("gl"sv);
    for (size_t i = 0; i < function_name.length(); ++i) {
        if (i == 0) {
            gl_function_name_builder.append(to_ascii_uppercase(function_name[i]));
        } else {
            gl_function_name_builder.append(function_name[i]);
        }
    }
    if (function_name == "clearDepth"sv || function_name == "depthRange"sv) {
        gl_function_name_builder.append("f"sv);
    }
    return gl_function_name_builder.to_byte_string();
}

struct NameAndType {
    StringView name;
    struct {
        StringView type;
        int element_count { 0 };
    } return_type;
};

static void generate_get_parameter(SourceGenerator& generator)
{
    Vector<NameAndType> const name_to_type = {
        { "ACTIVE_TEXTURE"sv, { "GLenum"sv } },
        { "ALIASED_LINE_WIDTH_RANGE"sv, { "Float32Array"sv, 2 } },
        { "ALIASED_POINT_SIZE_RANGE"sv, { "Float32Array"sv, 2 } },
        { "ALPHA_BITS"sv, { "GLint"sv } },
        { "ARRAY_BUFFER_BINDING"sv, { "WebGLBuffer"sv } },
        { "BLEND"sv, { "GLboolean"sv } },
        { "BLEND_COLOR"sv, { "Float32Array"sv, 4 } },
        { "BLEND_DST_ALPHA"sv, { "GLenum"sv } },
        { "BLEND_DST_RGB"sv, { "GLenum"sv } },
        { "BLEND_EQUATION_ALPHA"sv, { "GLenum"sv } },
        { "BLEND_EQUATION_RGB"sv, { "GLenum"sv } },
        { "BLEND_SRC_ALPHA"sv, { "GLenum"sv } },
        { "BLEND_SRC_RGB"sv, { "GLenum"sv } },
        { "BLUE_BITS"sv, { "GLint"sv } },
        { "COLOR_CLEAR_VALUE"sv, { "Float32Array"sv, 4 } },
        // FIXME: { "COLOR_WRITEMASK"sv, { "sequence<GLboolean>"sv, 4 } },
        // FIXME: { "COMPRESSED_TEXTURE_FORMATS"sv, { "Uint32Array"sv } },
        { "CULL_FACE"sv, { "GLboolean"sv } },
        { "CULL_FACE_MODE"sv, { "GLenum"sv } },
        { "CURRENT_PROGRAM"sv, { "WebGLProgram"sv } },
        { "DEPTH_BITS"sv, { "GLint"sv } },
        { "DEPTH_CLEAR_VALUE"sv, { "GLfloat"sv } },
        { "DEPTH_FUNC"sv, { "GLenum"sv } },
        { "DEPTH_RANGE"sv, { "Float32Array"sv, 2 } },
        { "DEPTH_TEST"sv, { "GLboolean"sv } },
        { "DEPTH_WRITEMASK"sv, { "GLboolean"sv } },
        { "DITHER"sv, { "GLboolean"sv } },
        { "ELEMENT_ARRAY_BUFFER_BINDING"sv, { "WebGLBuffer"sv } },
        { "FRAMEBUFFER_BINDING"sv, { "WebGLFramebuffer"sv } },
        { "FRONT_FACE"sv, { "GLenum"sv } },
        { "GENERATE_MIPMAP_HINT"sv, { "GLenum"sv } },
        { "GREEN_BITS"sv, { "GLint"sv } },
        { "IMPLEMENTATION_COLOR_READ_FORMAT"sv, { "GLenum"sv } },
        { "IMPLEMENTATION_COLOR_READ_TYPE"sv, { "GLenum"sv } },
        { "LINE_WIDTH"sv, { "GLfloat"sv } },
        { "MAX_COMBINED_TEXTURE_IMAGE_UNITS"sv, { "GLint"sv } },
        { "MAX_CUBE_MAP_TEXTURE_SIZE"sv, { "GLint"sv } },
        { "MAX_FRAGMENT_UNIFORM_VECTORS"sv, { "GLint"sv } },
        { "MAX_RENDERBUFFER_SIZE"sv, { "GLint"sv } },
        { "MAX_TEXTURE_IMAGE_UNITS"sv, { "GLint"sv } },
        { "MAX_TEXTURE_SIZE"sv, { "GLint"sv } },
        { "MAX_VARYING_VECTORS"sv, { "GLint"sv } },
        { "MAX_VERTEX_ATTRIBS"sv, { "GLint"sv } },
        { "MAX_VERTEX_TEXTURE_IMAGE_UNITS"sv, { "GLint"sv } },
        { "MAX_VERTEX_UNIFORM_VECTORS"sv, { "GLint"sv } },
        { "MAX_VIEWPORT_DIMS"sv, { "Int32Array"sv, 2 } },
        { "PACK_ALIGNMENT"sv, { "GLint"sv } },
        { "POLYGON_OFFSET_FACTOR"sv, { "GLfloat"sv } },
        { "POLYGON_OFFSET_FILL"sv, { "GLboolean"sv } },
        { "POLYGON_OFFSET_UNITS"sv, { "GLfloat"sv } },
        { "RED_BITS"sv, { "GLint"sv } },
        { "RENDERBUFFER_BINDING"sv, { "WebGLRenderbuffer"sv } },
        { "RENDERER"sv, { "DOMString"sv } },
        { "SAMPLE_ALPHA_TO_COVERAGE"sv, { "GLboolean"sv } },
        { "SAMPLE_BUFFERS"sv, { "GLint"sv } },
        { "SAMPLE_COVERAGE"sv, { "GLboolean"sv } },
        { "SAMPLE_COVERAGE_INVERT"sv, { "GLboolean"sv } },
        { "SAMPLE_COVERAGE_VALUE"sv, { "GLfloat"sv } },
        { "SAMPLES"sv, { "GLint"sv } },
        { "SCISSOR_BOX"sv, { "Int32Array"sv, 4 } },
        { "SCISSOR_TEST"sv, { "GLboolean"sv } },
        { "SHADING_LANGUAGE_VERSION"sv, { "DOMString"sv } },
        { "STENCIL_BACK_FAIL"sv, { "GLenum"sv } },
        { "STENCIL_BACK_FUNC"sv, { "GLenum"sv } },
        { "STENCIL_BACK_PASS_DEPTH_FAIL"sv, { "GLenum"sv } },
        { "STENCIL_BACK_PASS_DEPTH_PASS"sv, { "GLenum"sv } },
        { "STENCIL_BACK_REF"sv, { "GLint"sv } },
        { "STENCIL_BACK_VALUE_MASK"sv, { "GLuint"sv } },
        { "STENCIL_BACK_WRITEMASK"sv, { "GLuint"sv } },
        { "STENCIL_BITS"sv, { "GLint"sv } },
        { "STENCIL_CLEAR_VALUE"sv, { "GLint"sv } },
        { "STENCIL_FAIL"sv, { "GLenum"sv } },
        { "STENCIL_FUNC"sv, { "GLenum"sv } },
        { "STENCIL_PASS_DEPTH_FAIL"sv, { "GLenum"sv } },
        { "STENCIL_PASS_DEPTH_PASS"sv, { "GLenum"sv } },
        { "STENCIL_REF"sv, { "GLint"sv } },
        { "STENCIL_TEST"sv, { "GLboolean"sv } },
        { "STENCIL_VALUE_MASK"sv, { "GLuint"sv } },
        { "STENCIL_WRITEMASK"sv, { "GLuint"sv } },
        { "SUBPIXEL_BITS"sv, { "GLint"sv } },
        { "TEXTURE_BINDING_2D"sv, { "WebGLTexture"sv } },
        { "TEXTURE_BINDING_CUBE_MAP"sv, { "WebGLTexture"sv } },
        { "UNPACK_ALIGNMENT"sv, { "GLint"sv } },
        // FIXME: { "UNPACK_COLORSPACE_CONVERSION_WEBGL"sv, { "GLenum"sv } },
        // FIXME: { "UNPACK_FLIP_Y_WEBGL"sv, { "GLboolean"sv } },
        // FIXME: { "UNPACK_PREMULTIPLY_ALPHA_WEBGL"sv, { "GLboolean"sv } },
        { "VENDOR"sv, { "DOMString"sv } },
        { "VERSION"sv, { "DOMString"sv } },
        { "VIEWPORT"sv, { "Int32Array"sv, 4 } },
    };

    auto is_primitive_type = [](StringView type) {
        return type == "GLboolean"sv || type == "GLint"sv || type == "GLfloat"sv || type == "GLenum"sv || type == "GLuint"sv;
    };

    generator.append("    switch (pname) {");
    for (auto const& name_and_type : name_to_type) {
        auto const& parameter_name = name_and_type.name;
        auto const& type_name = name_and_type.return_type.type;

        StringBuilder string_builder;
        SourceGenerator impl_generator { string_builder };
        impl_generator.set("parameter_name", parameter_name);
        impl_generator.set("type_name", type_name);
        impl_generator.append(R"~~~(
    case GL_@parameter_name@: {)~~~");
        if (is_primitive_type(type_name)) {
            impl_generator.append(R"~~~(
        GLint result;
        glGetIntegerv(GL_@parameter_name@, &result);
        return JS::Value(result);
)~~~");
        } else if (type_name == "DOMString"sv) {
            impl_generator.append(R"~~~(
        auto result = reinterpret_cast<const char*>(glGetString(GL_@parameter_name@));
        return JS::PrimitiveString::create(m_realm->vm(), ByteString { result });)~~~");
        } else if (type_name == "Float32Array"sv || type_name == "Int32Array"sv) {
            auto element_count = name_and_type.return_type.element_count;
            impl_generator.set("element_count", MUST(String::formatted("{}", element_count)));
            if (type_name == "Int32Array"sv) {
                impl_generator.set("gl_function_name", "glGetIntegerv"sv);
                impl_generator.set("element_type", "GLint"sv);
            } else if (type_name == "Float32Array"sv) {
                impl_generator.set("gl_function_name", "glGetFloatv"sv);
                impl_generator.set("element_type", "GLfloat"sv);
            } else {
                VERIFY_NOT_REACHED();
            }
            impl_generator.append(R"~~~(
        Array<@element_type@, @element_count@> result;
        @gl_function_name@(GL_@parameter_name@, result.data());
        auto byte_buffer = MUST(ByteBuffer::copy(result.data(), @element_count@ * sizeof(@element_type@)));
        auto array_buffer = JS::ArrayBuffer::create(m_realm, move(byte_buffer));
        return JS::@type_name@::create(m_realm, @element_count@, array_buffer);
)~~~");
        } else if (type_name == "WebGLProgram"sv || type_name == "WebGLBuffer"sv || type_name == "WebGLTexture"sv || type_name == "WebGLFramebuffer"sv || type_name == "WebGLRenderbuffer"sv) {
            impl_generator.append(R"~~~(
        GLint result;
        glGetIntegerv(GL_@parameter_name@, &result);
        if (!result)
            return JS::js_null();
        return @type_name@::create(m_realm, result);
)~~~");
        } else {
            VERIFY_NOT_REACHED();
        }

        impl_generator.append("    }");

        generator.append(string_builder.string_view());
    }

    generator.appendln(R"~~~(
    default:
        dbgln("Unknown WebGL parameter name: {:x}", pname);
        set_error(GL_INVALID_ENUM);
        return JS::js_null();
    })~~~");
}

static void generate_get_buffer_parameter(SourceGenerator& generator)
{
    Vector<NameAndType> const name_to_type = {
        { "BUFFER_SIZE"sv, { "GLint"sv } },
        { "BUFFER_USAGE"sv, { "GLenum"sv } },
    };

    generator.append("    switch (pname) {");

    for (auto const& name_and_type : name_to_type) {
        auto const& parameter_name = name_and_type.name;
        auto const& type_name = name_and_type.return_type.type;

        StringBuilder string_builder;
        SourceGenerator impl_generator { string_builder };
        impl_generator.set("parameter_name", parameter_name);
        impl_generator.set("type_name", type_name);
        impl_generator.append(R"~~~(
    case GL_@parameter_name@: {
        GLint result;
        glGetBufferParameteriv(target, GL_@parameter_name@, &result);
        return JS::Value(result);
    }
)~~~");

        generator.append(string_builder.string_view());
    }

    generator.appendln(R"~~~(
    default:
        TODO();
    })~~~");
}

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    StringView generated_header_path;
    StringView generated_implementation_path;
    Vector<ByteString> base_paths;
    StringView webgl_context_idl_path;

    Core::ArgsParser args_parser;
    args_parser.add_option(webgl_context_idl_path, "Path to the WebGLRenderingContext idl file", "webgl-idl-path", 'i', "webgl-idl-path");
    args_parser.add_option(Core::ArgsParser::Option {
        .argument_mode = Core::ArgsParser::OptionArgumentMode::Required,
        .help_string = "Path to root of IDL file tree(s)",
        .long_name = "base-path",
        .short_name = 'b',
        .value_name = "base-path",
        .accept_value = [&](StringView s) {
            base_paths.append(s);
            return true;
        },
    });
    args_parser.add_option(generated_header_path, "Path to the header file to generate", "generated-header-path", 'h', "generated-header-path");
    args_parser.add_option(generated_implementation_path, "Path to the implementation file to generate", "generated-implementation-path", 'c', "generated-implementation-path");
    args_parser.parse(arguments);

    auto generated_header_file = TRY(Core::File::open(generated_header_path, Core::File::OpenMode::Write));
    auto generated_implementation_file = TRY(Core::File::open(generated_implementation_path, Core::File::OpenMode::Write));

    auto idl_file = MUST(Core::File::open(webgl_context_idl_path, Core::File::OpenMode::Read));
    auto webgl_context_idl_file_content = MUST(idl_file->read_until_eof());

    Vector<ByteString> import_base_paths;
    for (auto const& base_path : base_paths) {
        VERIFY(!base_path.is_empty());
        import_base_paths.append(base_path);
    }

    IDL::Parser parser(webgl_context_idl_path, StringView(webgl_context_idl_file_content), import_base_paths);
    auto const& interface = parser.parse();

    auto path = LexicalPath(generated_header_path);
    auto title = path.title();
    auto first_dot = title.find('.');
    ByteString class_name = title;
    if (first_dot.has_value())
        class_name = title.substring_view(0, *first_dot);

    StringBuilder header_file_string_builder;
    SourceGenerator header_file_generator { header_file_string_builder };
    header_file_generator.set("class_name", class_name);

    StringBuilder implementation_file_string_builder;
    SourceGenerator implementation_file_generator { implementation_file_string_builder };
    implementation_file_generator.set("class_name", class_name);

    auto webgl_version = class_name == "WebGLRenderingContext" ? 1 : 2;
    if (webgl_version == 1) {
        implementation_file_generator.append(R"~~~(
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
)~~~");
    } else {
        implementation_file_generator.append(R"~~~(
#include <GLES3/gl3.h>
)~~~");
    }

    implementation_file_generator.append(R"~~~(
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/DataView.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibWeb/HTML/HTMLCanvasElement.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/HTML/HTMLVideoElement.h>
#include <LibWeb/HTML/ImageBitmap.h>
#include <LibWeb/HTML/ImageData.h>
#include <LibWeb/WebGL/OpenGLContext.h>
#include <LibWeb/WebGL/WebGLActiveInfo.h>
#include <LibWeb/WebGL/WebGLBuffer.h>
#include <LibWeb/WebGL/WebGLFramebuffer.h>
#include <LibWeb/WebGL/WebGLProgram.h>
#include <LibWeb/WebGL/WebGLRenderbuffer.h>
#include <LibWeb/WebGL/@class_name@.h>
#include <LibWeb/WebGL/WebGLShader.h>
#include <LibWeb/WebGL/WebGLShaderPrecisionFormat.h>
#include <LibWeb/WebGL/WebGLTexture.h>
#include <LibWeb/WebGL/WebGLUniformLocation.h>
#include <LibWeb/WebGL/WebGLVertexArrayObject.h>
#include <LibWeb/WebIDL/Buffers.h>

namespace Web::WebGL {

static Vector<GLchar> null_terminated_string(StringView string)
{
    Vector<GLchar> result;
    for (auto c : string.bytes())
        result.append(c);
    result.append('\\0');
    return result;
}

@class_name@::@class_name@(JS::Realm& realm, NonnullOwnPtr<OpenGLContext> context)
    : m_realm(realm)
    , m_context(move(context))
{
}

)~~~");

    header_file_generator.append(R"~~~(
#pragma once

#include <AK/NonnullOwnPtr.h>
#include <LibGC/Ptr.h>
#include <LibGfx/Bitmap.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::WebGL {

using namespace Web::HTML;

class @class_name@ {
public:
    @class_name@(JS::Realm&, NonnullOwnPtr<OpenGLContext>);

    OpenGLContext& context() { return *m_context; }

    virtual void present() = 0;
    virtual void needs_to_present() = 0;
    virtual void set_error(GLenum) = 0;
)~~~");

    for (auto const& function : interface.functions) {
        if (function.extended_attributes.contains("FIXME")) {
            continue;
        }

        if (function.name == "getSupportedExtensions"sv || function.name == "getExtension"sv || function.name == "getContextAttributes"sv || function.name == "isContextLost"sv) {
            // Implemented in WebGLRenderingContext
            continue;
        }

        StringBuilder function_declaration;

        StringBuilder function_parameters;
        for (size_t i = 0; i < function.parameters.size(); ++i) {
            auto const& parameter = function.parameters[i];
            function_parameters.append(to_cpp_type(*parameter.type, interface));
            function_parameters.append(" "sv);
            function_parameters.append(parameter.name);
            if (i != function.parameters.size() - 1) {
                function_parameters.append(", "sv);
            }
        }

        auto function_name = function.name.to_snakecase();
        function_declaration.append(to_cpp_type(*function.return_type, interface));
        function_declaration.append(" "sv);
        function_declaration.append(function_name);
        function_declaration.append("("sv);

        function_declaration.append(function_parameters.string_view());
        function_declaration.append(");"sv);

        header_file_generator.append("    "sv);
        header_file_generator.append(function_declaration.string_view());
        header_file_generator.append("\n"sv);

        StringBuilder function_impl;
        SourceGenerator function_impl_generator { function_impl };
        function_impl_generator.set("class_name", class_name);

        ScopeGuard function_guard { [&] {
            function_impl_generator.append("}\n"sv);
            implementation_file_generator.append(function_impl_generator.as_string_view().bytes());
        } };

        function_impl_generator.set("function_name", function_name);
        function_impl_generator.set("function_parameters", function_parameters.string_view());
        function_impl_generator.set("function_return_type", to_cpp_type(*function.return_type, interface));
        function_impl_generator.append(R"~~~(
@function_return_type@ @class_name@::@function_name@(@function_parameters@)
{
    m_context->make_current();
)~~~");

        if (gl_function_modifies_framebuffer(function.name)) {
            function_impl_generator.append("    m_context->notify_content_will_change();\n"sv);
        }

        if (function.name == "createBuffer"sv) {
            function_impl_generator.append(R"~~~(
    GLuint handle = 0;
    glGenBuffers(1, &handle);
    return WebGLBuffer::create(m_realm, handle);
)~~~");
            continue;
        }

        if (function.name == "createTexture"sv) {
            function_impl_generator.append(R"~~~(
    GLuint handle = 0;
    glGenTextures(1, &handle);
    return WebGLTexture::create(m_realm, handle);
)~~~");
            continue;
        }

        if (function.name == "createFramebuffer"sv) {
            function_impl_generator.append(R"~~~(
    GLuint handle = 0;
    glGenFramebuffers(1, &handle);
    return WebGLFramebuffer::create(m_realm, handle);
)~~~");
            continue;
        }

        if (function.name == "createRenderbuffer"sv) {
            function_impl_generator.append(R"~~~(
    GLuint handle = 0;
    glGenRenderbuffers(1, &handle);
    return WebGLRenderbuffer::create(m_realm, handle);
)~~~");
            continue;
        }

        if (function.name == "createVertexArray"sv) {
            function_impl_generator.append(R"~~~(
    GLuint handle = 0;
    glGenVertexArrays(1, &handle);
    return WebGLVertexArrayObject::create(m_realm, handle);
)~~~");
            continue;
        }

        if (function.name == "shaderSource"sv) {
            function_impl_generator.append(R"~~~(
    Vector<GLchar*> strings;
    auto string = null_terminated_string(source);
    strings.append(string.data());
    Vector<GLint> length;
    length.append(source.bytes().size());
    glShaderSource(shader->handle(), 1, strings.data(), length.data());
)~~~");
            continue;
        }

        if (function.name == "getAttribLocation"sv) {
            function_impl_generator.append(R"~~~(
    auto name_str = null_terminated_string(name);
    return glGetAttribLocation(program->handle(), name_str.data());
)~~~");
            continue;
        }

        if (function.name == "vertexAttribPointer"sv) {
            function_impl_generator.append(R"~~~(
    glVertexAttribPointer(index, size, type, normalized, stride, reinterpret_cast<void*>(offset));
)~~~");
            continue;
        }

        if (function.name == "texImage2D"sv && function.overload_index == 0) {
            function_impl_generator.append(R"~~~(
    void const* pixels_ptr = nullptr;
    if (pixels) {
        auto const& viewed_array_buffer = pixels->viewed_array_buffer();
        auto const& byte_buffer = viewed_array_buffer->buffer();
        pixels_ptr = byte_buffer.data();
    }
    glTexImage2D(target, level, internalformat, width, height, border, format, type, pixels_ptr);
)~~~");
            continue;
        }

        if (function.name == "texImage2D"sv && function.overload_index == 1) {
            // FIXME: If this function is called with an ImageData whose data attribute has been neutered,
            //        an INVALID_VALUE error is generated.
            // FIXME: If this function is called with an ImageBitmap that has been neutered, an INVALID_VALUE
            //        error is generated.
            // FIXME: If this function is called with an HTMLImageElement or HTMLVideoElement whose origin
            //        differs from the origin of the containing Document, or with an HTMLCanvasElement,
            //        ImageBitmap or OffscreenCanvas whose bitmap's origin-clean flag is set to false,
            //        a SECURITY_ERR exception must be thrown. See Origin Restrictions.
            // FIXME: If source is null then an INVALID_VALUE error is generated.
            function_impl_generator.append(R"~~~(
    auto bitmap = source.visit(
        [](GC::Root<HTMLImageElement> const& source) -> RefPtr<Gfx::ImmutableBitmap> {
            return source->immutable_bitmap();
        },
        [](GC::Root<HTMLCanvasElement> const& source) -> RefPtr<Gfx::ImmutableBitmap> {
            auto surface = source->surface();
            if (!surface)
                return {};
            auto bitmap = MUST(Gfx::Bitmap::create(Gfx::BitmapFormat::RGBA8888, Gfx::AlphaType::Premultiplied, surface->size()));
            surface->read_into_bitmap(*bitmap);
            return Gfx::ImmutableBitmap::create(*bitmap);
        },
        [](GC::Root<HTMLVideoElement> const& source) -> RefPtr<Gfx::ImmutableBitmap> {
            return Gfx::ImmutableBitmap::create(*source->bitmap());
        },
        [](GC::Root<ImageBitmap> const& source) -> RefPtr<Gfx::ImmutableBitmap> {
            return Gfx::ImmutableBitmap::create(*source->bitmap());
        },
        [](GC::Root<ImageData> const& source) -> RefPtr<Gfx::ImmutableBitmap> {
            return Gfx::ImmutableBitmap::create(source->bitmap());
        });
    if (!bitmap)
        return;

    void const* pixels_ptr = bitmap->bitmap()->begin();
    int width = bitmap->width();
    int height = bitmap->height();
    glTexImage2D(target, level, internalformat, width, height, 0, format, type, pixels_ptr);
)~~~");
            continue;
        }

        if (function.name == "texSubImage2D"sv && function.overload_index == 0) {
            function_impl_generator.append(R"~~~(
    void const* pixels_ptr = nullptr;
    if (pixels) {
        auto const& viewed_array_buffer = pixels->viewed_array_buffer();
        auto const& byte_buffer = viewed_array_buffer->buffer();
        pixels_ptr = byte_buffer.data();
    }
    glTexSubImage2D(target, level, xoffset, yoffset, width, height, format, type, pixels_ptr);
)~~~");
            continue;
        }

        if (function.name == "getShaderParameter"sv) {
            function_impl_generator.append(R"~~~(
    GLint result = 0;
    glGetShaderiv(shader->handle(), pname, &result);
    return JS::Value(result);
)~~~");
            continue;
        }

        if (function.name == "getProgramParameter"sv) {
            function_impl_generator.append(R"~~~(
    GLint result = 0;
    glGetProgramiv(program->handle(), pname, &result);
    return JS::Value(result);
)~~~");
            continue;
        }

        if (function.name == "bufferData"sv && function.overload_index == 0) {
            function_impl_generator.append(R"~~~(
    glBufferData(target, size, 0, usage);
)~~~");
            continue;
        }

        if (function.name == "getUniformLocation"sv) {
            function_impl_generator.append(R"~~~(
    auto name_str = null_terminated_string(name);
    return WebGLUniformLocation::create(m_realm, glGetUniformLocation(program->handle(), name_str.data()));
)~~~");
            continue;
        }

        if (function.name == "drawElements"sv) {
            function_impl_generator.append(R"~~~(
    glDrawElements(mode, count, type, reinterpret_cast<void*>(offset));
    needs_to_present();
)~~~");
            continue;
        }

        if (function.name.starts_with("uniformMatrix"sv)) {
            auto number_of_matrix_elements = function.name.substring_view(13, 1);
            function_impl_generator.set("number_of_matrix_elements", number_of_matrix_elements);
            function_impl_generator.append(R"~~~(
    auto matrix_size = @number_of_matrix_elements@ * @number_of_matrix_elements@;
    if (value.has<Vector<float>>()) {
        auto& data = value.get<Vector<float>>();
        glUniformMatrix@number_of_matrix_elements@fv(location->handle(), data.size() / matrix_size, transpose, data.data());
        return;
    }

    auto& typed_array_base = static_cast<JS::TypedArrayBase&>(*value.get<GC::Root<WebIDL::BufferSource>>()->raw_object());
    auto& float32_array = verify_cast<JS::Float32Array>(typed_array_base);
    float const* data = float32_array.data().data();
    auto count = float32_array.array_length().length() / matrix_size;
    glUniformMatrix@number_of_matrix_elements@fv(location->handle(), count, transpose, data);
)~~~");
            continue;
        }

        if (function.name == "uniform1fv"sv || function.name == "uniform2fv"sv || function.name == "uniform3fv"sv || function.name == "uniform4fv"sv) {
            auto number_of_matrix_elements = function.name.substring_view(7, 1);
            function_impl_generator.set("number_of_matrix_elements", number_of_matrix_elements);
            function_impl_generator.append(R"~~~(
    if (v.has<Vector<float>>()) {
        auto& data = v.get<Vector<float>>();
        glUniform@number_of_matrix_elements@fv(location->handle(), data.size() / @number_of_matrix_elements@, data.data());
        return;
    }

    auto& typed_array_base = static_cast<JS::TypedArrayBase&>(*v.get<GC::Root<WebIDL::BufferSource>>()->raw_object());
    auto& float32_array = verify_cast<JS::Float32Array>(typed_array_base);
    float const* data = float32_array.data().data();
    auto count = float32_array.array_length().length() / @number_of_matrix_elements@;
    glUniform@number_of_matrix_elements@fv(location->handle(), count, data);
)~~~");
            continue;
        }

        if (function.name == "uniform1iv"sv || function.name == "uniform2iv"sv || function.name == "uniform3iv"sv || function.name == "uniform4iv"sv) {
            auto number_of_matrix_elements = function.name.substring_view(7, 1);
            function_impl_generator.set("number_of_matrix_elements", number_of_matrix_elements);
            function_impl_generator.append(R"~~~(
    if (v.has<Vector<int>>()) {
        auto& data = v.get<Vector<int>>();
        glUniform@number_of_matrix_elements@iv(location->handle(), data.size() / @number_of_matrix_elements@, data.data());
        return;
    }

    auto& typed_array_base = static_cast<JS::TypedArrayBase&>(*v.get<GC::Root<WebIDL::BufferSource>>()->raw_object());
    auto& int32_array = verify_cast<JS::Int32Array>(typed_array_base);
    int const* data = int32_array.data().data();
    auto count = int32_array.array_length().length() / @number_of_matrix_elements@;
    glUniform@number_of_matrix_elements@iv(location->handle(), count, data);
)~~~");
            continue;
        }

        if (function.name == "getParameter"sv) {
            generate_get_parameter(function_impl_generator);
            continue;
        }

        if (function.name == "getBufferParameter"sv) {
            generate_get_buffer_parameter(function_impl_generator);
            continue;
        }

        if (function.name == "getActiveUniform"sv) {
            function_impl_generator.append(R"~~~(
    GLint size = 0;
    GLenum type = 0;
    GLsizei buf_size = 256;
    GLsizei length = 0;
    GLchar name[256];
    glGetActiveUniform(program->handle(), index, buf_size, &length, &size, &type, name);
    auto readonly_bytes = ReadonlyBytes { name, static_cast<size_t>(length) };
    return WebGLActiveInfo::create(m_realm, String::from_utf8_without_validation(readonly_bytes), type, size);
)~~~");
            continue;
        }

        if (function.name == "getActiveAttrib"sv) {
            function_impl_generator.append(R"~~~(
    GLint size = 0;
    GLenum type = 0;
    GLsizei buf_size = 256;
    GLsizei length = 0;
    GLchar name[256];
    glGetActiveAttrib(program->handle(), index, buf_size, &length, &size, &type, name);
    auto readonly_bytes = ReadonlyBytes { name, static_cast<size_t>(length) };
    return WebGLActiveInfo::create(m_realm, String::from_utf8_without_validation(readonly_bytes), type, size);
)~~~");
            continue;
        }

        if (function.name == "getShaderInfoLog"sv) {
            function_impl_generator.append(R"~~~(
    GLint info_log_length = 0;
    glGetShaderiv(shader->handle(), GL_INFO_LOG_LENGTH, &info_log_length);
    Vector<GLchar> info_log;
    info_log.resize(info_log_length);
    if (!info_log_length)
        return String {};
    glGetShaderInfoLog(shader->handle(), info_log_length, nullptr, info_log.data());
    return String::from_utf8_without_validation(ReadonlyBytes { info_log.data(), static_cast<size_t>(info_log_length - 1) });
)~~~");
            continue;
        }

        if (function.name == "getProgramInfoLog"sv) {
            function_impl_generator.append(R"~~~(
    GLint info_log_length = 0;
    glGetProgramiv(program->handle(), GL_INFO_LOG_LENGTH, &info_log_length);
    Vector<GLchar> info_log;
    info_log.resize(info_log_length);
    if (!info_log_length)
        return String {};
    glGetProgramInfoLog(program->handle(), info_log_length, nullptr, info_log.data());
    return String::from_utf8_without_validation(ReadonlyBytes { info_log.data(), static_cast<size_t>(info_log_length - 1) });
)~~~");
            continue;
        }

        if (function.name == "getShaderPrecisionFormat"sv) {
            function_impl_generator.append(R"~~~(
    GLint range[2];
    GLint precision;
    glGetShaderPrecisionFormat(shadertype, precisiontype, range, &precision);
    return WebGLShaderPrecisionFormat::create(m_realm, range[0], range[1], precision);
)~~~");
            continue;
        }

        if (function.name == "deleteBuffer"sv) {
            function_impl_generator.append(R"~~~(
    auto handle = buffer ? buffer->handle() : 0;
    glDeleteBuffers(1, &handle);
)~~~");
            continue;
        }

        if (function.name == "deleteFramebuffer"sv) {
            function_impl_generator.append(R"~~~(
    auto handle = framebuffer ? framebuffer->handle() : 0;
    glDeleteFramebuffers(1, &handle);
)~~~");
            continue;
        }

        if (function.name == "deleteTexture"sv) {
            function_impl_generator.append(R"~~~(
    auto handle = texture ? texture->handle() : 0;
    glDeleteTextures(1, &handle);
)~~~");
            continue;
        }

        if (function.name == "deleteVertexArray"sv) {
            function_impl_generator.append(R"~~~(
    auto handle = vertexArray ? vertexArray->handle() : 0;
    glDeleteVertexArrays(1, &handle);
)~~~");
            continue;
        }

        Vector<ByteString> gl_call_arguments;
        for (size_t i = 0; i < function.parameters.size(); ++i) {
            auto const& parameter = function.parameters[i];
            if (parameter.type->is_numeric() || parameter.type->is_boolean()) {
                gl_call_arguments.append(parameter.name);
                continue;
            }
            if (parameter.type->is_string()) {
                function_impl_generator.set("parameter_name", parameter.name);
                function_impl_generator.append(R"~~~(
    auto @parameter_name@_null_terminated = null_terminated_string(@parameter_name@);
)~~~");
                gl_call_arguments.append(ByteString::formatted("{}_null_terminated.data()", parameter.name));
                continue;
            }
            if (is_webgl_object_type(parameter.type->name())) {
                gl_call_arguments.append(ByteString::formatted("{} ? {}->handle() : 0", parameter.name, parameter.name));
                continue;
            }
            if (parameter.type->name() == "BufferSource"sv) {
                function_impl_generator.set("buffer_source_name", parameter.name);
                function_impl_generator.append(R"~~~(
    void const* ptr = nullptr;
    size_t byte_size = 0;
    if (@buffer_source_name@->is_typed_array_base()) {
        auto& typed_array_base = static_cast<JS::TypedArrayBase&>(*@buffer_source_name@->raw_object());
        ptr = typed_array_base.viewed_array_buffer()->buffer().data();
        byte_size = typed_array_base.viewed_array_buffer()->byte_length();
    } else if (@buffer_source_name@->is_data_view()) {
        auto& data_view = static_cast<JS::DataView&>(*@buffer_source_name@->raw_object());
        ptr = data_view.viewed_array_buffer()->buffer().data();
        byte_size = data_view.viewed_array_buffer()->byte_length();
    } else if (@buffer_source_name@->is_array_buffer()) {
        auto& array_buffer = static_cast<JS::ArrayBuffer&>(*@buffer_source_name@->raw_object());
        ptr = array_buffer.buffer().data();
        byte_size = array_buffer.byte_length();
    } else {
        VERIFY_NOT_REACHED();
    }
)~~~");
                gl_call_arguments.append(ByteString::formatted("byte_size"));
                gl_call_arguments.append(ByteString::formatted("ptr"));
                continue;
            }
            VERIFY_NOT_REACHED();
        }

        StringBuilder gl_call_arguments_string_builder;
        gl_call_arguments_string_builder.join(", "sv, gl_call_arguments);

        auto gl_call_string = ByteString::formatted("{}({})", idl_to_gl_function_name(function.name), gl_call_arguments_string_builder.string_view());
        function_impl_generator.set("call_string", gl_call_string);

        if (gl_function_modifies_framebuffer(function.name)) {
            function_impl_generator.append("    needs_to_present();\n"sv);
        }

        if (function.return_type->name() == "undefined"sv) {
            function_impl_generator.append("    @call_string@;"sv);
        } else if (function.return_type->is_integer() || function.return_type->is_boolean()) {
            function_impl_generator.append("    return @call_string@;"sv);
        } else if (is_webgl_object_type(function.return_type->name())) {
            function_impl_generator.set("return_type_name", function.return_type->name());
            function_impl_generator.append("    return @return_type_name@::create(m_realm, @call_string@);"sv);
        } else {
            VERIFY_NOT_REACHED();
        }

        function_impl_generator.append("\n"sv);
    }

    header_file_generator.append(R"~~~(
private:
    GC::Ref<JS::Realm> m_realm;
    NonnullOwnPtr<OpenGLContext> m_context;
};

}
)~~~");

    implementation_file_generator.append(R"~~~(
}
)~~~");

    MUST(generated_header_file->write_until_depleted(header_file_generator.as_string_view().bytes()));
    MUST(generated_implementation_file->write_until_depleted(implementation_file_generator.as_string_view().bytes()));

    return 0;
}
