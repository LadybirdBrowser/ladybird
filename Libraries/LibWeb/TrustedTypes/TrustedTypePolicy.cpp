/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/TrustedTypes/TrustedTypePolicy.h>

#include <LibGC/Ptr.h>
#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/Value.h>
#include <LibWeb/Bindings/CppDictionary.h>
#include <LibWeb/Bindings/Sanitizer.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/Sanitizer.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/WindowOrWorkerGlobalScope.h>
#include <LibWeb/TrustedTypes/RequireTrustedTypesForDirective.h>
#include <LibWeb/TrustedTypes/TrustedHTML.h>
#include <LibWeb/TrustedTypes/TrustedHTMLParserOptions.h>
#include <LibWeb/TrustedTypes/TrustedScript.h>
#include <LibWeb/TrustedTypes/TrustedScriptURL.h>
#include <LibWeb/TrustedTypes/TrustedTypePolicyFactory.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/CallbackType.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/WebIDL/ExceptionOrUtils.h>

namespace Web::TrustedTypes {

extern "C" {
// Defined in Bindings/Rust/src/trusted_types/trusted_type_policy.rs.
WebIDL::CallbackType* web_rust_trusted_types_trusted_type_policy_create_parser_options(TrustedTypePolicy*);
u8 web_rust_trusted_types_get_trusted_type_compliant_string(JSVM*, u8 expected_type, JSObject* global, u8 input_kind, void* input_cell, FlatPtr input_string, FlatPtr sink, FlatPtr sink_group, FlatPtr* string, JSValue* exception);
u8 web_rust_trusted_types_get_trusted_types_compliant_attribute_value(JSVM*, FlatPtr attribute_name, bool has_attribute_ns, FlatPtr attribute_ns, FlatPtr element_local_name, bool has_element_namespace, FlatPtr element_namespace, JSObject* global, u8 input_kind, void* input_cell, FlatPtr input_string, FlatPtr* string, JSValue* exception);
bool web_rust_trusted_types_process_value_with_a_default_policy(JSVM*, u8 trusted_type_name, JSObject* global, u8 input_kind, void* input_cell, FlatPtr input_string, FlatPtr sink, void** result, JSValue* exception);

// Called by Bindings/Rust/src/trusted_types/trusted_type_policy.rs.
JSCompletion web_trusted_types_create_parser_options(JSObject* global, WebIDL::CallbackType* create_parser_options, Bindings::CppDictionary<Bindings::SetHTMLUnsafeOptions>* options, ReadonlySpan<JSValue> arguments);
}

GC::Ptr<WebIDL::CallbackType> TrustedTypePolicy::create_parser_options_callback() const
{
    return web_rust_trusted_types_trusted_type_policy_create_parser_options(const_cast<TrustedTypePolicy*>(this));
}

static TrustedScript* trusted_script_from_value(JS::Value value)
{
    if (!value.is_object())
        return nullptr;
    return Bindings::impl_from<TrustedScript>(&value.as_object());
}

bool trusted_script_value_matches(JS::Value value, StringView expected)
{
    auto* trusted_script = trusted_script_from_value(value);
    return trusted_script && trusted_script->to_string() == expected;
}

bool trusted_script_values_match(ReadonlySpan<JS::Value> values, ReadonlySpan<String> expected)
{
    if (values.size() != expected.size())
        return false;

    for (size_t i = 0; i < values.size(); ++i) {
        if (!trusted_script_value_matches(values[i], expected[i]))
            return false;
    }

    return true;
}

static JS::Completion invoke_trusted_type_policy_callback(WebIDL::CallbackType& callback, GC::RootVector<JS::Value> const& arguments)
{
    return WebIDL::invoke_callback(callback, {}, WebIDL::ExceptionBehavior::Rethrow, arguments);
}

// https://w3c.github.io/trusted-types/dist/spec/#create-a-parser-options-callback-argument
template<typename Options>
static WebIDL::ExceptionOr<JS::Value> create_a_parser_options_callback_argument(JS::Object& global, Options const& dictionary)
{
    // 1. Let runScriptsInput be dictionary["runScripts"], with default false.
    bool run_scripts_input = false;
    if constexpr (requires { dictionary.run_scripts; })
        run_scripts_input = dictionary.run_scripts;

    // 2. Let argument be the ordered map «[ "runScripts" → runScriptsInput ]».
    auto& realm = HTML::relevant_realm(global);
    auto argument = JS::Object::create(realm, realm.intrinsics().object_prototype());
    MUST(argument->create_data_property("runScripts"_utf16_fly_string, JS::Value(run_scripts_input)));

    // 3. Let sanitizerInput be the result of getting a sanitizer from realm and dictionary.
    // https://w3c.github.io/trusted-types/dist/spec/#get-a-sanitizer-from-dictionary
    // 1. Let sanitizerInput be dictionary["sanitizer"], defaulting to null.
    // 2. If sanitizerInput is null, return null.
    // 4. If sanitizerInput is not null, set argument["sanitizer"] to sanitizerInput.
    if (dictionary.sanitizer.has_value()) {
        auto sanitizer = TRY(HTML::get_a_sanitizer_from_sanitizer_input(*dictionary.sanitizer));
        auto wrapper = Bindings::wrap(Bindings::host_defined_wrapper_world(realm), realm, sanitizer);
        MUST(argument->create_data_property("sanitizer"_utf16_fly_string, wrapper));
    }

    // 5. Return argument.
    return argument;
}

// NB: Converts the return value of a CreateParserOptionsCallback, whose return type is SetHTMLUnsafeOptions?, to an
//     IDL value.
static WebIDL::ExceptionOr<Optional<Bindings::SetHTMLUnsafeOptions>> to_optional_set_html_unsafe_options(JS::VM& vm, JS::Value value)
{
    if (value.is_nullish())
        return Optional<Bindings::SetHTMLUnsafeOptions> {};
    return Optional<Bindings::SetHTMLUnsafeOptions> { TRY(Bindings::convert_to_idl_value_for_set_html_unsafe_options(vm, value)) };
}

// https://w3c.github.io/trusted-types/dist/spec/#get-a-sanitizer-config-copy-from-dictionary
static WebIDL::ExceptionOr<Optional<HTML::SanitizerConfiguration>> get_a_sanitizer_config_copy_from_dictionary(Bindings::SetHTMLUnsafeOptions const& dictionary)
{
    // 1. Let sanitizer be the result of getting a sanitizer from realm and dictionary.
    // 2. If sanitizer is null, return null.
    if (!dictionary.sanitizer.has_value())
        return Optional<HTML::SanitizerConfiguration> {};
    auto sanitizer = TRY(HTML::get_a_sanitizer_from_sanitizer_input(*dictionary.sanitizer));

    // 3. Return sanitizer's configuration.
    return Optional<HTML::SanitizerConfiguration> { sanitizer->configuration() };
}

// https://w3c.github.io/trusted-types/dist/spec/#get-trusted-type-policy-value
// NB: The steps for the "TrustedHTMLParserOptions" type name.
template<typename Options>
static WebIDL::ExceptionOr<JS::Value> get_parser_options_policy_value(JS::Object& global, GC::Ptr<WebIDL::CallbackType> function, Options const& value, GC::RootVector<JS::Value> const& arguments, ThrowIfCallbackMissing throw_if_missing)
{
    // 1. Let functionName be a function name for the given trustedTypeName, based on the following table:
    // 2. Let function be policy's options[functionName].
    // NB: The caller passes function.

    // 3. If function is null, then:
    if (!function) {
        // 1. If throwIfMissing throw a TypeError.
        if (throw_if_missing == ThrowIfCallbackMissing::Yes)
            return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Trying to create a trusted type without a callback"_utf16 };

        // 2. Else return null.
        return JS::js_null();
    }

    // 4. If trustedTypeName is "TrustedHTMLParserOptions":
    //    1. Set value to the result of creating a parser options callback argument given policy's relevant realm and
    //       value.
    auto argument = TRY(create_a_parser_options_callback_argument(global, value));

    // 5. Let args be « value ».
    GC::RootVector<JS::Value> args;
    args.append(argument);

    // 6. Append each item in arguments to args.
    args.extend(arguments);

    // 7. Let policyValue be the result of invoking function with args and "rethrow".
    // 8. Return policyValue.
    return TRY(invoke_trusted_type_policy_callback(*function, args));
}

// https://w3c.github.io/trusted-types/dist/spec/#dom-trustedtypepolicy-createparseroptions
static WebIDL::ExceptionOr<GC::Ref<TrustedHTMLParserOptions>> create_parser_options(JS::Object& global, GC::Ptr<WebIDL::CallbackType> create_parser_options_callback, Bindings::SetHTMLUnsafeOptions const& options, GC::RootVector<JS::Value> const& arguments)
{
    // Returns the result of executing the create a trusted type algorithm, with the following arguments:
    //   policy: this value
    //   trustedTypeName: "TrustedHTMLParserOptions"
    //   value: options
    //   arguments: arguments
    // https://w3c.github.io/trusted-types/dist/spec/#create-a-trusted-type-algorithm
    // 1. Let policyValue be the result of executing get trusted type policy value with the same arguments as this
    //    algorithm and additionally true as throwIfMissing.
    // 2. If the algorithm threw an error, rethrow the error and abort the following steps.
    auto policy_value = TRY(to_optional_set_html_unsafe_options(global.vm(), TRY(get_parser_options_policy_value(global, create_parser_options_callback, options, arguments, ThrowIfCallbackMissing::Yes))));

    // 3. If trustedTypeName is "TrustedHTMLParserOptions":
    // 1. If policyValue is null, throw a TypeError.
    // NOTE: Returning null from a named policy's createParserOptions callback throws a TypeError here, whereas
    //       returning null or undefined from createHTML, createScript, or createScriptURL yields the empty string.
    if (!policy_value.has_value())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "createParserOptions returned null"_utf16 };

