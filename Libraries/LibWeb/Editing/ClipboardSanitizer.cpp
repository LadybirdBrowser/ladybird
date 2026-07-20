/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/RootVector.h>
#include <LibWeb/DOM/Attr.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/DocumentFragment.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Range.h>
#include <LibWeb/Editing/ClipboardSanitizer.h>
#include <LibWeb/HTML/AttributeNames.h>
#include <LibWeb/HTML/Parser/ParserScriptingMode.h>
#include <LibWeb/HTML/TagNames.h>
#include <LibWeb/HTML/XMLSerializer.h>

namespace Web::Editing {

static bool is_active_element(DOM::Element const& element)
{
    auto const& local_name = element.local_name();
    return local_name.is_one_of(
        HTML::TagNames::base,
        HTML::TagNames::embed,
        HTML::TagNames::iframe,
        HTML::TagNames::link,
        HTML::TagNames::meta,
        HTML::TagNames::object,
        HTML::TagNames::script,
        HTML::TagNames::style,
        HTML::TagNames::template_);
}

static bool is_url_attribute(DOM::Attr const& attribute)
{
    return attribute.local_name().is_one_of(
        HTML::AttributeNames::action,
        HTML::AttributeNames::background,
        HTML::AttributeNames::cite,
        HTML::AttributeNames::data,
        HTML::AttributeNames::formaction,
        HTML::AttributeNames::href,
        HTML::AttributeNames::poster,
        HTML::AttributeNames::src);
}

static bool is_unsafe_attribute(DOM::Element const& element, DOM::Attr const& attribute)
{
    if (attribute.local_name().view().starts_with(u"on"sv) || attribute.local_name() == HTML::AttributeNames::srcdoc)
        return true;
    if (!is_url_attribute(attribute))
        return false;
    auto url = element.document().encoding_parse_url(attribute.value());
    return url.has_value() && url->scheme() == "javascript"sv;
}

WebIDL::ExceptionOr<Utf16String> sanitize_clipboard_html(DOM::Range& range, Utf16View html)
{
    // INTEROP: Blink and WebKit parse native clipboard markup in a staging document with scripting disabled before
    //          inserting it. Clipboard data is not TrustedHTML, even when it originated in another browser tab.
    auto fragment = TRY(range.create_contextual_fragment(Utf16String::from_utf16(html), HTML::ParserScriptingMode::Inert));
    GC::RootVector<GC::Ref<DOM::Element>> active_elements;
    fragment->for_each_in_subtree([&](GC::Ref<DOM::Node> node) {
        auto* element = as_if<DOM::Element>(*node);
        if (!element)
            return TraversalDecision::Continue;
        if (is_active_element(*element)) {
            active_elements.append(*element);
            return TraversalDecision::Continue;
        }

        Vector<Utf16FlyString> attributes_to_remove;
        element->for_each_attribute([&](DOM::Attr const& attribute) {
            if (is_unsafe_attribute(*element, attribute))
                attributes_to_remove.append(attribute.name());
        });
        for (auto const& attribute_name : attributes_to_remove)
            element->remove_attribute(attribute_name);
        return TraversalDecision::Continue;
    });

    for (auto& element : active_elements.in_reverse())
        element->remove();
    return fragment->serialize_fragment(HTML::RequireWellFormed::No);
}

}
