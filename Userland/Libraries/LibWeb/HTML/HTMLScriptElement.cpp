/*
 * Copyright (c) 2018-2021, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2022, networkException <networkexception@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/StringBuilder.h>
#include <LibTextCodec/Decoder.h>
#include <LibWeb/Bindings/HTMLScriptElementPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/HTMLScriptElement.h>
#include <LibWeb/HTML/Scripting/ClassicScript.h>
#include <LibWeb/HTML/Scripting/Fetching.h>
#include <LibWeb/HTML/Scripting/ImportMapParseResult.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Infra/CharacterTypes.h>
#include <LibWeb/Infra/Strings.h>
#include <LibWeb/MimeSniff/MimeType.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(HTMLScriptElement);

HTMLScriptElement::HTMLScriptElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLScriptElement::~HTMLScriptElement() = default;

void HTMLScriptElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLScriptElement);
}

void HTMLScriptElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    if (auto* script = m_result.get_pointer<JS::NonnullGCPtr<Script>>())
        visitor.visit(*script);
    visitor.visit(m_parser_document);
    visitor.visit(m_preparation_time_document);
}

void HTMLScriptElement::attribute_changed(FlyString const& name, Optional<String> const& value)
{
    Base::attribute_changed(name, value);

    if (name == HTML::AttributeNames::crossorigin) {
        m_crossorigin = cors_setting_attribute_from_keyword(value);
    } else if (name == HTML::AttributeNames::referrerpolicy) {
        m_referrer_policy = ReferrerPolicy::from_string(value.value_or(""_string)).value_or(ReferrerPolicy::ReferrerPolicy::EmptyString);
    }
}

void HTMLScriptElement::begin_delaying_document_load_event(DOM::Document& document)
{
    // https://html.spec.whatwg.org/multipage/scripting.html#concept-script-script
    // The user agent must delay the load event of the element's node document until the script is ready.
    m_document_load_event_delayer.emplace(document);
}

// https://html.spec.whatwg.org/multipage/scripting.html#execute-the-script-block
void HTMLScriptElement::execute_script()
{
    // https://html.spec.whatwg.org/multipage/document-lifecycle.html#read-html
    // Before any script execution occurs, the user agent must wait for scripts may run for the newly-created document to be true for document.
    if (!m_document->ready_to_run_scripts())
        main_thread_event_loop().spin_until([&] { return m_document->ready_to_run_scripts(); });

    // 1. Let document be el's node document.
    JS::NonnullGCPtr<DOM::Document> document = this->document();

    // 2. If el's preparation-time document is not equal to document, then return.
    if (m_preparation_time_document.ptr() != document.ptr()) {
        dbgln("HTMLScriptElement: Refusing to run script because the preparation time document is not the same as the node document.");
        return;
    }

    // FIXME: 3. Unblock rendering on el.

    // 3. If el's result is null, then fire an event named error at el, and return.
    if (m_result.has<ResultState::Null>()) {
        dbgln("HTMLScriptElement: Refusing to run script because the element's result is null.");
        dispatch_event(DOM::Event::create(realm(), HTML::EventNames::error));
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
        if (!is<DOM::ShadowRoot>(root()))
            document->set_current_script({}, this);
        else
            document->set_current_script({}, nullptr);

        if (m_from_an_external_file)
            dbgln_if(HTML_SCRIPT_DEBUG, "HTMLScriptElement: Running script {}", attribute(HTML::AttributeNames::src).value_or(String {}));
        else
            dbgln_if(HTML_SCRIPT_DEBUG, "HTMLScriptElement: Running inline script");

        // 3. Run the classic script given by el's result.
        (void)verify_cast<ClassicScript>(*m_result.get<JS::NonnullGCPtr<Script>>()).run();

        // 4. Set document's currentScript attribute to oldCurrentScript.
        document->set_current_script({}, old_current_script);
    }
    // -> "module"
    else if (m_script_type == ScriptType::Module) {
        // 1. Assert: document's currentScript attribute is null.
        VERIFY(document->current_script() == nullptr);

        // 2. Run the module script given by el's result.
        (void)verify_cast<JavaScriptModuleScript>(*m_result.get<JS::NonnullGCPtr<Script>>()).run();
    }
    // -> "importmap"
    else if (m_script_type == ScriptType::ImportMap) {
        // 1. Register an import map given el's relevant global object and el's result.
        m_result.get<JS::NonnullGCPtr<ImportMapParseResult>>()->register_import_map(verify_cast<Window>(relevant_global_object(*this)));
    }

    // 7. Decrement the ignore-destructive-writes counter of document, if it was incremented in the earlier step.
    if (incremented_destructive_writes_counter)
        document->decrement_ignore_destructive_writes_counter();

    // 8. If el's from an external file is true, then fire an event named load at el.
    if (m_from_an_external_file)
        dispatch_event(DOM::Event::create(realm(), HTML::EventNames::load));
}

// https://html.spec.whatwg.org/multipage/scripting.html#prepare-a-script
void HTMLScriptElement::prepare_script()
{
    // 1. If el's already started is true, then return.
    if (m_already_started) {
        dbgln("HTMLScriptElement: Refusing to run script because it has already started.");
        return;
    }

    // 2. Let parser document be el's parser document.
    JS::GCPtr<DOM::Document> parser_document = m_parser_document;

    // 3. Set el's parser document to null.
    m_parser_document = nullptr;

    // 4. If parser document is non-null and el does not have an async attribute, then set el's force async to true.
    if (parser_document && !has_attribute(HTML::AttributeNames::async)) {
        m_force_async = true;
    }

    // 5. Let source text be el's child text content.
    auto source_text = child_text_content();

    // 6. If el has no src attribute, and source text is the empty string, then return.
    if (!has_attribute(HTML::AttributeNames::src) && source_text.is_empty()) {
        return;
    }

    // 7. If el is not connected, then return.
    if (!is_connected()) {
        dbgln("HTMLScriptElement: Refusing to run script because the element is not connected.");
        return;
    }

    // 8. If any of the following are true:
    //    - el has a type attribute whose value is the empty string;
    //    - el has no type attribute but it has a language attribute and that attribute's value is the empty string; or
    //    - el has neither a type attribute nor a language attribute
    String script_block_type;
    auto maybe_type_attribute = attribute(HTML::AttributeNames::type);
    auto maybe_language_attribute = attribute(HTML::AttributeNames::language);
    if ((maybe_type_attribute.has_value() && maybe_type_attribute->is_empty())
        || (!maybe_type_attribute.has_value() && maybe_language_attribute.has_value() && maybe_language_attribute->is_empty())
        || (!maybe_type_attribute.has_value() && !maybe_language_attribute.has_value())) {
        // then let the script block's type string for this script element be "text/javascript".
        script_block_type = "text/javascript"_string;
    }
    // Otherwise, if el has a type attribute,
    else if (maybe_type_attribute.has_value()) {
        // then let the script block's type string be the value of that attribute with leading and trailing ASCII whitespace stripped.
        script_block_type = MUST(maybe_type_attribute->trim(Infra::ASCII_WHITESPACE));
    }
    // Otherwise, el has a non-empty language attribute;
    else if (maybe_language_attribute.has_value() && !maybe_language_attribute->is_empty()) {
        // let the script block's type string be the concatenation of "text/" and the value of el's language attribute.
        script_block_type = MUST(String::formatted("text/{}", maybe_language_attribute.value()));
    }

    // 9. If the script block's type string is a JavaScript MIME type essence match,
    if (MimeSniff::is_javascript_mime_type_essence_match(MUST(script_block_type.trim(Infra::ASCII_WHITESPACE)))) {
        // then set el's type to "classic".
        m_script_type = ScriptType::Classic;
    }
    // 10. Otherwise, if the script block's type string is an ASCII case-insensitive match for the string "module",
    else if (Infra::is_ascii_case_insensitive_match(script_block_type, "module"sv)) {
        // then set el's type to "module".
        m_script_type = ScriptType::Module;
    }
    // 11. Otherwise, if the script block's type string is an ASCII case-insensitive match for the string "importmap",
    else if (Infra::is_ascii_case_insensitive_match(script_block_type, "importmap"sv)) {
        // then set el's type to "importmap".
        m_script_type = ScriptType::ImportMap;
    }
    // 12. Otherwise, return. (No script is executed, and el's type is left as null.)
    else {
        VERIFY(m_script_type == ScriptType::Null);
        return;
    }

    // 13. If parser document is non-null, then set el's parser document back to parser document and set el's force async to false.
    if (parser_document) {
        m_parser_document = parser_document;
        m_force_async = false;
    }

    // 14. Set el's already started to true.
    m_already_started = true;

    // 15. Set el's preparation-time document to its node document.
    m_preparation_time_document = &document();

    // 16. If parser document is non-null, and parser document is not equal to el's preparation-time document, then return.
    if (parser_document != nullptr && parser_document != m_preparation_time_document) {
        dbgln("HTMLScriptElement: Refusing to run script because the parser document is not the same as the preparation time document.");
        return;
    }

    // 17. If scripting is disabled for el, then return.
    if (is_scripting_disabled()) {
        dbgln("HTMLScriptElement: Refusing to run script because scripting is disabled.");
        return;
    }

    // 18. If el has a nomodule content attribute and its type is "classic", then return.
    if (m_script_type == ScriptType::Classic && has_attribute(HTML::AttributeNames::nomodule)) {
        dbgln("HTMLScriptElement: Refusing to run classic script because it has the nomodule attribute.");
        return;
    }

    // FIXME: 19. If el does not have a src content attribute, and the Should element's inline behavior be blocked by Content Security Policy?
    //            algorithm returns "Blocked" when given el, "script", and source text, then return. [CSP]

    // 20. If el has an event attribute and a for attribute, and el's type is "classic", then:
    if (m_script_type == ScriptType::Classic && has_attribute(HTML::AttributeNames::event) && has_attribute(HTML::AttributeNames::for_)) {
        // 1. Let for be the value of el's' for attribute.
        auto for_ = get_attribute_value(HTML::AttributeNames::for_);

        // 2. Let event be the value of el's event attribute.
        auto event = get_attribute_value(HTML::AttributeNames::event);

        // 3. Strip leading and trailing ASCII whitespace from event and for.
        for_ = MUST(for_.trim(Infra::ASCII_WHITESPACE));
        event = MUST(event.trim(Infra::ASCII_WHITESPACE));

        // 4. If for is not an ASCII case-insensitive match for the string "window", then return.
        if (!Infra::is_ascii_case_insensitive_match(for_, "window"sv)) {
            dbgln("HTMLScriptElement: Refusing to run classic script because the provided 'for' attribute is not equal to 'window'");
            return;
        }

        // 5. If event is not an ASCII case-insensitive match for either the string "onload" or the string "onload()", then return.
        if (!Infra::is_ascii_case_insensitive_match(event, "onload"sv)
            && !Infra::is_ascii_case_insensitive_match(event, "onload()"sv)) {
            dbgln("HTMLScriptElement: Refusing to run classic script because the provided 'event' attribute is not equal to 'onload' or 'onload()'");
            return;
        }
    }

    // 21. If el has a charset attribute, then let encoding be the result of getting an encoding from the value of the charset attribute.
    //     If el does not have a charset attribute, or if getting an encoding failed, then let encoding be el's node document's the encoding.
    Optional<String> encoding;

    if (has_attribute(HTML::AttributeNames::charset)) {
        auto charset = TextCodec::get_standardized_encoding(get_attribute_value(HTML::AttributeNames::charset));
        if (charset.has_value())
            encoding = String::from_utf8(*charset).release_value_but_fixme_should_propagate_errors();
    }

    if (!encoding.has_value()) {
        encoding = document().encoding_or_default();
    }

    VERIFY(encoding.has_value());

    // 22. Let classic script CORS setting be the current state of el's crossorigin content attribute.
    auto classic_script_cors_setting = m_crossorigin;

    // 23. Let module script credentials mode be the CORS settings attribute credentials mode for el's crossorigin content attribute.
    auto module_script_credential_mode = cors_settings_attribute_credentials_mode(m_crossorigin);

    // FIXME: 24. Let cryptographic nonce be el's [[CryptographicNonce]] internal slot's value.

    // 25. If el has an integrity attribute, then let integrity metadata be that attribute's value.
    //     Otherwise, let integrity metadata be the empty string.
    String integrity_metadata;
    if (auto maybe_integrity = attribute(HTML::AttributeNames::integrity); maybe_integrity.has_value()) {
        integrity_metadata = *maybe_integrity;
    }

    // 26. Let referrer policy be the current state of el's referrerpolicy content attribute.
    auto referrer_policy = m_referrer_policy;

    // 27. Let fetch priority be the current state of el's fetchpriority content attribute.
    auto fetch_priority = Fetch::Infrastructure::request_priority_from_string(get_attribute_value(HTML::AttributeNames::fetchpriority)).value_or(Fetch::Infrastructure::Request::Priority::Auto);

    // 28. Let parser metadata be "parser-inserted" if el is parser-inserted, and "not-parser-inserted" otherwise.
    auto parser_metadata = is_parser_inserted()
        ? Fetch::Infrastructure::Request::ParserMetadata::ParserInserted
        : Fetch::Infrastructure::Request::ParserMetadata::NotParserInserted;

    // 29. Let options be a script fetch options whose cryptographic nonce is cryptographic nonce,
    //     integrity metadata is integrity metadata, parser metadata is parser metadata,
    //     credentials mode is module script credentials mode, referrer policy is referrer policy,
    //     and fetch priority is fetch priority.
    ScriptFetchOptions options {
        .cryptographic_nonce = {}, // FIXME
        .integrity_metadata = move(integrity_metadata),
        .parser_metadata = parser_metadata,
        .credentials_mode = module_script_credential_mode,
        .referrer_policy = move(referrer_policy),
        .fetch_priority = move(fetch_priority),
    };

    // 30. Let settings object be el's node document's relevant settings object.
    auto& settings_object = document().relevant_settings_object();

    // 31. If el has a src content attribute, then:
    if (has_attribute(HTML::AttributeNames::src)) {
        // 1. If el's type is "importmap",
        if (m_script_type == ScriptType::ImportMap) {
            // then queue an element task on the DOM manipulation task source given el to fire an event named error at el, and return.
            queue_an_element_task(HTML::Task::Source::DOMManipulation, [this] {
                dispatch_event(DOM::Event::create(realm(), HTML::EventNames::error));
            });
            return;
        }

        // 2. Let src be the value of el's src attribute.
        auto src = get_attribute_value(HTML::AttributeNames::src);

        // 3. If src is the empty string, then queue an element task on the DOM manipulation task source given el to fire an event named error at el, and return.
        if (src.is_empty()) {
            dbgln("HTMLScriptElement: Refusing to run script because the src attribute is empty.");
            queue_an_element_task(HTML::Task::Source::DOMManipulation, [this] {
                dispatch_event(DOM::Event::create(realm(), HTML::EventNames::error));
            });
            return;
        }

        // 4. Set el's from an external file to true.
        m_from_an_external_file = true;

        // 5. Parse src relative to el's node document.
        auto url = document().parse_url(src);

        // 6. If the previous step failed, then queue an element task on the DOM manipulation task source given el to fire an event named error at el, and return. Otherwise, let url be the resulting URL record.
        if (!url.is_valid()) {
            dbgln("HTMLScriptElement: Refusing to run script because the src URL '{}' is invalid.", url);
            queue_an_element_task(HTML::Task::Source::DOMManipulation, [this] {
                dispatch_event(DOM::Event::create(realm(), HTML::EventNames::error));
            });
            return;
        }

        // FIXME: 7. If el is potentially render-blocking, then block rendering on el.

        // 8. Set el's delaying the load event to true.
        begin_delaying_document_load_event(*m_preparation_time_document);

        // FIXME: 9. If el is currently render-blocking, then set options's render-blocking to true.

        // 10. Let onComplete given result be the following steps:
        OnFetchScriptComplete on_complete = create_on_fetch_script_complete(heap(), [this](auto result) {
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
            fetch_classic_script(*this, url, settings_object, move(options), classic_script_cors_setting, encoding.release_value(), on_complete).release_value_but_fixme_should_propagate_errors();
        }
        // -> "module"
        else if (m_script_type == ScriptType::Module) {
            // Fetch an external module script graph given url, settings object, options, and onComplete.
            fetch_external_module_script_graph(realm(), url, settings_object, options, on_complete);
        }
    }

    // 32. If el does not have a src content attribute:
    if (!has_attribute(HTML::AttributeNames::src)) {
        // Let base URL be el's node document's document base URL.
        auto base_url = document().base_url();

        // 2. Switch on el's type:
        // -> "classic"
        if (m_script_type == ScriptType::Classic) {
            // 1. Let script be the result of creating a classic script using source text, settings object, base URL, and options.
            // FIXME: Pass options.
            auto script = ClassicScript::create(m_document->url().to_byte_string(), source_text, settings_object, base_url, m_source_line_number);

            // 2. Mark as ready el given script.
            mark_as_ready(Result(move(script)));
        }
        // -> "module"
        else if (m_script_type == ScriptType::Module) {
            // 1. Set el's delaying the load event to true.
            begin_delaying_document_load_event(*m_preparation_time_document);

            auto steps = create_on_fetch_script_complete(heap(), [this](auto result) {
                // 1. Mark as ready el given result.
                if (!result)
                    mark_as_ready(ResultState::Null {});
                else
                    mark_as_ready(Result(*result));
            });

            // 2. Fetch an inline module script graph, given source text, base URL, settings object, options, and with the following steps given result:
            // FIXME: Pass options
            fetch_inline_module_script_graph(realm(), m_document->url().to_byte_string(), source_text.to_byte_string(), base_url, document().relevant_settings_object(), steps);
        }
        // -> "importmap"
        else if (m_script_type == ScriptType::ImportMap) {
            // FIXME: need to check if relevant global object is a Window - is this correct?
            auto& global = relevant_global_object(*this);

            // 1. If el's relevant global object's import maps allowed is false, then queue an element task on the DOM manipulation task source given el to fire an event named error at el, and return.
            if (is<Window>(global) && !verify_cast<Window>(global).import_maps_allowed()) {
                queue_an_element_task(HTML::Task::Source::DOMManipulation, [this] {
                    dispatch_event(DOM::Event::create(realm(), HTML::EventNames::error));
                });
                return;
            }

            // 2. Set el's relevant global object's import maps allowed to false.
            if (is<Window>(global))
                verify_cast<Window>(global).set_import_maps_allowed(false);

            // 3. Let result be the result of creating an import map parse result given source text and base URL.
            auto result = ImportMapParseResult::create(realm(), source_text.to_byte_string(), base_url);

            // 4. Mark as ready el given result.
            mark_as_ready(Result(move(result)));
        }
    }

    // 33. If el's type is "classic" and el has a src attribute, or el's type is "module":
    if ((m_script_type == ScriptType::Classic && has_attribute(HTML::AttributeNames::src)) || m_script_type == ScriptType::Module) {
        // 1. Assert: el's result is "uninitialized".
        // FIXME: I believe this step to be a spec bug, and it should be removed: https://github.com/whatwg/html/issues/8534

        // 2. If el has an async attribute or el's force async is true:
        if (has_attribute(HTML::AttributeNames::async) || m_force_async) {
            // 1. Let scripts be el's preparation-time document's set of scripts that will execute as soon as possible.
            // 2. Append el to scripts.
            m_preparation_time_document->scripts_to_execute_as_soon_as_possible().append(*this);

            // 3. Set el's steps to run when the result is ready to the following:
            m_steps_to_run_when_the_result_is_ready = [this] {
                // 1. Execute the script element el.
                execute_script();

                // 2. Remove el from scripts.
                m_preparation_time_document->scripts_to_execute_as_soon_as_possible().remove_first_matching([this](auto& entry) {
                    return entry.ptr() == this;
                });
            };
        }

        // 3. Otherwise, if el is not parser-inserted:
        else if (!is_parser_inserted()) {
            // 1. Let scripts be el's preparation-time document's list of scripts that will execute in order as soon as possible.
            // 2. Append el to scripts.
            m_preparation_time_document->scripts_to_execute_in_order_as_soon_as_possible().append(*this);

            // 3. Set el's steps to run when the result is ready to the following:
            m_steps_to_run_when_the_result_is_ready = [this] {
                auto& scripts = m_preparation_time_document->scripts_to_execute_in_order_as_soon_as_possible();
                // 1. If scripts[0] is not el, then abort these steps.
                if (scripts[0] != this)
                    return;

                // 2. While scripts is not empty, and scripts[0]'s result is not "uninitialized":
                while (!scripts.is_empty() && !scripts[0]->m_result.has<ResultState::Uninitialized>()) {
                    // 1. Execute the script element scripts[0].
                    scripts[0]->execute_script();

                    // 2. Remove scripts[0].
                    scripts.take_first();
                }
            };
        }

        // 4. Otherwise, if el has a defer attribute or el's type is "module":
        else if (has_attribute(HTML::AttributeNames::defer) || m_script_type == ScriptType::Module) {
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
            m_parser_document->set_pending_parsing_blocking_script({}, this);

            // FIXME: 2. Block rendering on el.

            // 3. Set el's steps to run when the result is ready to the following:
            m_steps_to_run_when_the_result_is_ready = [this] {
                // set el's ready to be parser-executed to true. (The parser will handle executing the script.)
                m_ready_to_be_parser_executed = true;
            };
        }
    }

    // 34. Otherwise:
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
            m_parser_document->set_pending_parsing_blocking_script({}, this);

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

void HTMLScriptElement::inserted()
{
    if (!is_parser_inserted()) {
        // FIXME: Only do this if the element was previously not connected.
        if (is_connected()) {
            prepare_script();
        }
    }
    HTMLElement::inserted();
}

// https://html.spec.whatwg.org/multipage/scripting.html#mark-as-ready
void HTMLScriptElement::mark_as_ready(Result result)
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
}

void HTMLScriptElement::unmark_as_already_started(Badge<DOM::Range>)
{
    m_already_started = false;
}

void HTMLScriptElement::unmark_as_parser_inserted(Badge<DOM::Range>)
{
    m_parser_document = nullptr;
}

// https://html.spec.whatwg.org/multipage/scripting.html#dom-script-async
bool HTMLScriptElement::async() const
{
    // 1. If this's force async is true, then return true.
    if (m_force_async)
        return true;

    // 2. If this's async content attribute is present, then return true.
    if (has_attribute(HTML::AttributeNames::async))
        return true;

    // 3. Return false.
    return false;
}

void HTMLScriptElement::set_async(bool async)
{
    // 1. Set this's force async to false.
    m_force_async = false;

    // 2. If the given value is true, then set this's async content attribute to the empty string.
    if (async) {
        MUST(set_attribute(HTML::AttributeNames::async, ""_string));
    }
    // 3. Otherwise, remove this's async content attribute.
    else {
        remove_attribute(HTML::AttributeNames::async);
    }
}

}