    // 2. Let sanitizerConfig be the result of getting a sanitizer configuration from policy's relevant realm and
    //    policyValue.
    auto sanitizer_config = TRY(get_a_sanitizer_config_copy_from_dictionary(*policy_value));

    // 3. Let runScripts be policyValue["runScripts"] with default false.
    auto run_scripts = policy_value->run_scripts;

    // 4. Return a new instance of TrustedHTMLParserOptions in policy's relevant realm with its sanitizer configuration
    //    set to sanitizerConfig, and its runScripts set to runScripts.
    return TrustedHTMLParserOptions::create(move(sanitizer_config), run_scripts);
}

JSCompletion web_trusted_types_create_parser_options(JSObject* global, WebIDL::CallbackType* create_parser_options_callback, Bindings::CppDictionary<Bindings::SetHTMLUnsafeOptions>* options, ReadonlySpan<JSValue> arguments)
{
    auto& global_object = *JS::HostABI::object_from_abi(global);
    GC::RootVector<JS::Value> argument_values;
    for (auto argument : arguments)
        argument_values.append(JS::HostABI::value_from_abi(argument));
    auto result = create_parser_options(global_object, create_parser_options_callback, options->dictionary(), argument_values);
    if (result.is_exception()) {
        auto& vm = global_object.vm();
        return JS::HostABI::throw_completion_to_abi(WebIDL::exception_to_throw_completion(vm, *vm.current_realm(), result.exception()).value());
    }
    auto& realm = HTML::relevant_realm(global_object);
    auto wrapper = Bindings::wrap(Bindings::host_defined_wrapper_world(realm), realm, result.release_value());
    return JS::HostABI::normal_completion_to_abi(JS::HostABI::value_to_abi(wrapper));
}

