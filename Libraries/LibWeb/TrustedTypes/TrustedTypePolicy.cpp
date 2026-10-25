/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/TrustedTypes/TrustedTypePolicy.h>

#include <LibGC/Heap.h>
#include <LibGC/Ptr.h>
#include <LibJS/Runtime/Value.h>
#include <LibWeb/Bindings/Sanitizer.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/Sanitizer.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/TagNames.h>
#include <LibWeb/HTML/WindowOrWorkerGlobalScope.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/SVG/TagNames.h>
#include <LibWeb/TrustedTypes/RequireTrustedTypesForDirective.h>
#include <LibWeb/TrustedTypes/TrustedHTML.h>
#include <LibWeb/TrustedTypes/TrustedHTMLParserOptions.h>
#include <LibWeb/TrustedTypes/TrustedScript.h>
#include <LibWeb/TrustedTypes/TrustedScriptURL.h>
#include <LibWeb/TrustedTypes/TrustedTypePolicyFactory.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/CallbackType.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::TrustedTypes {

GC_DEFINE_ALLOCATOR(TrustedTypePolicy);

TrustedTypePolicy::TrustedTypePolicy(Utf16String const& name, TrustedTypePolicyOptions const& options)
    : m_name(name)
    , m_create_html(options.create_html)
    , m_create_script(options.create_script)
    , m_create_script_url(options.create_script_url)
    , m_create_parser_options(options.create_parser_options)
{
}

void TrustedTypePolicy::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_create_html);
    visitor.visit(m_create_script);
    visitor.visit(m_create_script_url);
    visitor.visit(m_create_parser_options);
}

Utf16String to_string(TrustedTypeName trusted_type_name)
{
    switch (trusted_type_name) {
    case TrustedTypeName::TrustedHTML:
        return "TrustedHTML"_utf16;
    case TrustedTypeName::TrustedScript:
        return "TrustedScript"_utf16;
    case TrustedTypeName::TrustedScriptURL:
        return "TrustedScriptURL"_utf16;
    default:
        VERIFY_NOT_REACHED();
    }
}

TrustedHTML* trusted_html_from_value(JS::Value value)
{
    if (!value.is_object())
        return nullptr;
    return Bindings::impl_from<TrustedHTML>(&value.as_object());
}

TrustedScript* trusted_script_from_value(JS::Value value)
{
    if (!value.is_object())
        return nullptr;
    return Bindings::impl_from<TrustedScript>(&value.as_object());
}

TrustedScriptURL* trusted_script_url_from_value(JS::Value value)
{
    if (!value.is_object())
        return nullptr;
    return Bindings::impl_from<TrustedScriptURL>(&value.as_object());
}

bool is_trusted_html_value(JS::Value value)
{
    return trusted_html_from_value(value);
}

bool is_trusted_script_value(JS::Value value)
{
    return trusted_script_from_value(value);
}

