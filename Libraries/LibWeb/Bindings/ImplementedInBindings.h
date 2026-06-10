/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Types.h>
#include <AK/Utf16String.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibGC/Forward.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/Value.h>
#include <LibWeb/Bindings/CookieChangeEvent.h>
#include <LibWeb/Bindings/CustomElementRegistry.h>
#include <LibWeb/Bindings/EventTarget.h>
#include <LibWeb/Bindings/Global.h>
#include <LibWeb/Bindings/OffscreenCanvas.h>
#include <LibWeb/Bindings/Request.h>
#include <LibWeb/Bindings/Table.h>
#include <LibWeb/DOM/NodeFilter.h>
#include <LibWeb/Export.h>
#include <LibWeb/Fetch/Request.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/ImageBitmap.h>
#include <LibWeb/HTML/OffscreenCanvas.h>
#include <LibWeb/TrustedTypes/TrustedHTML.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/CallbackType.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/WebIDL/Promise.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::Animations {

class Animation;
class AnimationEffect;
class AnimationTimeline;

}

namespace Web::CookieStore {

class CookieChangeEvent;

}

namespace Web::CSS {

class CSSFontFeatureValuesMap;
class FontFace;
class FontFaceSet;
class StyleSheetList;

}

namespace Web::DOM {

class Document;
class Event;
class EventTarget;
class Node;
class NodeFilter;
class NodeIterator;
class ShadowRoot;
class TreeWalker;

}

namespace Web::Fetch {

class Response;

}

namespace Web::HTML {

class CustomElementRegistry;
class Location;
class OffscreenCanvas;
class WindowOrWorkerGlobalScopeMixin;

}

namespace Web::Streams {

class ReadableByteStreamController;
class ReadableStream;
class ReadableStreamDefaultController;
class TransformStream;
class TransformStreamDefaultController;
class WritableStream;
class WritableStreamDefaultController;

}

namespace Web::WebAssembly {

class Global;
class Instance;
class Memory;
class Module;
class Table;

}

namespace Web::WebGL {

class WebGL2RenderingContext;
class WebGLExtension;
class WebGLProgram;
class WebGLQuery;
class WebGLRenderingContext;
class WebGLRenderingContextBase;
class WebGLRenderingContextImpl;
class WebGLShader;
class WebGLSync;
class WebGLUniformLocation;

}