// NB: A SetHTMLUnsafeOptions or ParseHTMLUnsafeOptions options needs vetting if options["runScripts"] exists and is
//     true, or if options["sanitizer"] exists and is not an empty SanitizerConfig.
//     https://w3c.github.io/trusted-types/dist/spec/#parser-options-need-vetting
template<typename Options>
static bool needs_vetting(Options const& options)
{
    if constexpr (requires { options.run_scripts; }) {
        if (options.run_scripts)
            return true;
    }
    if (!options.sanitizer.has_value())
        return false;
    // NOTE: An empty SanitizerConfig does not change the parsed markup.
    auto const* config = options.sanitizer->template get_pointer<Bindings::SanitizerConfig>();
    if (!config)
        return true;
    return config->elements.has_value() || config->remove_elements.has_value() || config->replace_with_children_elements.has_value()
        || config->processing_instructions.has_value() || config->remove_processing_instructions.has_value()
        || config->attributes.has_value() || config->remove_attributes.has_value() || config->comments.has_value()
        || config->data_attributes.has_value() || config->javascript_ur_ls.has_value();
}

// https://w3c.github.io/trusted-types/dist/spec/#get-trusted-type-compliant-parser-options
template<typename Options>
WebIDL::ExceptionOr<CompliantParserOptions<Options>> get_trusted_type_compliant_parser_options(JS::Object& global, CompliantParserOptions<Options> const& input, ThrowIfCallbackMissing throw_if_missing, InjectionSink sink, OptionsFromAuthor options_from_author)
{
    // 1. If input is an instance of TrustedHTMLParserOptions, return input.
    if (input.template has<GC::Ref<TrustedHTMLParserOptions>>())
        return input;
    auto const& options = input.template get<Options>();

    // 2. Let requireTrustedTypes be the result of executing Does sink type require trusted types? algorithm, passing
    //    global, "script", and true.
    auto require_trusted_types = does_sink_require_trusted_types(global, Script.view(), IncludeReportOnlyPolicies::Yes);

    // 3. If requireTrustedTypes is false, return input.
    if (!require_trusted_types)
        return input;

    // 4. Let requirePolicy be throwIfMissing.
    bool require_policy = throw_if_missing == ThrowIfCallbackMissing::Yes;

    // 5. If optionsFromAuthor is true and input needs vetting, set requirePolicy to true.
    if (options_from_author == OptionsFromAuthor::Yes && needs_vetting(options))
        require_policy = true;

    auto reject_unless_allowed = [&]() -> WebIDL::ExceptionOr<CompliantParserOptions<Options>> {
        // 1. Let disposition be the result of executing should sink type mismatch violation be blocked by content
        //    security policy? algorithm, passing global, sink, "script", and the empty string as source.
        auto disposition = should_sink_type_mismatch_violation_be_blocked_by_content_security_policy(global, to_string(sink), Script.view(), {});

        // 2. If disposition is "Allowed", return input.
        if (disposition == ContentSecurityPolicy::Directives::Directive::Result::Allowed)
            return input;

        // 3. Throw a TypeError.
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, Utf16String::formatted("Sink {} requires parser options vetted by a Trusted Types policy", to_string(sink)) };
    };

    // 6. Let defaultPolicy be the value of global's trusted type policy factory's default policy.
    auto* global_scope = HTML::window_or_worker_global_scope_from_global_object(global);
    VERIFY(global_scope);
    auto default_policy = global_scope->trusted_types()->default_policy();

    // 7. If defaultPolicy is null:
    if (!default_policy) {
        // 1. If requirePolicy is true:
        if (require_policy)
            return reject_unless_allowed();

        // 2. Return input.
        return input;
    }

    // 8. Let function be defaultPolicy's options["createParserOptions"].
    auto function = default_policy->create_parser_options_callback();

    // 9. If function is null:
    if (!function) {
        // 1. If requirePolicy is true:
        if (require_policy)
            return reject_unless_allowed();

        // 2. Return input.
        return input;
    }

    // 10. Let argument be the result of creating a parser options callback argument given global's relevant realm and
    //     input.
    // 11. Let policyValue be the result of invoking function with « argument, "TrustedHTMLParserOptions", sink » and
    //     "rethrow".
    // 12. If the algorithm threw an error, rethrow the error and abort the following steps.
    auto& realm = HTML::relevant_realm(global);
    auto& vm = realm.vm();
    GC::RootVector<JS::Value> arguments;
    arguments.append(JS::PrimitiveString::create(vm, "TrustedHTMLParserOptions"_utf16));
    arguments.append(JS::PrimitiveString::create(vm, to_string(sink)));
    auto policy_value = TRY(to_optional_set_html_unsafe_options(vm, TRY(get_parser_options_policy_value(global, function, options, arguments, ThrowIfCallbackMissing::Yes))));

    // 13. If policyValue is null or undefined:
    if (!policy_value.has_value()) {
        // NOTE: This step assures that the default policy rejection will be reported, but ignored in a report-only
        //       mode.
        return reject_unless_allowed();
    }

    // 14. Let runScripts be policyValue["runScripts"], with default false.
    // NOTE: When invoked as a default policy, the createParserOptions callback determines the final script execution
    //       behavior, overriding the caller's requested runScripts option.
    auto run_scripts = policy_value->run_scripts;

    // 15. Return a new TrustedHTMLParserOptions in global's relevant realm whose sanitizer configuration is set to the
    //     result of getting a sanitizer configuration from global's relevant realm and policyValue, and whose
    //     runScripts is set to runScripts.
    auto sanitizer_config = TRY(get_a_sanitizer_config_copy_from_dictionary(*policy_value));
    return CompliantParserOptions<Options> { TrustedHTMLParserOptions::create(move(sanitizer_config), run_scripts) };
}