bool is_trusted_script_url_value(JS::Value value)
{
    return trusted_script_url_from_value(value);
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

JS::Completion invoke_trusted_type_policy_callback(WebIDL::CallbackType& callback, GC::RootVector<JS::Value> const& arguments)
{
    return WebIDL::invoke_callback(callback, {}, WebIDL::ExceptionBehavior::Rethrow, arguments);
}

// https://w3c.github.io/trusted-types/dist/spec/#dom-trustedtypepolicy-createhtml
WebIDL::ExceptionOr<GC::Ref<TrustedHTML>> TrustedTypePolicy::create_html(JS::VM& vm, Utf16String const& input, GC::RootVector<JS::Value> const& arguments)
{
    // 1. Returns the result of executing the Create a Trusted Type algorithm, with the following arguments:
    //    policy
    //      this value
    //    trustedTypeName
    //      "TrustedHTML"
    //    value
    //      input
    //    arguments
    //      arguments
    auto const trusted_type = TRY(create_a_trusted_type(vm, TrustedTypeName::TrustedHTML, input, arguments));
    return trusted_type.get<GC::Ref<TrustedHTML>>();
}

// https://w3c.github.io/trusted-types/dist/spec/#dom-trustedtypepolicy-createscript
WebIDL::ExceptionOr<GC::Ref<TrustedScript>> TrustedTypePolicy::create_script(JS::VM& vm, Utf16String const& input, GC::RootVector<JS::Value> const& arguments)
{
    // 1. Returns the result of executing the Create a Trusted Type algorithm, with the following arguments:
    //    policy
    //      this value
    //    trustedTypeName
    //      "TrustedScript"
    //    value
    //      input
    //    arguments
    //      arguments
    auto const trusted_type = TRY(create_a_trusted_type(vm, TrustedTypeName::TrustedScript, input, arguments));
    return trusted_type.get<GC::Ref<TrustedScript>>();
}

// https://w3c.github.io/trusted-types/dist/spec/#dom-trustedtypepolicy-createscripturl
WebIDL::ExceptionOr<GC::Ref<TrustedScriptURL>> TrustedTypePolicy::create_script_url(JS::VM& vm, Utf16String const& input, GC::RootVector<JS::Value> const& arguments)
{
    // 1. Returns the result of executing the Create a Trusted Type algorithm, with the following arguments:
    //    policy
    //      this value
    //    trustedTypeName
    //      "TrustedScriptURL"
    //    value
    //      input
    //    arguments
    //      arguments
    auto const trusted_type = TRY(create_a_trusted_type(vm, TrustedTypeName::TrustedScriptURL, input, arguments));
    return trusted_type.get<GC::Ref<TrustedScriptURL>>();
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

// https://w3c.github.io/trusted-types/dist/spec/#dom-trustedtypepolicy-createparseroptions
WebIDL::ExceptionOr<GC::Ref<TrustedHTMLParserOptions>> TrustedTypePolicy::create_parser_options(JS::Object& global, Bindings::SetHTMLUnsafeOptions const& options, GC::RootVector<JS::Value> const& arguments)
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
    auto policy_value = TRY(to_optional_set_html_unsafe_options(global.vm(), TRY(get_parser_options_policy_value(global, options, arguments, ThrowIfCallbackMissing::Yes))));

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

// https://w3c.github.io/trusted-types/dist/spec/#get-trusted-type-policy-value
// NB: The steps of "get trusted type policy value" for the "TrustedHTMLParserOptions" type name.
template<typename Options>
WebIDL::ExceptionOr<JS::Value> TrustedTypePolicy::get_parser_options_policy_value(JS::Object& global, Options const& value, GC::RootVector<JS::Value> const& arguments, ThrowIfCallbackMissing throw_if_missing)
{
    // 1. Let functionName be a function name for the given trustedTypeName, based on the following table:
    // 2. Let function be policy's options[functionName].
    auto function = m_create_parser_options;

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

// NB: A SetHTMLUnsafeOptions or ParseHTMLUnsafeOptions options needs vetting if options["runScripts"] exists and is
//     true, or if options["sanitizer"] exists and is not an empty SanitizerConfig.
//     https://w3c.github.io/trusted-types/dist/spec/#needs-vetting
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
        auto disposition = should_sink_type_mismatch_violation_be_blocked_by_content_security_policy(global, sink, Script.view(), {});

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
    // 9. If function is null:
    if (!default_policy->has_create_parser_options()) {
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
    auto policy_value = TRY(to_optional_set_html_unsafe_options(vm, TRY(default_policy->get_parser_options_policy_value(global, options, arguments, ThrowIfCallbackMissing::Yes))));

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

// https://w3c.github.io/trusted-types/dist/spec/#create-a-trusted-type-algorithm
TrustedTypesVariants TrustedTypePolicy::create_a_trusted_type(JS::VM& vm, TrustedTypeName trusted_type_name, Utf16String const& value, GC::RootVector<JS::Value> const& arguments)
{
    // 1. Let policyValue be the result of executing Get Trusted Type policy value with the same arguments
    // as this algorithm and additionally true as throwIfMissing.
    // 2. If the algorithm threw an error, rethrow the error and abort the following steps.
    auto const policy_value = TRY(get_trusted_type_policy_value(vm, trusted_type_name, value, arguments, ThrowIfCallbackMissing::Yes));

    // 3. Let dataString be the result of stringifying policyValue.
    Utf16String data_string;
    switch (trusted_type_name) {
    case TrustedTypeName::TrustedHTML:
    case TrustedTypeName::TrustedScript:
        data_string = TRY(WebIDL::to_utf16_string(vm, policy_value));
        break;
    case TrustedTypeName::TrustedScriptURL:
        data_string = TRY(WebIDL::to_utf16_usv_string(vm, policy_value));
        break;
    default:
        VERIFY_NOT_REACHED();
    }

    // 4. If policyValue is null or undefined, set dataString to the empty string.
    if (policy_value.is_nullish())
        data_string = ""_utf16;

    // 5. Return a new instance of an interface with a type name trustedTypeName, with its associated data value set to dataString.
    switch (trusted_type_name) {
    case TrustedTypeName::TrustedHTML:
        return TrustedType { GC::Heap::the().allocate<TrustedHTML>(move(data_string)) };
    case TrustedTypeName::TrustedScript:
        return TrustedType { GC::Heap::the().allocate<TrustedScript>(move(data_string)) };
    case TrustedTypeName::TrustedScriptURL:
        return TrustedType { GC::Heap::the().allocate<TrustedScriptURL>(move(data_string)) };
    default:
        VERIFY_NOT_REACHED();
    }
}

// https://w3c.github.io/trusted-types/dist/spec/#get-trusted-type-policy-value
WebIDL::ExceptionOr<JS::Value> TrustedTypePolicy::get_trusted_type_policy_value(JS::VM& vm, TrustedTypeName trusted_type_name, Utf16String const& value, GC::RootVector<JS::Value> const& values, ThrowIfCallbackMissing throw_if_missing)
{
    // 1. Let functionName be a function name for the given trustedTypeName, based on the following table:
    // 2. Let function be policy’s options[functionName].
    GC::Ptr<WebIDL::CallbackType> function;
    switch (trusted_type_name) {
    case TrustedTypeName::TrustedHTML:
        function = m_create_html;
        break;
    case TrustedTypeName::TrustedScript:
        function = m_create_script;
        break;
    case TrustedTypeName::TrustedScriptURL:
        function = m_create_script_url;
        break;
    default:
        VERIFY_NOT_REACHED();
    }

    // 3. If function is null, then:
    if (!function) {
        // 1. If throwIfMissing throw a TypeError.
        if (throw_if_missing == ThrowIfCallbackMissing::Yes)
            return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Trying to create a trusted type without a callback"_utf16 };

        // 2. Else return null
        return JS::js_null();
    }

    // 4. Let args be << value >>.
    GC::RootVector<JS::Value> args;
    args.append(JS::PrimitiveString::create(vm, value));

    // 5. Append each item in arguments to args.
    args.extend(values);

    // 6. Let policyValue be the result of invoking function with args and "rethrow".
    auto const policy_value = TRY(invoke_trusted_type_policy_callback(*function, args));

    // 7. Return policyValue.
    return policy_value;
}

// https://www.w3.org/TR/trusted-types/#process-value-with-a-default-policy-algorithm
WebIDL::ExceptionOr<Optional<TrustedType>> process_value_with_a_default_policy(TrustedTypeName trusted_type_name, JS::Object& global, Variant<GC::Ref<TrustedHTML>, GC::Ref<TrustedScript>, GC::Ref<TrustedScriptURL>, Utf16String> input, InjectionSink sink)
{
    auto& vm = global.vm();
    // 1. Let defaultPolicy be the value of global’s trusted type policy factory’s default policy.
    auto* global_scope = HTML::window_or_worker_global_scope_from_global_object(global);
    VERIFY(global_scope);
    auto const& default_policy = global_scope->trusted_types()->default_policy();

    // This algorithm routes a value to be assigned to an injection sink through a default policy, should one exist.
    // FIXME: Open an issue upstream. It is not immediately clear what to do if the default policy does not exist.
    // Ref: https://github.com/w3c/trusted-types/issues/595
    if (!default_policy)
        return Optional<TrustedType> {};

    // 2. Let policyValue be the result of executing Get Trusted Type policy value, with the following arguments:
    //    policy:
    //      defaultPolicy
    //    value:
    //      stringified input
    //    trustedTypeName:
    //      expectedType’s type name
    //    arguments:
    //      « trustedTypeName, sink »
    //    throwIfMissing:
    //      false
    //  3. If the algorithm threw an error, rethrow the error and abort the following steps.
    auto arguments = GC::RootVector<JS::Value> {};
    arguments.append(JS::PrimitiveString::create(vm, to_string(trusted_type_name)));
    arguments.append(JS::PrimitiveString::create(vm, to_string(sink)));
    auto policy_value = TRY(default_policy->get_trusted_type_policy_value(
        vm,
        trusted_type_name,
        input.visit(
            [](auto& value) { return value->to_string(); },
            [](Utf16String& value) { return value; }),
        arguments,
        ThrowIfCallbackMissing::No));

    //  4. If policyValue is null or undefined, return policyValue.
    if (policy_value.is_nullish())
        return Optional<TrustedType> {};

    //  5. Let dataString be the result of stringifying policyValue.
    Utf16String data_string;
    switch (trusted_type_name) {
    case TrustedTypeName::TrustedHTML:
    case TrustedTypeName::TrustedScript:
        data_string = TRY(WebIDL::to_utf16_string(vm, policy_value));
        break;
    case TrustedTypeName::TrustedScriptURL:
        data_string = TRY(WebIDL::to_utf16_usv_string(vm, policy_value));
        break;
    default:
        VERIFY_NOT_REACHED();
    }

    //  6. Return a new instance of an interface with a type name trustedTypeName, with its associated data value set to dataString.
    switch (trusted_type_name) {
    case TrustedTypeName::TrustedHTML:
        return TrustedType { GC::Heap::the().allocate<TrustedHTML>(move(data_string)) };
    case TrustedTypeName::TrustedScript:
        return TrustedType { GC::Heap::the().allocate<TrustedScript>(move(data_string)) };
    case TrustedTypeName::TrustedScriptURL:
        return TrustedType { GC::Heap::the().allocate<TrustedScriptURL>(move(data_string)) };
    }
    VERIFY_NOT_REACHED();
}

// https://www.w3.org/TR/trusted-types/#get-trusted-type-compliant-string-algorithm
WebIDL::ExceptionOr<Utf16String> get_trusted_type_compliant_string(TrustedTypeName expected_type, JS::Object& global, Variant<GC::Ref<TrustedHTML>, GC::Ref<TrustedScript>, GC::Ref<TrustedScriptURL>, Utf16String> input, InjectionSink sink, Utf16View sink_group)
{
    // 1. If input is an instance of expectedType, return stringified input and abort these steps.
    switch (expected_type) {
    case TrustedTypeName::TrustedHTML:
        if (auto* const value = input.get_pointer<GC::Ref<TrustedHTML>>(); value)
            return (*value)->to_string();
        break;
    case TrustedTypeName::TrustedScript:
        if (auto* const value = input.get_pointer<GC::Ref<TrustedScript>>(); value)
            return (*value)->to_string();
        break;
    case TrustedTypeName::TrustedScriptURL:
        if (auto* const value = input.get_pointer<GC::Ref<TrustedScriptURL>>(); value)
            return (*value)->to_string();
        break;
    }

    // 2. Let requireTrustedTypes be the result of executing Does sink type require trusted types? algorithm, passing global, sinkGroup, and true.
    auto const require_trusted_types = does_sink_require_trusted_types(global, sink_group, IncludeReportOnlyPolicies::Yes);

    // 3. If requireTrustedTypes is false, return stringified input and abort these steps.
    if (!require_trusted_types)
        return input.visit(
            [](auto const& value) {
                return value->to_string();
            },
            [](Utf16String const& value) {
                return value;
            });

    // 4. Let convertedInput be the result of executing Process value with a default policy with the same arguments as this algorithm.
    // 5. If the algorithm threw an error, rethrow the error and abort the following steps.
    auto const converted_input = TRY(process_value_with_a_default_policy(expected_type, global, input, sink));

    // 6. If convertedInput is null or undefined, execute the following steps:
    if (!converted_input.has_value()) {
        // 1. Let disposition be the result of executing Should sink type mismatch violation be blocked by Content Security Policy?
        //    algorithm, passing global, stringified input as source, sinkGroup and sink.
        auto const disposition = should_sink_type_mismatch_violation_be_blocked_by_content_security_policy(
            global,
            sink,
            sink_group,
            input.visit(
                [](auto const& value) {
                    return value->to_string();
                },
                [](Utf16String const& value) {
                    return value;
                }));

        // 2. If disposition is “Allowed”, return stringified input and abort further steps.
        if (disposition == ContentSecurityPolicy::Directives::Directive::Result::Allowed) {
            return input.visit(
                [](auto const& value) {
                    return value->to_string();
                },
                [](Utf16String const& value) {
                    return value;
                });
        }

        // 3. Throw a TypeError and abort further steps.
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, Utf16String::formatted("Sink {} of type {} requires a TrustedType to be used", to_string(sink), sink_group) };
    }

    // 7. Assert: convertedInput is an instance of expectedType.
    // 8. Return stringified convertedInput.
    VERIFY(converted_input.has_value());
    return converted_input.value().visit([&]<typename Type>(Type const& trusted_type) {
        switch (expected_type) {
        case TrustedTypeName::TrustedHTML:
            VERIFY(IsSame<Type, GC::Ref<TrustedHTML>>);
            break;
        case TrustedTypeName::TrustedScript:
            VERIFY(IsSame<Type, GC::Ref<TrustedScript>>);
            break;
        case TrustedTypeName::TrustedScriptURL:
            VERIFY(IsSame<Type, GC::Ref<TrustedScriptURL>>);
            break;
        }
        return trusted_type->to_string();
    });
}

// https://w3c.github.io/trusted-types/dist/spec/#validate-attribute-mutation
WebIDL::ExceptionOr<Utf16String> get_trusted_types_compliant_attribute_value(Utf16FlyString const& attribute_name, Optional<Utf16FlyString> attribute_ns, DOM::Element const& element, Variant<GC::Ref<TrustedHTML>, GC::Ref<TrustedScript>, GC::Ref<TrustedScriptURL>, Utf16String> const& new_value)
{
    // 1. If attributeNs is the empty string, set attributeNs to null.
    if (attribute_ns.has_value() && attribute_ns.value().is_empty())
        attribute_ns.clear();

    // 2. Set attributeData to the result of Get Trusted Type data for attribute algorithm, with the following arguments:
    //    element
    //    attributeName
    //    attributeNs
    auto const attribute_data = get_trusted_type_data_for_attribute(
        element_interface(
            element.local_name(),
            element.namespace_uri().value_or(Namespace::HTML)),
        attribute_name,
        attribute_ns);

    // 3. If attributeData is null, then:
    if (!attribute_data.has_value()) {
        // 1. If newValue is a string, return newValue.
        if (new_value.has<Utf16String>())
            return new_value.get<Utf16String>();

        // 2. Assert: newValue is TrustedHTML or TrustedScript or TrustedScriptURL.
        VERIFY(new_value.has<GC::Ref<TrustedHTML>>() || new_value.has<GC::Ref<TrustedScript>>() || new_value.has<GC::Ref<TrustedScriptURL>>());

        // 3. Return value’s associated data.
        // FIXME: This is badly worded in the spec it should say "Return stringified newvalues's"
        return new_value.downcast<TrustedType>().visit([](auto& value) { return value->to_string(); });
    }

    // 4. Let expectedType be the value of the fourth member of attributeData.
    auto const expected_type = attribute_data->trusted_type;

    // 5. Let sink be the value of the fifth member of attributeData.
    auto const sink = attribute_data->sink;

    // 6. Return the result of executing Get Trusted Type compliant string with the following arguments:
    //      expectedType
    //      newValue as input
    //      element’s node document’s relevant global object as global
    //      sink
    //      'script' as sinkGroup
    return get_trusted_type_compliant_string(
        expected_type,
        HTML::relevant_global_object(element.document()),
        new_value,
        sink,
        Script.view());
}

ElementInterface element_interface(Utf16FlyString const& local_name, Utf16FlyString const& element_ns)
{
    // FIXME: We don't have a method in ElementFactory that can give us the interface name but these are all the cases
    // we care about in the table in get_trusted_type_data_for_attribute function
    if (local_name == HTML::TagNames::iframe && element_ns == Namespace::HTML)
        return { "HTMLIFrameElement"_utf16, element_ns };
    if (local_name == HTML::TagNames::script && element_ns == Namespace::HTML)
        return { "HTMLScriptElement"_utf16, element_ns };
    if (local_name == SVG::TagNames::script && element_ns == Namespace::SVG)
        return { "SVGScriptElement"_utf16, element_ns };

    return { "Element"_utf16, element_ns };
}

}
