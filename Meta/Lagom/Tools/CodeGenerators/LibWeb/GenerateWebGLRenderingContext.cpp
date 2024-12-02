/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "BindingsGenerator/IDLGenerators.h"
#include <AK/SourceGenerator.h>
#include <AK/StringBuilder.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/File.h>
#include <LibIDL/IDLParser.h>
#include <LibMain/Main.h>

static bool is_webgl_object_type(StringView type_name)
{
    return type_name == "WebGLShader"sv
        || type_name == "WebGLBuffer"sv
        || type_name == "WebGLFramebuffer"sv
        || type_name == "WebGLProgram"sv
        || type_name == "WebGLRenderbuffer"sv
        || type_name == "WebGLTexture"sv
        || type_name == "WebGLUniformLocation"sv;
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

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    StringView generated_header_path;
    StringView generated_implementation_path;
    Vector<ByteString> base_paths;
    StringView webgl_context_idl_path;

    Core::ArgsParser args_parser;
    args_parser.add_option(webgl_context_idl_path, "Path to the WebGLRenderingContext.idl file", "webgl-idl-path", 'i', "webgl-idl-path");
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
    args_parser.add_option(generated_header_path, "Path to the Enums header file to generate", "generated-header-path", 'h', "generated-header-path");
    args_parser.add_option(generated_implementation_path, "Path to the Enums implementation file to generate", "generated-implementation-path", 'c', "generated-implementation-path");
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

    StringBuilder header_file_string_builder;
    SourceGenerator header_file_generator { header_file_string_builder };

    StringBuilder implementation_file_string_builder;
    SourceGenerator implementation_file_generator { implementation_file_string_builder };

    implementation_file_generator.append(R"~~~(
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibWeb/WebGL/OpenGLContext.h>
#include <LibWeb/WebGL/WebGLActiveInfo.h>
#include <LibWeb/WebGL/WebGLBuffer.h>
#include <LibWeb/WebGL/WebGLFramebuffer.h>
#include <LibWeb/WebGL/WebGLProgram.h>
#include <LibWeb/WebGL/WebGLRenderbuffer.h>
#include <LibWeb/WebGL/WebGLRenderingContextImpl.h>
#include <LibWeb/WebGL/WebGLShader.h>
#include <LibWeb/WebGL/WebGLTexture.h>
#include <LibWeb/WebGL/WebGLUniformLocation.h>
#include <LibWeb/WebIDL/Buffers.h>

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

namespace Web::WebGL {

static Vector<GLchar> null_terminated_string(StringView string)
{
    Vector<GLchar> result;
    for (auto c : string.bytes())
        result.append(c);
    result.append('\\0');
    return result;
}

WebGLRenderingContextImpl::WebGLRenderingContextImpl(JS::Realm& realm, NonnullOwnPtr<OpenGLContext> context)
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
#include <LibWeb/HTML/HTMLCanvasElement.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::WebGL {

using namespace Web::HTML;

class WebGLRenderingContextImpl {
public:
    WebGLRenderingContextImpl(JS::Realm&, NonnullOwnPtr<OpenGLContext>);

    OpenGLContext& context() { return *m_context; }

    virtual void present() = 0;
    virtual void needs_to_present() = 0;
)~~~");

    for (auto const& function : interface.functions) {
        if (function.extended_attributes.contains("FIXME")) {
            continue;
        }

        if (function.name == "getSupportedExtensions"sv || function.name == "getExtension"sv) {
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

        ScopeGuard function_guard { [&] {
            function_impl_generator.append("}\n"sv);
            implementation_file_generator.append(function_impl_generator.as_string_view().bytes());
        } };

        function_impl_generator.set("function_name", function_name);
        function_impl_generator.set("function_parameters", function_parameters.string_view());
        function_impl_generator.set("function_return_type", to_cpp_type(*function.return_type, interface));
        function_impl_generator.append(R"~~~(
@function_return_type@ WebGLRenderingContextImpl::@function_name@(@function_parameters@)
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

        if (function.name == "getParameter"sv) {
            function_impl_generator.append(R"~~~(
    GLint result = 0;
    glGetIntegerv(pname, &result);
    return JS::Value(result);
)~~~");
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

        Vector<ByteString> gl_call_arguments;
        for (size_t i = 0; i < function.parameters.size(); ++i) {
            auto const& parameter = function.parameters[i];
            if (parameter.type->is_numeric() || parameter.type->is_boolean()) {
                gl_call_arguments.append(parameter.name);
                continue;
            }
            if (parameter.type->is_string()) {
                gl_call_arguments.append(ByteString::formatted("{}", parameter.name));
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
        VERIFY_NOT_REACHED();
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