// https://w3c.github.io/trusted-types/dist/spec/#get-trusted-type-compliant-input
template<typename Options>
WebIDL::ExceptionOr<CompliantInput<Options>> get_trusted_type_compliant_input(JS::Object& global, TrustedHTMLOrString const& input, CompliantParserOptions<Options> const& options, InjectionSink sink, DocumentType document_type, OptionsFromAuthor options_from_author)
{
    // 1. Let compliantOptions be options.
    auto compliant_options = options;

    // 2. If documentType is "html":
    if (document_type == DocumentType::HTML) {
        // 1. Set compliantOptions to the result of calling get trusted type compliant parser options, passing global,
        //    options, false, sink, and optionsFromAuthor.
        // NOTE: Sinks that construct options themselves, rather than taking them from the author, pass false for
        //       optionsFromAuthor. For example, createContextualFragment() sets runScripts to true.
        compliant_options = TRY(get_trusted_type_compliant_parser_options(global, options, ThrowIfCallbackMissing::No, sink, options_from_author));

        // 2. If compliantOptions is an instance of TrustedHTMLParserOptions, return (stringified input,
        //    compliantOptions).
        if (compliant_options.template has<GC::Ref<TrustedHTMLParserOptions>>()) {
            auto stringified_input = input.visit(
                [](GC::Ref<TrustedHTML> const& trusted_html) { return trusted_html->to_string(); },
                [](Utf16String const& string) { return string; });
            return CompliantInput<Options> { move(stringified_input), move(compliant_options) };
        }
    }

    // 3. Let compliantHTML be the result of calling get trusted type compliant string, passing TrustedHTML, global,
    //    input, sink, and "script".
    // 4. If the algorithm threw an error, rethrow the error.
    auto compliant_html = TRY(get_trusted_type_compliant_string(TrustedTypeName::TrustedHTML, global, input, sink, Script.view()));

    // 5. Return (compliantHTML, compliantOptions).
    return CompliantInput<Options> { move(compliant_html), move(compliant_options) };
}