namespace Web::Bindings {

class Wrappable;
class WrapperWorld;
struct WebGLContextAttributes;

WEB_API GC::Ref<Animations::Animation> construct_animation(JS::Realm&, GC::Ptr<Animations::AnimationEffect>, Optional<GC::Ptr<Animations::AnimationTimeline>>, size_t argument_count);
WEB_API void resolve_animation_promise(JS::Realm&, WebIDL::Promise&, Animations::Animation const&);
WEB_API GC::Ref<WebIDL::Promise> create_resolved_animation_promise(JS::Realm&, Animations::Animation const&);

WEB_API GC::Ptr<JS::Object> cached_changed(CookieStore::CookieChangeEvent const&, WrapperWorld const&);
WEB_API void set_cached_changed(CookieStore::CookieChangeEvent const&, WrapperWorld const&, GC::Ptr<JS::Object>);
WEB_API GC::Ptr<JS::Object> cached_deleted(CookieStore::CookieChangeEvent const&, WrapperWorld const&);
WEB_API void set_cached_deleted(CookieStore::CookieChangeEvent const&, WrapperWorld const&, GC::Ptr<JS::Object>);

WEB_API IndexedDB::IDBKeyRange* key_range_from_value(JS::Value);
WEB_API WebIDL::ExceptionOr<GC::Ref<IndexedDB::IDBKeyRange>> only(JS::Realm&, JS::Value);
WEB_API WebIDL::ExceptionOr<GC::Ref<IndexedDB::IDBKeyRange>> lower_bound(JS::Realm&, JS::Value, bool open);
WEB_API WebIDL::ExceptionOr<GC::Ref<IndexedDB::IDBKeyRange>> upper_bound(JS::Realm&, JS::Value, bool open);
WEB_API WebIDL::ExceptionOr<GC::Ref<IndexedDB::IDBKeyRange>> bound(JS::Realm&, JS::Value lower, JS::Value upper, bool lower_open, bool upper_open);
WEB_API WebIDL::ExceptionOr<bool> includes(JS::Realm&, IndexedDB::IDBKeyRange&, JS::Value key);
WEB_API JS::Value key_range_lower(JS::Realm&, IndexedDB::IDBKeyRange&);
WEB_API JS::Value key_range_upper(JS::Realm&, IndexedDB::IDBKeyRange&);

WEB_API WebIDL::ExceptionOr<JS::Value> invoke_readable_byte_stream_start_algorithm_callback(JS::Realm&, WebIDL::CallbackType&, JS::Value underlying_source, GC::Ref<Streams::ReadableByteStreamController>);
WEB_API GC::Ref<WebIDL::Promise> invoke_readable_byte_stream_pull_algorithm_callback(JS::Realm&, WebIDL::CallbackType&, JS::Value underlying_source, GC::Ref<Streams::ReadableByteStreamController>);
WEB_API Streams::ReadableStream* readable_stream_from_object(JS::Object&);
WEB_API void serialize_readable_stream_with_transfer(JS::Realm&, HTML::TransferDataEncoder&, GC::Ref<Streams::ReadableStream>);
WEB_API JS::Completion invoke_queuing_strategy_size_callback(WebIDL::CallbackType&, JS::Value chunk);
WEB_API WebIDL::ExceptionOr<JS::Value> invoke_readable_stream_start_algorithm_callback(JS::Realm&, WebIDL::CallbackType&, JS::Value underlying_source, GC::Ref<Streams::ReadableStreamDefaultController>);
WEB_API GC::Ref<WebIDL::Promise> invoke_readable_stream_pull_algorithm_callback(JS::Realm&, WebIDL::CallbackType&, JS::Value underlying_source, GC::Ref<Streams::ReadableStreamDefaultController>);
WEB_API void prepare_transform_stream_readable_writable_wrappers(JS::Realm&, Streams::TransformStream&);
WEB_API WebIDL::ExceptionOr<JS::Value> invoke_transform_stream_start_algorithm_callback(JS::Realm&, WebIDL::CallbackType&, JS::Value transformer, GC::Ref<Streams::TransformStreamDefaultController>);
WEB_API GC::Ref<WebIDL::Promise> invoke_transform_stream_transform_algorithm_callback(JS::Realm&, WebIDL::CallbackType&, JS::Value transformer, JS::Value chunk, GC::Ref<Streams::TransformStreamDefaultController>);
WEB_API GC::Ref<WebIDL::Promise> invoke_transform_stream_flush_algorithm_callback(JS::Realm&, WebIDL::CallbackType&, JS::Value transformer, GC::Ref<Streams::TransformStreamDefaultController>);
WEB_API Streams::WritableStream* writable_stream_from_object(JS::Object&);
WEB_API void serialize_writable_stream_with_transfer(JS::Realm&, HTML::TransferDataEncoder&, GC::Ref<Streams::WritableStream>);
WEB_API WebIDL::ExceptionOr<JS::Value> invoke_writable_stream_start_algorithm_callback(JS::Realm&, WebIDL::CallbackType&, JS::Value underlying_sink, GC::Ref<Streams::WritableStreamDefaultController>);
WEB_API GC::Ref<WebIDL::Promise> invoke_writable_stream_write_algorithm_callback(JS::Realm&, WebIDL::CallbackType&, JS::Value underlying_sink, JS::Value chunk, GC::Ref<Streams::WritableStreamDefaultController>);

WEB_API JS::Value location_wrapper(JS::Realm&, GC::Ref<HTML::Location>);

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

WEB_API JS::Object* webgl_extension(JS::Realm&, GC::Ref<WebGL::WebGLExtension>);
WEB_API JS::Value webgl_wrappable(JS::Realm&, GC::Ref<Wrappable>);
WEB_API JS::Value webgl_buffer(JS::Realm&, GC::Ref<WebGL::WebGLRenderingContextBase>, u32 handle);
WEB_API Optional<WebGLContextAttributes> get_context_attributes(JS::Realm&, WebGL::WebGLRenderingContext&);
WEB_API Optional<WebGLContextAttributes> get_context_attributes(JS::Realm&, WebGL::WebGL2RenderingContext&);
WEB_API JS::Object* get_extension(JS::Realm&, WebGL::WebGLRenderingContext&, String const& name);
WEB_API JS::Object* get_extension(JS::Realm&, WebGL::WebGL2RenderingContext&, String const& name);
WEB_API JS::Value get_buffer_parameter(JS::Realm&, WebGL::WebGLRenderingContextImpl&, WebIDL::UnsignedLong target, WebIDL::UnsignedLong pname);
WEB_API JS::Value get_parameter(JS::Realm&, WebGL::WebGLRenderingContextImpl&, WebIDL::UnsignedLong pname);
WEB_API JS::Value get_program_parameter(JS::Realm&, WebGL::WebGLRenderingContextImpl&, GC::Ref<WebGL::WebGLProgram>, WebIDL::UnsignedLong pname);
WEB_API JS::Value get_renderbuffer_parameter(JS::Realm&, WebGL::WebGLRenderingContextImpl&, WebIDL::UnsignedLong target, WebIDL::UnsignedLong pname);
WEB_API JS::Value get_shader_parameter(JS::Realm&, WebGL::WebGLRenderingContextImpl&, GC::Ref<WebGL::WebGLShader>, WebIDL::UnsignedLong pname);
WEB_API JS::Value get_tex_parameter(JS::Realm&, WebGL::WebGLRenderingContextImpl&, WebIDL::UnsignedLong target, WebIDL::UnsignedLong pname);
WEB_API JS::Value get_vertex_attrib(JS::Realm&, WebGL::WebGLRenderingContextImpl&, WebIDL::UnsignedLong index, WebIDL::UnsignedLong pname);
WEB_API JS::Value get_uniform(JS::Realm&, WebGL::WebGLRenderingContextImpl&, GC::Ref<WebGL::WebGLProgram>, GC::Ref<WebGL::WebGLUniformLocation>);
WEB_API JS::Value get_query_parameter(JS::Realm&, WebGL::WebGL2RenderingContext&, GC::Ref<WebGL::WebGLQuery>, WebIDL::UnsignedLong pname);
WEB_API JS::Value get_sync_parameter(JS::Realm&, WebGL::WebGL2RenderingContext&, GC::Ref<WebGL::WebGLSync>, WebIDL::UnsignedLong pname);
WEB_API JS::Value get_active_uniforms(JS::Realm&, WebGL::WebGL2RenderingContext&, GC::Ref<WebGL::WebGLProgram>, Vector<WebIDL::UnsignedLong> uniform_indices, WebIDL::UnsignedLong pname);
WEB_API JS::Value get_internalformat_parameter(JS::Realm&, WebGL::WebGL2RenderingContext&, WebIDL::UnsignedLong target, WebIDL::UnsignedLong internalformat, WebIDL::UnsignedLong pname);
WEB_API JS::Value get_active_uniform_block_parameter(JS::Realm&, WebGL::WebGL2RenderingContext&, GC::Ref<WebGL::WebGLProgram>, WebIDL::UnsignedLong uniform_block_index, WebIDL::UnsignedLong pname);
WEB_API WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> make_xr_compatible(JS::Realm&, WebGL::WebGLRenderingContextBase&);

template<typename T>
JS::Value webgl_wrappable(JS::Realm& realm, GC::Ptr<T> wrappable)
{
    if (!wrappable)
        return JS::js_null();
    return webgl_wrappable(realm, GC::Ref<Wrappable> { *wrappable });
}

WEB_API WebIDL::ExceptionOr<GC::Ref<HTML::OffscreenCanvas>> construct_offscreen_canvas(JS::Realm&, WebIDL::UnsignedLong width, WebIDL::UnsignedLong height);
WEB_API GC::Ref<WebIDL::Promise> convert_to_blob(JS::Realm&, HTML::OffscreenCanvas&, Optional<ImageEncodeOptions> const&);
WEB_API JS::ThrowCompletionOr<HTML::OffscreenRenderingContext> get_context(JS::Realm&, HTML::OffscreenCanvas&, OffscreenRenderingContextId, JS::Value options);

WEB_API JS::Value document(JS::Realm&, GC::Ref<DOM::Document>);
WEB_API GC::Ref<CSS::StyleSheetList> style_sheets(JS::Realm&, DOM::Document&);
WEB_API JS::Value adopted_style_sheets(JS::Realm&, DOM::Document&);
WEB_API WebIDL::ExceptionOr<void> set_adopted_style_sheets(JS::Realm&, DOM::Document&, JS::Value);
WEB_API WebIDL::ExceptionOr<GC::Root<DOM::Document>> parse_html_unsafe(JS::Realm&, TrustedTypes::TrustedHTMLOrString const&);
WEB_API WebIDL::ExceptionOr<void> write(JS::Realm&, DOM::Document&, Vector<TrustedTypes::TrustedHTMLOrString> const&);
WEB_API WebIDL::ExceptionOr<void> writeln(JS::Realm&, DOM::Document&, Vector<TrustedTypes::TrustedHTMLOrString> const&);
WEB_API GC::Ptr<ViewTransition::ViewTransition> start_view_transition(JS::Realm&, DOM::Document&, GC::Ptr<WebIDL::CallbackType> update_callback);
WEB_API JS::Value document_named_item_value(WrapperWorld&, JS::Realm&, DOM::Document const&, FlyString const& name);

WEB_API DOM::Event* event_from_value(JS::Value);
WEB_API DOM::Event* event_from_callback_argument(JS::VM&);
WEB_API JS::Value event(JS::Realm&, GC::Ref<DOM::Event>);
WEB_API GC::Ptr<PlatformObject> current_target_wrapper(JS::Realm&, DOM::Event const&);
WEB_API JS::Value current_target_value(JS::Realm&, DOM::Event const&);

WEB_API WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> fetch(JS::Realm&, HTML::WindowOrWorkerGlobalScopeMixin&, Fetch::RequestInfo const&, RequestInit const&);

WEB_API GC::Ref<JS::Map> map_entries(JS::Realm&, CSS::CSSFontFeatureValuesMap&);
WEB_API Optional<JS::Value> map_get(JS::Realm&, CSS::CSSFontFeatureValuesMap&, FlyString const& key);
WEB_API bool map_has(CSS::CSSFontFeatureValuesMap&, FlyString const& key);
WEB_API bool map_remove(CSS::CSSFontFeatureValuesMap&, FlyString const& key);
WEB_API void map_clear(CSS::CSSFontFeatureValuesMap&);
WEB_API WebIDL::ExceptionOr<void> set(JS::Realm&, CSS::CSSFontFeatureValuesMap&, String const& feature_value_name, Variant<u32, Vector<u32>> const& values);

WEB_API void resolve_font_face_list_promise(JS::Realm&, WebIDL::Promise const&, Vector<GC::Ref<CSS::FontFace>> const&);
WEB_API void resolve_font_face_set_promise(JS::Realm&, WebIDL::Promise const&, CSS::FontFaceSet&);
WEB_API GC::Ref<JS::Set> setlike_entries(JS::Realm&, WrapperWorld const&, CSS::FontFaceSet const&);
WEB_API bool setlike_has(CSS::FontFaceSet const&, JS::Value);
WEB_API void did_add_font_face(CSS::FontFaceSet const&, GC::Ref<CSS::FontFace>);
WEB_API void did_remove_font_face(CSS::FontFaceSet const&, GC::Ref<CSS::FontFace>);

WEB_API void add_event_listener(JS::Realm&, DOM::EventTarget&, FlyString const& type, DOM::IDLEventListener* callback, Variant<AddEventListenerOptions, bool> const& options);
WEB_API void remove_event_listener(JS::Realm&, DOM::EventTarget&, FlyString const& type, DOM::IDLEventListener* callback, Variant<EventListenerOptions, bool> const& options);
WEB_API GC::Ref<JS::Environment> new_event_handler_object_environment(JS::Realm&, GC::Ref<DOM::EventTarget>, GC::Ref<JS::Environment> outer_environment);
WEB_API JS::Realm* callback_realm(WebIDL::CallbackType* callback);
WEB_API HTML::Window* window_from_callback(WebIDL::CallbackType&);
WEB_API void report_exception_for_callback(WebIDL::CallbackType&, JS::Value exception);
WEB_API JS::Completion invoke_event_listener(WebIDL::CallbackType&, DOM::Event&);
WEB_API JS::Completion invoke_event_handler(WebIDL::CallbackType&, DOM::Event&, bool special_error_event_handling);

WEB_API GC::Ref<HTML::CustomElementRegistry> construct_custom_element_registry(JS::Realm&);
WEB_API JS::Realm& wrapper_realm_for_custom_element_registry(WrapperWorld const&, JS::Realm& preferred_realm, HTML::CustomElementRegistry&);
WEB_API JS::ThrowCompletionOr<void> define(JS::Realm&, HTML::CustomElementRegistry&, String const& name, WebIDL::CallbackType* constructor, ElementDefinitionOptions const& options);
WEB_API GC::Ref<WebIDL::Promise> when_defined(JS::Realm&, HTML::CustomElementRegistry&, String const& name);

WEB_API JS::ThrowCompletionOr<DOM::NodeFilter::Result> accept_node(JS::Realm&, DOM::NodeFilter&, DOM::Node&);
WEB_API JS::Value filter(JS::Realm&, DOM::NodeIterator&);
WEB_API JS::ThrowCompletionOr<GC::Ptr<DOM::Node>> next_node(JS::Realm&, DOM::NodeIterator&);
WEB_API JS::ThrowCompletionOr<GC::Ptr<DOM::Node>> previous_node(JS::Realm&, DOM::NodeIterator&);
WEB_API JS::Value filter(JS::Realm&, DOM::TreeWalker&);
WEB_API JS::ThrowCompletionOr<GC::Ptr<DOM::Node>> parent_node(JS::Realm&, DOM::TreeWalker&);
WEB_API JS::ThrowCompletionOr<GC::Ptr<DOM::Node>> first_child(JS::Realm&, DOM::TreeWalker&);
WEB_API JS::ThrowCompletionOr<GC::Ptr<DOM::Node>> last_child(JS::Realm&, DOM::TreeWalker&);
WEB_API JS::ThrowCompletionOr<GC::Ptr<DOM::Node>> previous_sibling(JS::Realm&, DOM::TreeWalker&);
WEB_API JS::ThrowCompletionOr<GC::Ptr<DOM::Node>> next_sibling(JS::Realm&, DOM::TreeWalker&);
WEB_API JS::ThrowCompletionOr<GC::Ptr<DOM::Node>> previous_node(JS::Realm&, DOM::TreeWalker&);
WEB_API JS::ThrowCompletionOr<GC::Ptr<DOM::Node>> next_node(JS::Realm&, DOM::TreeWalker&);

WEB_API GC::Ref<JS::Set> setlike_entries(JS::Realm&, WrapperWorld const&, HTML::CustomStateSet const&);
WEB_API bool setlike_has(HTML::CustomStateSet const&, JS::Value);
WEB_API void setlike_add(HTML::CustomStateSet&, JS::Value);
WEB_API bool setlike_remove(HTML::CustomStateSet&, JS::Value);
WEB_API void setlike_clear(HTML::CustomStateSet&);
WEB_API void setlike_on_set_modified_from_js(HTML::CustomStateSet&);

WEB_API WebIDL::ExceptionOr<TrustedTypes::TrustedHTMLOrString> inner_html(JS::Realm&, DOM::ShadowRoot&);
WEB_API WebIDL::ExceptionOr<void> set_inner_html(JS::Realm&, DOM::ShadowRoot&, TrustedTypes::TrustedHTMLOrString const&);
WEB_API WebIDL::ExceptionOr<void> set_html_unsafe(JS::Realm&, DOM::ShadowRoot&, Variant<GC::Ref<TrustedTypes::TrustedHTML>, Utf16String> const& html);
WEB_API GC::Ref<CSS::StyleSheetList> style_sheets(JS::Realm&, DOM::ShadowRoot&);
WEB_API JS::Value adopted_style_sheets(JS::Realm&, DOM::ShadowRoot&);
WEB_API WebIDL::ExceptionOr<void> set_adopted_style_sheets(JS::Realm&, DOM::ShadowRoot&, JS::Value);

} // namespace Web::Bindings
