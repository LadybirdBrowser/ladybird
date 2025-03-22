/*
 * Copyright (c) 2020-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/String.h>
#include <AK/StringBuilder.h>
#include <LibGC/RootVector.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/PropertyDescriptor.h>
#include <LibJS/Runtime/PropertyKey.h>
#include <LibURL/Parser.h>
#include <LibWeb/Bindings/LocationPrototype.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/CrossOrigin/AbstractOperations.h>
#include <LibWeb/HTML/Location.h>
#include <LibWeb/HTML/Navigation.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/WebIDL/DOMException.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(Location);

// https://html.spec.whatwg.org/multipage/history.html#the-location-interface
Location::Location(JS::Realm& realm)
    : PlatformObject(realm, MayInterfereWithIndexedPropertyAccess::Yes)
{
}

Location::~Location() = default;

void Location::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_default_properties);
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#the-location-interface
void Location::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(Location);
    Bindings::LocationPrototype::define_unforgeable_attributes(realm, *this);

    auto& vm = this->vm();

    // Step 2: Let valueOf be location's relevant realm.[[Intrinsics]].[[%Object.prototype.valueOf%]].
    auto& intrinsics = realm.intrinsics();
    auto value_of_function = intrinsics.object_prototype()->get_without_side_effects(vm.names.valueOf);

    // Step 3: Perform ! location.[[DefineOwnProperty]]("valueOf", { [[Value]]: valueOf, [[Writable]]: false, [[Enumerable]]: false, [[Configurable]]: false }).
    auto value_of_property_descriptor = JS::PropertyDescriptor {
        .value = value_of_function,
        .writable = false,
        .enumerable = false,
        .configurable = false,
    };
    MUST(internal_define_own_property(vm.names.valueOf, value_of_property_descriptor));

    // Step 4: Perform ! location.[[DefineOwnProperty]](%Symbol.toPrimitive%, { [[Value]]: undefined, [[Writable]]: false, [[Enumerable]]: false, [[Configurable]]: false }).
    auto to_primitive_property_descriptor = JS::PropertyDescriptor {
        .value = JS::js_undefined(),
        .writable = false,
        .enumerable = false,
        .configurable = false,
    };
    MUST(internal_define_own_property(vm.well_known_symbol_to_primitive(), to_primitive_property_descriptor));

    // 5. Set the value of the [[DefaultProperties]] internal slot of location to location.[[OwnPropertyKeys]]().
    // NOTE: In LibWeb this happens before the ESO is set up, so we must avoid location's custom [[OwnPropertyKeys]].
    m_default_properties.extend(MUST(Object::internal_own_property_keys()));
}

// https://html.spec.whatwg.org/multipage/history.html#relevant-document
GC::Ptr<DOM::Document> Location::relevant_document() const
{
    // A Location object has an associated relevant Document, which is this Location object's
    // relevant global object's browsing context's active document, if this Location object's
    // relevant global object's browsing context is non-null, and null otherwise.
    auto* browsing_context = as<HTML::Window>(HTML::relevant_global_object(*this)).browsing_context();
    return browsing_context ? browsing_context->active_document() : nullptr;
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#location-object-navigate
WebIDL::ExceptionOr<void> Location::navigate(URL::URL url, Bindings::NavigationHistoryBehavior history_handling)
{
    // 1. Let navigable be location's relevant global object's navigable.
    auto navigable = as<HTML::Window>(HTML::relevant_global_object(*this)).navigable();

    // 2. Let sourceDocument be the incumbent global object's associated Document.
    auto& source_document = as<HTML::Window>(incumbent_global_object()).associated_document();

    // 3. If location's relevant Document is not yet completely loaded, and the incumbent global object does not have transient activation, then set historyHandling to "replace".
    if (!relevant_document()->is_completely_loaded() && !as<HTML::Window>(incumbent_global_object()).has_transient_activation()) {
        history_handling = Bindings::NavigationHistoryBehavior::Replace;
    }

    // 4. Navigate navigable to url using sourceDocument, with exceptionsEnabled set to true and historyHandling set to historyHandling.
    TRY(navigable->navigate({ .url = url,
        .source_document = source_document,
        .exceptions_enabled = true,
        .history_handling = history_handling }));

    return {};
}

// https://html.spec.whatwg.org/multipage/history.html#concept-location-url
URL::URL Location::url() const
{
    // A Location object has an associated url, which is this Location object's relevant Document's URL,
    // if this Location object's relevant Document is non-null, and about:blank otherwise.
    auto const relevant_document = this->relevant_document();
    return relevant_document ? relevant_document->url() : URL::about_blank();
}

// https://html.spec.whatwg.org/multipage/history.html#dom-location-href
WebIDL::ExceptionOr<String> Location::href() const
{
    // 1. If this's relevant Document is non-null and its origin is not same origin-domain with the entry settings object's origin, then throw a "SecurityError" DOMException.
    auto const relevant_document = this->relevant_document();
    if (relevant_document && !relevant_document->origin().is_same_origin_domain(entry_settings_object().origin()))
        return WebIDL::SecurityError::create(realm(), "Location's relevant document is not same origin-domain with the entry settings object's origin"_string);

    // 2. Return this's url, serialized.
    return url().serialize();
}

// https://html.spec.whatwg.org/multipage/history.html#the-location-interface:dom-location-href-2
WebIDL::ExceptionOr<void> Location::set_href(String const& new_href)
{
    auto& realm = this->realm();

    // 1. If this's relevant Document is null, then return.
    auto const relevant_document = this->relevant_document();
    if (!relevant_document)
        return {};

    // 2. Let url be the result of encoding-parsing a URL given the given value, relative to the entry settings object.
    auto url = entry_settings_object().encoding_parse_url(new_href.to_byte_string());

    // 3. If url is failure, then throw a "SyntaxError" DOMException.
    if (!url.has_value())
        return WebIDL::SyntaxError::create(realm, MUST(String::formatted("Invalid URL '{}'", new_href)));

    // 4. Location-object navigate this to url.
    TRY(navigate(url.release_value()));

    return {};
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-location-origin
WebIDL::ExceptionOr<String> Location::origin() const
{
    // 1. If this's relevant Document is non-null and its origin is not same origin-domain with the entry settings object's origin, then throw a "SecurityError" DOMException.
    auto const relevant_document = this->relevant_document();
    if (relevant_document && !relevant_document->origin().is_same_origin_domain(entry_settings_object().origin()))
        return WebIDL::SecurityError::create(realm(), "Location's relevant document is not same origin-domain with the entry settings object's origin"_string);

    // 2. Return the serialization of this's url's origin.
    return url().origin().serialize();
}

// https://html.spec.whatwg.org/multipage/history.html#dom-location-protocol
WebIDL::ExceptionOr<String> Location::protocol() const
{
    auto& vm = this->vm();

    // 1. If this's relevant Document is non-null and its origin is not same origin-domain with the entry settings object's origin, then throw a "SecurityError" DOMException.
    auto const relevant_document = this->relevant_document();
    if (relevant_document && !relevant_document->origin().is_same_origin_domain(entry_settings_object().origin()))
        return WebIDL::SecurityError::create(realm(), "Location's relevant document is not same origin-domain with the entry settings object's origin"_string);

    // 2. Return this's url's scheme, followed by ":".
    return TRY_OR_THROW_OOM(vm, String::formatted("{}:", url().scheme()));
}

// https://html.spec.whatwg.org/multipage/history.html#dom-location-protocol
WebIDL::ExceptionOr<void> Location::set_protocol(String const& value)
{
    auto relevant_document = this->relevant_document();

    // 1. If this's relevant Document is null, then return.
    if (!relevant_document)
        return {};

    // 2. If this's relevant Document's origin is not same origin-domain with the entry settings object's origin, then throw a "SecurityError" DOMException.
    if (!relevant_document->origin().is_same_origin_domain(entry_settings_object().origin()))
        return WebIDL::SecurityError::create(realm(), "Location's relevant document is not same origin-domain with the entry settings object's origin"_string);

    // 3. Let copyURL be a copy of this's url.
    auto copy_url = this->url();

    // 4. Let possibleFailure be the result of basic URL parsing the given value, followed by ":", with copyURL as url and scheme start state as state override.
    auto possible_failure = URL::Parser::basic_parse(value, {}, &copy_url, URL::Parser::State::SchemeStart);

    // 5. If possibleFailure is failure, then throw a "SyntaxError" DOMException.
    if (!possible_failure.has_value())
        return WebIDL::SyntaxError::create(realm(), MUST(String::formatted("Failed to set protocol. '{}' is an invalid protocol", value)));

    // 6. if copyURL's scheme is not an HTTP(S) scheme, then terminate these steps.
    if (!(copy_url.scheme() == "http"sv || copy_url.scheme() == "https"sv))
        return {};

    // 7. Location-object navigate this to copyURL.
    TRY(navigate(copy_url));

    return {};
}

// https://html.spec.whatwg.org/multipage/history.html#dom-location-host
WebIDL::ExceptionOr<String> Location::host() const
{
    auto& vm = this->vm();

    // 1. If this's relevant Document is non-null and its origin is not same origin-domain with the entry settings object's origin, then throw a "SecurityError" DOMException.
    auto const relevant_document = this->relevant_document();
    if (relevant_document && !relevant_document->origin().is_same_origin_domain(entry_settings_object().origin()))
        return WebIDL::SecurityError::create(realm(), "Location's relevant document is not same origin-domain with the entry settings object's origin"_string);

    // 2. Let url be this's url.
    auto url = this->url();

    // 3. If url's host is null, return the empty string.
    if (!url.host().has_value())
        return String {};

    // 4. If url's port is null, return url's host, serialized.
    if (!url.port().has_value())
        return url.serialized_host();

    // 5. Return url's host, serialized, followed by ":" and url's port, serialized.
    return TRY_OR_THROW_OOM(vm, String::formatted("{}:{}", url.serialized_host(), *url.port()));
}

WebIDL::ExceptionOr<void> Location::set_host(String const&)
{
    auto& vm = this->vm();
    return vm.throw_completion<JS::InternalError>(JS::ErrorType::NotImplemented, "Location.host setter");
}

// https://html.spec.whatwg.org/multipage/history.html#dom-location-hostname
WebIDL::ExceptionOr<String> Location::hostname() const
{
    // 1. If this's relevant Document is non-null and its origin is not same origin-domain with the entry settings object's origin, then throw a "SecurityError" DOMException.
    auto const relevant_document = this->relevant_document();
    if (relevant_document && !relevant_document->origin().is_same_origin_domain(entry_settings_object().origin()))
        return WebIDL::SecurityError::create(realm(), "Location's relevant document is not same origin-domain with the entry settings object's origin"_string);

    auto url = this->url();

    // 2. If this's url's host is null, return the empty string.
    if (!url.host().has_value())
        return String {};

    // 3. Return this's url's host, serialized.
    return url.serialized_host();
}

WebIDL::ExceptionOr<void> Location::set_hostname(String const&)
{
    auto& vm = this->vm();
    return vm.throw_completion<JS::InternalError>(JS::ErrorType::NotImplemented, "Location.hostname setter");
}

// https://html.spec.whatwg.org/multipage/history.html#dom-location-port
WebIDL::ExceptionOr<String> Location::port() const
{
    // 1. If this's relevant Document is non-null and its origin is not same origin-domain with the entry settings object's origin, then throw a "SecurityError" DOMException.
    auto const relevant_document = this->relevant_document();
    if (relevant_document && !relevant_document->origin().is_same_origin_domain(entry_settings_object().origin()))
        return WebIDL::SecurityError::create(realm(), "Location's relevant document is not same origin-domain with the entry settings object's origin"_string);

    auto url = this->url();

    // 2. If this's url's port is null, return the empty string.
    if (!url.port().has_value())
        return String {};

    // 3. Return this's url's port, serialized.
    return String::number(*url.port());
}

WebIDL::ExceptionOr<void> Location::set_port(String const&)
{
    auto& vm = this->vm();
    return vm.throw_completion<JS::InternalError>(JS::ErrorType::NotImplemented, "Location.port setter");
}

// https://html.spec.whatwg.org/multipage/history.html#dom-location-pathname
WebIDL::ExceptionOr<String> Location::pathname() const
{
    // 1. If this's relevant Document is non-null and its origin is not same origin-domain with the entry settings object's origin, then throw a "SecurityError" DOMException.
    auto const relevant_document = this->relevant_document();
    if (relevant_document && !relevant_document->origin().is_same_origin_domain(entry_settings_object().origin()))
        return WebIDL::SecurityError::create(realm(), "Location's relevant document is not same origin-domain with the entry settings object's origin"_string);

    // 2. Return the result of URL path serializing this Location object's url.
    return url().serialize_path();
}

WebIDL::ExceptionOr<void> Location::set_pathname(String const&)
{
    auto& vm = this->vm();
    return vm.throw_completion<JS::InternalError>(JS::ErrorType::NotImplemented, "Location.pathname setter");
}

// https://html.spec.whatwg.org/multipage/history.html#dom-location-search
WebIDL::ExceptionOr<String> Location::search() const
{
    auto& vm = this->vm();

    // 1. If this's relevant Document is non-null and its origin is not same origin-domain with the entry settings object's origin, then throw a "SecurityError" DOMException.
    auto const relevant_document = this->relevant_document();
    if (relevant_document && !relevant_document->origin().is_same_origin_domain(entry_settings_object().origin()))
        return WebIDL::SecurityError::create(realm(), "Location's relevant document is not same origin-domain with the entry settings object's origin"_string);

    auto url = this->url();

    // 2. If this's url's query is either null or the empty string, return the empty string.
    if (!url.query().has_value() || url.query()->is_empty())
        return String {};

    // 3. Return "?", followed by this's url's query.
    return TRY_OR_THROW_OOM(vm, String::formatted("?{}", url.query()));
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-location-search
WebIDL::ExceptionOr<void> Location::set_search(String const& value)
{
    // The search setter steps are:
    auto const relevant_document = this->relevant_document();

    // 1. If this's relevant Document is null, then return.
    if (!relevant_document)
        return {};

    // 2. If this's relevant Document's origin is not same origin-domain with the entry settings object's origin, then throw a "SecurityError" DOMException.
    if (!relevant_document->origin().is_same_origin_domain(entry_settings_object().origin()))
        return WebIDL::SecurityError::create(realm(), "Location's relevant document is not same origin-domain with the entry settings object's origin"_string);

    // 3. Let copyURL be a copy of this's url.
    auto copy_url = this->url();

    // 4. If the given value is the empty string, set copyURL's query to null.
    if (value.is_empty()) {
        copy_url.set_query({});
    }
    // 5. Otherwise, run these substeps:
    else {
        // 5.1. Let input be the given value with a single leading "?" removed, if any.
        auto value_as_string_view = value.bytes_as_string_view();
        auto input = value_as_string_view.substring_view(value_as_string_view.starts_with('?'));

        // 5.2. Set copyURL's query to the empty string.
        copy_url.set_query(String {});

        // 5.3. Basic URL parse input, with null, the relevant Document's document's character encoding, copyURL as url, and query state as state override.
        (void)URL::Parser::basic_parse(input, {}, &copy_url, URL::Parser::State::Query);
    }

    // 6. Location-object navigate this to copyURL.
    TRY(navigate(copy_url));

    return {};
}

// https://html.spec.whatwg.org/multipage/history.html#dom-location-hash
WebIDL::ExceptionOr<String> Location::hash() const
{
    auto& vm = this->vm();

    // 1. If this's relevant Document is non-null and its origin is not same origin-domain with the entry settings object's origin, then throw a "SecurityError" DOMException.
    auto const relevant_document = this->relevant_document();
    if (relevant_document && !relevant_document->origin().is_same_origin_domain(entry_settings_object().origin()))
        return WebIDL::SecurityError::create(realm(), "Location's relevant document is not same origin-domain with the entry settings object's origin"_string);

    auto url = this->url();

    // 2. If this's url's fragment is either null or the empty string, return the empty string.
    if (!url.fragment().has_value() || url.fragment()->is_empty())
        return String {};

    // 3. Return "#", followed by this's url's fragment.
    return TRY_OR_THROW_OOM(vm, String::formatted("#{}", *url.fragment()));
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-location-hash
WebIDL::ExceptionOr<void> Location::set_hash(String const& value)
{
    // The hash setter steps are:
    auto const relevant_document = this->relevant_document();

    // 1. If this's relevant Document is null, then return.
    if (!relevant_document)
        return {};

    // 2. If this's relevant Document's origin is not same origin-domain with the entry settings object's origin, then throw a "SecurityError" DOMException.
    if (!relevant_document->origin().is_same_origin_domain(entry_settings_object().origin()))
        return WebIDL::SecurityError::create(realm(), "Location's relevant document is not same origin-domain with the entry settings object's origin"_string);

    // 3. Let copyURL be a copy of this's url.
    auto copy_url = this->url();

    // 4. Let input be the given value with a single leading "#" removed, if any.
    auto input = value.bytes_as_string_view().trim("#"sv, TrimMode::Left);

    // 5. Set copyURL's fragment to the empty string.
    copy_url.set_fragment(String {});

    // 6. Basic URL parse input, with copyURL as url and fragment state as state override.
    (void)URL::Parser::basic_parse(input, {}, &copy_url, URL::Parser::State::Fragment);

    // 7. If copyURL's fragment is this's url's fragment, then return.
    // NOTE: Ignore null values when comparing fragments. This behavior is not explicitly mentioned in the specs, potential bug?
    auto copy_url_fragment = copy_url.fragment().has_value() ? copy_url.fragment() : String {};
    auto this_url_fragment = this->url().fragment().has_value() ? this->url().fragment() : String {};
    if (copy_url_fragment == this_url_fragment)
        return {};

    // 8. Location-object navigate this to copyURL.
    TRY(navigate(copy_url));

    return {};
}

// https://html.spec.whatwg.org/multipage/history.html#dom-location-reload
void Location::reload() const
{
    // 1. Let document be this's relevant Document.
    auto document = relevant_document();

    // 2. If document is null, then return.
    if (!document)
        return;

    // FIXME: 3. If document's origin is not same origin-domain with the entry settings object's origin, then throw a "SecurityError" DOMException.

    // 4. Reload document's node navigable.
    document->navigable()->reload();
}

// https://html.spec.whatwg.org/multipage/history.html#dom-location-replace
WebIDL::ExceptionOr<void> Location::replace(String const& url)
{
    // 1. If this's relevant Document is null, then return.
    if (!relevant_document())
        return {};

    // 2. Parse url relative to the entry settings object. If that failed, throw a "SyntaxError" DOMException.
    auto replace_url = entry_settings_object().parse_url(url);
    if (!replace_url.has_value())
        return WebIDL::SyntaxError::create(realm(), MUST(String::formatted("Invalid URL '{}'", url)));

    // 3. Location-object navigate this to the resulting URL record given "replace".
    TRY(navigate(replace_url.release_value(), Bindings::NavigationHistoryBehavior::Replace));

    return {};
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-location-assign
WebIDL::ExceptionOr<void> Location::assign(String const& url)
{
    // 1. If this's relevant Document is null, then return.
    auto const relevant_document = this->relevant_document();
    if (!relevant_document)
        return {};

    // 2. If this's relevant Document's origin is not same origin-domain with the entry settings object's origin, then throw a "SecurityError" DOMException.
    if (!relevant_document->origin().is_same_origin_domain(entry_settings_object().origin()))
        return WebIDL::SecurityError::create(realm(), "Location's relevant document is not same origin-domain with the entry settings object's origin"_string);

    // 3. Parse url relative to the entry settings object. If that failed, throw a "SyntaxError" DOMException.
    auto assign_url = entry_settings_object().parse_url(url);
    if (!assign_url.has_value())
        return WebIDL::SyntaxError::create(realm(), MUST(String::formatted("Invalid URL '{}'", url)));

    // 4. Location-object navigate this to the resulting URL record.
    TRY(navigate(assign_url.release_value()));

    return {};
}

// 7.10.5.1 [[GetPrototypeOf]] ( ), https://html.spec.whatwg.org/multipage/history.html#location-getprototypeof
JS::ThrowCompletionOr<JS::Object*> Location::internal_get_prototype_of() const
{
    // 1. If IsPlatformObjectSameOrigin(this) is true, then return ! OrdinaryGetPrototypeOf(this).
    if (HTML::is_platform_object_same_origin(*this))
        return MUST(JS::Object::internal_get_prototype_of());

    // 2. Return null.
    return nullptr;
}

// 7.10.5.2 [[SetPrototypeOf]] ( V ), https://html.spec.whatwg.org/multipage/history.html#location-setprototypeof
JS::ThrowCompletionOr<bool> Location::internal_set_prototype_of(Object* prototype)
{
    // 1. Return ! SetImmutablePrototype(this, V).
    return MUST(set_immutable_prototype(prototype));
}

// 7.10.5.3 [[IsExtensible]] ( ), https://html.spec.whatwg.org/multipage/history.html#location-isextensible
JS::ThrowCompletionOr<bool> Location::internal_is_extensible() const
{
    // 1. Return true.
    return true;
}

// 7.10.5.4 [[PreventExtensions]] ( ), https://html.spec.whatwg.org/multipage/history.html#location-preventextensions
JS::ThrowCompletionOr<bool> Location::internal_prevent_extensions()
{
    // 1. Return false.
    return false;
}

// 7.10.5.5 [[GetOwnProperty]] ( P ), https://html.spec.whatwg.org/multipage/history.html#location-getownproperty
JS::ThrowCompletionOr<Optional<JS::PropertyDescriptor>> Location::internal_get_own_property(JS::PropertyKey const& property_key) const
{
    auto& vm = this->vm();

    // 1. If IsPlatformObjectSameOrigin(this) is true, then:
    if (HTML::is_platform_object_same_origin(*this)) {
        // 1. Let desc be OrdinaryGetOwnProperty(this, P).
        auto descriptor = MUST(Object::internal_get_own_property(property_key));

        // 2. If the value of the [[DefaultProperties]] internal slot of this contains P, then set desc.[[Configurable]] to true.
        // FIXME: This doesn't align with what the other browsers do. Spec issue: https://github.com/whatwg/html/issues/4157
        auto property_key_value = property_key.is_symbol()
            ? JS::Value { property_key.as_symbol() }
            : JS::PrimitiveString::create(vm, property_key.to_string());
        if (m_default_properties.contains_slow(property_key_value))
            descriptor->configurable = true;

        // 3. Return desc.
        return descriptor;
    }

    // 2. Let property be CrossOriginGetOwnPropertyHelper(this, P).
    auto property = HTML::cross_origin_get_own_property_helper(const_cast<Location*>(this), property_key);

    // 3. If property is not undefined, then return property.
    if (property.has_value())
        return property;

    // 4. Return ? CrossOriginPropertyFallback(P).
    return TRY(HTML::cross_origin_property_fallback(vm, property_key));
}

// 7.10.5.6 [[DefineOwnProperty]] ( P, Desc ), https://html.spec.whatwg.org/multipage/history.html#location-defineownproperty
JS::ThrowCompletionOr<bool> Location::internal_define_own_property(JS::PropertyKey const& property_key, JS::PropertyDescriptor const& descriptor, Optional<JS::PropertyDescriptor>* precomputed_get_own_property)
{
    // 1. If IsPlatformObjectSameOrigin(this) is true, then:
    if (HTML::is_platform_object_same_origin(*this)) {
        // 1. If the value of the [[DefaultProperties]] internal slot of this contains P, then return false.
        // 2. Return ? OrdinaryDefineOwnProperty(this, P, Desc).
        return JS::Object::internal_define_own_property(property_key, descriptor, precomputed_get_own_property);
    }

    // 2. Throw a "SecurityError" DOMException.
    return throw_completion(WebIDL::SecurityError::create(realm(), MUST(String::formatted("Can't define property '{}' on cross-origin object", property_key))));
}

// 7.10.5.7 [[Get]] ( P, Receiver ), https://html.spec.whatwg.org/multipage/history.html#location-get
JS::ThrowCompletionOr<JS::Value> Location::internal_get(JS::PropertyKey const& property_key, JS::Value receiver, JS::CacheablePropertyMetadata* cacheable_metadata, PropertyLookupPhase phase) const
{
    auto& vm = this->vm();

    // 1. If IsPlatformObjectSameOrigin(this) is true, then return ? OrdinaryGet(this, P, Receiver).
    if (HTML::is_platform_object_same_origin(*this))
        return JS::Object::internal_get(property_key, receiver, cacheable_metadata, phase);

    // 2. Return ? CrossOriginGet(this, P, Receiver).
    return HTML::cross_origin_get(vm, static_cast<JS::Object const&>(*this), property_key, receiver);
}

// 7.10.5.8 [[Set]] ( P, V, Receiver ), https://html.spec.whatwg.org/multipage/history.html#location-set
JS::ThrowCompletionOr<bool> Location::internal_set(JS::PropertyKey const& property_key, JS::Value value, JS::Value receiver, JS::CacheablePropertyMetadata* cacheable_metadata)
{
    auto& vm = this->vm();

    // 1. If IsPlatformObjectSameOrigin(this) is true, then return ? OrdinarySet(this, P, V, Receiver).
    if (HTML::is_platform_object_same_origin(*this))
        return JS::Object::internal_set(property_key, value, receiver, cacheable_metadata);

    // 2. Return ? CrossOriginSet(this, P, V, Receiver).
    return HTML::cross_origin_set(vm, static_cast<JS::Object&>(*this), property_key, value, receiver);
}

// 7.10.5.9 [[Delete]] ( P ), https://html.spec.whatwg.org/multipage/history.html#location-delete
JS::ThrowCompletionOr<bool> Location::internal_delete(JS::PropertyKey const& property_key)
{
    // 1. If IsPlatformObjectSameOrigin(this) is true, then return ? OrdinaryDelete(this, P).
    if (HTML::is_platform_object_same_origin(*this))
        return JS::Object::internal_delete(property_key);

    // 2. Throw a "SecurityError" DOMException.
    return throw_completion(WebIDL::SecurityError::create(realm(), MUST(String::formatted("Can't delete property '{}' on cross-origin object", property_key))));
}

// 7.10.5.10 [[OwnPropertyKeys]] ( ), https://html.spec.whatwg.org/multipage/history.html#location-ownpropertykeys
JS::ThrowCompletionOr<GC::RootVector<JS::Value>> Location::internal_own_property_keys() const
{
    // 1. If IsPlatformObjectSameOrigin(this) is true, then return OrdinaryOwnPropertyKeys(this).
    if (HTML::is_platform_object_same_origin(*this))
        return JS::Object::internal_own_property_keys();

    // 2. Return CrossOriginOwnPropertyKeys(this).
    return HTML::cross_origin_own_property_keys(this);
}

}
