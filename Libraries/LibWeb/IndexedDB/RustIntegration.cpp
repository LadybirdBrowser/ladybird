/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonValue.h>
#include <AK/ScopeGuard.h>
#include <LibGC/Heap.h>
#include <LibGC/Weak.h>
#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/Realm.h>
#include <LibURL/RustIntegration.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/DOMStringList.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/StorageAPI/StorageKey.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/WebIDL/ExceptionOrUtils.h>
#include <LibWebCommon/IndexedDB/TransactionChanges.h>

// What the Rust implementation of IndexedDB asks of LibWeb's C++ code, declared on the Rust side in
// Libraries/LibWeb/Bindings/Rust/src/indexed_db/cpp.rs.

using namespace Web;

extern "C" {

void web_rust_indexed_db_check_pending_connection_wait(JSVM*, void* database);

void* web_indexed_db_create_dom_exception(ReadonlyBytes name, FlatPtr message);
void* web_indexed_db_dom_exception_from_value(JSValue);
JSObject* web_indexed_db_wrap_dom_exception(void* realm, void* exception);
bool web_indexed_db_obtain_a_storage_key(void* realm, void* ctx, void (*on_origin)(void*, URL::FFI::FfiOrigin const*));
JSObject* web_indexed_db_create_dom_string_list(void* realm, FlatPtr const* strings, size_t count);
JSCompletion web_indexed_db_structured_serialize_for_storage(void* realm, JSValue, void* bytes, void (*append)(void*, u8 const*, size_t));
JSCompletion web_indexed_db_structured_deserialize(void* realm, ReadonlyBytes);
void web_indexed_db_queue_a_database_task(void* cell, void (*steps)(JSVM*, void*));
void web_indexed_db_run_in_parallel(void* cell, void (*steps)(JSVM*, void*));
void* web_indexed_db_main_thread_event_loop();
void web_indexed_db_enqueue_check_pending_connection_wait(void* database);
bool web_indexed_db_devtools_client_is_active(void* realm);
IndexedDB::TransactionChanges* web_indexed_db_create_transaction_changes();
void web_indexed_db_append_transaction_change(IndexedDB::TransactionChanges*, u8 kind, FlatPtr database_name, FlatPtr object_store_name, JsonValue* key);
void web_indexed_db_notify_devtools(void* realm, IndexedDB::TransactionChanges*);
JsonValue* web_indexed_db_json_number(double);
JsonValue* web_indexed_db_json_string(FlatPtr);
JsonValue* web_indexed_db_json_binary(ReadonlyBytes);
JsonValue* web_indexed_db_json_null();
JsonValue* web_indexed_db_json_array();
void web_indexed_db_json_array_append(JsonValue* array, JsonValue* value);
}

void* web_indexed_db_create_dom_exception(ReadonlyBytes name, FlatPtr message)
{
    return WebIDL::DOMException::create(FlyString::from_utf8_without_validation(name), Utf16String::from_raw(message)).ptr();
}

void* web_indexed_db_dom_exception_from_value(JSValue raw_value)
{
    auto value = JS::HostABI::value_from_abi(raw_value);
    if (!value.is_object())
        return nullptr;
    return Bindings::impl_from<WebIDL::DOMException>(&value.as_object());
}

JSObject* web_indexed_db_wrap_dom_exception(void* realm, void* exception)
{
    auto& js_realm = *static_cast<JS::Realm*>(realm);
    auto& dom_exception = *static_cast<WebIDL::DOMException*>(exception);
    return JS::HostABI::object_to_abi(Bindings::wrap(Bindings::host_defined_wrapper_world(js_realm), js_realm, GC::Ref { dom_exception }).ptr());
}

bool web_indexed_db_obtain_a_storage_key(void* realm, void* ctx, void (*on_origin)(void*, URL::FFI::FfiOrigin const*))
{
    auto& js_realm = *static_cast<JS::Realm*>(realm);
    auto storage_key = StorageAPI::obtain_a_storage_key(HTML::relevant_settings_object(js_realm.global_object()));
    if (!storage_key.has_value())
        return false;
    URL::RustIntegration::lend_origin_to_ffi(storage_key->origin, [&](URL::FFI::FfiOrigin const& origin) {
        on_origin(ctx, &origin);
    });
    return true;
}

JSObject* web_indexed_db_create_dom_string_list(void* realm, FlatPtr const* strings, size_t count)
{
    auto& js_realm = *static_cast<JS::Realm*>(realm);
    Vector<String> names;
    names.ensure_capacity(count);
    for (size_t i = 0; i < count; ++i)
        names.unchecked_append(Utf16String::from_raw(strings[i]).to_utf8());
    auto list = HTML::DOMStringList::create(move(names));
    return JS::HostABI::object_to_abi(Bindings::wrap(Bindings::host_defined_wrapper_world(js_realm), js_realm, list).ptr());
}

