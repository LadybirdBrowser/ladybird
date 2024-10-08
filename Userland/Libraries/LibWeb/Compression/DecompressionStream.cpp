/*
 * Copyright (c) 2024, Johan Dahlin <jdahlin@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCompress/Deflate.h>
#include <LibCompress/Gzip.h>
#include <LibCompress/Zlib.h>
#include <LibJS/Bytecode/Interpreter.h>
#include <LibJS/Runtime/ECMAScriptFunctionObject.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/DecompressionStreamPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Compression/DecompressionStream.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/Buffers.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Compression {

JS_DEFINE_ALLOCATOR(DecompressionStream);

WebIDL::ExceptionOr<JS::NonnullGCPtr<DecompressionStream>> DecompressionStream::construct_impl(JS::Realm& realm, Bindings::CompressionFormat format)
{
    auto stream = realm.heap().allocate<DecompressionStream>(realm, realm, format);

    auto& vm = realm.vm();
    auto* env = vm.variable_environment();
    if (env) {
        // FIXME: Make this private to the web execution context
        auto& global_object = realm.global_object();
        auto constructor_value_or_error = global_object.get("DecompressionStream_constructor");
        auto constructor_value = TRY(constructor_value_or_error);
        if (constructor_value.is_empty() || constructor_value.is_undefined()) {
            return WebIDL::SimpleException {
                WebIDL::SimpleExceptionType::TypeError,
                "DecompressionStream constructor not found"sv
            };
        }

        auto& func = static_cast<JS::ECMAScriptFunctionObject&>(constructor_value.as_function());
        JS::MarkedVector<JS::Value> arguments_list { vm.heap() };
        arguments_list.append(JS::PrimitiveString::create(vm, Bindings::idl_enum_to_string(format)));
        TRY(func.internal_call(stream->m_this_value, move(arguments_list)));
    }
    return stream;
}

WebIDL::ExceptionOr<JS::NonnullGCPtr<JS::Uint8Array>> DecompressionStream::decompress(JS::VM& vm, Bindings::CompressionFormat format, JS::Handle<WebIDL::BufferSource> buffer_source)
{
    auto realm = vm.current_realm();
    auto data_buffer_or_error = WebIDL::get_buffer_source_copy(*buffer_source->raw_object());
    if (data_buffer_or_error.is_error())
        return WebIDL::OperationError::create(*realm, "Failed to copy bytes from ArrayBuffer"_fly_string);

    ByteBuffer const& data_buffer = data_buffer_or_error.value();
    Optional<ErrorOr<ByteBuffer>> decompressed;

    if (format == Bindings::CompressionFormat::Deflate) {
        decompressed = Compress::ZlibDecompressor::decompress_all(data_buffer);
    } else if (format == Bindings::CompressionFormat::Gzip) {
        decompressed = Compress::GzipDecompressor::decompress_all(data_buffer);
    } else if (format == Bindings::CompressionFormat::DeflateRaw) {
        decompressed = Compress::DeflateDecompressor::decompress_all(data_buffer);
    } else {
        return WebIDL::SimpleException {
            WebIDL::SimpleExceptionType::TypeError,
            "Invalid compression format"sv
        };
    }

    if (decompressed.value().is_error()) {
        outln("Failed to decompress data: {}", decompressed.value().error());
        return WebIDL::OperationError::create(*realm, "Failed to decompress data"_fly_string);
    }

    auto decompressed_data = decompressed.value().release_value();
    auto array_buffer = JS::ArrayBuffer::create(*realm, decompressed_data);
    return JS::Uint8Array::create(*realm, array_buffer->byte_length(), *array_buffer);
}

static JS::GCPtr<JS::Script> import_js_script(JS::Realm& realm)
{
    auto& vm = realm.vm();
    auto file = MUST(Core::File::open("/Users/johandahlin/dev/ladybird/Userland/Libraries/LibWeb/Compression/DecompressionStream.js"sv, Core::File::OpenMode::Read));
    auto file_contents = MUST(file->read_until_eof());
    auto source = StringView { file_contents };

    auto script = MUST(JS::Script::parse(source, realm, "DecompressionStream.js"sv));
    MUST(vm.bytecode_interpreter().run(*script));
    return script;
}

DecompressionStream::DecompressionStream(JS::Realm& realm, Bindings::CompressionFormat format)
    : Bindings::PlatformObject(realm)
    , m_format(format)
    , m_js_script(import_js_script(realm))
    , m_this_value(JS::Object::create(realm, realm.intrinsics().object_prototype()))
{
}

DecompressionStream::~DecompressionStream() = default;

void DecompressionStream::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(DecompressionStream);
}

void DecompressionStream::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_js_script);
    visitor.visit(m_this_value);
}

}
