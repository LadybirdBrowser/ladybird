/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <LibGC/Forward.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/Value.h>
#include <LibWeb/Bindings/Forward.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/WebIDL/Promise.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::WebAssembly {

class Global;
class Instance;
class Memory;
class Module;
class Table;

}

namespace Web::Bindings {

WEB_API WebIDL::ExceptionOr<GC::Ref<WebAssembly::Global>> construct_global(JS::Realm&, GlobalDescriptor const&, Optional<JS::Value>);
WEB_API WebIDL::ExceptionOr<JS::Value> value(JS::Realm&, WebAssembly::Global&);
WEB_API WebIDL::ExceptionOr<void> set_value(JS::Realm&, WebAssembly::Global&, JS::Value);
WEB_API WebIDL::ExceptionOr<JS::Value> value_of(JS::Realm&, WebAssembly::Global&);

WEB_API WebIDL::ExceptionOr<GC::Ref<WebAssembly::Instance>> construct_instance(JS::Realm&, WebAssembly::Module&, GC::Ptr<JS::Object> import_object);
WEB_API JS::Object const* exports(JS::Realm&, GC::Ref<WebAssembly::Instance>);
WEB_API JS::ThrowCompletionOr<void> initialize_webassembly_export_binding(JS::Realm&, JS::Environment&, Utf16FlyString const& name, GC::Ref<WebAssembly::Instance>);

WEB_API WebIDL::ExceptionOr<GC::Ref<WebAssembly::Table>> construct_table(JS::Realm&, TableDescriptor const&, Optional<JS::Value>);
WEB_API JS::Value table(JS::Realm&, GC::Ref<WebAssembly::Table>);
WEB_API WebIDL::ExceptionOr<JS::Value> grow(JS::Realm&, WebAssembly::Table&, JS::Value delta_value, Optional<JS::Value>);
WEB_API WebIDL::ExceptionOr<JS::Value> get(JS::Realm&, WebAssembly::Table&, JS::Value index_value);
WEB_API WebIDL::ExceptionOr<void> set(JS::Realm&, WebAssembly::Table&, JS::Value index_value, Optional<JS::Value>);
WEB_API WebIDL::ExceptionOr<JS::Value> length(JS::Realm&, WebAssembly::Table&);

WEB_API WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> compile(JS::Realm&, WebIDL::BufferSource bytes);
WEB_API WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> compile_streaming(JS::Realm&, GC::Ref<WebIDL::Promise> source);
WEB_API WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> instantiate(JS::Realm&, WebIDL::BufferSource bytes, GC::Ptr<JS::Object> import_object);
WEB_API WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> instantiate(JS::Realm&, GC::Ref<WebAssembly::Module>, GC::Ptr<JS::Object> import_object);
WEB_API WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> instantiate_streaming(JS::Realm&, GC::Ref<WebIDL::Promise> source, GC::Ptr<JS::Object> import_object);
WEB_API WebAssembly::Global* global_from_value(JS::Value);
WEB_API WebAssembly::Instance* instance_from_value(JS::Value);
WEB_API WebAssembly::Memory* memory_from_value(JS::Value);
WEB_API WebAssembly::Module* module_from_value(JS::Value);
WEB_API WebAssembly::Table* table_from_value(JS::Value);
WEB_API void resolve_webassembly_module_promise(JS::Realm&, WebIDL::Promise&, GC::Ref<WebAssembly::Module>);
WEB_API void resolve_webassembly_instance_promise(JS::Realm&, WebIDL::Promise&, GC::Ref<WebAssembly::Instance>);
WEB_API GC::Ref<JS::Object> create_webassembly_instantiated_source(JS::Realm&, GC::Ref<WebAssembly::Module>, GC::Ref<WebAssembly::Instance>);
WEB_API WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> array_buffer_for_webassembly_response(JS::Realm&, Fetch::Response const&);

}