template WebIDL::ExceptionOr<CompliantInput<Bindings::SetHTMLUnsafeOptions>> get_trusted_type_compliant_input(JS::Object&, TrustedHTMLOrString const&, CompliantParserOptions<Bindings::SetHTMLUnsafeOptions> const&, InjectionSink, DocumentType, OptionsFromAuthor);
template WebIDL::ExceptionOr<CompliantInput<Bindings::ParseHTMLUnsafeOptions>> get_trusted_type_compliant_input(JS::Object&, TrustedHTMLOrString const&, CompliantParserOptions<Bindings::ParseHTMLUnsafeOptions> const&, InjectionSink, DocumentType, OptionsFromAuthor);

// The kind is 0 for a string, or the TrustedTypeName plus one.
struct InputABI {
    u8 kind { 0 };
    void* cell { nullptr };
    FlatPtr string { 0 };
};

static InputABI input_to_abi(Variant<GC::Ref<TrustedHTML>, GC::Ref<TrustedScript>, GC::Ref<TrustedScriptURL>, Utf16String> const& input)
{
    return input.visit(
        [](GC::Ref<TrustedHTML> const& value) { return InputABI { 1, value.ptr(), 0 }; },
        [](GC::Ref<TrustedScript> const& value) { return InputABI { 2, value.ptr(), 0 }; },
        [](GC::Ref<TrustedScriptURL> const& value) { return InputABI { 3, value.ptr(), 0 }; },
        [](Utf16String const& value) { return InputABI { 0, nullptr, Utf16String { value }.into_raw() }; });
}

static Utf16String stringified(Variant<GC::Ref<TrustedHTML>, GC::Ref<TrustedScript>, GC::Ref<TrustedScriptURL>, Utf16String> const& input)
{
    return input.visit(
        [](Utf16String const& value) { return value; },
        [](auto const& value) { return value->to_string(); });
}

static WebIDL::ExceptionOr<Utf16String> result_from_abi(u8 result, FlatPtr string, JSValue exception)
{
    switch (result) {
    case 0:
        return Utf16String::adopt_raw(string);
    case 1:
        return JS::throw_completion(JS::HostABI::value_from_abi(exception));
    case 2:
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, Utf16String::adopt_raw(string) };
    default:
        VERIFY_NOT_REACHED();
    }
}

