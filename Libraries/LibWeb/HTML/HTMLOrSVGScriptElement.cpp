/*
 * Copyright (c) 2018-2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, networkException <networkexception@serenityos.org>
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/Utf16StringBuilder.h>
#include <LibTextCodec/Decoder.h>
#include <LibWeb/ContentSecurityPolicy/BlockingAlgorithms.h>
#include <LibWeb/ContentSecurityPolicy/Directives/HTMLIntegration.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/HTML/AttributeNames.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/HTMLOrSVGScriptElement.h>
#include <LibWeb/HTML/HTMLScriptElement.h>
#include <LibWeb/HTML/Scripting/ClassicScript.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/Fetching.h>
#include <LibWeb/HTML/Scripting/ImportMapParseResult.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/SVG/SVGScriptElement.h>
#include <LibWeb/SVG/SVGUseElement.h>
#include <LibWeb/TrustedTypes/RequireTrustedTypesForDirective.h>
#include <LibWeb/TrustedTypes/TrustedTypePolicy.h>
#include <LibWebCommon/Infra/CharacterTypes.h>
#include <LibWebCommon/Infra/Strings.h>
#include <LibWebCommon/MimeSniff/MimeType.h>

namespace Web::HTML {

static GC::Ref<DOM::Event> create_event_for_element(DOM::Element& element, Utf16FlyString const& event_name)
{
    return DOM::Event::create(event_name, HighResolutionTime::current_high_resolution_time(relevant_global_object(element)));
}

static bool is_in_a_use_element_shadow_tree(DOM::Node const& node)
{
    for (auto const* shadow_root = as_if<DOM::ShadowRoot>(node.root()); shadow_root && shadow_root->host(); shadow_root = as_if<DOM::ShadowRoot>(shadow_root->host()->root())) {
        if (is<SVG::SVGUseElement>(*shadow_root->host()))
            return true;
    }
    return false;
}

void HTMLOrSVGScriptElement::visit_script_element_edges(GC::Cell::Visitor& visitor)
{
    m_result.visit(
        [](ResultState::Uninitialized) { },
        [](ResultState::Null) { },
        [&]<typename T>(GC::Ref<T> result) { visitor.visit(result); });
    visitor.visit(m_parser_document);
    visitor.visit(m_preparation_time_document);
}

void HTMLOrSVGScriptElement::script_element_attribute_changed(Utf16FlyString const& name)
{
    // https://html.spec.whatwg.org/multipage/scripting.html#script-processing-model:script-force-async
    // When an async attribute is added to a script element el, the user agent must set el's force async to false.
    if (name == HTML::AttributeNames::async)
        m_force_async = false;
}

// https://www.w3.org/TR/trusted-types/#prepare-script-text
WebIDL::ExceptionOr<void> HTMLOrSVGScriptElement::prepare_script_text()
{
    auto& element = as_element();

    // 1. Let sink be "HTMLScriptElement text" if script is an HTMLScriptElement; otherwise "SVGScriptElement text".
    auto sink = is<HTMLScriptElement>(element) ? TrustedTypes::InjectionSink::HTMLScriptElement_text : TrustedTypes::InjectionSink::SVGScriptElement_text;

    // 2. If script’s script text value is not equal to its child text content,
    //    set script’s script text to the result of executing get Trusted Type compliant string, with the following arguments:
    //      TrustedScriptURL as expectedType,
    //      script’s Document’s relevant global object as global,
    //      script’s child text content attribute value as input,
    //      sink,
    //      'script' as sinkGroup.
    if (m_script_text != element.child_text_content()) {
        // INTEROP: Step 2 currently names TrustedScriptURL, but script bodies use the createScript policy callback
        //          in other engines, consistently with the script text setters.
        m_script_text = TRY(TrustedTypes::get_trusted_type_compliant_string(
            TrustedTypes::TrustedTypeName::TrustedScript,
            HTML::relevant_global_object(element.document()),
            element.child_text_content(),
            sink,
            TrustedTypes::Script.view()));
    }

    return {};
}

void HTMLOrSVGScriptElement::begin_delaying_document_load_event(DOM::Document& document)
{
    // https://html.spec.whatwg.org/multipage/scripting.html#concept-script-script
    // The user agent must delay the load event of the element's node document until the script is ready.
    m_document_load_event_delayer.emplace(document);
}

// https://html.spec.whatwg.org/multipage/scripting.html#execute-the-script-block
void HTMLOrSVGScriptElement::execute_script()
{
    auto& element = as_element();

    // 1. Let document be el's node document.
    GC::Ref<DOM::Document> document = element.document();

    // 2. If el's preparation-time document is not equal to document, then return.
    if (m_preparation_time_document.ptr() != document.ptr()) {
        dbgln("HTMLScriptElement: Refusing to run script because the preparation time document is not the same as the node document.");
        return;
    }

    // https://html.spec.whatwg.org/multipage/document-lifecycle.html#read-html
    // Before any script execution occurs, the user agent must wait for scripts may run for the newly-created document to be true for document.
    VERIFY(document->ready_to_run_scripts());

    // 3. Unblock rendering on el.
    element.unblock_rendering();

    // 3. If el's result is null, then fire an event named error at el, and return.
    if (m_result.has<ResultState::Null>()) {
        dbgln("HTMLScriptElement: Refusing to run script because the element's result is null.");
        element.dispatch_event(create_event_for_element(element, HTML::EventNames::error));
        return;
    }

    // 5. If el's from an external file is true, or el's type is "module", then increment document's ignore-destructive-writes counter.
    bool incremented_destructive_writes_counter = false;
    if (m_from_an_external_file || m_script_type == ScriptType::Module) {
        document->increment_ignore_destructive_writes_counter();
        incremented_destructive_writes_counter = true;
    }

    // 5. Switch on el's type:
    // -> "classic"
    if (m_script_type == ScriptType::Classic) {
        // 1. Let oldCurrentScript be the value to which document's currentScript object was most recently set.
        auto old_current_script = document->current_script();
        // 2. If el's root is not a shadow root, then set document's currentScript attribute to el. Otherwise, set it to null.
        if (!is<DOM::ShadowRoot>(element.root()))
            document->set_current_script({}, &element);
        else
            document->set_current_script({}, nullptr);

        if (m_from_an_external_file)
            dbgln_if(HTML_SCRIPT_DEBUG, "HTMLScriptElement: Running script {}", source_attribute_value());
        else
            dbgln_if(HTML_SCRIPT_DEBUG, "HTMLScriptElement: Running inline script");

        // 3. Run the classic script given by el's result.
        (void)as<ClassicScript>(*m_result.get<GC::Ref<Script>>()).run();

        // 4. Set document's currentScript attribute to oldCurrentScript.
        document->set_current_script({}, old_current_script);
    }
    // -> "module"
    else if (m_script_type == ScriptType::Module) {
        // 1. Assert: document's currentScript attribute is null.
        VERIFY(document->current_script() == nullptr);

        // 2. Run the module script given by el's result.
        (void)as<ModuleScript>(*m_result.get<GC::Ref<Script>>()).run();
    }
    // -> "importmap"
    else if (m_script_type == ScriptType::ImportMap) {
        HTML::TemporaryExecutionContext execution_context { document->relevant_settings_object() };

        // 1. Register an import map given el's relevant global object and el's result.
        m_result.get<GC::Ref<ImportMapParseResult>>()->register_import_map(relevant_window(element));
    }

    // 7. Decrement the ignore-destructive-writes counter of document, if it was incremented in the earlier step.
    if (incremented_destructive_writes_counter)
        document->decrement_ignore_destructive_writes_counter();

    // 8. If el's from an external file is true, then fire an event named load at el.
    if (m_from_an_external_file)
        element.dispatch_event(create_event_for_element(element, HTML::EventNames::load));
}

// https://html.spec.whatwg.org/multipage/scripting.html#prepare-the-script-element
ScriptFetchOptions script_element_fetch_options(DOM::Element const& element, ScriptElementFetchAttributes const& attributes)
{
    // 25. Let module script credentials mode be the CORS settings attribute credentials mode for el's crossorigin content attribute.
    auto module_script_credential_mode = cors_settings_attribute_credentials_mode(attributes.crossorigin);

    // 26. Let cryptographic nonce be el's [[CryptographicNonce]] internal slot's value.
    // AD-HOC: A script element that is not nonceable gets no nonce. Content Security Policy only checks this for inline
    //         scripts, which lets dangling markup take the nonce of a following element to load an external script.
    auto cryptographic_nonce = ContentSecurityPolicy::Directives::is_element_nonceable(element) == ContentSecurityPolicy::Directives::Nonceable::Yes
        ? attributes.cryptographic_nonce
        : Utf16String {};

    // 27. If el has an integrity attribute, then let integrity metadata be that attribute's value.
    //     Otherwise, let integrity metadata be the empty string.
    auto integrity_metadata = attributes.integrity.value_or({});

    // 28. Let referrer policy be the current state of el's referrerpolicy content attribute.
    auto referrer_policy = attributes.referrer_policy;

    // 29. Let fetch priority be the current state of el's fetchpriority content attribute.
    auto fetch_priority = attributes.fetch_priority;

    // 30. Let parser metadata be "parser-inserted" if el is parser-inserted, and "not-parser-inserted" otherwise.
    auto parser_metadata = attributes.parser_inserted
        ? Fetch::Infrastructure::Request::ParserMetadata::ParserInserted
        : Fetch::Infrastructure::Request::ParserMetadata::NotParserInserted;

    // 31. Let options be a script fetch options whose cryptographic nonce is cryptographic nonce,
    //     integrity metadata is integrity metadata, parser metadata is parser metadata,
    //     credentials mode is module script credentials mode, referrer policy is referrer policy,
    //     and fetch priority is fetch priority.
    return {
        .cryptographic_nonce = move(cryptographic_nonce),
        .integrity_metadata = move(integrity_metadata),
        .parser_metadata = parser_metadata,
        .credentials_mode = module_script_credential_mode,
        .referrer_policy = referrer_policy,
        .fetch_priority = fetch_priority,
    };
}

// https://w3c.github.io/trusted-types/dist/spec/#slot-value-verification
// https://html.spec.whatwg.org/multipage/scripting.html#prepare-a-script
void HTMLOrSVGScriptElement::prepare_script()
{
    auto& element = as_element();

    // 1. If el's already started is true, then return.
    if (m_already_started)
        return;

    // AD-HOC: Scripts that a use element copies into its shadow tree are inert, matching other engines.
    //         See https://github.com/w3c/svgwg/issues/876
    if (is_in_a_use_element_shadow_tree(element))
        return;

    // 2. Let parser document be el's parser document.
    GC::Ptr<DOM::Document> parser_document = m_parser_document;

    // 3. Set el's parser document to null.
    m_parser_document = nullptr;

    // 4. If parser document is non-null and el does not have an async attribute, then set el's force async to true.
    if (parser_document && !element.has_attribute(HTML::AttributeNames::async)) {
        m_force_async = true;
    }

    // NB: The Trusted Types integration prepares the cached script text value before the HTML source-text step.
    if (prepare_script_text().is_exception())
        return;

    // 5. Let source text be el's child text content.
    // NB: The Trusted Types integration supplies this through the cached script text value.
    auto source_text = m_script_text;

    // 6. If el has no src attribute, and source text is the empty string, then return.
    if (!has_source_attribute() && source_text.is_empty()) {
        return;
    }

    // 7. If el is not connected, then return.
    if (!element.is_connected())
        return;

    // 8. If any of the following are true:
    //    - el has a type attribute whose value is the empty string;
    //    - el has no type attribute but it has a language attribute and that attribute's value is the empty string; or
    //    - el has neither a type attribute nor a language attribute
    Utf16String script_block_type;
    auto maybe_type_attribute = element.get_attribute_ns({}, HTML::AttributeNames::type);
    auto maybe_language_attribute = language_attribute_value();
    if ((maybe_type_attribute.has_value() && maybe_type_attribute->is_empty())
        || (!maybe_type_attribute.has_value() && maybe_language_attribute.has_value() && maybe_language_attribute->is_empty())
        || (!maybe_type_attribute.has_value() && !maybe_language_attribute.has_value())) {
        // then let the script block's type string for this script element be "text/javascript".
        script_block_type = "text/javascript"_utf16;
    }
    // Otherwise, if el has a type attribute,
    else if (maybe_type_attribute.has_value()) {
        // then let the script block's type string be the value of that attribute.
        script_block_type = *maybe_type_attribute;
    }
    // Otherwise, el has a non-empty language attribute;
    else if (maybe_language_attribute.has_value() && !maybe_language_attribute->is_empty()) {
        // let the script block's type string be the concatenation of "text/" and the value of el's language attribute.
        Utf16StringBuilder builder;
        builder.append("text/"_utf16);
        builder.append(*maybe_language_attribute);
        script_block_type = builder.to_string();
    }

    // 9. If the script block's type string is a JavaScript MIME type essence match, or el has a type attribute whose
    //    value with leading and trailing ASCII whitespace stripped is a JavaScript MIME type essence match,
    if (MimeSniff::is_javascript_mime_type_essence_match(script_block_type)
        || (maybe_type_attribute.has_value() && MimeSniff::is_javascript_mime_type_essence_match(maybe_type_attribute->trim(Infra::ASCII_WHITESPACE)))) {
        // then set el's type to "classic".
        m_script_type = ScriptType::Classic;
    }
    // 10. Otherwise, if the script block's type string is an ASCII case-insensitive match for the string "module",
    else if (script_block_type.equals_ignoring_ascii_case(u"module"sv)) {
        // then set el's type to "module".
        m_script_type = ScriptType::Module;
    }
    // 11. Otherwise, if the script block's type string is an ASCII case-insensitive match for the string "importmap",
    else if (script_block_type.equals_ignoring_ascii_case(u"importmap"sv)) {
        // then set el's type to "importmap".
        m_script_type = ScriptType::ImportMap;
    }
    // FIXME: 12. Otherwise, if the script block's type string is an ASCII case-insensitive match for the string "speculationrules", then set el's type to "speculationrules".
    // 13. Otherwise, return. (No script is executed, and el's type is left as null.)
    else {
        VERIFY(m_script_type == ScriptType::Null);
        return;
    }

    // 14. If parser document is non-null, then set el's parser document back to parser document and set el's force async to false.
    if (parser_document) {
        m_parser_document = parser_document;
        m_force_async = false;
    }

    // 15. Set el's already started to true.
    m_already_started = true;

    // 16. Set el's preparation-time document to its node document.
    m_preparation_time_document = &element.document();

    // 17. If parser document is non-null, and parser document is not equal to el's preparation-time document, then return.
    if (parser_document != nullptr && parser_document != m_preparation_time_document) {
        dbgln("HTMLScriptElement: Refusing to run script because the parser document is not the same as the preparation time document.");
        return;
    }

    // 18. If scripting is disabled for el, then return.
    if (element.is_scripting_disabled()) {
        dbgln("HTMLScriptElement: Refusing to run script because scripting is disabled.");
        return;
    }

    // 19. If el has a nomodule content attribute and its type is "classic", then return.
    if (m_script_type == ScriptType::Classic && has_nomodule_attribute()) {
        dbgln("HTMLScriptElement: Refusing to run classic script because it has the nomodule attribute.");
        return;
    }

    // FIXME: 20. Let cspType be "script speculationrules" if el's type is "speculationrules"; otherwise, "script".

    // 21. If el does not have a src content attribute, and the Should element's inline behavior be blocked by Content
    //     Security Policy? algorithm returns "Blocked" when given el, cspType, and source text, then return [CSP]
    if (!has_source_attribute()
        && ContentSecurityPolicy::should_elements_inline_type_behavior_be_blocked_by_content_security_policy(element, ContentSecurityPolicy::Directives::Directive::InlineType::Script, source_text.utf16_view()) == ContentSecurityPolicy::Directives::Directive::Result::Blocked) {
        dbgln("HTMLScriptElement: Refusing to run inline script because it violates the Content Security Policy.");
        return;
    }

    // 22. If el has an event attribute and a for attribute, and el's type is "classic", then:
    if (m_script_type == ScriptType::Classic && event_attribute_value().has_value() && for_attribute_value().has_value()) {
        // 1. Let for be the value of el's' for attribute.
        auto for_ = for_attribute_value().value_or({});

        // 2. Let event be the value of el's event attribute.
        auto event = event_attribute_value().value_or({});

        // 3. Strip leading and trailing ASCII whitespace from event and for.
        for_ = for_.trim_ascii_whitespace();
        event = event.trim_ascii_whitespace();

        // 4. If for is not an ASCII case-insensitive match for the string "window", then return.
        if (!for_.equals_ignoring_ascii_case(u"window"sv)) {
            dbgln("HTMLScriptElement: Refusing to run classic script because the provided 'for' attribute is not equal to 'window'");
            return;
        }

        // 5. If event is not an ASCII case-insensitive match for either the string "onload" or the string "onload()", then return.
        if (!event.equals_ignoring_ascii_case(u"onload"sv)
            && !event.equals_ignoring_ascii_case(u"onload()"sv)) {
            dbgln("HTMLScriptElement: Refusing to run classic script because the provided 'event' attribute is not equal to 'onload' or 'onload()'");
            return;
        }
    }

    // 23. If el has a charset attribute, then let encoding be the result of getting an encoding from the value of the charset attribute.
    //     If el does not have a charset attribute, or if getting an encoding failed, then let encoding be el's node document's the encoding.
    Optional<Utf16String> encoding;

    if (auto charset_attribute = charset_attribute_value(); charset_attribute.has_value()) {
        auto charset = TextCodec::get_standardized_encoding(*charset_attribute);
        if (charset.has_value())
            encoding = Utf16String::from_ascii_without_validation(charset->bytes());
    }

    if (!encoding.has_value()) {
        encoding = element.document().encoding_or_default();
    }

    VERIFY(encoding.has_value());

    auto fetch_attributes = this->fetch_attributes();

    // 24. Let classic script CORS setting be the current state of el's crossorigin content attribute.
    auto classic_script_cors_setting = fetch_attributes.crossorigin;

    // 25-31. Let options be a script fetch options for el.
    auto options = script_element_fetch_options(element, fetch_attributes);

    // 32. Let settings object be el's node document's relevant settings object.
    auto& settings_object = element.document().relevant_settings_object();

    // 33. If el has a src content attribute, then:
    if (has_source_attribute()) {
        // 1. If el's type is "importmap" or "speculationrules", then:
        // FIXME: Add "speculationrules" support.
        if (m_script_type == ScriptType::ImportMap) {
            // then queue an element task on the DOM manipulation task source given el to fire an event named error at el, and return.
            element.queue_an_element_task(HTML::Task::Source::DOMManipulation, [&element] {
                element.dispatch_event(create_event_for_element(element, HTML::EventNames::error));
            });
            return;
        }

        // 2. Let src be the value of el's src attribute.
        auto src = source_attribute_value();

        // 3. If src is the empty string, then queue an element task on the DOM manipulation task source given el to fire an event named error at el, and return.
        if (src.is_empty()) {
            dbgln("HTMLScriptElement: Refusing to run script because the src attribute is empty.");
            element.queue_an_element_task(HTML::Task::Source::DOMManipulation, [&element] {
                element.dispatch_event(create_event_for_element(element, HTML::EventNames::error));
            });
            return;
        }

        // 4. Set el's from an external file to true.
        m_from_an_external_file = true;

        // 5. Let url be the result of encoding-parsing a URL given src, relative to el's node document.
        auto url = element.document().encoding_parse_url(src);

        // 6. If url is failure, then queue an element task on the DOM manipulation task source given el to fire an event named error at el, and return.
        if (!url.has_value()) {
            dbgln("HTMLScriptElement: Refusing to run script because the src URL '{}' is invalid.", url);
            element.queue_an_element_task(HTML::Task::Source::DOMManipulation, [&element] {
                element.dispatch_event(create_event_for_element(element, HTML::EventNames::error));
            });
            return;
        }

        // 7. If el is potentially render-blocking, then block rendering on el.
        if (element.is_potentially_render_blocking()) {
            element.block_rendering();
        }

        // 8. Set el's delaying the load event to true.
        begin_delaying_document_load_event(*m_preparation_time_document);

        // 9. If el is currently render-blocking, then set options's render-blocking to true.
        if (element.document().is_render_blocking_element(element))
            options.render_blocking = true;

        // 10. Let onComplete given result be the following steps:
        OnFetchScriptComplete on_complete = create_on_fetch_script_complete(element.heap(), [this](auto result) {
            // 1. Mark as ready el given result.
            if (result)
                mark_as_ready(Result { *result });
            else
                mark_as_ready(ResultState::Null {});
        });

        // 11. Switch on el's type:
        // -> "classic"
        if (m_script_type == ScriptType::Classic) {
            // Fetch a classic script given url, settings object, options, classic script CORS setting, encoding, and onComplete.
            fetch_classic_script(relevant_realm(element), *url, settings_object, move(options), classic_script_cors_setting, encoding.release_value(), on_complete);
        }
        // -> "module"
        else if (m_script_type == ScriptType::Module) {
            // If el does not have an integrity attribute, then set options's integrity metadata to the result of resolving a module integrity metadata with url and settings object.
            if (!fetch_attributes.integrity.has_value())
                options.integrity_metadata = resolve_a_module_integrity_metadata(*url, settings_object);

            // AD-HOC: Queue an element task on the networking task source to run the onComplete steps
            // This resolves an edge case where mark_as_ready() can execute before we set
            // m_steps_to_run_when_the_result_is_ready
            // See https://github.com/whatwg/html/issues/12073
            auto on_complete = create_on_fetch_script_complete(element.heap(), [this, &element](auto result) {
                element.queue_an_element_task(Task::Source::Networking, [this, result = move(result)] {
                    if (!result)
                        mark_as_ready(ResultState::Null {});
                    else
                        mark_as_ready(Result(*result));
                });
            });

            // Fetch an external module script graph given url, settings object, options, and onComplete.
            fetch_external_module_script_graph(HTML::relevant_realm(element), *url, settings_object, options, on_complete);
        }
    }

    // 34. If el does not have a src content attribute:
    if (!has_source_attribute()) {
        // 1. Let base URL be el's node document's document base URL.
        auto base_url = element.document().base_url();

        // 2. Switch on el's type:
        // -> "classic"
        if (m_script_type == ScriptType::Classic) {
            // 1. Let script be the result of creating a classic script using source text, settings object, base URL, and options.
            auto script = ClassicScript::create(element.document().url().to_byte_string(), source_text, settings_object, base_url, m_source_line_number, ClassicScript::MutedErrors::No, ScriptRegistry::IsInlineSource::Yes);
            script->set_fetch_options(options);

            // 2. Mark as ready el given script.
            mark_as_ready(Result(move(script)));
        }
        // -> "module"
        else if (m_script_type == ScriptType::Module) {
            // 1. Set el's delaying the load event to true.
            begin_delaying_document_load_event(*m_preparation_time_document);

            // 2. If el is potentially render-blocking:
            if (element.is_potentially_render_blocking()) {
                // 1. Block rendering on el.
                element.block_rendering();
                // 2. Set options's render-blocking to true.
                options.render_blocking = true;
            }

            auto steps = create_on_fetch_script_complete(element.heap(), [this, &element](auto result) {
                // 1. Queue an element task on the networking task source given el to perform the following steps:
                element.queue_an_element_task(Task::Source::Networking, [this, result = move(result)] {
                    // 1. Mark as ready el given result.
                    if (!result)
                        mark_as_ready(ResultState::Null {});
                    else
                        mark_as_ready(Result(*result));
                });
            });

            // 3. Fetch an inline module script graph, given source text, base URL, settings object, options, and with the following steps given result:
            fetch_inline_module_script_graph(HTML::relevant_realm(element), element.document().url().to_byte_string(), source_text.utf16_view(), base_url, element.document().relevant_settings_object(), options, m_source_line_number, steps);
        }
        // -> "importmap"
        else if (m_script_type == ScriptType::ImportMap) {
            // 1. Let result be the result of creating an import map parse result given source text and base URL.
            auto result = ImportMapParseResult::parse(HTML::relevant_realm(element), source_text.utf16_view(), base_url);

            // 2. Mark as ready el given result.
            mark_as_ready(Result(move(result)));
        }
        // FIXME: -> "speculationrules"
    }

    // 35. If el's type is "classic" and el has a src attribute, or el's type is "module":
    if ((m_script_type == ScriptType::Classic && has_source_attribute()) || m_script_type == ScriptType::Module) {
        // 1. Assert: el's result is "uninitialized".
        // FIXME: I believe this step to be a spec bug, and it should be removed: https://github.com/whatwg/html/issues/8534

        // 2. If el has an async attribute or el's force async is true:
        if (element.has_attribute(HTML::AttributeNames::async) || m_force_async) {
            // 1. Let scripts be el's preparation-time document's set of scripts that will execute as soon as possible.
            // 2. Append el to scripts.
            m_preparation_time_document->scripts_to_execute_as_soon_as_possible().append(element);

            // 3. Set el's steps to run when the result is ready to the following:
            m_steps_to_run_when_the_result_is_ready = [this] {
                // 1. Execute the script element el.
                execute_script();

                // 2. Remove el from scripts.
                m_preparation_time_document->scripts_to_execute_as_soon_as_possible().remove_first_matching([this](auto& entry) {
                    return entry.ptr() == &as_element();
                });
            };
        }

        // 3. Otherwise, if el is not parser-inserted:
        else if (!is_parser_inserted()) {
            // 1. Let scripts be el's preparation-time document's list of scripts that will execute in order as soon as possible.
            // 2. Append el to scripts.
            m_preparation_time_document->scripts_to_execute_in_order_as_soon_as_possible().append(element);

            // 3. Set el's steps to run when the result is ready to the following:
            m_steps_to_run_when_the_result_is_ready = [this] {
                auto& scripts = m_preparation_time_document->scripts_to_execute_in_order_as_soon_as_possible();
                // 1. If scripts[0] is not el, then abort these steps.
                if (scripts[0].ptr() != &as_element())
                    return;

                // 2. While scripts is not empty, and scripts[0]'s result is not "uninitialized":
                while (!scripts.is_empty() && !as<HTMLOrSVGScriptElement>(*scripts[0]).m_result.has<ResultState::Uninitialized>()) {
                    // 1. Execute the script element scripts[0].
                    as<HTMLOrSVGScriptElement>(*scripts[0]).execute_script();

                    // 2. Remove scripts[0].
                    scripts.take_first();
                }
            };
        }

        // 4. Otherwise, if el has a defer attribute or el's type is "module":
        else if (has_defer_attribute() || m_script_type == ScriptType::Module) {
            // 1. Append el to its parser document's list of scripts that will execute when the document has finished parsing.
            m_parser_document->add_script_to_execute_when_parsing_has_finished({}, *this);

            // 2. Set el's steps to run when the result is ready to the following:
            m_steps_to_run_when_the_result_is_ready = [this] {
                // set el's ready to be parser-executed to true. (The parser will handle executing the script.)
                m_ready_to_be_parser_executed = true;
            };
        }

        // 5. Otherwise:
        else {
            // 1. Set el's parser document's pending parsing-blocking script to el.
            m_parser_document->set_pending_parsing_blocking_script(this);

            // 2. Block rendering on el.
            element.block_rendering();

            // 3. Set el's steps to run when the result is ready to the following:
            m_steps_to_run_when_the_result_is_ready = [this] {
                // set el's ready to be parser-executed to true. (The parser will handle executing the script.)
                m_ready_to_be_parser_executed = true;
            };
        }
    }

    // 36. Otherwise:
    else {
        // 1. Assert: el's result is not "uninitialized".
        VERIFY(!m_result.has<ResultState::Uninitialized>());

        // 2. If all of the following are true:
        //    - el's type is "classic";
        //    - el is parser-inserted;
        //    - el's parser document has a style sheet that is blocking scripts; and
        //    FIXME: - either the parser that created el is an XML parser, or it's an HTML parser whose script nesting level is not greater than one,
        //    then:
        if (m_script_type == ScriptType::Classic
            && is_parser_inserted()
            && m_parser_document->has_a_style_sheet_that_is_blocking_scripts()) {
            // 1. Set el's parser document's pending parsing-blocking script to el.
            m_parser_document->set_pending_parsing_blocking_script(this);

            // 2. Set el's ready to be parser-executed to true. (The parser will handle executing the script.)
            m_ready_to_be_parser_executed = true;
        }

        // 3. Otherwise,
        else {
            // immediately execute the script element el, even if other scripts are already executing.
            execute_script();
        }
    }
}

void HTMLOrSVGScriptElement::set_script_text_from_parser()
{
    // https://w3c.github.io/trusted-types/dist/spec/#setting-slot-values-from-parser
    // NOTE: The above algorithm doesn’t account for the case when the script element’s content is changed mid-parse.
    //       Implementors must ensure they protect against this case.
    //       See https://github.com/w3c/trusted-types/issues/507.
    if (m_children_changed_by_api)
        return;

    m_script_text = as_element().child_text_content();
}

void HTMLOrSVGScriptElement::ParserChildrenChangeScope::begin_changing_children(DOM::Element& element)
{
    m_script_element = as_if<HTMLOrSVGScriptElement>(element);
    if (m_script_element)
        m_was_changing_children = exchange(m_script_element->m_parser_is_changing_children, true);
}

// https://html.spec.whatwg.org/multipage/scripting.html#script-processing-model:children-changed-steps
void HTMLOrSVGScriptElement::script_element_children_changed(DOM::Node::ChildrenChangedMetadata const& metadata)
{
    if (!exchange(m_parser_is_changing_children, false))
        m_children_changed_by_api = true;

    // AD-HOC: Avoid running script on child removal, matching the behaviour of other browsers.
    //         We also do not run script on child mutation, matching the behaviour of chromium and webkit.
    //         See: https://github.com/whatwg/html/issues/12279
    if (metadata.type != DOM::Node::ChildrenChangedMetadata::Type::Inserted)
        return;

    // 1. If the script element is not connected, then return.
    if (!as_element().is_connected())
        return;

    // 2. Run the script HTML element post-connection steps, given the script element.
    script_element_post_connection();
}

// https://html.spec.whatwg.org/multipage/scripting.html#script-processing-model:html-element-post-connection-steps
void HTMLOrSVGScriptElement::script_element_post_connection()
{
    // 1. If insertedNode is parser-inserted, then return.
    if (is_parser_inserted())
        return;

    // 2. Prepare the script element given insertedNode.
    prepare_script();
}

// https://html.spec.whatwg.org/multipage/scripting.html#mark-as-ready
void HTMLOrSVGScriptElement::mark_as_ready(Result result)
{
    // 1. Set el's result to result.
    m_result = move(result);

    // 2. If el's steps to run when the result is ready are not null, then run them.
    if (m_steps_to_run_when_the_result_is_ready)
        m_steps_to_run_when_the_result_is_ready();

    // 3. Set el's steps to run when the result is ready to null.
    m_steps_to_run_when_the_result_is_ready = nullptr;

    // 4. Set el's delaying the load event to false.
    m_document_load_event_delayer.clear();

    if (m_preparation_time_document)
        m_preparation_time_document->schedule_html_parser_end_check();
}

// https://html.spec.whatwg.org/multipage/scripting.html#script-processing-model:concept-node-clone-ext
void HTMLOrSVGScriptElement::script_element_cloned(HTMLOrSVGScriptElement& copy) const
{
    // The cloning steps for script elements given node, copy, and subtree are to set copy's already started to node's already started.
    copy.m_already_started = m_already_started;
}

}

namespace Web::DOM {

template<>
HTML::HTMLOrSVGScriptElement* Node::fast_as<HTML::HTMLOrSVGScriptElement>()
{
    if (auto* html_script_element = as_if<HTML::HTMLScriptElement>(*this))
        return html_script_element;
    if (auto* svg_script_element = as_if<SVG::SVGScriptElement>(*this))
        return svg_script_element;
    return nullptr;
}

template<>
HTML::HTMLOrSVGScriptElement const* Node::fast_as<HTML::HTMLOrSVGScriptElement>() const
{
    return const_cast<Node&>(*this).fast_as<HTML::HTMLOrSVGScriptElement>();
}

}
