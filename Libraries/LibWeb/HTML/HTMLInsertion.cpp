/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Sanitizer.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/DocumentFragment.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/HTML/HTMLHtmlElement.h>
#include <LibWeb/HTML/HTMLInsertion.h>
#include <LibWeb/HTML/HTMLTemplateElement.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>
#include <LibWeb/HTML/SanitizerConfiguration.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/TagNames.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/TrustedTypes/TrustedHTMLParserOptions.h>
#include <LibWeb/TrustedTypes/TrustedTypePolicy.h>
#include <LibWeb/XML/XMLFragmentParser.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#target-for-html-insertion
HTMLInsertionTarget target_for_html_insertion(DOM::Element& element)
{
    // 1. If element is a template element, then return element's template contents.
    if (auto* template_element = as_if<HTMLTemplateElement>(element))
        return GC::Ref { *template_element->content() };

    // 2. Return element.
    return GC::Ref { element };
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#parent-for-html-insertion
WebIDL::ExceptionOr<Optional<HTMLInsertionTarget>> parent_for_html_insertion(DOM::Node& node)
{
    // 1. Let parent be node's parent.
    auto* parent = node.parent();

    // 2. If parent is null, then return null.
    if (!parent)
        return Optional<HTMLInsertionTarget> {};

    // 3. If parent is a template element, then throw a "HierarchyRequestError" DOMException.
    if (is<HTMLTemplateElement>(*parent))
        return WebIDL::HierarchyRequestError::create("Cannot insert HTML into the children of a template element"_utf16);

    // 4. If parent is an Element, or a DocumentFragment with a non-null host, then return parent.
    if (auto* element = as_if<DOM::Element>(*parent))
        return Optional<HTMLInsertionTarget> { GC::Ref { *element } };
    if (auto* fragment = as_if<DOM::DocumentFragment>(*parent); fragment && fragment->host())
        return Optional<HTMLInsertionTarget> { GC::Ref { *fragment } };

    // 5. Throw a "HierarchyRequestError" DOMException.
    return WebIDL::HierarchyRequestError::create("Cannot insert HTML into this parent"_utf16);
}

static GC::Ref<DOM::Node> target_node(HTMLInsertionTarget const& target)
{
    return target.visit([](auto node) -> GC::Ref<DOM::Node> { return node; });
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#set-and-filter-html
WebIDL::ExceptionOr<void> filter_and_set_html(HTMLInsertionTarget target, Utf16View html, SanitizerOptions const& options, FragmentParserMode mode)
{
    // 1. Let fragment be the result of parsing HTML for insertion given target, html, options, and mode.
    auto fragment = TRY(parse_html_for_insertion(target, html, options, mode));

    // 2. If fragment is non-null, then replace all with fragment within target.
    if (fragment)
        target_node(target)->replace_all(fragment);

    return {};
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#filter-and-pre-insert-html
WebIDL::ExceptionOr<void> filter_and_pre_insert_html(HTMLInsertionTarget parent, GC::Ptr<DOM::Node> child, Utf16View html, SanitizerOptions const& options, FragmentParserMode mode)
{
    // 1. If parent is an html element, then throw a "HierarchyRequestError" DOMException.
    if (auto element = parent.get_pointer<GC::Ref<DOM::Element>>(); element && is<HTMLHtmlElement>(**element))
        return WebIDL::HierarchyRequestError::create("Cannot insert HTML into an html element"_utf16);

    // 2. Let fragment be the result of parsing HTML for insertion given parent, html, options, and mode.
    auto fragment = TRY(parse_html_for_insertion(parent, html, options, mode));

    // 3. If fragment is non-null, then pre-insert fragment into parent before child.
    if (fragment)
        TRY(target_node(parent)->pre_insert(*fragment, child));

    return {};
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#filter-and-replace-with-html
WebIDL::ExceptionOr<void> filter_and_replace_with_html(HTMLInsertionTarget parent, DOM::Node& node, Utf16View html, SanitizerOptions const& options, FragmentParserMode mode)
{
    // 1. If parent is an html element, then throw a "HierarchyRequestError" DOMException.
    if (auto element = parent.get_pointer<GC::Ref<DOM::Element>>(); element && is<HTMLHtmlElement>(**element))
        return WebIDL::HierarchyRequestError::create("Cannot insert HTML into an html element"_utf16);

    // 2. Let fragment be the result of parsing HTML for insertion given parent, html, options, and mode.
    auto fragment = TRY(parse_html_for_insertion(parent, html, options, mode));

    // 3. If fragment is non-null, then replace node with fragment within parent.
    if (fragment)
        TRY(target_node(parent)->replace_child(*fragment, node));

    return {};
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#parse-html-for-insertion
WebIDL::ExceptionOr<GC::Ptr<DOM::DocumentFragment>> parse_html_for_insertion(HTMLInsertionTarget target, Utf16View html, SanitizerOptions const& options, FragmentParserMode mode)
{
    // 1. If mode is Safe:
    if (mode == FragmentParserMode::Safe) {
        // 1. Let context be target if target is an Element; otherwise target's host.
        GC::Ptr<DOM::Element> context = target.visit(
            [](GC::Ref<DOM::Element> element) -> GC::Ptr<DOM::Element> { return element; },
            [](GC::Ref<DOM::DocumentFragment> fragment) -> GC::Ptr<DOM::Element> { return fragment->host(); });

        // 2. Assert: context is non-null.
        VERIFY(context);

        // 3. If all of the following are true:
        //    - context's local name is "script"; and
        //    - context's namespace is the HTML namespace or the SVG namespace,
        //    then return null.
        if (context->local_name() == TagNames::script && (context->namespace_uri() == Namespace::HTML || context->namespace_uri() == Namespace::SVG))
            return nullptr;
    }

    // 2. Return the result of invoking the fragment parsing algorithm steps given target, html, options, and mode.
    return TRY(fragment_parsing_algorithm_steps(target, html, options, mode)).ptr();
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#fragment-parsing-algorithm-steps
WebIDL::ExceptionOr<GC::Ref<DOM::DocumentFragment>> fragment_parsing_algorithm_steps(HTMLInsertionTarget target, Utf16View markup, SanitizerOptions const& options, FragmentParserMode mode)
{
    // 1. If mode is Legacy and target's node document is an XML document, then return the result of invoking the XML
    //    fragment parsing algorithm given target and markup.
    // NOTE: Sanitization is not supported for XML documents.
    if (mode == FragmentParserMode::Legacy && target_node(target)->document().is_xml_document())
        return XMLFragmentParser::parse_xml_fragment(target, markup);

    // 2. Let safe be true if mode is Safe; otherwise false.
    auto safety = mode == FragmentParserMode::Safe ? SanitizerSafety::Safe : SanitizerSafety::Unsafe;

    // 3. Let sanitizerConfig be the result of getting a sanitizer config from options given options and safe.
    auto sanitizer_config = TRY(get_a_sanitizer_config_from_options(options, safety));

    // 4. Let scriptingMode be Inert.
    auto scripting_mode = ParserScriptingMode::Inert;

    // 5. Let runScripts be options's runScripts if options is a TrustedHTMLParserOptions; otherwise
    //    options["runScripts"] with default false.
    bool run_scripts = options.visit(
        [](GC::Ref<TrustedTypes::TrustedHTMLParserOptions> trusted_options) { return trusted_options->run_scripts(); },
        [](Bindings::SetHTMLUnsafeOptions const* set_html_unsafe_options) { return set_html_unsafe_options->run_scripts; },
        [](auto const*) { return false; });

    // 6. If runScripts is true:
    if (run_scripts) {
        // 1. Assert: mode is not Safe.
        VERIFY(mode != FragmentParserMode::Safe);

        // 2. Set scriptingMode to Fragment.
        scripting_mode = ParserScriptingMode::Fragment;
    }

    // 7. Let allowDeclarativeShadowRoots be false if mode is Legacy; otherwise true.
    auto allow_declarative_shadow_roots = mode == FragmentParserMode::Legacy ? HTMLParser::AllowDeclarativeShadowRoots::No : HTMLParser::AllowDeclarativeShadowRoots::Yes;

    // 8. Return the result of the HTML fragment parsing algorithm given target, markup, allowDeclarativeShadowRoots,
    //    scriptingMode, and sanitizerConfig.
    // NOTE: Scripts in the returned fragment will only execute once they are inserted into a document, and only if
    //       scriptingMode is Fragment.
    return HTMLParser::parse_html_fragment(target, markup, allow_declarative_shadow_roots, scripting_mode, FragmentFastPath::Allow, sanitizer_config.has_value() ? &*sanitizer_config : nullptr);
}

WebIDL::ExceptionOr<LegacySinkCompliantInput> get_trusted_type_compliant_input_for_legacy_sink(DOM::Node& node, Variant<GC::Ref<TrustedTypes::TrustedHTML>, Utf16String> const& value, TrustedTypes::InjectionSink sink, LegacySinkRunScripts run_scripts)
{
    // Let (compliantString, compliantOptions) be the result of invoking the get trusted type compliant input algorithm
    // with this's relevant global object, the given value, a new SetHTMLUnsafeOptions dictionary, sink, this's node
    // document's type, and false.
    Bindings::SetHTMLUnsafeOptions options;
    options.run_scripts = run_scripts == LegacySinkRunScripts::Yes;
    auto document_type = node.document().is_xml_document() ? TrustedTypes::DocumentType::XML : TrustedTypes::DocumentType::HTML;
    auto compliant_input = TRY(TrustedTypes::get_trusted_type_compliant_input(
        relevant_global_object(node),
        value,
        TrustedTypes::CompliantParserOptions<Bindings::SetHTMLUnsafeOptions> { move(options) },
        sink,
        document_type,
        TrustedTypes::OptionsFromAuthor::No));

    GC::Ptr<TrustedTypes::TrustedHTMLParserOptions> trusted_options;
    if (auto const* trusted = compliant_input.options.template get_pointer<GC::Ref<TrustedTypes::TrustedHTMLParserOptions>>())
        trusted_options = *trusted;
    return LegacySinkCompliantInput { move(compliant_input.html), trusted_options };
}

}
