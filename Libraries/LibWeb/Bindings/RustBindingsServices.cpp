/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Transport.h>
#include <LibIPC/TransportHandle.h>
#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/HostObject.h>
#include <LibJS/Runtime/Realm.h>
#include <LibURL/RustIntegration.h>
#include <LibWeb/Bindings/DOMException.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/Bindings/RustSerialization.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/CustomElements/CustomElementReactions.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/Scripting/Agent.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/SimilarOriginWindowAgent.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WindowProxy.h>
#include <LibWeb/HTML/WorkerGlobalScope.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/CallbackType.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/WebIDL/ExceptionOrUtils.h>
#include <LibWeb/WebIDL/Promise.h>

// What the Rust bindings of the interfaces implemented in Rust ask of LibWeb's C++ bindings, declared on the Rust
// side in Libraries/LibWeb/Bindings/Rust/src/bindings/services.rs.

// The wrapper of an object of an interface implemented in Rust that the C++ bindings name, in the wrapper world of
// the realm, which the Rust bindings generated for those interfaces define.
extern "C" JSObject* web_rust_bindings_wrap(JSVM*, void* realm, u16 interface_name, void* cell);

namespace Web::Bindings {

GC::Ref<JS::HostObject> wrap_rust_interface_object(WrapperWorld& wrapper_world, JS::Realm& preferred_realm, InterfaceName interface_name, RustInterfaceCell& cell)
{
    // NB: The Rust bindings find the wrapper world of an object through the realm that asks for its wrapper.
    VERIFY(&wrapper_world == &host_defined_wrapper_world(preferred_realm));
    auto* wrapper = web_rust_bindings_wrap(JS::HostABI::vm_to_abi(preferred_realm.vm()), &preferred_realm, to_underlying(interface_name), &cell);
    return as<JS::HostObject>(*JS::HostABI::object_from_abi(wrapper));
}

constexpr JSHostClass web_bindings_rust_interface_wrapper_host_class = JS::make_host_class(JS_HOST_CLASS_OBJECT, "RustInterface"sv,
    nullptr, nullptr, nullptr, JS_HOST_CLASS_IS_PLATFORM_OBJECT);

RustInterfaceCell* rust_interface_cell_of_wrapper(JS::Object& object, JSHostClass const& wrapper_host_class)
{
    auto* wrapper = as_if<JS::HostObject>(object);
    if (!wrapper || &wrapper->host_class() != &wrapper_host_class)
        return nullptr;
    return GC::static_cell_cast<RustInterfaceCell>(JS::host_data_of(object).ptr());
}

bool rust_interface_object_implements_interface(JS::Object const& object, StringView interface_name)
{
    auto const* wrapper = as_if<JS::HostObject>(object);
    if (!wrapper || !JS::host_data_of(object))
        return false;
    // NB: The host class of an interface's wrappers is named after it, and chains through the parent interfaces'
    //     classes up to the class of every Rust interface, or else to that of a C++ interface it inherits from.
    for (auto const* host_class = &wrapper->host_class(); host_class && host_class != &web_bindings_rust_interface_wrapper_host_class; host_class = host_class->parent) {
        if (StringView { host_class->name, host_class->name_length } == interface_name)
            return true;
    }
    return false;
}

JSCompletion completion_to_abi(JS::Realm& realm, WebIDL::ExceptionOr<JS::Value> result)
{
    if (result.is_exception())
        return JS::HostABI::completion_to_abi(JS::ThrowCompletionOr<JS::Value> { WebIDL::exception_to_throw_completion(realm.vm(), realm, result.exception()) });
    return JS::HostABI::completion_to_abi(JS::ThrowCompletionOr<JS::Value> { result.release_value() });
}

extern "C" {

void* web_bindings_wrapper_world(void* realm, bool* is_main_world);
JSObject* web_bindings_rust_wrapper_for(void* realm, void* cell);
void web_bindings_set_rust_wrapper(void* realm, void* cell, JSObject* wrapper);
JSObject* web_bindings_rejected_promise(void* current_realm, JSValue this_value, JSValue reason);
void web_bindings_queue_a_microtask(void* realm, JSObject* function);
void web_bindings_run_in_parallel(void* realm, void* cell, void (*steps)(JSVM*, void*));
void web_bindings_queue_a_task(void* realm, u8 task_source, void* cell, void (*steps)(JSVM*, void*));
u64 web_bindings_queue_a_global_task(JSObject* global_object, u8 task_source, void* cell, void (*steps)(JSVM*, void*));
void web_bindings_remove_tasks(void* realm, ReadonlySpan<u64> task_ids);
u64 web_bindings_currently_running_task(void* realm);
void* web_bindings_create_callback_function(JSObject* callable, bool returns_promise);
JSCompletion web_bindings_invoke_callback_function(void* callback, bool has_this_value, JSValue this_value, ReadonlySpan<JSValue> arguments);
void* web_bindings_invoke_promise_callback_function(void* callback, bool has_this_value, JSValue this_value, ReadonlySpan<JSValue> arguments);
void web_bindings_push_element_queue();
void web_bindings_pop_element_queue_and_invoke_reactions();
bool web_bindings_realm_is_window(void* realm);
bool web_bindings_realm_is_secure_context(void* realm);
HTML::Window* web_bindings_window_from_global_object(JSObject* global_object);
void web_bindings_for_each_active_window(void* ctx, void (*on_window)(void*, HTML::Window*));
DOM::Document* web_bindings_window_associated_document(HTML::Window*);
u64 web_bindings_window_page_id(HTML::Window*);
DOM::EventTarget* web_bindings_window_as_event_target(HTML::Window*);
bool web_bindings_document_has_navigable(DOM::Document*);
bool web_bindings_document_is_fully_active(DOM::Document*);
URL::FFI::RustUrl web_bindings_document_url(DOM::Document*);
HTML::WorkerGlobalScope* web_bindings_worker_global_scope_from_global_object(JSObject* global_object);
HTML::EnvironmentSettingsObject* web_bindings_relevant_settings_object(JSObject* global_object);
JSObject* web_bindings_environment_settings_object_global_object(HTML::EnvironmentSettingsObject*);
void web_bindings_environment_settings_object_id(HTML::EnvironmentSettingsObject*, void* ctx, void (*on_id)(void*, ReadonlyBytes));
bool web_bindings_worker_global_scope_is_closing(HTML::WorkerGlobalScope*);
void web_bindings_with_temporary_execution_context(void* realm, bool callbacks_enabled, void* context, void (*steps)(void*));
bool web_bindings_object_implements_interface(JSObject* object, u16 interface_name);
JSObject* web_bindings_create_dom_exception(void* realm, ReadonlyBytes name, FlatPtr message);
bool web_bindings_extract_an_origin(JSObject* object, void* ctx, void (*on_origin)(void*, URL::FFI::FfiOrigin const*));
void web_bindings_transfer_data_encode_records(HTML::TransferDataEncoder*, HTML::SerializedTransferRecord const* const*, size_t count);
void web_bindings_transfer_data_encode_transport_handle(HTML::TransferDataEncoder*, IPC::TransportHandle const*);
void web_bindings_transfer_data_extend(HTML::TransferDataEncoder*, HTML::SerializedTransferRecord*);
JSCompletion web_bindings_transfer_data_decode_records(HTML::TransferDataDecoder*, void* realm, size_t* offset, void* records, void (*append)(void*, HTML::SerializedTransferRecord*));
JSCompletion web_bindings_transfer_data_decode_transport_handle(HTML::TransferDataDecoder*, void* realm, size_t* offset, IPC::TransportHandle** out_handle);
void web_bindings_transport_handle_destroy(IPC::TransportHandle*);
IPC::Transport* web_bindings_transport_handle_create_transport(IPC::TransportHandle*);
IPC::Transport* web_bindings_message_port_transport_create_paired(IPC::TransportHandle** out_remote_handle);
void web_bindings_message_port_transport_destroy(IPC::Transport*);
IPC::TransportHandle* web_bindings_message_port_transport_release_for_transfer(IPC::Transport*);
void web_bindings_message_port_transport_set_up_read_hook(IPC::Transport*, void* state, void (*steps)(void*));
bool web_bindings_message_port_transport_is_open(IPC::Transport*);
void web_bindings_message_port_transport_close(IPC::Transport*);
void web_bindings_message_port_transport_close_after_sending_all_pending_messages(IPC::Transport*);
bool web_bindings_message_port_transport_send(IPC::Transport*, HTML::SerializedTransferRecord const*);
bool web_bindings_message_port_transport_read_messages(IPC::Transport*, void* records, void (*append)(void*, HTML::SerializedTransferRecord*));

void* web_css_wrap_object(void* realm, void* cell);
void* web_css_realm_document(void* realm);

void* web_bindings_wrapper_world(void* realm, bool* is_main_world)
{
    auto& wrapper_world = host_defined_wrapper_world(*static_cast<JS::Realm*>(realm));
    *is_main_world = wrapper_world.is_main_world();
    return &wrapper_world;
}

JSObject* web_bindings_rust_wrapper_for(void* realm, void* cell)
{
    auto& js_realm = *static_cast<JS::Realm*>(realm);
    auto wrapper = host_defined_wrapper_world(js_realm).rust_wrapper_for(*static_cast<RustInterfaceCell*>(cell), js_realm);
    return JS::HostABI::object_to_abi(wrapper.ptr());
}

void web_bindings_set_rust_wrapper(void* realm, void* cell, JSObject* wrapper)
{
    auto& host_object = as<JS::HostObject>(*JS::HostABI::object_from_abi(wrapper));
    host_defined_wrapper_world(*static_cast<JS::Realm*>(realm)).set_rust_wrapper(*static_cast<RustInterfaceCell*>(cell), host_object);
}

void* web_css_wrap_object(void* realm, void* cell)
{
    auto& js_realm = *static_cast<JS::Realm*>(realm);
    auto& wrappable = *static_cast<GCAllocatedWrappable*>(static_cast<GC::Cell*>(cell));
    auto wrapper = wrap(host_defined_wrapper_world(js_realm), js_realm, GC::Ref<Wrappable> { wrappable });
    return JS::HostABI::object_to_abi(wrapper.ptr());
}

void* web_css_realm_document(void* realm)
{
    return static_cast<GC::Cell*>(const_cast<DOM::Document*>(HTML::relevant_window(static_cast<JS::Realm*>(realm)->global_object()).document().ptr()));
}

bool web_bindings_realm_is_secure_context(void* realm)
{
    return HTML::is_secure_context(principal_host_defined_environment_settings_object(*static_cast<JS::Realm*>(realm)));
}

void web_bindings_push_element_queue()
{
    main_thread_similar_origin_window_agent().custom_element_reactions_stack.element_queue_stack.append({});
}

void web_bindings_pop_element_queue_and_invoke_reactions()
{
    auto queue = main_thread_similar_origin_window_agent().custom_element_reactions_stack.element_queue_stack.take_last();
    HTML::invoke_custom_element_reactions(queue);
}

bool web_bindings_realm_is_window(void* realm)
{
    return HTML::window_from_global_object(static_cast<JS::Realm*>(realm)->global_object());
}

HTML::Window* web_bindings_window_from_global_object(JSObject* global_object)
{
    return HTML::window_from_global_object(*JS::HostABI::object_from_abi(global_object));
}

void web_bindings_for_each_active_window(void* ctx, void (*on_window)(void*, HTML::Window*))
{
    HTML::Window::for_each_active([&](HTML::Window& window) {
        on_window(ctx, &window);
        return IterationDecision::Continue;
    });
}

DOM::Document* web_bindings_window_associated_document(HTML::Window* window)
{
    return &window->associated_document();
}

u64 web_bindings_window_page_id(HTML::Window* window)
{
    return window->page().client().id().value();
}

DOM::EventTarget* web_bindings_window_as_event_target(HTML::Window* window)
{
    return window;
}

bool web_bindings_document_has_navigable(DOM::Document* document)
{
    return document->navigable() != nullptr;
}

bool web_bindings_document_is_fully_active(DOM::Document* document)
{
    return document->is_fully_active();
}

URL::FFI::RustUrl web_bindings_document_url(DOM::Document* document)
{
    return document->url().to_rust();
}

HTML::EnvironmentSettingsObject* web_bindings_relevant_settings_object(JSObject* global_object)
{
    return &HTML::relevant_settings_object(*JS::HostABI::object_from_abi(global_object));
}

JSObject* web_bindings_environment_settings_object_global_object(HTML::EnvironmentSettingsObject* settings_object)
{
    return JS::HostABI::object_to_abi(&settings_object->global_object());
}

void web_bindings_environment_settings_object_id(HTML::EnvironmentSettingsObject* settings_object, void* ctx, void (*on_id)(void*, ReadonlyBytes))
{
    on_id(ctx, settings_object->id.to_string().bytes());
}

HTML::WorkerGlobalScope* web_bindings_worker_global_scope_from_global_object(JSObject* global_object)
{
    return Bindings::worker_global_scope_from_global_object(*JS::HostABI::object_from_abi(global_object));
}

bool web_bindings_worker_global_scope_is_closing(HTML::WorkerGlobalScope* worker_global_scope)
{
    return worker_global_scope->is_closing();
}

void web_bindings_with_temporary_execution_context(void* realm, bool callbacks_enabled, void* context, void (*steps)(void*))
{
    HTML::TemporaryExecutionContext execution_context { *static_cast<JS::Realm*>(realm), callbacks_enabled ? HTML::TemporaryExecutionContext::CallbacksEnabled::Yes : HTML::TemporaryExecutionContext::CallbacksEnabled::No };
    steps(context);
}

bool web_bindings_object_implements_interface(JSObject* object, u16 interface_name)
{
    auto* wrappable = wrappable_impl_from(JS::HostABI::object_from_abi(object));
    return wrappable && wrappable->implements_interface(static_cast<InterfaceName>(interface_name));
}

JSObject* web_bindings_create_dom_exception(void* realm, ReadonlyBytes name, FlatPtr message)
{
    auto& js_realm = *static_cast<JS::Realm*>(realm);
    auto exception = WebIDL::DOMException::create(FlyString::from_utf8_without_validation(name), Utf16String::from_raw(message));
    return JS::HostABI::object_to_abi(wrap(host_defined_wrapper_world(js_realm), js_realm, exception).ptr());
}

// https://html.spec.whatwg.org/multipage/browsers.html#extract-an-origin
bool web_bindings_extract_an_origin(JSObject* object, void* ctx, void (*on_origin)(void*, URL::FFI::FfiOrigin const*))
{
    auto* js_object = JS::HostABI::object_from_abi(object);
    Optional<URL::Origin> origin;

    // NB: IDL only ever sees HTML::WindowProxy but we want to use HTML::Window.
    if (auto* window_proxy = HTML::WindowProxy::from_object(*js_object)) {
        if (auto window = window_proxy->window())
            origin = window->extract_an_origin();
    } else if (auto const* wrappable = wrappable_impl_from(js_object)) {
        origin = wrappable->extract_an_origin();
    }

    if (!origin.has_value())
        return false;
    URL::RustIntegration::lend_origin_to_ffi(*origin, [&](URL::FFI::FfiOrigin const& ffi_origin) {
        on_origin(ctx, &ffi_origin);
    });
    return true;
}

// Writes the records as a Vector of them is encoded, which web_bindings_transfer_data_decode_records() and C++ read.
void web_bindings_transfer_data_encode_records(HTML::TransferDataEncoder* data_holder, HTML::SerializedTransferRecord const* const* records, size_t count)
{
    MUST(data_holder->encode_size(count));
    for (size_t i = 0; i < count; ++i)
        MUST(data_holder->encode(*records[i]));
}

void web_bindings_transfer_data_encode_transport_handle(HTML::TransferDataEncoder* data_holder, IPC::TransportHandle const* handle)
{
    MUST(data_holder->encode(*handle));
}

void web_bindings_transfer_data_extend(HTML::TransferDataEncoder* data_holder, HTML::SerializedTransferRecord* record)
{
    data_holder->extend(move(record->transfer_data_holders));
}

// Reads what web_bindings_transfer_data_encode_records() writes, and hands each record over.
JSCompletion web_bindings_transfer_data_decode_records(HTML::TransferDataDecoder* data_holder, void* realm, size_t* offset, void* records, void (*append)(void*, HTML::SerializedTransferRecord*))
{
    auto& js_realm = *static_cast<JS::Realm*>(realm);
    auto decoded = data_holder->decode_at<Vector<HTML::SerializedTransferRecord>>(*offset);
    if (decoded.is_error())
        return completion_to_abi(js_realm, HTML::data_clone_error_from_serialization_error(js_realm, decoded.release_error()));
    for (auto& record : decoded.value())
        append(records, new HTML::SerializedTransferRecord(move(record)));
    return completion_to_abi(js_realm, JS::js_undefined());
}

// Reads a transport handle, and hands it over.
JSCompletion web_bindings_transfer_data_decode_transport_handle(HTML::TransferDataDecoder* data_holder, void* realm, size_t* offset, IPC::TransportHandle** out_handle)
{
    auto& js_realm = *static_cast<JS::Realm*>(realm);
    auto handle = data_holder->decode_at<IPC::TransportHandle>(*offset);
    if (handle.is_error())
        return completion_to_abi(js_realm, HTML::data_clone_error_from_serialization_error(js_realm, handle.release_error()));
    *out_handle = new IPC::TransportHandle(handle.release_value());
    return completion_to_abi(js_realm, JS::js_undefined());
}

void web_bindings_transport_handle_destroy(IPC::TransportHandle* handle)
{
    delete handle;
}

// The transport of the end whose rights the handle holds, which it gives up, handed over.
IPC::Transport* web_bindings_transport_handle_create_transport(IPC::TransportHandle* handle)
{
    return MUST(handle->create_transport()).leak_ptr();
}

IPC::Transport* web_bindings_message_port_transport_create_paired(IPC::TransportHandle** out_remote_handle)
{
    auto paired = MUST(IPC::Transport::create_paired());
    *out_remote_handle = new IPC::TransportHandle(move(paired.remote_handle));
    return paired.local.leak_ptr();
}

void web_bindings_message_port_transport_destroy(IPC::Transport* transport)
{
    delete transport;
}

// The rights of the transport's end as a handle, handed over, after its IO thread stops.
IPC::TransportHandle* web_bindings_message_port_transport_release_for_transfer(IPC::Transport* transport)
{
    return new IPC::TransportHandle(MUST(transport->release_for_transfer()));
}

void web_bindings_message_port_transport_set_up_read_hook(IPC::Transport* transport, void* state, void (*steps)(void*))
{
    transport->set_up_read_hook([state, steps] { steps(state); });
}

bool web_bindings_message_port_transport_is_open(IPC::Transport* transport)
{
    return transport->is_open();
}

void web_bindings_message_port_transport_close(IPC::Transport* transport)
{
    transport->close();
}

void web_bindings_message_port_transport_close_after_sending_all_pending_messages(IPC::Transport* transport)
{
    transport->close_after_sending_all_pending_messages();
}

// Sends the record to the peer as one message, or logs the failure and returns false.
bool web_bindings_message_port_transport_send(IPC::Transport* transport, HTML::SerializedTransferRecord const* record)
{
    IPC::MessageBuffer buffer;
    IPC::Encoder encoder(buffer);
    MUST(encoder.encode(*record));
    if (auto result = buffer.transfer_message(*transport); result.is_error()) {
        dbgln("Failed to post message: {}", result.error());
        return false;
    }
    return true;
}

// Hands over every message that has arrived as the record it carries, and returns whether the peer is gone, so that
// nothing more arrives. A message that is not a record is treated as the peer going away, along with what follows it.
bool web_bindings_message_port_transport_read_messages(IPC::Transport* transport, void* records, void (*append)(void*, HTML::SerializedTransferRecord*))
{
    bool failed_to_decode = false;
    auto should_shutdown = transport->read_as_many_messages_as_possible_without_blocking([&](auto&& message) {
        if (failed_to_decode)
            return;
        FixedMemoryStream stream { message.bytes.bytes() };
        IPC::Decoder decoder { stream, message.attachments };
        auto record = decoder.decode<HTML::SerializedTransferRecord>();
        if (record.is_error()) {
            dbgln("Failed to decode a MessagePort message: {}", record.error());
            failed_to_decode = true;
            return;
        }
        append(records, new HTML::SerializedTransferRecord(record.release_value()));
    });
    return failed_to_decode || should_shutdown == IPC::Transport::ShouldShutdown::Yes;
}

JSObject* web_bindings_rejected_promise(void* current_realm, JSValue this_value, JSValue reason)
{
    auto& realm = *static_cast<JS::Realm*>(current_realm);
    auto promise_realm_this_value = JS::HostABI::value_from_abi(this_value);
    if (promise_realm_this_value.is_nullish())
        promise_realm_this_value = &realm.global_object();
    auto& promise_realm = this_value_realm(realm, promise_realm_this_value);
    auto promise = WebIDL::create_rejected_promise(promise_realm, JS::HostABI::value_from_abi(reason));
    return JS::HostABI::object_to_abi(promise->promise().ptr());
}

void web_bindings_queue_a_microtask(void* realm, JSObject* function)
{
    GC::Ref<JS::Realm> js_realm = *static_cast<JS::Realm*>(realm);
    GC::Ref<JS::FunctionObject> steps = as<JS::FunctionObject>(*JS::HostABI::object_from_abi(function));
    HTML::queue_a_microtask(nullptr, GC::create_function(GC::Heap::the(), [js_realm, steps] {
        HTML::TemporaryExecutionContext execution_context { js_realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
        MUST(JS::call(steps->vm(), *steps, JS::js_undefined()));
    }));
}

void web_bindings_run_in_parallel(void* realm, void* cell, void (*steps)(JSVM*, void*))
{
    GC::Ref<JS::Realm> js_realm = *static_cast<JS::Realm*>(realm);
    // NB: The function's captures, which the collector scans, keep the cell alive until the steps run.
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(GC::Heap::the(), [js_realm, cell, steps] {
        HTML::TemporaryExecutionContext execution_context { js_realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
        steps(JS::HostABI::vm_to_abi(js_realm->vm()), cell);
    }));
}

// NB: The task sources of the Rust bindings' TaskSource.
static_assert(to_underlying(HTML::Task::Source::Unspecified) == 0);
static_assert(to_underlying(HTML::Task::Source::Networking) == 3);
static_assert(to_underlying(HTML::Task::Source::FileReading) == 13);
static_assert(to_underlying(HTML::Task::Source::Storage) == 29);

void web_bindings_queue_a_task(void* realm, u8 task_source, void* cell, void (*steps)(JSVM*, void*))
{
    GC::Ref<JS::Realm> js_realm = *static_cast<JS::Realm*>(realm);
    // NB: The task's captures, which the collector scans, keep the cell alive until the steps run.
    HTML::queue_a_task(static_cast<HTML::Task::Source>(task_source), nullptr, nullptr, GC::create_function(GC::Heap::the(), [js_realm, cell, steps] {
        HTML::TemporaryExecutionContext execution_context { js_realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
        steps(JS::HostABI::vm_to_abi(js_realm->vm()), cell);
    }));
}

u64 web_bindings_queue_a_global_task(JSObject* global_object, u8 task_source, void* cell, void (*steps)(JSVM*, void*))
{
    auto& global = *JS::HostABI::object_from_abi(global_object);
    // NB: The task's captures, which the collector scans, keep the cell alive until the steps run.
    auto task_id = HTML::queue_global_task(static_cast<HTML::Task::Source>(task_source), global, GC::create_function(GC::Heap::the(), [&vm = global.vm(), cell, steps] {
        steps(JS::HostABI::vm_to_abi(vm), cell);
    }));
    return task_id.value();
}

void web_bindings_remove_tasks(void* realm, ReadonlySpan<u64> task_ids)
{
    auto& event_loop = *HTML::relevant_agent(static_cast<JS::Realm*>(realm)->global_object()).event_loop;
    event_loop.task_queue().remove_tasks_matching([&](auto const& task) {
        return task_ids.contains_slow(task.id().value());
    });
}

u64 web_bindings_currently_running_task(void* realm)
{
    auto& event_loop = *HTML::relevant_agent(static_cast<JS::Realm*>(realm)->global_object()).event_loop;
    VERIFY(event_loop.currently_running_task());
    return event_loop.currently_running_task()->id().value();
}

void* web_bindings_create_callback_function(JSObject* callable, bool returns_promise)
{
    auto& object = *JS::HostABI::object_from_abi(callable);
    return GC::Heap::the().allocate<WebIDL::CallbackType>(object, HTML::incumbent_settings_object(), returns_promise ? WebIDL::OperationReturnsPromise::Yes : WebIDL::OperationReturnsPromise::No).ptr();
}

static GC::RootVector<JS::Value> callback_arguments(ReadonlySpan<JSValue> arguments)
{
    GC::RootVector<JS::Value> values;
    values.ensure_capacity(arguments.size());
    for (auto argument : arguments)
        values.unchecked_append(JS::HostABI::value_from_abi(argument));
    return values;
}

JSCompletion web_bindings_invoke_callback_function(void* callback, bool has_this_value, JSValue this_value, ReadonlySpan<JSValue> arguments)
{
    Optional<JS::Value> this_argument;
    if (has_this_value)
        this_argument = JS::HostABI::value_from_abi(this_value);
    auto completion = WebIDL::invoke_callback(*static_cast<WebIDL::CallbackType*>(callback), this_argument, callback_arguments(arguments));
    if (completion.is_error())
        return JS::HostABI::throw_completion_to_abi(completion.value());
    return JS::HostABI::normal_completion_to_abi(JS::HostABI::value_to_abi(completion.value()));
}

void* web_bindings_invoke_promise_callback_function(void* callback, bool has_this_value, JSValue this_value, ReadonlySpan<JSValue> arguments)
{
    Optional<JS::Value> this_argument;
    if (has_this_value)
        this_argument = JS::HostABI::value_from_abi(this_value);
    return WebIDL::invoke_promise_callback(*static_cast<WebIDL::CallbackType*>(callback), this_argument, callback_arguments(arguments)).ptr();
}
}

}
