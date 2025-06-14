/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2024-2025, Luke Wilde <luke@ladybird.org>
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
    return type_name == "WebGLBuffer"_sv
        || type_name == "WebGLFramebuffer"_sv
        || type_name == "WebGLProgram"_sv
        || type_name == "WebGLRenderbuffer"_sv
        || type_name == "WebGLSampler"_sv
        || type_name == "WebGLShader"_sv
        || type_name == "WebGLTexture"_sv
        || type_name == "WebGLVertexArrayObject"_sv;
}

static bool gl_function_modifies_framebuffer(StringView function_name)
{
    return function_name == "clear"_sv
        || function_name == "clearBufferfv"_sv
        || function_name == "clearBufferiv"_sv
        || function_name == "clearBufferuiv"_sv
        || function_name == "clearBufferfi"_sv
        || function_name == "drawArrays"_sv
        || function_name == "drawArraysInstanced"_sv
        || function_name == "drawElements"_sv
        || function_name == "drawElementsInstanced"_sv
        || function_name == "blitFramebuffer"_sv
        || function_name == "invalidateFramebuffer"_sv;
}

static ByteString to_cpp_type(const IDL::Type& type, const IDL::Interface& interface)
{
    if (type.name() == "undefined"_sv)
        return "void"_sv;
    if (type.name() == "object"_sv) {
        if (type.is_nullable())
            return "JS::Object*"_sv;
        return "JS::Object&"_sv;
    }
    if (type.name() == "DOMString"_sv) {
        if (type.is_nullable())
            return "Optional<String>"_sv;
        return "String"_sv;
    }
    if (type.name() == "sequence") {
        auto& parameterized_type = as<IDL::ParameterizedType>(type);
        auto sequence_cpp_type = idl_type_name_to_cpp_type(parameterized_type.parameters().first(), interface);

        if (type.is_nullable()) {
            return ByteString::formatted("Optional<Vector<{}>>", sequence_cpp_type.name);
        }

        return ByteString::formatted("Vector<{}>", sequence_cpp_type.name);
    }
    auto cpp_type = idl_type_name_to_cpp_type(type, interface);
    return cpp_type.name;
}

static ByteString idl_to_gl_function_name(StringView function_name)
{
    StringBuilder gl_function_name_builder;
    gl_function_name_builder.append("gl"_sv);
    for (size_t i = 0; i < function_name.length(); ++i) {
        if (i == 0) {
            gl_function_name_builder.append(to_ascii_uppercase(function_name[i]));
        } else {
            gl_function_name_builder.append(function_name[i]);
        }
    }
    if (function_name == "clearDepth"_sv || function_name == "depthRange"_sv) {
        gl_function_name_builder.append("f"_sv);
    }
    return gl_function_name_builder.to_byte_string();
}

struct NameAndType {
    StringView name;
    struct {
        StringView type;
        int element_count { 0 };
    } return_type;
    Optional<int> webgl_version { OptionalNone {} };
};