// https://html.spec.whatwg.org/multipage/structured-data.html#structuredserializeforstorage
JSCompletion web_indexed_db_structured_serialize_for_storage(void* realm, JSValue raw_value, void* bytes, void (*append)(void*, u8 const*, size_t))
{
    auto& js_realm = *static_cast<JS::Realm*>(realm);
    auto serialized = HTML::structured_serialize_for_storage(js_realm.vm(), JS::HostABI::value_from_abi(raw_value));
    if (serialized.is_exception())
        return JS::HostABI::completion_to_abi(JS::ThrowCompletionOr<JS::Value> { WebIDL::exception_to_throw_completion(js_realm.vm(), js_realm, serialized.release_error()) });
    auto record = serialized.release_value();
    append(bytes, record.data.data(), record.data.size());
    return JS::HostABI::completion_to_abi(JS::ThrowCompletionOr<JS::Value> { JS::js_undefined() });
}

// https://html.spec.whatwg.org/multipage/structured-data.html#structureddeserialize
JSCompletion web_indexed_db_structured_deserialize(void* realm, ReadonlyBytes bytes)
{
    auto& js_realm = *static_cast<JS::Realm*>(realm);
    HTML::StorageSerializationRecord record { MUST(ByteBuffer::copy(bytes)) };
    auto value = HTML::structured_deserialize(js_realm.vm(), record, js_realm);
    if (value.is_exception())
        return JS::HostABI::completion_to_abi(JS::ThrowCompletionOr<JS::Value> { WebIDL::exception_to_throw_completion(js_realm.vm(), js_realm, value.release_error()) });
    return JS::HostABI::completion_to_abi(JS::ThrowCompletionOr<JS::Value> { value.release_value() });
}

// https://w3c.github.io/IndexedDB/#queue-a-database-task
void web_indexed_db_queue_a_database_task(void* cell, void (*steps)(JSVM*, void*))
{
    GC::Ref<GC::Cell> function = *static_cast<GC::Cell*>(cell);
    // NB: The task's captures, which the collector scans, keep the function alive until the steps run.
    HTML::queue_a_task(HTML::Task::Source::DatabaseAccess, nullptr, nullptr, GC::create_function(GC::Heap::the(), [function, steps] {
        steps(JS::HostABI::vm_to_abi(Bindings::main_thread_vm()), function.ptr());
    }));
}

void web_indexed_db_run_in_parallel(void* cell, void (*steps)(JSVM*, void*))
{
    GC::Ref<GC::Cell> function = *static_cast<GC::Cell*>(cell);
    // NB: The function's captures, which the collector scans, keep the cell alive until the steps run.
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(GC::Heap::the(), [function, steps] {
        steps(JS::HostABI::vm_to_abi(Bindings::main_thread_vm()), function.ptr());
    }));
}

void* web_indexed_db_main_thread_event_loop()
{
    return &HTML::main_thread_event_loop();
}

void web_indexed_db_enqueue_check_pending_connection_wait(void* database)
{
    GC::Heap::the().enqueue_post_gc_task([database = GC::Weak<GC::Cell>(*static_cast<GC::Cell*>(database))] {
        if (!database)
            return;
        web_rust_indexed_db_check_pending_connection_wait(JS::HostABI::vm_to_abi(Bindings::main_thread_vm()), database.ptr().ptr());
    });
}

static GC::Ptr<DOM::Document> responsible_document(void* realm)
{
    auto& js_realm = *static_cast<JS::Realm*>(realm);
    return HTML::relevant_settings_object(js_realm.global_object()).responsible_document();
}

bool web_indexed_db_devtools_client_is_active(void* realm)
{
    auto document = responsible_document(realm);
    return document && document->page().client().has_active_devtools_client();
}

IndexedDB::TransactionChanges* web_indexed_db_create_transaction_changes()
{
    return new IndexedDB::TransactionChanges;
}

void web_indexed_db_append_transaction_change(IndexedDB::TransactionChanges* changes, u8 kind, FlatPtr database_name, FlatPtr object_store_name, JsonValue* key)
{
    IndexedDB::TransactionChange change { Utf16String::from_raw(database_name), Utf16String::from_raw(object_store_name) };
    if (key) {
        change.key = move(*key);
        delete key;
    }
    switch (kind) {
    case 0:
        changes->added.append(move(change));
        break;
    case 1:
        changes->changed.append(move(change));
        break;
    case 2:
        changes->deleted.append(move(change));
        break;
    default:
        VERIFY_NOT_REACHED();
    }
}

void web_indexed_db_notify_devtools(void* realm, IndexedDB::TransactionChanges* changes)
{
    ScopeGuard delete_changes = [&] { delete changes; };
    auto document = responsible_document(realm);
    if (!document)
        return;
    document->page().client().page_did_update_indexed_database(document->url().serialize(), *changes);
}

JsonValue* web_indexed_db_json_number(double value)
{
    return new JsonValue(value);
}

JsonValue* web_indexed_db_json_string(FlatPtr value)
{
    return new JsonValue(Utf16String::from_raw(value).to_utf8());
}

JsonValue* web_indexed_db_json_binary(ReadonlyBytes bytes)
{
    return new JsonValue(MUST(String::formatted("{}", bytes)));
}

JsonValue* web_indexed_db_json_null()
{
    return new JsonValue;
}

JsonValue* web_indexed_db_json_array()
{
    return new JsonValue(JsonArray {});
}

void web_indexed_db_json_array_append(JsonValue* array, JsonValue* value)
{
    array->as_array().must_append(move(*value));
    delete value;
}