// https://w3c.github.io/trusted-types/dist/spec/#process-value-with-a-default-policy-algorithm
WebIDL::ExceptionOr<Optional<TrustedType>> process_value_with_a_default_policy(TrustedTypeName trusted_type_name, JS::Object& global, Variant<GC::Ref<TrustedHTML>, GC::Ref<TrustedScript>, GC::Ref<TrustedScriptURL>, Utf16String> input, InjectionSink sink)
{
    auto input_abi = input_to_abi(input);
    void* result = nullptr;
    JSValue exception {};
    if (web_rust_trusted_types_process_value_with_a_default_policy(JS::HostABI::vm_to_abi(global.vm()), to_underlying(trusted_type_name), JS::HostABI::object_to_abi(&global), input_abi.kind, input_abi.cell, input_abi.string, to_string(sink).into_raw(), &result, &exception))
        return JS::throw_completion(JS::HostABI::value_from_abi(exception));
    if (!result)
        return Optional<TrustedType> {};
    switch (trusted_type_name) {
    case TrustedTypeName::TrustedHTML:
        return TrustedType { GC::Ref { *static_cast<TrustedHTML*>(result) } };
    case TrustedTypeName::TrustedScript:
        return TrustedType { GC::Ref { *static_cast<TrustedScript*>(result) } };
    case TrustedTypeName::TrustedScriptURL:
        return TrustedType { GC::Ref { *static_cast<TrustedScriptURL*>(result) } };
    }
    VERIFY_NOT_REACHED();
}

// https://w3c.github.io/trusted-types/dist/spec/#get-trusted-type-compliant-string-algorithm
WebIDL::ExceptionOr<Utf16String> get_trusted_type_compliant_string(TrustedTypeName expected_type, JS::Object& global, Variant<GC::Ref<TrustedHTML>, GC::Ref<TrustedScript>, GC::Ref<TrustedScriptURL>, Utf16String> input, InjectionSink sink, Utf16View sink_group)
{
    // NB: Steps 1 to 3, so that pages that don't require Trusted Types never call into Rust.
    auto input_is_expected_type = input.visit(
        [&](GC::Ref<TrustedHTML> const&) { return expected_type == TrustedTypeName::TrustedHTML; },
        [&](GC::Ref<TrustedScript> const&) { return expected_type == TrustedTypeName::TrustedScript; },
        [&](GC::Ref<TrustedScriptURL> const&) { return expected_type == TrustedTypeName::TrustedScriptURL; },
        [](Utf16String const&) { return false; });
    if (input_is_expected_type || !does_sink_require_trusted_types(global, sink_group, IncludeReportOnlyPolicies::Yes))
        return stringified(input);

    auto input_abi = input_to_abi(input);
    FlatPtr string = 0;
    JSValue exception {};
    auto result = web_rust_trusted_types_get_trusted_type_compliant_string(JS::HostABI::vm_to_abi(global.vm()), to_underlying(expected_type), JS::HostABI::object_to_abi(&global), input_abi.kind, input_abi.cell, input_abi.string, to_string(sink).into_raw(), Utf16String::from_utf16(sink_group).into_raw(), &string, &exception);
    return result_from_abi(result, string, exception);
}

// https://w3c.github.io/trusted-types/dist/spec/#validate-attribute-mutation
WebIDL::ExceptionOr<Utf16String> get_trusted_types_compliant_attribute_value(Utf16FlyString const& attribute_name, Optional<Utf16FlyString> attribute_ns, DOM::Element const& element, Variant<GC::Ref<TrustedHTML>, GC::Ref<TrustedScript>, GC::Ref<TrustedScriptURL>, Utf16String> const& new_value)
{
    auto& global = HTML::relevant_global_object(element.document());

    // NB: Unless 'script' requires Trusted Types, every step returns stringified newValue.
    if (!does_sink_require_trusted_types(global, Script.view(), IncludeReportOnlyPolicies::Yes))
        return stringified(new_value);

    auto input_abi = input_to_abi(new_value);
    FlatPtr string = 0;
    JSValue exception {};
    auto result = web_rust_trusted_types_get_trusted_types_compliant_attribute_value(
        JS::HostABI::vm_to_abi(global.vm()),
        attribute_name.to_utf16_string().into_raw(),
        attribute_ns.has_value(),
        attribute_ns.has_value() ? attribute_ns->to_utf16_string().into_raw() : 0,
        element.local_name().to_utf16_string().into_raw(),
        element.namespace_uri().has_value(),
        element.namespace_uri().has_value() ? element.namespace_uri()->to_utf16_string().into_raw() : 0,
        JS::HostABI::object_to_abi(&global),
        input_abi.kind,
        input_abi.cell,
        input_abi.string,
        &string,
        &exception);
    return result_from_abi(result, string, exception);
}

}