static void generate_get_parameter(SourceGenerator& generator, int webgl_version)
{
    Vector<NameAndType> const name_to_type = {
        { "ACTIVE_TEXTURE"_sv, { "GLenum"_sv } },
        { "ALIASED_LINE_WIDTH_RANGE"_sv, { "Float32Array"_sv, 2 } },
        { "ALIASED_POINT_SIZE_RANGE"_sv, { "Float32Array"_sv, 2 } },
        { "ALPHA_BITS"_sv, { "GLint"_sv } },
        { "ARRAY_BUFFER_BINDING"_sv, { "WebGLBuffer"_sv } },
        { "BLEND"_sv, { "GLboolean"_sv } },
        { "BLEND_COLOR"_sv, { "Float32Array"_sv, 4 } },
        { "BLEND_DST_ALPHA"_sv, { "GLenum"_sv } },
        { "BLEND_DST_RGB"_sv, { "GLenum"_sv } },
        { "BLEND_EQUATION_ALPHA"_sv, { "GLenum"_sv } },
        { "BLEND_EQUATION_RGB"_sv, { "GLenum"_sv } },
        { "BLEND_SRC_ALPHA"_sv, { "GLenum"_sv } },
        { "BLEND_SRC_RGB"_sv, { "GLenum"_sv } },
        { "BLUE_BITS"_sv, { "GLint"_sv } },
        { "COLOR_CLEAR_VALUE"_sv, { "Float32Array"_sv, 4 } },
        // FIXME: { "COLOR_WRITEMASK"_sv, { "sequence<GLboolean>"_sv, 4 } },
        // FIXME: { "COMPRESSED_TEXTURE_FORMATS"_sv, { "Uint32Array"_sv } },
        { "CULL_FACE"_sv, { "GLboolean"_sv } },
        { "CULL_FACE_MODE"_sv, { "GLenum"_sv } },
        { "CURRENT_PROGRAM"_sv, { "WebGLProgram"_sv } },
        { "DEPTH_BITS"_sv, { "GLint"_sv } },
        { "DEPTH_CLEAR_VALUE"_sv, { "GLfloat"_sv } },
        { "DEPTH_FUNC"_sv, { "GLenum"_sv } },
        { "DEPTH_RANGE"_sv, { "Float32Array"_sv, 2 } },
        { "DEPTH_TEST"_sv, { "GLboolean"_sv } },
        { "DEPTH_WRITEMASK"_sv, { "GLboolean"_sv } },
        { "DITHER"_sv, { "GLboolean"_sv } },
        { "ELEMENT_ARRAY_BUFFER_BINDING"_sv, { "WebGLBuffer"_sv } },
        { "FRAMEBUFFER_BINDING"_sv, { "WebGLFramebuffer"_sv } },
        { "FRONT_FACE"_sv, { "GLenum"_sv } },
        { "GENERATE_MIPMAP_HINT"_sv, { "GLenum"_sv } },
        { "GREEN_BITS"_sv, { "GLint"_sv } },
        { "IMPLEMENTATION_COLOR_READ_FORMAT"_sv, { "GLenum"_sv } },
        { "IMPLEMENTATION_COLOR_READ_TYPE"_sv, { "GLenum"_sv } },
        { "LINE_WIDTH"_sv, { "GLfloat"_sv } },
        { "MAX_COMBINED_TEXTURE_IMAGE_UNITS"_sv, { "GLint"_sv } },
        { "MAX_CUBE_MAP_TEXTURE_SIZE"_sv, { "GLint"_sv } },
        { "MAX_FRAGMENT_UNIFORM_VECTORS"_sv, { "GLint"_sv } },
        { "MAX_RENDERBUFFER_SIZE"_sv, { "GLint"_sv } },
        { "MAX_TEXTURE_IMAGE_UNITS"_sv, { "GLint"_sv } },
        { "MAX_TEXTURE_SIZE"_sv, { "GLint"_sv } },
        { "MAX_VARYING_VECTORS"_sv, { "GLint"_sv } },
        { "MAX_VERTEX_ATTRIBS"_sv, { "GLint"_sv } },
        { "MAX_VERTEX_TEXTURE_IMAGE_UNITS"_sv, { "GLint"_sv } },
        { "MAX_VERTEX_UNIFORM_VECTORS"_sv, { "GLint"_sv } },
        { "MAX_VIEWPORT_DIMS"_sv, { "Int32Array"_sv, 2 } },
        { "PACK_ALIGNMENT"_sv, { "GLint"_sv } },
        { "POLYGON_OFFSET_FACTOR"_sv, { "GLfloat"_sv } },
        { "POLYGON_OFFSET_FILL"_sv, { "GLboolean"_sv } },
        { "POLYGON_OFFSET_UNITS"_sv, { "GLfloat"_sv } },
        { "RED_BITS"_sv, { "GLint"_sv } },
        { "RENDERBUFFER_BINDING"_sv, { "WebGLRenderbuffer"_sv } },
        { "RENDERER"_sv, { "DOMString"_sv } },
        { "SAMPLE_ALPHA_TO_COVERAGE"_sv, { "GLboolean"_sv } },
        { "SAMPLE_BUFFERS"_sv, { "GLint"_sv } },
        { "SAMPLE_COVERAGE"_sv, { "GLboolean"_sv } },
        { "SAMPLE_COVERAGE_INVERT"_sv, { "GLboolean"_sv } },
        { "SAMPLE_COVERAGE_VALUE"_sv, { "GLfloat"_sv } },
        { "SAMPLES"_sv, { "GLint"_sv } },
        { "SCISSOR_BOX"_sv, { "Int32Array"_sv, 4 } },
        { "SCISSOR_TEST"_sv, { "GLboolean"_sv } },
        { "SHADING_LANGUAGE_VERSION"_sv, { "DOMString"_sv } },
        { "STENCIL_BACK_FAIL"_sv, { "GLenum"_sv } },
        { "STENCIL_BACK_FUNC"_sv, { "GLenum"_sv } },
        { "STENCIL_BACK_PASS_DEPTH_FAIL"_sv, { "GLenum"_sv } },
        { "STENCIL_BACK_PASS_DEPTH_PASS"_sv, { "GLenum"_sv } },
        { "STENCIL_BACK_REF"_sv, { "GLint"_sv } },
        { "STENCIL_BACK_VALUE_MASK"_sv, { "GLuint"_sv } },
        { "STENCIL_BACK_WRITEMASK"_sv, { "GLuint"_sv } },
        { "STENCIL_BITS"_sv, { "GLint"_sv } },
        { "STENCIL_CLEAR_VALUE"_sv, { "GLint"_sv } },
        { "STENCIL_FAIL"_sv, { "GLenum"_sv } },
        { "STENCIL_FUNC"_sv, { "GLenum"_sv } },
        { "STENCIL_PASS_DEPTH_FAIL"_sv, { "GLenum"_sv } },
        { "STENCIL_PASS_DEPTH_PASS"_sv, { "GLenum"_sv } },
        { "STENCIL_REF"_sv, { "GLint"_sv } },
        { "STENCIL_TEST"_sv, { "GLboolean"_sv } },
        { "STENCIL_VALUE_MASK"_sv, { "GLuint"_sv } },
        { "STENCIL_WRITEMASK"_sv, { "GLuint"_sv } },
        { "SUBPIXEL_BITS"_sv, { "GLint"_sv } },
        { "TEXTURE_BINDING_2D"_sv, { "WebGLTexture"_sv } },
        { "TEXTURE_BINDING_CUBE_MAP"_sv, { "WebGLTexture"_sv } },
        { "UNPACK_ALIGNMENT"_sv, { "GLint"_sv } },
        // FIXME: { "UNPACK_COLORSPACE_CONVERSION_WEBGL"_sv, { "GLenum"_sv } },
        // FIXME: { "UNPACK_FLIP_Y_WEBGL"_sv, { "GLboolean"_sv } },
        // FIXME: { "UNPACK_PREMULTIPLY_ALPHA_WEBGL"_sv, { "GLboolean"_sv } },
        { "VENDOR"_sv, { "DOMString"_sv } },
        { "VERSION"_sv, { "DOMString"_sv } },
        { "VIEWPORT"_sv, { "Int32Array"_sv, 4 } },
        { "MAX_SAMPLES"_sv, { "GLint"_sv }, 2 },
        { "MAX_3D_TEXTURE_SIZE"_sv, { "GLint"_sv }, 2 },
        { "MAX_ARRAY_TEXTURE_LAYERS"_sv, { "GLint"_sv }, 2 },
        { "MAX_COLOR_ATTACHMENTS"_sv, { "GLint"_sv }, 2 },
        { "MAX_VERTEX_UNIFORM_COMPONENTS"_sv, { "GLint"_sv }, 2 },
        { "MAX_UNIFORM_BLOCK_SIZE"_sv, { "GLint64"_sv }, 2 },
        { "MAX_UNIFORM_BUFFER_BINDINGS"_sv, { "GLint"_sv }, 2 },
        { "UNIFORM_BUFFER_OFFSET_ALIGNMENT"_sv, { "GLint"_sv }, 2 },
        { "MAX_DRAW_BUFFERS"_sv, { "GLint"_sv }, 2 },
        { "MAX_VERTEX_UNIFORM_BLOCKS"_sv, { "GLint"_sv }, 2 },
        { "MAX_FRAGMENT_INPUT_COMPONENTS"_sv, { "GLint"_sv }, 2 },
        { "MAX_FRAGMENT_UNIFORM_COMPONENTS"_sv, { "GLint"_sv }, 2 },
        { "MAX_COMBINED_UNIFORM_BLOCKS"_sv, { "GLint"_sv }, 2 },
        { "MAX_COMBINED_VERTEX_UNIFORM_COMPONENTS"_sv, { "GLint64"_sv }, 2 },
        { "MAX_COMBINED_FRAGMENT_UNIFORM_COMPONENTS"_sv, { "GLint64"_sv }, 2 },
        { "UNIFORM_BUFFER_BINDING"_sv, { "WebGLBuffer"_sv }, 2 },
        { "TEXTURE_BINDING_2D_ARRAY"_sv, { "WebGLTexture"_sv }, 2 },
        { "COPY_READ_BUFFER_BINDING"_sv, { "WebGLBuffer"_sv }, 2 },
        { "COPY_WRITE_BUFFER_BINDING"_sv, { "WebGLBuffer"_sv }, 2 },
        { "MAX_ELEMENT_INDEX"_sv, { "GLint64"_sv }, 2 },
        { "MAX_FRAGMENT_UNIFORM_BLOCKS"_sv, { "GLint"_sv }, 2 },
        { "MAX_VARYING_COMPONENTS"_sv, { "GLint"_sv }, 2 },
        { "MAX_ELEMENTS_INDICES"_sv, { "GLint"_sv }, 2 },
        { "MAX_ELEMENTS_VERTICES"_sv, { "GLint"_sv }, 2 },
        { "MAX_TEXTURE_LOD_BIAS"_sv, { "GLfloat"_sv }, 2 },
        { "MAX_TRANSFORM_FEEDBACK_INTERLEAVED_COMPONENTS"_sv, { "GLint"_sv }, 2 },
        { "MAX_TRANSFORM_FEEDBACK_SEPARATE_ATTRIBS"_sv, { "GLint"_sv }, 2 },
        { "MAX_TRANSFORM_FEEDBACK_SEPARATE_COMPONENTS"_sv, { "GLint"_sv }, 2 },
        { "MIN_PROGRAM_TEXEL_OFFSET"_sv, { "GLint"_sv }, 2 },
        { "MAX_PROGRAM_TEXEL_OFFSET"_sv, { "GLint"_sv }, 2 },
        { "MAX_VERTEX_OUTPUT_COMPONENTS"_sv, { "GLint"_sv }, 2 },
        { "MAX_SERVER_WAIT_TIMEOUT"_sv, { "GLint64"_sv }, 2 },
    };

    auto is_integer_type = [](StringView type) {
        return type == "GLint"_sv || type == "GLenum"_sv || type == "GLuint"_sv;
    };

    generator.append("    switch (pname) {");
    for (auto const& name_and_type : name_to_type) {
        if (name_and_type.webgl_version.has_value() && name_and_type.webgl_version.value() != webgl_version)
            continue;

        auto const& parameter_name = name_and_type.name;
        auto const& type_name = name_and_type.return_type.type;

        StringBuilder string_builder;
        SourceGenerator impl_generator { string_builder };
        impl_generator.set("parameter_name", parameter_name);
        impl_generator.set("type_name", type_name);
        impl_generator.append(R"~~~(
    case GL_@parameter_name@: {)~~~");
        if (is_integer_type(type_name)) {
            impl_generator.append(R"~~~(
        GLint result;
        glGetIntegerv(GL_@parameter_name@, &result);
        return JS::Value(result);
)~~~");
        } else if (type_name == "GLfloat"_sv) {
            impl_generator.append(R"~~~(
        GLfloat result;
        glGetFloatv(GL_@parameter_name@, &result);
        return JS::Value(result);
)~~~");
        } else if (type_name == "GLboolean"_sv) {
            impl_generator.append(R"~~~(
        GLboolean result;
        glGetBooleanv(GL_@parameter_name@, &result);
        return JS::Value(result == GL_TRUE);
)~~~");
        } else if (type_name == "GLint64"_sv) {
            impl_generator.append(R"~~~(
        GLint64 result;
        glGetInteger64v(GL_@parameter_name@, &result);
        return JS::Value(static_cast<double>(result));
)~~~");
        } else if (type_name == "DOMString"_sv) {
            impl_generator.append(R"~~~(
        auto result = reinterpret_cast<const char*>(glGetString(GL_@parameter_name@));
        return JS::PrimitiveString::create(m_realm->vm(), ByteString { result });)~~~");
        } else if (type_name == "Float32Array"_sv || type_name == "Int32Array"_sv) {
            auto element_count = name_and_type.return_type.element_count;
            impl_generator.set("element_count", MUST(String::formatted("{}", element_count)));
            if (type_name == "Int32Array"_sv) {
                impl_generator.set("gl_function_name", "glGetIntegerv"_sv);
                impl_generator.set("element_type", "GLint"_sv);
            } else if (type_name == "Float32Array"_sv) {
                impl_generator.set("gl_function_name", "glGetFloatv"_sv);
                impl_generator.set("element_type", "GLfloat"_sv);
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
        } else if (type_name == "WebGLProgram"_sv || type_name == "WebGLBuffer"_sv || type_name == "WebGLTexture"_sv || type_name == "WebGLFramebuffer"_sv || type_name == "WebGLRenderbuffer"_sv) {
            impl_generator.set("stored_name", name_and_type.name.to_ascii_lowercase_string());
            impl_generator.append(R"~~~(
        if (!m_@stored_name@)
            return JS::js_null();
        return JS::Value(m_@stored_name@);
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
        { "BUFFER_SIZE"_sv, { "GLint"_sv } },
        { "BUFFER_USAGE"_sv, { "GLenum"_sv } },
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
        dbgln("Unknown WebGL buffer parameter name: {:x}", pname);
        set_error(GL_INVALID_ENUM);
        return JS::js_null();
    })~~~");
}

static void generate_get_internal_format_parameter(SourceGenerator& generator)
{
    generator.append(R"~~~(
    switch (pname) {
    case GL_SAMPLES: {
        GLint num_sample_counts { 0 };
        glGetInternalformativ(target, internalformat, GL_NUM_SAMPLE_COUNTS, 1, &num_sample_counts);
        auto samples_buffer = MUST(ByteBuffer::create_zeroed(num_sample_counts * sizeof(GLint)));
        glGetInternalformativ(target, internalformat, GL_SAMPLES, num_sample_counts, reinterpret_cast<GLint*>(samples_buffer.data()));
        auto array_buffer = JS::ArrayBuffer::create(m_realm, move(samples_buffer));
        return JS::Int32Array::create(m_realm, num_sample_counts, array_buffer);
    }
    default:
        dbgln("Unknown WebGL internal format parameter name: {:x}", pname);
        set_error(GL_INVALID_ENUM);
        return JS::js_null();
    }
)~~~");
}

static void generate_webgl_object_handle_unwrap(SourceGenerator& generator, StringView object_name, StringView early_return_value)
{
    StringBuilder string_builder;
    SourceGenerator unwrap_generator { string_builder };
    unwrap_generator.set("object_name", object_name);
    unwrap_generator.set("early_return_value", early_return_value);
    unwrap_generator.append(R"~~~(
    GLuint @object_name@_handle = 0;
    if (@object_name@) {
        auto handle_or_error = @object_name@->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return @early_return_value@;
        }
        @object_name@_handle = handle_or_error.release_value();
    }
)~~~");

    generator.append(unwrap_generator.as_string_view());
}

static void generate_get_active_uniform_block_parameter(SourceGenerator& generator)
{
    generate_webgl_object_handle_unwrap(generator, "program"_sv, "JS::js_null()"_sv);
    generator.append(R"~~~(
    switch (pname) {
    case GL_UNIFORM_BLOCK_BINDING:
    case GL_UNIFORM_BLOCK_DATA_SIZE:
    case GL_UNIFORM_BLOCK_ACTIVE_UNIFORMS: {
        GLint result = 0;
        glGetActiveUniformBlockiv(program_handle, uniform_block_index, pname, &result);
        return JS::Value(result);
    }
    case GL_UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES: {
        GLint num_active_uniforms = 0;
        glGetActiveUniformBlockiv(program_handle, uniform_block_index, GL_UNIFORM_BLOCK_ACTIVE_UNIFORMS, &num_active_uniforms);
        auto active_uniform_indices_buffer = MUST(ByteBuffer::create_zeroed(num_active_uniforms * sizeof(GLint)));
        glGetActiveUniformBlockiv(program_handle, uniform_block_index, GL_UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES, reinterpret_cast<GLint*>(active_uniform_indices_buffer.data()));
        auto array_buffer = JS::ArrayBuffer::create(m_realm, move(active_uniform_indices_buffer));
        return JS::Uint32Array::create(m_realm, num_active_uniforms, array_buffer);
    }
    case GL_UNIFORM_BLOCK_REFERENCED_BY_VERTEX_SHADER:
    case GL_UNIFORM_BLOCK_REFERENCED_BY_FRAGMENT_SHADER: {
        GLint result = 0;
        glGetActiveUniformBlockiv(program_handle, uniform_block_index, pname, &result);
        return JS::Value(result == GL_TRUE);
    }
    default:
        dbgln("Unknown WebGL active uniform block parameter name: {:x}", pname);
        set_error(GL_INVALID_ENUM);
        return JS::js_null();
    }
)~~~");
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

    auto webgl_version = class_name == "WebGLRenderingContextImpl" ? 1 : 2;
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
#include <LibWeb/WebGL/WebGLSampler.h>
#include <LibWeb/WebGL/WebGLShader.h>
#include <LibWeb/WebGL/WebGLSync.h>
#include <LibWeb/WebGL/WebGLShaderPrecisionFormat.h>
#include <LibWeb/WebGL/WebGLTexture.h>
#include <LibWeb/WebGL/WebGLUniformLocation.h>
#include <LibWeb/WebGL/WebGLVertexArrayObject.h>
#include <LibWeb/WebIDL/Buffers.h>

#include <core/SkColorSpace.h>
#include <core/SkColorType.h>
#include <core/SkImage.h>
#include <core/SkPixmap.h>

namespace Web::WebGL {

static Vector<GLchar> null_terminated_string(StringView string)
{
    Vector<GLchar> result;
    for (auto c : string.bytes())
        result.append(c);
    result.append('\\0');
    return result;
}
)~~~");

    if (webgl_version == 2) {
        implementation_file_generator.append(R"~~~(
static constexpr Optional<int> opengl_format_number_of_components(WebIDL::UnsignedLong format)
{
    switch (format) {
    case GL_RED:
    case GL_RED_INTEGER:
    case GL_LUMINANCE:
    case GL_ALPHA:
    case GL_DEPTH_COMPONENT:
        return 1;
    case GL_RG:
    case GL_RG_INTEGER:
    case GL_DEPTH_STENCIL:
    case GL_LUMINANCE_ALPHA:
        return 2;
    case GL_RGB:
    case GL_RGB_INTEGER:
        return 3;
    case GL_RGBA:
    case GL_RGBA_INTEGER:
        return 4;
    default:
        return OptionalNone {};
    }
}

static constexpr Optional<int> opengl_type_size_in_bytes(WebIDL::UnsignedLong type)
{
    switch (type) {
    case GL_UNSIGNED_BYTE:
    case GL_BYTE:
        return 1;
    case GL_UNSIGNED_SHORT:
    case GL_SHORT:
    case GL_HALF_FLOAT:
    case GL_UNSIGNED_SHORT_5_6_5:
    case GL_UNSIGNED_SHORT_4_4_4_4:
    case GL_UNSIGNED_SHORT_5_5_5_1:
        return 2;
    case GL_UNSIGNED_INT:
    case GL_INT:
    case GL_UNSIGNED_INT_2_10_10_10_REV:
    case GL_UNSIGNED_INT_10F_11F_11F_REV:
    case GL_UNSIGNED_INT_5_9_9_9_REV:
    case GL_UNSIGNED_INT_24_8:
        return 4;
    case GL_FLOAT_32_UNSIGNED_INT_24_8_REV:
        return 8;
    default:
        return OptionalNone {};
    }
}

static constexpr SkColorType opengl_format_and_type_to_skia_color_type(WebIDL::UnsignedLong format, WebIDL::UnsignedLong type)
{
    switch (format)
    {
    case GL_RGB:
        switch (type) {
        case GL_UNSIGNED_BYTE:
            return SkColorType::kRGB_888x_SkColorType;
        case GL_UNSIGNED_SHORT_5_6_5:
            return SkColorType::kRGB_565_SkColorType;
        default:
            break;
        }
        break;
    case GL_RGBA:
        switch (type) {
        case GL_UNSIGNED_BYTE:
            return SkColorType::kRGBA_8888_SkColorType;
        case GL_UNSIGNED_SHORT_4_4_4_4:
            // FIXME: This is not exactly the same as RGBA.
            return SkColorType::kARGB_4444_SkColorType;
        case GL_UNSIGNED_SHORT_5_5_5_1:
            dbgln("WebGL2 FIXME: Support conversion to RGBA5551.");
            break;
        default:
            break;
        }
        break;
    case GL_ALPHA:
        switch (type) {
        case GL_UNSIGNED_BYTE:
            return SkColorType::kAlpha_8_SkColorType;
        default:
            break;
        }
        break;
    case GL_LUMINANCE:
        switch (type) {
        case GL_UNSIGNED_BYTE:
            return SkColorType::kGray_8_SkColorType;
        default:
            break;
        }
        break;
    default:
        break;
    }

    dbgln("WebGL2: Unsupported format and type combination. format: 0x{:04x}, type: 0x{:04x}", format, type);
    return SkColorType::kUnknown_SkColorType;
}
)~~~");
    } else {
        implementation_file_generator.append(R"~~~(
static constexpr Optional<int> opengl_format_number_of_components(WebIDL::UnsignedLong format)
{
    switch (format) {
    case GL_LUMINANCE:
    case GL_ALPHA:
        return 1;
    case GL_LUMINANCE_ALPHA:
        return 2;
    case GL_RGB:
        return 3;
    case GL_RGBA:
        return 4;
    default:
        return OptionalNone {};
    }
}

static constexpr Optional<int> opengl_type_size_in_bytes(WebIDL::UnsignedLong type)
{
    switch (type) {
    case GL_UNSIGNED_BYTE:
        return 1;
    case GL_UNSIGNED_SHORT_5_6_5:
    case GL_UNSIGNED_SHORT_4_4_4_4:
    case GL_UNSIGNED_SHORT_5_5_5_1:
        return 2;
    default:
        return OptionalNone {};
    }
}

static constexpr SkColorType opengl_format_and_type_to_skia_color_type(WebIDL::UnsignedLong format, WebIDL::UnsignedLong type)
{
    switch (format)
    {
    case GL_RGB:
        switch (type) {
        case GL_UNSIGNED_BYTE:
            return SkColorType::kRGB_888x_SkColorType;
        case GL_UNSIGNED_SHORT_5_6_5:
            return SkColorType::kRGB_565_SkColorType;
        default:
            break;
        }
        break;
    case GL_RGBA:
        switch (type) {
        case GL_UNSIGNED_BYTE:
            return SkColorType::kRGBA_8888_SkColorType;
        case GL_UNSIGNED_SHORT_4_4_4_4:
            // FIXME: This is not exactly the same as RGBA.
            return SkColorType::kARGB_4444_SkColorType;
        case GL_UNSIGNED_SHORT_5_5_5_1:
            dbgln("WebGL FIXME: Support conversion to RGBA5551.");
            break;
        default:
            break;
        }
        break;
    case GL_ALPHA:
        switch (type) {
        case GL_UNSIGNED_BYTE:
            return SkColorType::kAlpha_8_SkColorType;
        default:
            break;
        }
        break;
    case GL_LUMINANCE:
        switch (type) {
        case GL_UNSIGNED_BYTE:
            return SkColorType::kGray_8_SkColorType;
        default:
            break;
        }
        break;
    default:
        break;
    }

    dbgln("WebGL: Unsupported format and type combination. format: 0x{:04x}, type: 0x{:04x}", format, type);
    return SkColorType::kUnknown_SkColorType;
}
)~~~");
    }

    implementation_file_generator.append(R"~~~(
struct ConvertedTexture {
    ByteBuffer buffer;
    int width { 0 };
    int height { 0 };
};

static Optional<ConvertedTexture> read_and_pixel_convert_texture_image_source(Variant<GC::Root<ImageBitmap>, GC::Root<ImageData>, GC::Root<HTMLImageElement>, GC::Root<HTMLCanvasElement>, GC::Root<HTMLVideoElement>> const& source, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, Optional<int> destination_width = OptionalNone {}, Optional<int> destination_height = OptionalNone {})
{
    // FIXME: If this function is called with an ImageData whose data attribute has been neutered,
    //        an INVALID_VALUE error is generated.
    // FIXME: If this function is called with an ImageBitmap that has been neutered, an INVALID_VALUE
    //        error is generated.
    // FIXME: If this function is called with an HTMLImageElement or HTMLVideoElement whose origin
    //        differs from the origin of the containing Document, or with an HTMLCanvasElement,
    //        ImageBitmap or OffscreenCanvas whose bitmap's origin-clean flag is set to false,
    //        a SECURITY_ERR exception must be thrown. See Origin Restrictions.
    // FIXME: If source is null then an INVALID_VALUE error is generated.
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
        return OptionalNone {};

    int width = destination_width.value_or(bitmap->width());
    int height = destination_height.value_or(bitmap->height());

    Checked<size_t> buffer_pitch = width;

    auto number_of_components = opengl_format_number_of_components(format);
    if (!number_of_components.has_value())
        return OptionalNone {};

    buffer_pitch *= number_of_components.value();

    auto type_size = opengl_type_size_in_bytes(type);
    if (!type_size.has_value())
        return OptionalNone {};

    buffer_pitch *= type_size.value();

    if (buffer_pitch.has_overflow())
        return OptionalNone {};

    if (Checked<size_t>::multiplication_would_overflow(buffer_pitch.value(), height))
        return OptionalNone {};

    auto buffer = MUST(ByteBuffer::create_zeroed(buffer_pitch.value() * height));

    auto skia_format = opengl_format_and_type_to_skia_color_type(format, type);

    // FIXME: Respect UNPACK_PREMULTIPLY_ALPHA_WEBGL
    // FIXME: Respect unpackColorSpace
    auto color_space = SkColorSpace::MakeSRGB();
    auto image_info = SkImageInfo::Make(width, height, skia_format, SkAlphaType::kPremul_SkAlphaType, color_space);
    SkPixmap const pixmap(image_info, buffer.data(), buffer_pitch.value());
    bitmap->sk_image()->readPixels(pixmap, 0, 0);
    return ConvertedTexture {
        .buffer = move(buffer),
        .width = width,
        .height = height,
    };
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
#include <LibWeb/WebGL/WebGLRenderingContextBase.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::WebGL {

using namespace Web::HTML;

class @class_name@ : public WebGLRenderingContextBase {
public:
    @class_name@(JS::Realm&, NonnullOwnPtr<OpenGLContext>);

    virtual OpenGLContext& context() override { return *m_context; }

    virtual void present() = 0;
    virtual void needs_to_present() = 0;
    virtual void set_error(GLenum) = 0;
)~~~");

    for (auto const& function : interface.functions) {
        if (function.extended_attributes.contains("FIXME")) {
            continue;
        }

        if (function.name == "getSupportedExtensions"_sv || function.name == "getExtension"_sv || function.name == "getContextAttributes"_sv || function.name == "isContextLost"_sv) {
            // Implemented in WebGLRenderingContext
            continue;
        }

        StringBuilder function_declaration;

        StringBuilder function_parameters;
        for (size_t i = 0; i < function.parameters.size(); ++i) {
            auto const& parameter = function.parameters[i];
            function_parameters.append(to_cpp_type(*parameter.type, interface));
            function_parameters.append(" "_sv);
            function_parameters.append(parameter.name.to_snakecase());
            if (i != function.parameters.size() - 1) {
                function_parameters.append(", "_sv);
            }
        }

        auto function_name = function.name.to_snakecase();
        function_declaration.append(to_cpp_type(*function.return_type, interface));
        function_declaration.append(" "_sv);
        function_declaration.append(function_name);
        function_declaration.append("("_sv);

        function_declaration.append(function_parameters.string_view());
        function_declaration.append(");"_sv);

        header_file_generator.append("    "_sv);
        header_file_generator.append(function_declaration.string_view());
        header_file_generator.append("\n"_sv);

        StringBuilder function_impl;
        SourceGenerator function_impl_generator { function_impl };
        function_impl_generator.set("class_name", class_name);

        ScopeGuard function_guard { [&] {
            function_impl_generator.append("}\n"_sv);
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
            function_impl_generator.append("    m_context->notify_content_will_change();\n"_sv);
        }

        if (function.name == "attachShader"_sv) {
            generate_webgl_object_handle_unwrap(function_impl_generator, "program"_sv, ""_sv);
            generate_webgl_object_handle_unwrap(function_impl_generator, "shader"_sv, ""_sv);
            function_impl_generator.append(R"~~~(
    if (program->attached_vertex_shader() == shader || program->attached_fragment_shader() == shader) {
        dbgln("WebGL: Shader is already attached to program");
        set_error(GL_INVALID_OPERATION);
        return;
    }

    if (shader->type() == GL_VERTEX_SHADER && program->attached_vertex_shader()) {
        dbgln("WebGL: Not attaching vertex shader to program as it already has a vertex shader attached");
        set_error(GL_INVALID_OPERATION);
        return;
    }

    if (shader->type() == GL_FRAGMENT_SHADER && program->attached_fragment_shader()) {
        dbgln("WebGL: Not attaching fragment shader to program as it already has a fragment shader attached");
        set_error(GL_INVALID_OPERATION);
        return;
    }

    glAttachShader(program_handle, shader_handle);

    switch (shader->type()) {
    case GL_VERTEX_SHADER:
        program->set_attached_vertex_shader(shader.ptr());
        break;
    case GL_FRAGMENT_SHADER:
        program->set_attached_fragment_shader(shader.ptr());
        break;
    default:
        VERIFY_NOT_REACHED();
    }
)~~~");
            continue;
        }

        if (function.name == "getUniformLocation"_sv) {
            generate_webgl_object_handle_unwrap(function_impl_generator, "program"_sv, "{}"_sv);
            function_impl_generator.append(R"~~~(
    auto name_null_terminated = null_terminated_string(name);
    return WebGLUniformLocation::create(m_realm, glGetUniformLocation(program_handle, name_null_terminated.data()));
)~~~");
            continue;
        }

        if (function.name == "createBuffer"_sv) {
            function_impl_generator.append(R"~~~(
    GLuint handle = 0;
    glGenBuffers(1, &handle);
    return WebGLBuffer::create(m_realm, *this, handle);
)~~~");
            continue;
        }

        if (function.name == "createShader"_sv) {
            function_impl_generator.append(R"~~~(
    if (type != GL_VERTEX_SHADER && type != GL_FRAGMENT_SHADER) {
        dbgln("Unknown WebGL shader type: 0x{:04x}", type);
        set_error(GL_INVALID_ENUM);
        return nullptr;
    }

    GLuint handle = glCreateShader(type);
    return WebGLShader::create(m_realm, *this, handle, type);
)~~~");
            continue;
        }

        if (function.name == "createTexture"_sv) {
            function_impl_generator.append(R"~~~(
    GLuint handle = 0;
    glGenTextures(1, &handle);
    return WebGLTexture::create(m_realm, *this, handle);
)~~~");
            continue;
        }

        if (function.name == "createFramebuffer"_sv) {
            function_impl_generator.append(R"~~~(
    GLuint handle = 0;
    glGenFramebuffers(1, &handle);
    return WebGLFramebuffer::create(m_realm, *this, handle);
)~~~");
            continue;
        }

        if (function.name == "createRenderbuffer"_sv) {
            function_impl_generator.append(R"~~~(
    GLuint handle = 0;
    glGenRenderbuffers(1, &handle);
    return WebGLRenderbuffer::create(m_realm, *this, handle);
)~~~");
            continue;
        }

        if (function.name == "invalidateFramebuffer"_sv) {
            function_impl_generator.append(R"~~~(
    glInvalidateFramebuffer(target, attachments.size(), attachments.data());
    needs_to_present();
)~~~");
            continue;
        }

        if (function.name == "createVertexArray"_sv) {
            function_impl_generator.append(R"~~~(
    GLuint handle = 0;
    glGenVertexArrays(1, &handle);
    return WebGLVertexArrayObject::create(m_realm, *this, handle);
)~~~");
            continue;
        }

        if (function.name == "createSampler"_sv) {
            function_impl_generator.append(R"~~~(
    GLuint handle = 0;
    glGenSamplers(1, &handle);
    return WebGLSampler::create(m_realm, *this, handle);
)~~~");
            continue;
        }

        if (function.name == "fenceSync"_sv) {
            function_impl_generator.append(R"~~~(
    GLsync handle = glFenceSync(condition, flags);
    return WebGLSync::create(m_realm, *this, handle);
)~~~");
            continue;
        }

        if (function.name == "shaderSource"_sv) {
            generate_webgl_object_handle_unwrap(function_impl_generator, "shader"_sv, ""_sv);
            function_impl_generator.append(R"~~~(
    Vector<GLchar*> strings;
    auto string = null_terminated_string(source);
    strings.append(string.data());
    Vector<GLint> length;
    length.append(source.bytes().size());
    glShaderSource(shader_handle, 1, strings.data(), length.data());
)~~~");
            continue;
        }

        if (function.name == "vertexAttribPointer"_sv) {
            function_impl_generator.append(R"~~~(
    glVertexAttribPointer(index, size, type, normalized, stride, reinterpret_cast<void*>(offset));
)~~~");
            continue;
        }

        if (function.name == "texStorage2D") {
            function_impl_generator.append(R"~~~(
    glTexStorage2D(target, levels, internalformat, width, height);
)~~~");
            continue;
        }

        if (function.name == "texImage2D"_sv && function.overload_index == 0) {
            function_impl_generator.append(R"~~~(
    void const* pixels_ptr = nullptr;
    if (pixels) {
        auto const& viewed_array_buffer = pixels->viewed_array_buffer();
        auto const& byte_buffer = viewed_array_buffer->buffer();
        pixels_ptr = byte_buffer.data() + pixels->byte_offset();
    }
    glTexImage2D(target, level, internalformat, width, height, border, format, type, pixels_ptr);
)~~~");
            continue;
        }

        if (function.name == "texImage3D"_sv && function.overload_index == 0) {
            // FIXME: If a WebGLBuffer is bound to the PIXEL_UNPACK_BUFFER target, generates an INVALID_OPERATION error.
            // FIXME: If srcData is null, a buffer of sufficient size initialized to 0 is passed.
            // FIXME: If type is specified as FLOAT_32_UNSIGNED_INT_24_8_REV, srcData must be null; otherwise, generates an INVALID_OPERATION error.
            // FIXME: If srcData is non-null, the type of srcData must match the type according to the above table; otherwise, generate an INVALID_OPERATION error.
            // FIXME: If an attempt is made to call this function with no WebGLTexture bound (see above), generates an INVALID_OPERATION error.
            // FIXME: If there's not enough data in srcData starting at srcOffset, generate INVALID_OPERATION.
            function_impl_generator.append(R"~~~(
    void const* src_data_ptr = nullptr;
    if (src_data) {
        auto const& viewed_array_buffer = src_data->viewed_array_buffer();
        auto const& byte_buffer = viewed_array_buffer->buffer();
        src_data_ptr = byte_buffer.data() + src_data->byte_offset();
    }
    glTexImage3D(target, level, internalformat, width, height, depth, border, format, type, src_data_ptr);
)~~~");
            continue;
        }

        if (function.name == "texImage3D"_sv && function.overload_index == 1) {
            // FIXME: If a WebGLBuffer is bound to the PIXEL_UNPACK_BUFFER target, generates an INVALID_OPERATION error.
            // FIXME: If srcData is null, a buffer of sufficient size initialized to 0 is passed.
            // FIXME: If type is specified as FLOAT_32_UNSIGNED_INT_24_8_REV, srcData must be null; otherwise, generates an INVALID_OPERATION error.
            // FIXME: If srcData is non-null, the type of srcData must match the type according to the above table; otherwise, generate an INVALID_OPERATION error.
            // FIXME: If an attempt is made to call this function with no WebGLTexture bound (see above), generates an INVALID_OPERATION error.
            // FIXME: If there's not enough data in srcData starting at srcOffset, generate INVALID_OPERATION.
            function_impl_generator.append(R"~~~(
    void const* src_data_ptr = nullptr;
    if (src_data) {
        auto const& viewed_array_buffer = src_data->viewed_array_buffer();
        auto const& byte_buffer = viewed_array_buffer->buffer();
        src_data_ptr = byte_buffer.data() + src_offset;
    }
    glTexImage3D(target, level, internalformat, width, height, depth, border, format, type, src_data_ptr);
)~~~");
            continue;
        }

        if (function.name == "texImage2D"_sv && (function.overload_index == 1 || (webgl_version == 2 && function.overload_index == 2))) {
            if (webgl_version == 2 && function.overload_index == 2) {
                function_impl_generator.append(R"~~~(
    auto maybe_converted_texture = read_and_pixel_convert_texture_image_source(source, format, type, width, height);
    if (!maybe_converted_texture.has_value())
        return;
    auto converted_texture = maybe_converted_texture.release_value();
    glTexImage2D(target, level, internalformat, converted_texture.width, converted_texture.height, border, format, type, converted_texture.buffer.data());
)~~~");
            } else {
                function_impl_generator.append(R"~~~(
    auto maybe_converted_texture = read_and_pixel_convert_texture_image_source(source, format, type);
    if (!maybe_converted_texture.has_value())
        return;
    auto converted_texture = maybe_converted_texture.release_value();
    glTexImage2D(target, level, internalformat, converted_texture.width, converted_texture.height, 0, format, type, converted_texture.buffer.data());
)~~~");
            }
            continue;
        }

        if (webgl_version == 2 && function.name == "texImage2D"_sv && function.overload_index == 3) {
            function_impl_generator.append(R"~~~(
    void const* pixels_ptr = nullptr;
    if (src_data) {
        auto const& viewed_array_buffer = src_data->viewed_array_buffer();
        auto const& byte_buffer = viewed_array_buffer->buffer();
        pixels_ptr = byte_buffer.data() + src_offset;
    }
    glTexImage2D(target, level, internalformat, width, height, border, format, type, pixels_ptr);
)~~~");
            continue;
        }

        if (function.name == "texSubImage2D"_sv && function.overload_index == 0) {
            function_impl_generator.append(R"~~~(
    void const* pixels_ptr = nullptr;
    if (pixels) {
        auto const& viewed_array_buffer = pixels->viewed_array_buffer();
        auto const& byte_buffer = viewed_array_buffer->buffer();
        pixels_ptr = byte_buffer.data() + pixels->byte_offset();
    }
    glTexSubImage2D(target, level, xoffset, yoffset, width, height, format, type, pixels_ptr);
)~~~");
            continue;
        }

        if (function.name == "texSubImage2D" && (function.overload_index == 1 || (webgl_version == 2 && function.overload_index == 2))) {
            if (webgl_version == 2 && function.overload_index == 2) {
                function_impl_generator.append(R"~~~(
    auto maybe_converted_texture = read_and_pixel_convert_texture_image_source(source, format, type, width, height);
)~~~");
            } else {
                function_impl_generator.append(R"~~~(
    auto maybe_converted_texture = read_and_pixel_convert_texture_image_source(source, format, type);
)~~~");
            }

            function_impl_generator.append(R"~~~(
    if (!maybe_converted_texture.has_value())
        return;
    auto converted_texture = maybe_converted_texture.release_value();
    glTexSubImage2D(target, level, xoffset, yoffset, converted_texture.width, converted_texture.height, format, type, converted_texture.buffer.data());
)~~~");
            continue;
        }

        if (webgl_version == 2 && function.name == "texSubImage2D"_sv && function.overload_index == 3) {
            function_impl_generator.append(R"~~~(
    void const* pixels_ptr = nullptr;
    if (src_data) {
        auto const& viewed_array_buffer = src_data->viewed_array_buffer();
        auto const& byte_buffer = viewed_array_buffer->buffer();
        pixels_ptr = byte_buffer.data() + src_data->byte_offset() + src_offset;
    }
    glTexSubImage2D(target, level, xoffset, yoffset, width, height, format, type, pixels_ptr);
)~~~");
            continue;
        }

        if (function.name == "texSubImage3D"_sv && function.overload_index == 0) {
            function_impl_generator.append(R"~~~(
    void const* pixels_ptr = nullptr;
    if (src_data) {
        auto const& viewed_array_buffer = src_data->viewed_array_buffer();
        auto const& byte_buffer = viewed_array_buffer->buffer();
        pixels_ptr = byte_buffer.data() + src_offset;
    }
    glTexSubImage3D(target, level, xoffset, yoffset, zoffset, width, height, depth, format, type, pixels_ptr);
)~~~");
            continue;
        }

        if (webgl_version == 2 && function.name == "compressedTexImage2D"_sv && function.overload_index == 0) {
            function_impl_generator.append(R"~~~(
    u8 const* pixels_ptr = src_data->viewed_array_buffer()->buffer().data();
    size_t count = src_data->byte_length();
    auto src_data_element_size = src_data->element_size();

    if ((src_offset * src_data_element_size) + src_length_override > count) {
        set_error(GL_INVALID_VALUE);
        return;
    }

    pixels_ptr += src_data->byte_offset();
    pixels_ptr += src_offset * src_data_element_size;
    if (src_length_override == 0) {
        count -= src_offset;
    } else {
        count = src_length_override;
    }

    glCompressedTexImage2D(target, level, internalformat, width, height, border, count, pixels_ptr);
)~~~");
            continue;
        }

        if (webgl_version == 2 && function.name == "compressedTexSubImage2D"_sv && function.overload_index == 0) {
            function_impl_generator.append(R"~~~(
    u8 const* pixels_ptr = src_data->viewed_array_buffer()->buffer().data();
    size_t count = src_data->byte_length();
    auto src_data_element_size = src_data->element_size();

    if ((src_offset * src_data_element_size) + src_length_override > count) {
        set_error(GL_INVALID_VALUE);
        return;
    }

    pixels_ptr += src_data->byte_offset();
    pixels_ptr += src_offset * src_data_element_size;
    if (src_length_override == 0) {
        count -= src_offset;
    } else {
        count = src_length_override;
    }

    glCompressedTexSubImage2D(target, level, xoffset, yoffset, width, height, format, count, pixels_ptr);
)~~~");
            continue;
        }

        if (function.name == "getShaderParameter"_sv) {
            generate_webgl_object_handle_unwrap(function_impl_generator, "shader"_sv, "JS::js_null()"_sv);
            function_impl_generator.append(R"~~~(
    GLint result = 0;
    glGetShaderiv(shader_handle, pname, &result);
    switch (pname) {
    case GL_SHADER_TYPE:
        return JS::Value(result);
    case GL_DELETE_STATUS:
    case GL_COMPILE_STATUS:
        return JS::Value(result == GL_TRUE);
    default:
        dbgln("Unknown WebGL shader parameter name: 0x{:04x}", pname);
        set_error(GL_INVALID_ENUM);
        return JS::js_null();
    }
)~~~");
            continue;
        }

        if (function.name == "getProgramParameter"_sv) {
            generate_webgl_object_handle_unwrap(function_impl_generator, "program"_sv, "JS::js_null()"_sv);
            function_impl_generator.append(R"~~~(
    GLint result = 0;
    glGetProgramiv(program_handle, pname, &result);
    switch (pname) {
    case GL_ATTACHED_SHADERS:
    case GL_ACTIVE_ATTRIBUTES:
    case GL_ACTIVE_UNIFORMS:
)~~~");

            if (webgl_version == 2) {
                function_impl_generator.append(R"~~~(
    case GL_TRANSFORM_FEEDBACK_BUFFER_MODE:
    case GL_TRANSFORM_FEEDBACK_VARYINGS:
    case GL_ACTIVE_UNIFORM_BLOCKS:
)~~~");
            }

            function_impl_generator.append(R"~~~(
        return JS::Value(result);
    case GL_DELETE_STATUS:
    case GL_LINK_STATUS:
    case GL_VALIDATE_STATUS:
        return JS::Value(result == GL_TRUE);
    default:
        dbgln("Unknown WebGL program parameter name: 0x{:04x}", pname);
        set_error(GL_INVALID_ENUM);
        return JS::js_null();
    }
)~~~");
            continue;
        }

        if (function.name == "getActiveUniformBlockParameter"_sv) {
            generate_get_active_uniform_block_parameter(function_impl_generator);
            continue;
        }

        if (function.name == "getSyncParameter"_sv) {
            // FIXME: In order to ensure consistent behavior across platforms, sync objects may only transition to the
            //        signaled state when the user agent's event loop is not executing a task. In other words:
            //          - A sync object must not become signaled until control has returned to the user agent's main
            //            loop.
            //          - Repeatedly fetching a sync object's SYNC_STATUS parameter in a loop, without returning
            //            control to the user agent, must always return the same value.
            // FIXME: Remove the GLsync cast once sync_handle actually returns the proper GLsync type.
            function_impl_generator.append(R"~~~(
    GLint result = 0;
    glGetSynciv((GLsync)(sync ? sync->sync_handle() : nullptr), pname, 1, nullptr, &result);
    return JS::Value(result);
)~~~");
            continue;
        }

        if (function.name == "getAttachedShaders"_sv) {
            generate_webgl_object_handle_unwrap(function_impl_generator, "program"_sv, "OptionalNone {}"_sv);
            function_impl_generator.append(R"~~~(
    (void)program_handle;

    Vector<GC::Root<WebGLShader>> result;

    if (program->attached_vertex_shader())
        result.append(GC::make_root(*program->attached_vertex_shader()));

    if (program->attached_fragment_shader())
        result.append(GC::make_root(*program->attached_fragment_shader()));

    return result;
)~~~");
            continue;
        }

        if (function.name == "bufferData"_sv && function.overload_index == 0) {
            function_impl_generator.append(R"~~~(
    glBufferData(target, size, 0, usage);
)~~~");
            continue;
        }

        if (webgl_version == 2 && function.name == "bufferData"_sv && function.overload_index == 2) {
            function_impl_generator.append(R"~~~(
    VERIFY(src_data);
    auto const& viewed_array_buffer = src_data->viewed_array_buffer();
    auto const& byte_buffer = viewed_array_buffer->buffer();
    auto src_data_length = src_data->byte_length();
    auto src_data_element_size = src_data->element_size();
    u8 const* buffer_ptr = byte_buffer.data();

    u64 copy_length = length == 0 ? src_data_length - src_offset : length;
    copy_length *= src_data_element_size;

    if (src_offset > src_data_length) {
        set_error(GL_INVALID_VALUE);
        return;
    }

    if (src_offset + copy_length > src_data_length) {
        set_error(GL_INVALID_VALUE);
        return;
    }

    buffer_ptr += src_offset * src_data_element_size;
    glBufferData(target, copy_length, buffer_ptr, usage);
)~~~");
            continue;
        }

        if (webgl_version == 2 && function.name == "bufferSubData"_sv && function.overload_index == 1) {
            function_impl_generator.append(R"~~~(
    VERIFY(src_data);
    auto const& viewed_array_buffer = src_data->viewed_array_buffer();
    auto const& byte_buffer = viewed_array_buffer->buffer();
    auto src_data_length = src_data->byte_length();
    auto src_data_element_size = src_data->element_size();
    u8 const* buffer_ptr = byte_buffer.data();

    u64 copy_length = length == 0 ? src_data_length - src_offset : length;
    copy_length *= src_data_element_size;

    if (src_offset > src_data_length) {
        set_error(GL_INVALID_VALUE);
        return;
    }

    if (src_offset + copy_length > src_data_length) {
        set_error(GL_INVALID_VALUE);
        return;
    }

    buffer_ptr += src_offset * src_data_element_size;
    glBufferSubData(target, dst_byte_offset, copy_length, buffer_ptr);
)~~~");
            continue;
        }

        if (function.name == "readPixels"_sv) {
            function_impl_generator.append(R"~~~(
    if (!pixels) {
        return;
    }

    void *ptr = pixels->viewed_array_buffer()->buffer().data() + pixels->byte_offset();
    glReadPixels(x, y, width, height, format, type, ptr);
)~~~");
            continue;
        }

        if (function.name == "drawElements"_sv) {
            function_impl_generator.append(R"~~~(
    glDrawElements(mode, count, type, reinterpret_cast<void*>(offset));
    needs_to_present();
)~~~");
            continue;
        }

        if (function.name == "drawElementsInstanced"_sv) {
            function_impl_generator.append(R"~~~(
    glDrawElementsInstanced(mode, count, type, reinterpret_cast<void*>(offset), instance_count);
    needs_to_present();
)~~~");
            continue;
        }

        if (function.name == "drawBuffers"_sv) {
            function_impl_generator.append(R"~~~(
    glDrawBuffers(buffers.size(), buffers.data());
)~~~");
            continue;
        }

        if (function.name.starts_with("uniformMatrix"_sv)) {
            auto number_of_matrix_elements = function.name.substring_view(13, 1);
            function_impl_generator.set("number_of_matrix_elements", number_of_matrix_elements);

            if (webgl_version == 1) {
                function_impl_generator.set("array_argument_name", "value");
            } else {
                function_impl_generator.set("array_argument_name", "data");
            }

            // "If the passed location is null, the data passed in will be silently ignored and no uniform variables will be changed."
            function_impl_generator.append(R"~~~(
    if (!location)
        return;

    auto matrix_size = @number_of_matrix_elements@ * @number_of_matrix_elements@;
    float const* raw_data = nullptr;
    u64 count = 0;
    if (@array_argument_name@.has<Vector<float>>()) {
        auto& vector_data = @array_argument_name@.get<Vector<float>>();
        raw_data = vector_data.data();
        count = vector_data.size() / matrix_size;
    } else {
        auto& typed_array_base = static_cast<JS::TypedArrayBase&>(*@array_argument_name@.get<GC::Root<WebIDL::BufferSource>>()->raw_object());
        auto& float32_array = as<JS::Float32Array>(typed_array_base);
        raw_data = float32_array.data().data();
        count = float32_array.array_length().length() / matrix_size;
    }
)~~~");

            if (webgl_version == 2) {
                function_impl_generator.append(R"~~~(
    if (src_offset + src_length > (count * matrix_size)) {
        set_error(GL_INVALID_VALUE);
        return;
    }

    raw_data += src_offset;
    if (src_length == 0) {
        count -= src_offset;
    } else {
        count = src_length;
    }
)~~~");
            }

            function_impl_generator.append(R"~~~(
    glUniformMatrix@number_of_matrix_elements@fv(location->handle(), count, transpose, raw_data);
)~~~");
            continue;
        }

        if (function.name == "uniform1fv"_sv || function.name == "uniform2fv"_sv || function.name == "uniform3fv"_sv || function.name == "uniform4fv"_sv || function.name == "uniform1iv"_sv || function.name == "uniform2iv"_sv || function.name == "uniform3iv"_sv || function.name == "uniform4iv"_sv) {
            auto number_of_vector_elements = function.name.substring_view(7, 1);
            auto element_type = function.name.substring_view(8, 1);
            if (element_type == "f"_sv) {
                function_impl_generator.set("cpp_element_type", "float"_sv);
                function_impl_generator.set("typed_array_type", "Float32Array"_sv);
                function_impl_generator.set("gl_postfix", "f"_sv);
            } else if (element_type == "i"_sv) {
                function_impl_generator.set("cpp_element_type", "int"_sv);
                function_impl_generator.set("typed_array_type", "Int32Array"_sv);
                function_impl_generator.set("gl_postfix", "i"_sv);
            } else {
                VERIFY_NOT_REACHED();
            }
            function_impl_generator.set("number_of_vector_elements", number_of_vector_elements);

            // "If the passed location is null, the data passed in will be silently ignored and no uniform variables will be changed."
            function_impl_generator.append(R"~~~(
    if (!location)
        return;

    @cpp_element_type@ const* data = nullptr;
    size_t count = 0;
    if (v.has<Vector<@cpp_element_type@>>()) {
        auto& vector = v.get<Vector<@cpp_element_type@>>();
        data = vector.data();
        count = vector.size();
    } else if (v.has<GC::Root<WebIDL::BufferSource>>()) {
        auto& typed_array_base = static_cast<JS::TypedArrayBase&>(*v.get<GC::Root<WebIDL::BufferSource>>()->raw_object());
        auto& typed_array = as<JS::@typed_array_type@>(typed_array_base);
        data = typed_array.data().data();
        count = typed_array.array_length().length();
    } else {
        VERIFY_NOT_REACHED();
    }
)~~~");

            if (webgl_version == 2) {
                function_impl_generator.append(R"~~~(
    if (src_offset + src_length > count) {
        set_error(GL_INVALID_VALUE);
        return;
    }

    data += src_offset;
    if (src_length == 0) {
        count -= src_offset;
    } else {
        count = src_length;
    }
)~~~");
            }

            function_impl_generator.append(R"~~~(
    glUniform@number_of_vector_elements@@gl_postfix@v(location->handle(), count / @number_of_vector_elements@, data);
)~~~");
            continue;
        }

        if (function.name == "vertexAttrib1fv"_sv || function.name == "vertexAttrib2fv"_sv || function.name == "vertexAttrib3fv"_sv || function.name == "vertexAttrib4fv"_sv) {
            auto number_of_vector_elements = function.name.substring_view(12, 1);
            function_impl_generator.set("number_of_vector_elements", number_of_vector_elements);
            function_impl_generator.append(R"~~~(
    if (values.has<Vector<float>>()) {
        auto& data = values.get<Vector<float>>();
        glVertexAttrib@number_of_vector_elements@fv(index, data.data());
        return;
    }

    auto& typed_array_base = static_cast<JS::TypedArrayBase&>(*values.get<GC::Root<WebIDL::BufferSource>>()->raw_object());
    auto& float32_array = as<JS::Float32Array>(typed_array_base);
    float const* data = float32_array.data().data();
    glVertexAttrib@number_of_vector_elements@fv(index, data);
)~~~");
            continue;
        }

        if (function.name == "vertexAttribIPointer"_sv) {
            function_impl_generator.append(R"~~~(
    glVertexAttribIPointer(index, size, type, stride, reinterpret_cast<void*>(offset));
)~~~");
            continue;
        }

        if (function.name == "getParameter"_sv) {
            generate_get_parameter(function_impl_generator, webgl_version);
            continue;
        }

        if (function.name == "getBufferParameter"_sv) {
            generate_get_buffer_parameter(function_impl_generator);
            continue;
        }

        if (function.name == "getInternalformatParameter") {
            generate_get_internal_format_parameter(function_impl_generator);
            continue;
        }

        if (function.name == "getActiveUniform"_sv) {
            generate_webgl_object_handle_unwrap(function_impl_generator, "program"_sv, "{}"_sv);
            function_impl_generator.append(R"~~~(
    GLint size = 0;
    GLenum type = 0;
    GLsizei buf_size = 256;
    GLsizei length = 0;
    GLchar name[256];
    glGetActiveUniform(program_handle, index, buf_size, &length, &size, &type, name);
    auto readonly_bytes = ReadonlyBytes { name, static_cast<size_t>(length) };
    return WebGLActiveInfo::create(m_realm, String::from_utf8_without_validation(readonly_bytes), type, size);
)~~~");
            continue;
        }

        if (function.name == "getActiveUniforms"_sv) {
            generate_webgl_object_handle_unwrap(function_impl_generator, "program"_sv, "{}"_sv);
            function_impl_generator.append(R"~~~(
    auto params = MUST(ByteBuffer::create_zeroed(uniform_indices.size() * sizeof(GLint)));
    Span<GLint> params_span(reinterpret_cast<GLint*>(params.data()), uniform_indices.size());
    glGetActiveUniformsiv(program_handle, uniform_indices.size(), uniform_indices.data(), pname, params_span.data());

    Vector<JS::Value> params_as_values;
    params_as_values.ensure_capacity(params.size());
    for (GLint param : params_span) {
        switch (pname) {
        case GL_UNIFORM_TYPE:
            params_as_values.unchecked_append(JS::Value(static_cast<GLenum>(param)));
            break;
        case GL_UNIFORM_SIZE:
            params_as_values.unchecked_append(JS::Value(static_cast<GLuint>(param)));
            break;
        case GL_UNIFORM_BLOCK_INDEX:
        case GL_UNIFORM_OFFSET:
        case GL_UNIFORM_ARRAY_STRIDE:
        case GL_UNIFORM_MATRIX_STRIDE:
            params_as_values.unchecked_append(JS::Value(param));
            break;
        case GL_UNIFORM_IS_ROW_MAJOR:
            params_as_values.unchecked_append(JS::Value(param == GL_TRUE));
            break;
        default:
            dbgln("Unknown WebGL uniform parameter name in getActiveUniforms: 0x{:04x}", pname);
            set_error(GL_INVALID_ENUM);
            return JS::js_null();
        }
    }

    return JS::Array::create_from(m_realm, params_as_values);
)~~~");
            continue;
        }

        if (function.name == "getActiveAttrib"_sv) {
            generate_webgl_object_handle_unwrap(function_impl_generator, "program"_sv, "{}"_sv);
            function_impl_generator.append(R"~~~(
    GLint size = 0;
    GLenum type = 0;
    GLsizei buf_size = 256;
    GLsizei length = 0;
    GLchar name[256];
    glGetActiveAttrib(program_handle, index, buf_size, &length, &size, &type, name);
    auto readonly_bytes = ReadonlyBytes { name, static_cast<size_t>(length) };
    return WebGLActiveInfo::create(m_realm, String::from_utf8_without_validation(readonly_bytes), type, size);
)~~~");
            continue;
        }

        if (function.name == "getShaderInfoLog"_sv) {
            generate_webgl_object_handle_unwrap(function_impl_generator, "shader"_sv, "{}"_sv);
            function_impl_generator.append(R"~~~(
    GLint info_log_length = 0;
    glGetShaderiv(shader_handle, GL_INFO_LOG_LENGTH, &info_log_length);
    Vector<GLchar> info_log;
    info_log.resize(info_log_length);
    if (!info_log_length)
        return String {};
    glGetShaderInfoLog(shader_handle, info_log_length, nullptr, info_log.data());
    return String::from_utf8_without_validation(ReadonlyBytes { info_log.data(), static_cast<size_t>(info_log_length - 1) });
)~~~");
            continue;
        }

        if (function.name == "getProgramInfoLog"_sv) {
            generate_webgl_object_handle_unwrap(function_impl_generator, "program"_sv, "{}"_sv);
            function_impl_generator.append(R"~~~(
    GLint info_log_length = 0;
    glGetProgramiv(program_handle, GL_INFO_LOG_LENGTH, &info_log_length);
    Vector<GLchar> info_log;
    info_log.resize(info_log_length);
    if (!info_log_length)
        return String {};
    glGetProgramInfoLog(program_handle, info_log_length, nullptr, info_log.data());
    return String::from_utf8_without_validation(ReadonlyBytes { info_log.data(), static_cast<size_t>(info_log_length - 1) });
)~~~");
            continue;
        }

        if (function.name == "getShaderPrecisionFormat"_sv) {
            function_impl_generator.append(R"~~~(
    GLint range[2];
    GLint precision;
    glGetShaderPrecisionFormat(shadertype, precisiontype, range, &precision);
    return WebGLShaderPrecisionFormat::create(m_realm, range[0], range[1], precision);
)~~~");
            continue;
        }

        if (function.name == "getActiveUniformBlockName"_sv) {
            generate_webgl_object_handle_unwrap(function_impl_generator, "program"_sv, "OptionalNone {}"_sv);
            function_impl_generator.append(R"~~~(
    GLint uniform_block_name_length = 0;
    glGetActiveUniformBlockiv(program_handle, uniform_block_index, GL_UNIFORM_BLOCK_NAME_LENGTH, &uniform_block_name_length);
    Vector<GLchar> uniform_block_name;
    uniform_block_name.resize(uniform_block_name_length);
    if (!uniform_block_name_length)
        return String {};
    glGetActiveUniformBlockName(program_handle, uniform_block_index, uniform_block_name_length, nullptr, uniform_block_name.data());
    return String::from_utf8_without_validation(ReadonlyBytes { uniform_block_name.data(), static_cast<size_t>(uniform_block_name_length - 1) });
)~~~");
            continue;
        }

        if (function.name == "deleteBuffer"_sv) {
            generate_webgl_object_handle_unwrap(function_impl_generator, "buffer"_sv, ""_sv);
            function_impl_generator.append(R"~~~(
    glDeleteBuffers(1, &buffer_handle);
)~~~");
            continue;
        }

        if (function.name == "deleteFramebuffer"_sv) {
            generate_webgl_object_handle_unwrap(function_impl_generator, "framebuffer"_sv, ""_sv);
            function_impl_generator.append(R"~~~(
    glDeleteFramebuffers(1, &framebuffer_handle);
)~~~");
            continue;
        }

        if (function.name == "deleteRenderbuffer"_sv) {
            generate_webgl_object_handle_unwrap(function_impl_generator, "renderbuffer"_sv, ""_sv);
            function_impl_generator.append(R"~~~(
    glDeleteRenderbuffers(1, &renderbuffer_handle);
)~~~");
            continue;
        }

        if (function.name == "deleteTexture"_sv) {
            generate_webgl_object_handle_unwrap(function_impl_generator, "texture"_sv, ""_sv);
            function_impl_generator.append(R"~~~(
    glDeleteTextures(1, &texture_handle);
)~~~");
            continue;
        }

        if (function.name == "deleteVertexArray"_sv) {
            generate_webgl_object_handle_unwrap(function_impl_generator, "vertex_array"_sv, ""_sv);
            function_impl_generator.append(R"~~~(
    glDeleteVertexArrays(1, &vertex_array_handle);
)~~~");
            continue;
        }

        if (function.name == "deleteSampler"_sv) {
            generate_webgl_object_handle_unwrap(function_impl_generator, "sampler"_sv, ""_sv);
            function_impl_generator.append(R"~~~(
    glDeleteSamplers(1, &sampler_handle);
)~~~");
            continue;
        }

        if (function.name == "bindBuffer"_sv) {
            // FIXME: Implement Buffer Object Binding restrictions.
            generate_webgl_object_handle_unwrap(function_impl_generator, "buffer"_sv, ""_sv);
            function_impl_generator.append(R"~~~(
    switch (target) {
    case GL_ELEMENT_ARRAY_BUFFER:
        m_element_array_buffer_binding = buffer;
        break;
    case GL_ARRAY_BUFFER:
        m_array_buffer_binding = buffer;
        break;
)~~~");

            if (webgl_version == 2) {
                function_impl_generator.append(R"~~~(
    case GL_UNIFORM_BUFFER:
        m_uniform_buffer_binding = buffer;
        break;
    case GL_COPY_READ_BUFFER:
        m_copy_read_buffer_binding = buffer;
        break;
    case GL_COPY_WRITE_BUFFER:
        m_copy_write_buffer_binding = buffer;
        break;
)~~~");
            }

            function_impl_generator.append(R"~~~(
    default:
        dbgln("Unknown WebGL buffer object binding target for storing current binding: 0x{:04x}", target);
        set_error(GL_INVALID_ENUM);
        return;
    }

    glBindBuffer(target, buffer_handle);
)~~~");
            continue;
        }

        if (function.name == "useProgram"_sv) {
            generate_webgl_object_handle_unwrap(function_impl_generator, "program"_sv, ""_sv);
            function_impl_generator.append(R"~~~(
    glUseProgram(program_handle);
    m_current_program = program;
)~~~");
            continue;
        }

        if (function.name == "bindFramebuffer"_sv) {
            generate_webgl_object_handle_unwrap(function_impl_generator, "framebuffer"_sv, ""_sv);
            function_impl_generator.append(R"~~~(
    glBindFramebuffer(target, framebuffer ? framebuffer_handle : m_context->default_framebuffer());
    m_framebuffer_binding = framebuffer;
)~~~");
            continue;
        }

        if (function.name == "bindRenderbuffer"_sv) {
            generate_webgl_object_handle_unwrap(function_impl_generator, "renderbuffer"_sv, ""_sv);
            function_impl_generator.append(R"~~~(
    glBindRenderbuffer(target, renderbuffer ? renderbuffer_handle : m_context->default_renderbuffer());
    m_renderbuffer_binding = renderbuffer;
)~~~");
            continue;
        }

        if (function.name == "bindTexture"_sv) {
            generate_webgl_object_handle_unwrap(function_impl_generator, "texture"_sv, ""_sv);
            function_impl_generator.append(R"~~~(
    switch (target) {
    case GL_TEXTURE_2D:
        m_texture_binding_2d = texture;
        break;
    case GL_TEXTURE_CUBE_MAP:
        m_texture_binding_cube_map = texture;
        break;
)~~~");

            if (webgl_version == 2) {
                function_impl_generator.append(R"~~~(
    case GL_TEXTURE_2D_ARRAY:
        m_texture_binding_2d_array = texture;
        break;
    case GL_TEXTURE_3D:
        m_texture_binding_3d = texture;
        break;
)~~~");
            }

            function_impl_generator.append(R"~~~(
    default:
        dbgln("Unknown WebGL texture target for storing current binding: 0x{:04x}", target);
        set_error(GL_INVALID_ENUM);
        return;
    }
    glBindTexture(target, texture_handle);
)~~~");
            continue;
        }

        if (function.name == "renderbufferStorage"_sv) {
            // To be backward compatible with WebGL 1, also accepts internal format DEPTH_STENCIL, which should be
            // mapped to DEPTH24_STENCIL8 by implementations.
            if (webgl_version == 1) {
                function_impl_generator.append(R"~~~(
#define GL_DEPTH_STENCIL 0x84F9
#define GL_DEPTH24_STENCIL8 0x88F0
)~~~");
            }

            function_impl_generator.append(R"~~~(
    if (internalformat == GL_DEPTH_STENCIL)
        internalformat = GL_DEPTH24_STENCIL8;
)~~~");

            if (webgl_version == 1) {
                function_impl_generator.append(R"~~~(
#undef GL_DEPTH_STENCIL
#undef GL_DEPTH24_STENCIL8
)~~~");
            }

            function_impl_generator.append(R"~~~(
    glRenderbufferStorage(target, internalformat, width, height);
)~~~");
            continue;
        }

        if (function.name.starts_with("samplerParameter"_sv)) {
            generate_webgl_object_handle_unwrap(function_impl_generator, "sampler"_sv, ""_sv);
            function_impl_generator.set("param_type", function.name.substring_view(16, 1));
            // pname is given in the following table:
            // - TEXTURE_COMPARE_FUNC
            // - TEXTURE_COMPARE_MODE
            // - TEXTURE_MAG_FILTER
            // - TEXTURE_MAX_LOD
            // - TEXTURE_MIN_FILTER
            // - TEXTURE_MIN_LOD
            // - TEXTURE_WRAP_R
            // - TEXTURE_WRAP_S
            // - TEXTURE_WRAP_T
            // If pname is not in the table above, generates an INVALID_ENUM error.
            // NOTE: We have to do this ourselves, as OpenGL does not.
            function_impl_generator.append(R"~~~(
    switch (pname) {
    case GL_TEXTURE_COMPARE_FUNC:
    case GL_TEXTURE_COMPARE_MODE:
    case GL_TEXTURE_MAG_FILTER:
    case GL_TEXTURE_MAX_LOD:
    case GL_TEXTURE_MIN_FILTER:
    case GL_TEXTURE_MIN_LOD:
    case GL_TEXTURE_WRAP_R:
    case GL_TEXTURE_WRAP_S:
    case GL_TEXTURE_WRAP_T:
        break;
    default:
        dbgln("Unknown WebGL sampler parameter name: 0x{:04x}", pname);
        set_error(GL_INVALID_ENUM);
        return;
    }
    glSamplerParameter@param_type@(sampler_handle, pname, param);
)~~~");
            continue;
        }

        if (function.name.starts_with("clearBuffer"_sv) && function.name.ends_with('v')) {
            auto element_type = function.name.substring_view(11, 2);
            if (element_type == "fv"_sv) {
                function_impl_generator.set("cpp_element_type", "float"_sv);
                function_impl_generator.set("typed_array_type", "Float32Array"_sv);
                function_impl_generator.set("gl_postfix", "f"_sv);
            } else if (element_type == "iv"_sv) {
                function_impl_generator.set("cpp_element_type", "int"_sv);
                function_impl_generator.set("typed_array_type", "Int32Array"_sv);
                function_impl_generator.set("gl_postfix", "i"_sv);
            } else if (element_type == "ui"_sv) {
                function_impl_generator.set("cpp_element_type", "u32"_sv);
                function_impl_generator.set("typed_array_type", "Uint32Array"_sv);
                function_impl_generator.set("gl_postfix", "ui"_sv);
            } else {
                VERIFY_NOT_REACHED();
            }
            function_impl_generator.append(R"~~~(
    @cpp_element_type@ const* data = nullptr;
    size_t count = 0;
    if (values.has<Vector<@cpp_element_type@>>()) {
        auto& vector = values.get<Vector<@cpp_element_type@>>();
        data = vector.data();
        count = vector.size();
    } else if (values.has<GC::Root<WebIDL::BufferSource>>()) {
        auto& typed_array_base = static_cast<JS::TypedArrayBase&>(*values.get<GC::Root<WebIDL::BufferSource>>()->raw_object());
        auto& typed_array = as<JS::@typed_array_type@>(typed_array_base);
        data = typed_array.data().data();
        count = typed_array.array_length().length();
    } else {
        VERIFY_NOT_REACHED();
    }

    switch (buffer) {
    case GL_COLOR:
        if (src_offset + 4 > count) {
            set_error(GL_INVALID_VALUE);
            return;
        }
        break;
    case GL_DEPTH:
    case GL_STENCIL:
        if (src_offset + 1 > count) {
            set_error(GL_INVALID_VALUE);
            return;
        }
        break;
    default:
        dbgln("Unknown WebGL buffer target for buffer clearing: 0x{:04x}", buffer);
        set_error(GL_INVALID_ENUM);
        return;
    }

    data += src_offset;
    glClearBuffer@gl_postfix@v(buffer, drawbuffer, data);
    needs_to_present();
)~~~");
            continue;
        }

        Vector<ByteString> gl_call_arguments;
        for (size_t i = 0; i < function.parameters.size(); ++i) {
            auto const& parameter = function.parameters[i];
            auto parameter_name = parameter.name.to_snakecase();
            if (parameter.type->is_numeric() || parameter.type->is_boolean()) {
                gl_call_arguments.append(parameter_name);
                continue;
            }
            if (parameter.type->is_string()) {
                function_impl_generator.set("parameter_name", parameter_name);
                function_impl_generator.append(R"~~~(
    auto @parameter_name@_null_terminated = null_terminated_string(@parameter_name@);
)~~~");
                gl_call_arguments.append(ByteString::formatted("{}_null_terminated.data()", parameter_name));
                continue;
            }
            if (is_webgl_object_type(parameter.type->name())) {
                if (function.return_type->name() == "undefined"_sv) {
                    function_impl_generator.set("early_return_value", "");
                } else if (function.return_type->is_integer()) {
                    function_impl_generator.set("early_return_value", "-1");
                } else if (function.return_type->is_boolean()) {
                    function_impl_generator.set("early_return_value", "false");
                } else {
                    VERIFY_NOT_REACHED();
                }
                function_impl_generator.set("handle_parameter_name", parameter_name);
                function_impl_generator.append(R"~~~(
    auto @handle_parameter_name@_handle = 0;
    if (@handle_parameter_name@) {
        auto handle_or_error = @handle_parameter_name@->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return @early_return_value@;
        }
        @handle_parameter_name@_handle = handle_or_error.release_value();
    }
)~~~");
                gl_call_arguments.append(ByteString::formatted("{}_handle", parameter_name));
                continue;
            }
            if (parameter.type->name() == "WebGLUniformLocation"_sv) {
                gl_call_arguments.append(ByteString::formatted("{} ? {}->handle() : 0", parameter_name, parameter_name));
                continue;
            }
            if (parameter.type->name() == "WebGLSync"_sv) {
                // FIXME: Remove the GLsync cast once sync_handle actually returns the proper GLsync type.
                gl_call_arguments.append(ByteString::formatted("(GLsync)({} ? {}->sync_handle() : nullptr)", parameter_name, parameter_name));
                continue;
            }
            if (parameter.type->name() == "BufferSource"_sv) {
                function_impl_generator.set("buffer_source_name", parameter_name);
                function_impl_generator.append(R"~~~(
    void const* ptr = nullptr;
    size_t byte_size = 0;
    if (@buffer_source_name@->is_typed_array_base()) {
        auto& typed_array_base = static_cast<JS::TypedArrayBase&>(*@buffer_source_name@->raw_object());
        ptr = typed_array_base.viewed_array_buffer()->buffer().data() + typed_array_base.byte_offset();
        byte_size = @buffer_source_name@->byte_length();
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
            if (parameter.type->name() == "ArrayBufferView"_sv) {
                function_impl_generator.set("buffer_source_name", parameter_name);

                function_impl_generator.append(R"~~~(
    void const* ptr = @buffer_source_name@->viewed_array_buffer()->buffer().data() + @buffer_source_name@->byte_offset();
    size_t byte_size = @buffer_source_name@->byte_length();
)~~~");
                gl_call_arguments.append(ByteString::formatted("byte_size"));
                gl_call_arguments.append(ByteString::formatted("ptr"));
                continue;
            }
            VERIFY_NOT_REACHED();
        }

        StringBuilder gl_call_arguments_string_builder;
        gl_call_arguments_string_builder.join(", "_sv, gl_call_arguments);

        auto gl_call_string = ByteString::formatted("{}({})", idl_to_gl_function_name(function.name), gl_call_arguments_string_builder.string_view());
        function_impl_generator.set("call_string", gl_call_string);

        if (gl_function_modifies_framebuffer(function.name)) {
            function_impl_generator.append("    needs_to_present();\n"_sv);
        }

        if (function.return_type->name() == "undefined"_sv) {
            function_impl_generator.append("    @call_string@;"_sv);
        } else if (function.return_type->is_integer() || function.return_type->is_boolean()) {
            function_impl_generator.append("    return @call_string@;"_sv);
        } else if (is_webgl_object_type(function.return_type->name())) {
            function_impl_generator.set("return_type_name", function.return_type->name());
            function_impl_generator.append("    return @return_type_name@::create(m_realm, *this, @call_string@);"_sv);
        } else {
            VERIFY_NOT_REACHED();
        }

        function_impl_generator.append("\n"_sv);
    }

    header_file_generator.append(R"~~~(
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
)~~~");

    if (webgl_version == 2) {
        header_file_generator.append(R"~~~(
    GC::Ptr<WebGLBuffer> m_uniform_buffer_binding;
    GC::Ptr<WebGLBuffer> m_copy_read_buffer_binding;
    GC::Ptr<WebGLBuffer> m_copy_write_buffer_binding;
    GC::Ptr<WebGLTexture> m_texture_binding_2d_array;
    GC::Ptr<WebGLTexture> m_texture_binding_3d;
)~~~");
    }

    header_file_generator.append(R"~~~(
    NonnullOwnPtr<OpenGLContext> m_context;
};

}
)~~~");

    implementation_file_generator.append(R"~~~(
void @class_name@::visit_edges(JS::Cell::Visitor& visitor)
{
    visitor.visit(m_realm);
    visitor.visit(m_array_buffer_binding);
    visitor.visit(m_element_array_buffer_binding);
    visitor.visit(m_current_program);
    visitor.visit(m_framebuffer_binding);
    visitor.visit(m_renderbuffer_binding);
    visitor.visit(m_texture_binding_2d);
    visitor.visit(m_texture_binding_cube_map);
)~~~");

    if (webgl_version == 2) {
        implementation_file_generator.append(R"~~~(
    visitor.visit(m_uniform_buffer_binding);
    visitor.visit(m_copy_read_buffer_binding);
    visitor.visit(m_copy_write_buffer_binding);
    visitor.visit(m_texture_binding_2d_array);
    visitor.visit(m_texture_binding_3d);
)~~~");
    }

    implementation_file_generator.append(R"~~~(
}

}
)~~~");

    MUST(generated_header_file->write_until_depleted(header_file_generator.as_string_view().bytes()));
    MUST(generated_implementation_file->write_until_depleted(implementation_file_generator.as_string_view().bytes()));

    return 0;
}
