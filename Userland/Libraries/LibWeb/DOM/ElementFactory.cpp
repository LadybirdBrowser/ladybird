/*
 * Copyright (c) 2018-2022, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2020-2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/ElementFactory.h>
#include <LibWeb/HTML/CustomElements/CustomElementDefinition.h>
#include <LibWeb/HTML/CustomElements/CustomElementName.h>
#include <LibWeb/HTML/HTMLAnchorElement.h>
#include <LibWeb/HTML/HTMLAreaElement.h>
#include <LibWeb/HTML/HTMLAudioElement.h>
#include <LibWeb/HTML/HTMLBRElement.h>
#include <LibWeb/HTML/HTMLBaseElement.h>
#include <LibWeb/HTML/HTMLBodyElement.h>
#include <LibWeb/HTML/HTMLButtonElement.h>
#include <LibWeb/HTML/HTMLCanvasElement.h>
#include <LibWeb/HTML/HTMLDListElement.h>
#include <LibWeb/HTML/HTMLDataElement.h>
#include <LibWeb/HTML/HTMLDataListElement.h>
#include <LibWeb/HTML/HTMLDetailsElement.h>
#include <LibWeb/HTML/HTMLDialogElement.h>
#include <LibWeb/HTML/HTMLDirectoryElement.h>
#include <LibWeb/HTML/HTMLDivElement.h>
#include <LibWeb/HTML/HTMLEmbedElement.h>
#include <LibWeb/HTML/HTMLFieldSetElement.h>
#include <LibWeb/HTML/HTMLFontElement.h>
#include <LibWeb/HTML/HTMLFormElement.h>
#include <LibWeb/HTML/HTMLFrameElement.h>
#include <LibWeb/HTML/HTMLFrameSetElement.h>
#include <LibWeb/HTML/HTMLHRElement.h>
#include <LibWeb/HTML/HTMLHeadElement.h>
#include <LibWeb/HTML/HTMLHeadingElement.h>
#include <LibWeb/HTML/HTMLHtmlElement.h>
#include <LibWeb/HTML/HTMLIFrameElement.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/HTMLLIElement.h>
#include <LibWeb/HTML/HTMLLabelElement.h>
#include <LibWeb/HTML/HTMLLegendElement.h>
#include <LibWeb/HTML/HTMLLinkElement.h>
#include <LibWeb/HTML/HTMLMapElement.h>
#include <LibWeb/HTML/HTMLMarqueeElement.h>
#include <LibWeb/HTML/HTMLMenuElement.h>
#include <LibWeb/HTML/HTMLMetaElement.h>
#include <LibWeb/HTML/HTMLMeterElement.h>
#include <LibWeb/HTML/HTMLModElement.h>
#include <LibWeb/HTML/HTMLOListElement.h>
#include <LibWeb/HTML/HTMLObjectElement.h>
#include <LibWeb/HTML/HTMLOptGroupElement.h>
#include <LibWeb/HTML/HTMLOptionElement.h>
#include <LibWeb/HTML/HTMLOutputElement.h>
#include <LibWeb/HTML/HTMLParagraphElement.h>
#include <LibWeb/HTML/HTMLParamElement.h>
#include <LibWeb/HTML/HTMLPictureElement.h>
#include <LibWeb/HTML/HTMLPreElement.h>
#include <LibWeb/HTML/HTMLProgressElement.h>
#include <LibWeb/HTML/HTMLQuoteElement.h>
#include <LibWeb/HTML/HTMLScriptElement.h>
#include <LibWeb/HTML/HTMLSelectElement.h>
#include <LibWeb/HTML/HTMLSlotElement.h>
#include <LibWeb/HTML/HTMLSourceElement.h>
#include <LibWeb/HTML/HTMLSpanElement.h>
#include <LibWeb/HTML/HTMLStyleElement.h>
#include <LibWeb/HTML/HTMLSummaryElement.h>
#include <LibWeb/HTML/HTMLTableCaptionElement.h>
#include <LibWeb/HTML/HTMLTableCellElement.h>
#include <LibWeb/HTML/HTMLTableColElement.h>
#include <LibWeb/HTML/HTMLTableElement.h>
#include <LibWeb/HTML/HTMLTableRowElement.h>
#include <LibWeb/HTML/HTMLTableSectionElement.h>
#include <LibWeb/HTML/HTMLTemplateElement.h>
#include <LibWeb/HTML/HTMLTextAreaElement.h>
#include <LibWeb/HTML/HTMLTimeElement.h>
#include <LibWeb/HTML/HTMLTitleElement.h>
#include <LibWeb/HTML/HTMLTrackElement.h>
#include <LibWeb/HTML/HTMLUListElement.h>
#include <LibWeb/HTML/HTMLUnknownElement.h>
#include <LibWeb/HTML/HTMLVideoElement.h>
#include <LibWeb/HTML/Scripting/ExceptionReporter.h>
#include <LibWeb/Infra/Strings.h>
#include <LibWeb/MathML/MathMLElement.h>
#include <LibWeb/MathML/TagNames.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/SVG/SVGAElement.h>
#include <LibWeb/SVG/SVGCircleElement.h>
#include <LibWeb/SVG/SVGClipPathElement.h>
#include <LibWeb/SVG/SVGDefsElement.h>
#include <LibWeb/SVG/SVGDescElement.h>
#include <LibWeb/SVG/SVGEllipseElement.h>
#include <LibWeb/SVG/SVGForeignObjectElement.h>
#include <LibWeb/SVG/SVGGElement.h>
#include <LibWeb/SVG/SVGLineElement.h>
#include <LibWeb/SVG/SVGLinearGradientElement.h>
#include <LibWeb/SVG/SVGMaskElement.h>
#include <LibWeb/SVG/SVGPathElement.h>
#include <LibWeb/SVG/SVGPolygonElement.h>
#include <LibWeb/SVG/SVGPolylineElement.h>
#include <LibWeb/SVG/SVGRadialGradientElement.h>
#include <LibWeb/SVG/SVGRectElement.h>
#include <LibWeb/SVG/SVGSVGElement.h>
#include <LibWeb/SVG/SVGScriptElement.h>
#include <LibWeb/SVG/SVGStopElement.h>
#include <LibWeb/SVG/SVGStyleElement.h>
#include <LibWeb/SVG/SVGSymbolElement.h>
#include <LibWeb/SVG/SVGTSpanElement.h>
#include <LibWeb/SVG/SVGTextElement.h>
#include <LibWeb/SVG/SVGTextPathElement.h>
#include <LibWeb/SVG/SVGTitleElement.h>
#include <LibWeb/SVG/SVGUseElement.h>
#include <LibWeb/SVG/TagNames.h>
#include <LibWeb/WebIDL/AbstractOperations.h>

namespace Web::DOM {

ErrorOr<FixedArray<FlyString>> valid_local_names_for_given_html_element_interface(StringView html_element_interface_name)
{
    if (html_element_interface_name == "HTMLAnchorElement"sv)
        return FixedArray<FlyString>::create({ HTML::TagNames::a });
    if (html_element_interface_name == "HTMLAreaElement"sv)
        return FixedArray<FlyString>::create({ HTML::TagNames::area });
    if (html_element_interface_name == "HTMLAudioElement"sv)
        return FixedArray<FlyString>::create({ HTML::TagNames::audio });
    if (html_element_interface_name == "HTMLBaseElement"sv)
        return FixedArray<FlyString>::create({ HTML::TagNames::base });
    if (html_element_interface_name == "HTMLBodyElement"sv)
        return FixedArray<FlyString>::create({ HTML::TagNames::body });
    if (html_element_interface_name == "HTMLBRElement"sv)
        return FixedArray<FlyString>::create({ HTML::TagNames::br });
    if (html_element_interface_name == "HTMLButtonElement"sv)
        return FixedArray<FlyString>::create({ HTML::TagNames::button });
    if (html_element_interface_name == "HTMLCanvasElement"sv)
        return FixedArray<FlyString>::create({ HTML::TagNames::canvas });
    if (html_element_interface_name == "HTMLDataElement"sv)
        return FixedArray<FlyString>::create({ HTML::TagNames::data });
    if (html_element_interface_name == "HTMLDataListElement"sv)
        return FixedArray<FlyString>::create({ HTML::TagNames::datalist });
    if (html_element_interface_name == "HTMLDetailsElement"sv)
        return FixedArray<FlyString>::create({ HTML::TagNames::details });
    if (html_element_interface_name == "HTMLDialogElement"sv)
        return FixedArray<FlyString>::create({ HTML::TagNames::dialog });
    if (html_element_interface_name == "HTMLDirectoryElement"sv)
        return FixedArray<FlyString>::create({ HTML::TagNames::dir });
    if (html_element_interface_name == "HTMLDivElement"sv)
        return FixedArray<FlyString>::create({ HTML::TagNames::div });
    if (html_element_interface_name == "HTMLDListElement"sv)
        return FixedArray<FlyString>::create({ HTML::TagNames::dl });
    if (html_element_interface_name == "HTMLEmbedElement"sv)
        return FixedArray<FlyString>::create({ HTML::TagNames::embed });
    if (html_element_interface_name == "HTMLFieldsetElement"sv)
        return FixedArray<FlyString>::create({ HTML::TagNames::fieldset });
    if (html_element_interface_name == "HTMLFontElement"sv)
        return FixedArray<FlyString>::create({ HTML::TagNames::font });
    if (html_element_interface_name == "HTMLFormElement"sv)
        return FixedArray<FlyString>::create({ HTML::TagNames::form });
    if (html_element_interface_name == "HTMLFrameElement"sv)
        return FixedArray<FlyString>::create({ HTML::TagNames::frame });
    if (html_element_interface_name == "HTMLFrameSetElement"sv)
        return FixedArray<FlyString>::create({ HTML::TagNames::frameset });
    if (html_element_interface_name == "HTMLHeadElement"sv)
        return FixedArray<FlyString>::create({ HTML::TagNames::head });
    if (html_element_interface_name == "HTMLHeadingElement"sv)
        return FixedArray<FlyString>::create({ HTML::TagNames::h1, HTML::TagNames::h2, HTML::TagNames::h3, HTML::TagNames::h4, HTML::TagNames::h5, HTML::TagNames::h6 });
    if (html_element_interface_name == "HTMLHRElement"sv)
        return FixedArray<FlyString>::create({ HTML::TagNames::hr });
    if (html_element_interface_name == "HTMLHtmlElement"sv)
        return FixedArray<FlyString>::create({ HTML::TagNames::html });
    if (html_element_interface_name == "HTMLIFrameElement"sv)
        return FixedArray<FlyString>::create({ HTML::TagNames::iframe });
    if (html_element_interface_name == "HTMLImageElement"sv)
        return FixedArray<FlyString>::create({ HTML::TagNames::img });
    if (html_element_interface_name == "HTMLInputElement"sv)
        return FixedArray<FlyString>::create({ HTML::TagNames::input });
    if (html_element_interface_name == "HTMLLabelElement"sv)
        return FixedArray<FlyString>::create({ HTML::TagNames::label });
    if (html_element_interface_name == "HTMLLIElement"sv)
        return FixedArray<FlyString>::create({ HTML::TagNames::li });
    if (html_element_interface_name == "HTMLLinkElement"sv)
        return FixedArray<FlyString>::create({ HTML::TagNames::link });
    if (html_element_interface_name == "HTMLMapElement"sv)
        return FixedArray<FlyString>::create({ HTML::TagNames::map });
    if (html_element_interface_name == "HTMLMarqueeElement"sv)
        return FixedArray<FlyString>::create({ HTML::TagNames::marquee });
    if (html_element_interface_name == "HTMLMenuElement"sv)
        return FixedArray<FlyString>::create({ HTML::TagNames::menu });
    if (html_element_interface_name == "HTMLMeterElement"sv)
        return FixedArray<FlyString>::create({ HTML::TagNames::meter });
    if (html_element_interface_name == "HTMLModElement"sv)
        return FixedArray<FlyString>::create({ HTML::TagNames::ins, HTML::TagNames::del });
    if (html_element_interface_name == "HTMLObjectElement"sv)
        return FixedArray<FlyString>::create({ HTML::TagNames::object });
    if (html_element_interface_name == "HTMLOutputElement"sv)
        return FixedArray<FlyString>::create({ HTML::TagNames::output });
    if (html_element_interface_name == "HTMLParagraphElement"sv)
        return FixedArray<FlyString>::create({ HTML::TagNames::p });
    if (html_element_interface_name == "HTMLParamElement"sv)
        return FixedArray<FlyString>::create({ HTML::TagNames::param });
    if (html_element_interface_name == "HTMLPictureElement"sv)
        return FixedArray<FlyString>::create({ HTML::TagNames::picture });
    if (html_element_interface_name == "HTMLPreElement"sv)
        return FixedArray<FlyString>::create({ HTML::TagNames::pre, HTML::TagNames::listing, HTML::TagNames::xmp });
    if (html_element_interface_name == "HTMLProgressElement"sv)
        return FixedArray<FlyString>::create({ HTML::TagNames::progress });
    if (html_element_interface_name == "HTMLQuoteElement"sv)
        return FixedArray<FlyString>::create({ HTML::TagNames::blockquote, HTML::TagNames::q });
    if (html_element_interface_name == "HTMLScriptElement"sv)
        return FixedArray<FlyString>::create({ HTML::TagNames::script });
    if (html_element_interface_name == "HTMLSelectElement"sv)
        return FixedArray<FlyString>::create({ HTML::TagNames::select });
    if (html_element_interface_name == "HTMLSlotElement"sv)
        return FixedArray<FlyString>::create({ HTML::TagNames::slot });
    if (html_element_interface_name == "HTMLSourceElement"sv)
        return FixedArray<FlyString>::create({ HTML::TagNames::source });
    if (html_element_interface_name == "HTMLSpanElement"sv)
        return FixedArray<FlyString>::create({ HTML::TagNames::span });
    if (html_element_interface_name == "HTMLStyleElement"sv)
        return FixedArray<FlyString>::create({ HTML::TagNames::style });
    if (html_element_interface_name == "HTMLTableCaptionElement"sv)
        return FixedArray<FlyString>::create({ HTML::TagNames::caption });
    if (html_element_interface_name == "HTMLTableCellElement"sv)
        return FixedArray<FlyString>::create({ HTML::TagNames::td, HTML::TagNames::th });
    if (html_element_interface_name == "HTMLTableColElement"sv)
        return FixedArray<FlyString>::create({ HTML::TagNames::colgroup, HTML::TagNames::col });
    if (html_element_interface_name == "HTMLTableElement"sv)
        return FixedArray<FlyString>::create({ HTML::TagNames::table });
    if (html_element_interface_name == "HTMLTableSectionElement"sv)
        return FixedArray<FlyString>::create({ HTML::TagNames::tbody, HTML::TagNames::thead, HTML::TagNames::tfoot });
    if (html_element_interface_name == "HTMLTemplateElement"sv)
        return FixedArray<FlyString>::create({ HTML::TagNames::template_ });
    if (html_element_interface_name == "HTMLTextAreaElement"sv)
        return FixedArray<FlyString>::create({ HTML::TagNames::textarea });
    if (html_element_interface_name == "HTMLTimeElement"sv)
        return FixedArray<FlyString>::create({ HTML::TagNames::time });
    if (html_element_interface_name == "HTMLTitleElement"sv)
        return FixedArray<FlyString>::create({ HTML::TagNames::title });
    if (html_element_interface_name == "HTMLTrackElement"sv)
        return FixedArray<FlyString>::create({ HTML::TagNames::track });
    if (html_element_interface_name == "HTMLUListElement"sv)
        return FixedArray<FlyString>::create({ HTML::TagNames::ul });
    if (html_element_interface_name == "HTMLVideoElement"sv)
        return FixedArray<FlyString>::create({ HTML::TagNames::video });
    if (html_element_interface_name == "HTMLElement"sv)
        return FixedArray<FlyString>::create({ HTML::TagNames::article, HTML::TagNames::section, HTML::TagNames::nav, HTML::TagNames::aside, HTML::TagNames::hgroup, HTML::TagNames::header, HTML::TagNames::footer, HTML::TagNames::address, HTML::TagNames::dt, HTML::TagNames::dd, HTML::TagNames::figure, HTML::TagNames::figcaption, HTML::TagNames::main, HTML::TagNames::em, HTML::TagNames::strong, HTML::TagNames::small, HTML::TagNames::s, HTML::TagNames::cite, HTML::TagNames::dfn, HTML::TagNames::abbr, HTML::TagNames::ruby, HTML::TagNames::rt, HTML::TagNames::rp, HTML::TagNames::code, HTML::TagNames::var, HTML::TagNames::samp, HTML::TagNames::kbd, HTML::TagNames::sub, HTML::TagNames::sup, HTML::TagNames::i, HTML::TagNames::b, HTML::TagNames::u, HTML::TagNames::mark, HTML::TagNames::bdi, HTML::TagNames::bdo, HTML::TagNames::wbr, HTML::TagNames::summary, HTML::TagNames::noscript, HTML::TagNames::acronym, HTML::TagNames::basefont, HTML::TagNames::big, HTML::TagNames::center, HTML::TagNames::nobr, HTML::TagNames::noembed, HTML::TagNames::noframes, HTML::TagNames::plaintext, HTML::TagNames::rb, HTML::TagNames::rtc, HTML::TagNames::strike, HTML::TagNames::tt });
    return FixedArray<FlyString>::create({});
}

// https://html.spec.whatwg.org/multipage/dom.html#elements-in-the-dom%3Aelement-interface
bool is_unknown_html_element(FlyString const& tag_name)
{
    // NOTE: This is intentionally case-sensitive.

    // 1. If name is applet, bgsound, blink, isindex, keygen, multicol, nextid, or spacer, then return HTMLUnknownElement.
    if (tag_name.is_one_of(HTML::TagNames::applet, HTML::TagNames::bgsound, HTML::TagNames::blink, HTML::TagNames::isindex, HTML::TagNames::keygen, HTML::TagNames::multicol, HTML::TagNames::nextid, HTML::TagNames::spacer))
        return true;

        // 2. If name is acronym, basefont, big, center, nobr, noembed, noframes, plaintext, rb, rtc, strike, or tt, then return HTMLElement.
        // 3. If name is listing or xmp, then return HTMLPreElement.
        // 4. Otherwise, if this specification defines an interface appropriate for the element type corresponding to the local name name, then return that interface.
        // 5. If other applicable specifications define an appropriate interface for name, then return the interface they define.
#define __ENUMERATE_HTML_TAG(name)        \
    if (tag_name == HTML::TagNames::name) \
        return false;
    ENUMERATE_HTML_TAGS
#undef __ENUMERATE_HTML_TAG

    // 6. If name is a valid custom element name, then return HTMLElement.
    if (HTML::is_valid_custom_element_name(tag_name))
        return false;

    // 7. Return HTMLUnknownElement.
    return true;
}

// https://html.spec.whatwg.org/#elements-in-the-dom:element-interface
static JS::NonnullGCPtr<Element> create_html_element(JS::Realm& realm, Document& document, QualifiedName qualified_name)
{
    FlyString lowercase_tag_name = MUST(Infra::to_ascii_lowercase(qualified_name.local_name()));

    if (lowercase_tag_name == HTML::TagNames::a)
        return realm.heap().allocate<HTML::HTMLAnchorElement>(realm, document, move(qualified_name));
    if (lowercase_tag_name == HTML::TagNames::area)
        return realm.heap().allocate<HTML::HTMLAreaElement>(realm, document, move(qualified_name));
    if (lowercase_tag_name == HTML::TagNames::audio)
        return realm.heap().allocate<HTML::HTMLAudioElement>(realm, document, move(qualified_name));
    if (lowercase_tag_name == HTML::TagNames::base)
        return realm.heap().allocate<HTML::HTMLBaseElement>(realm, document, move(qualified_name));
    if (lowercase_tag_name == HTML::TagNames::body)
        return realm.heap().allocate<HTML::HTMLBodyElement>(realm, document, move(qualified_name));
    if (lowercase_tag_name == HTML::TagNames::br)
        return realm.heap().allocate<HTML::HTMLBRElement>(realm, document, move(qualified_name));
    if (lowercase_tag_name == HTML::TagNames::button)
        return realm.heap().allocate<HTML::HTMLButtonElement>(realm, document, move(qualified_name));
    if (lowercase_tag_name == HTML::TagNames::canvas)
        return realm.heap().allocate<HTML::HTMLCanvasElement>(realm, document, move(qualified_name));
    if (lowercase_tag_name == HTML::TagNames::data)
        return realm.heap().allocate<HTML::HTMLDataElement>(realm, document, move(qualified_name));
    if (lowercase_tag_name == HTML::TagNames::datalist)
        return realm.heap().allocate<HTML::HTMLDataListElement>(realm, document, move(qualified_name));
    if (lowercase_tag_name == HTML::TagNames::details)
        return realm.heap().allocate<HTML::HTMLDetailsElement>(realm, document, move(qualified_name));
    if (lowercase_tag_name == HTML::TagNames::dialog)
        return realm.heap().allocate<HTML::HTMLDialogElement>(realm, document, move(qualified_name));
    if (lowercase_tag_name == HTML::TagNames::dir)
        return realm.heap().allocate<HTML::HTMLDirectoryElement>(realm, document, move(qualified_name));
    if (lowercase_tag_name == HTML::TagNames::div)
        return realm.heap().allocate<HTML::HTMLDivElement>(realm, document, move(qualified_name));
    if (lowercase_tag_name == HTML::TagNames::dl)
        return realm.heap().allocate<HTML::HTMLDListElement>(realm, document, move(qualified_name));
    if (lowercase_tag_name == HTML::TagNames::embed)
        return realm.heap().allocate<HTML::HTMLEmbedElement>(realm, document, move(qualified_name));
    if (lowercase_tag_name == HTML::TagNames::fieldset)
        return realm.heap().allocate<HTML::HTMLFieldSetElement>(realm, document, move(qualified_name));
    if (lowercase_tag_name == HTML::TagNames::font)
        return realm.heap().allocate<HTML::HTMLFontElement>(realm, document, move(qualified_name));
    if (lowercase_tag_name == HTML::TagNames::form)
        return realm.heap().allocate<HTML::HTMLFormElement>(realm, document, move(qualified_name));
    if (lowercase_tag_name == HTML::TagNames::frame)
        return realm.heap().allocate<HTML::HTMLFrameElement>(realm, document, move(qualified_name));
    if (lowercase_tag_name == HTML::TagNames::frameset)
        return realm.heap().allocate<HTML::HTMLFrameSetElement>(realm, document, move(qualified_name));
    if (lowercase_tag_name == HTML::TagNames::head)
        return realm.heap().allocate<HTML::HTMLHeadElement>(realm, document, move(qualified_name));
    if (lowercase_tag_name.is_one_of(HTML::TagNames::h1, HTML::TagNames::h2, HTML::TagNames::h3, HTML::TagNames::h4, HTML::TagNames::h5, HTML::TagNames::h6))
        return realm.heap().allocate<HTML::HTMLHeadingElement>(realm, document, move(qualified_name));
    if (lowercase_tag_name == HTML::TagNames::hr)
        return realm.heap().allocate<HTML::HTMLHRElement>(realm, document, move(qualified_name));
    if (lowercase_tag_name == HTML::TagNames::html)
        return realm.heap().allocate<HTML::HTMLHtmlElement>(realm, document, move(qualified_name));
    if (lowercase_tag_name == HTML::TagNames::iframe)
        return realm.heap().allocate<HTML::HTMLIFrameElement>(realm, document, move(qualified_name));
    if (lowercase_tag_name == HTML::TagNames::img)
        return realm.heap().allocate<HTML::HTMLImageElement>(realm, document, move(qualified_name));
    if (lowercase_tag_name == HTML::TagNames::input)
        return realm.heap().allocate<HTML::HTMLInputElement>(realm, document, move(qualified_name));
    if (lowercase_tag_name == HTML::TagNames::label)
        return realm.heap().allocate<HTML::HTMLLabelElement>(realm, document, move(qualified_name));
    if (lowercase_tag_name == HTML::TagNames::legend)
        return realm.heap().allocate<HTML::HTMLLegendElement>(realm, document, move(qualified_name));
    if (lowercase_tag_name == HTML::TagNames::li)
        return realm.heap().allocate<HTML::HTMLLIElement>(realm, document, move(qualified_name));
    if (lowercase_tag_name == HTML::TagNames::link)
        return realm.heap().allocate<HTML::HTMLLinkElement>(realm, document, move(qualified_name));
    if (lowercase_tag_name == HTML::TagNames::map)
        return realm.heap().allocate<HTML::HTMLMapElement>(realm, document, move(qualified_name));
    if (lowercase_tag_name == HTML::TagNames::marquee)
        return realm.heap().allocate<HTML::HTMLMarqueeElement>(realm, document, move(qualified_name));
    if (lowercase_tag_name == HTML::TagNames::menu)
        return realm.heap().allocate<HTML::HTMLMenuElement>(realm, document, move(qualified_name));
    if (lowercase_tag_name == HTML::TagNames::meta)
        return realm.heap().allocate<HTML::HTMLMetaElement>(realm, document, move(qualified_name));
    if (lowercase_tag_name == HTML::TagNames::meter)
        return realm.heap().allocate<HTML::HTMLMeterElement>(realm, document, move(qualified_name));
    if (lowercase_tag_name.is_one_of(HTML::TagNames::ins, HTML::TagNames::del))
        return realm.heap().allocate<HTML::HTMLModElement>(realm, document, move(qualified_name));
    if (lowercase_tag_name == HTML::TagNames::object)
        return realm.heap().allocate<HTML::HTMLObjectElement>(realm, document, move(qualified_name));
    if (lowercase_tag_name == HTML::TagNames::ol)
        return realm.heap().allocate<HTML::HTMLOListElement>(realm, document, move(qualified_name));
    if (lowercase_tag_name == HTML::TagNames::optgroup)
        return realm.heap().allocate<HTML::HTMLOptGroupElement>(realm, document, move(qualified_name));
    if (lowercase_tag_name == HTML::TagNames::option)
        return realm.heap().allocate<HTML::HTMLOptionElement>(realm, document, move(qualified_name));
    if (lowercase_tag_name == HTML::TagNames::output)
        return realm.heap().allocate<HTML::HTMLOutputElement>(realm, document, move(qualified_name));
    if (lowercase_tag_name == HTML::TagNames::p)
        return realm.heap().allocate<HTML::HTMLParagraphElement>(realm, document, move(qualified_name));
    if (lowercase_tag_name == HTML::TagNames::param)
        return realm.heap().allocate<HTML::HTMLParamElement>(realm, document, move(qualified_name));
    if (lowercase_tag_name == HTML::TagNames::picture)
        return realm.heap().allocate<HTML::HTMLPictureElement>(realm, document, move(qualified_name));
    // NOTE: The obsolete elements "listing" and "xmp" are explicitly mapped to HTMLPreElement in the specification.
    if (lowercase_tag_name.is_one_of(HTML::TagNames::pre, HTML::TagNames::listing, HTML::TagNames::xmp))
        return realm.heap().allocate<HTML::HTMLPreElement>(realm, document, move(qualified_name));
    if (lowercase_tag_name == HTML::TagNames::progress)
        return realm.heap().allocate<HTML::HTMLProgressElement>(realm, document, move(qualified_name));
    if (lowercase_tag_name.is_one_of(HTML::TagNames::blockquote, HTML::TagNames::q))
        return realm.heap().allocate<HTML::HTMLQuoteElement>(realm, document, move(qualified_name));
    if (lowercase_tag_name == HTML::TagNames::script)
        return realm.heap().allocate<HTML::HTMLScriptElement>(realm, document, move(qualified_name));
    if (lowercase_tag_name == HTML::TagNames::select)
        return realm.heap().allocate<HTML::HTMLSelectElement>(realm, document, move(qualified_name));
    if (lowercase_tag_name == HTML::TagNames::slot)
        return realm.heap().allocate<HTML::HTMLSlotElement>(realm, document, move(qualified_name));
    if (lowercase_tag_name == HTML::TagNames::source)
        return realm.heap().allocate<HTML::HTMLSourceElement>(realm, document, move(qualified_name));
    if (lowercase_tag_name == HTML::TagNames::span)
        return realm.heap().allocate<HTML::HTMLSpanElement>(realm, document, move(qualified_name));
    if (lowercase_tag_name == HTML::TagNames::style)
        return realm.heap().allocate<HTML::HTMLStyleElement>(realm, document, move(qualified_name));
    if (lowercase_tag_name == HTML::TagNames::summary)
        return realm.heap().allocate<HTML::HTMLSummaryElement>(realm, document, move(qualified_name));
    if (lowercase_tag_name == HTML::TagNames::caption)
        return realm.heap().allocate<HTML::HTMLTableCaptionElement>(realm, document, move(qualified_name));
    if (lowercase_tag_name.is_one_of(Web::HTML::TagNames::td, Web::HTML::TagNames::th))
        return realm.heap().allocate<HTML::HTMLTableCellElement>(realm, document, move(qualified_name));
    if (lowercase_tag_name.is_one_of(HTML::TagNames::colgroup, HTML::TagNames::col))
        return realm.heap().allocate<HTML::HTMLTableColElement>(realm, document, move(qualified_name));
    if (lowercase_tag_name == HTML::TagNames::table)
        return realm.heap().allocate<HTML::HTMLTableElement>(realm, document, move(qualified_name));
    if (lowercase_tag_name == HTML::TagNames::tr)
        return realm.heap().allocate<HTML::HTMLTableRowElement>(realm, document, move(qualified_name));
    if (lowercase_tag_name.is_one_of(HTML::TagNames::tbody, HTML::TagNames::thead, HTML::TagNames::tfoot))
        return realm.heap().allocate<HTML::HTMLTableSectionElement>(realm, document, move(qualified_name));
    if (lowercase_tag_name == HTML::TagNames::template_)
        return realm.heap().allocate<HTML::HTMLTemplateElement>(realm, document, move(qualified_name));
    if (lowercase_tag_name == HTML::TagNames::textarea)
        return realm.heap().allocate<HTML::HTMLTextAreaElement>(realm, document, move(qualified_name));
    if (lowercase_tag_name == HTML::TagNames::time)
        return realm.heap().allocate<HTML::HTMLTimeElement>(realm, document, move(qualified_name));
    if (lowercase_tag_name == HTML::TagNames::title)
        return realm.heap().allocate<HTML::HTMLTitleElement>(realm, document, move(qualified_name));
    if (lowercase_tag_name == HTML::TagNames::track)
        return realm.heap().allocate<HTML::HTMLTrackElement>(realm, document, move(qualified_name));
    if (lowercase_tag_name == HTML::TagNames::ul)
        return realm.heap().allocate<HTML::HTMLUListElement>(realm, document, move(qualified_name));
    if (lowercase_tag_name == HTML::TagNames::video)
        return realm.heap().allocate<HTML::HTMLVideoElement>(realm, document, move(qualified_name));
    if (lowercase_tag_name.is_one_of(
            HTML::TagNames::article, HTML::TagNames::section, HTML::TagNames::nav, HTML::TagNames::aside, HTML::TagNames::hgroup, HTML::TagNames::header, HTML::TagNames::footer, HTML::TagNames::address, HTML::TagNames::dt, HTML::TagNames::dd, HTML::TagNames::figure, HTML::TagNames::figcaption, HTML::TagNames::main, HTML::TagNames::em, HTML::TagNames::strong, HTML::TagNames::small, HTML::TagNames::s, HTML::TagNames::cite, HTML::TagNames::dfn, HTML::TagNames::abbr, HTML::TagNames::ruby, HTML::TagNames::rt, HTML::TagNames::rp, HTML::TagNames::code, HTML::TagNames::var, HTML::TagNames::samp, HTML::TagNames::kbd, HTML::TagNames::sub, HTML::TagNames::sup, HTML::TagNames::i, HTML::TagNames::b, HTML::TagNames::u, HTML::TagNames::mark, HTML::TagNames::bdi, HTML::TagNames::bdo, HTML::TagNames::wbr, HTML::TagNames::noscript,
            // Obsolete
            HTML::TagNames::acronym, HTML::TagNames::basefont, HTML::TagNames::big, HTML::TagNames::center, HTML::TagNames::nobr, HTML::TagNames::noembed, HTML::TagNames::noframes, HTML::TagNames::plaintext, HTML::TagNames::rb, HTML::TagNames::rtc, HTML::TagNames::strike, HTML::TagNames::tt))
        return realm.heap().allocate<HTML::HTMLElement>(realm, document, move(qualified_name));
    if (HTML::is_valid_custom_element_name(qualified_name.local_name()))
        return realm.heap().allocate<HTML::HTMLElement>(realm, document, move(qualified_name));

    return realm.heap().allocate<HTML::HTMLUnknownElement>(realm, document, move(qualified_name));
}

static JS::GCPtr<SVG::SVGElement> create_svg_element(JS::Realm& realm, Document& document, QualifiedName qualified_name)
{
    auto const& local_name = qualified_name.local_name();

    if (local_name == SVG::TagNames::svg)
        return realm.heap().allocate<SVG::SVGSVGElement>(realm, document, move(qualified_name));
    // FIXME: Support SVG's mixedCase tag names properly.
    if (local_name.equals_ignoring_ascii_case(SVG::TagNames::clipPath))
        return realm.heap().allocate<SVG::SVGClipPathElement>(realm, document, move(qualified_name));
    if (local_name == SVG::TagNames::circle)
        return realm.heap().allocate<SVG::SVGCircleElement>(realm, document, move(qualified_name));
    if (local_name.equals_ignoring_ascii_case(SVG::TagNames::defs))
        return realm.heap().allocate<SVG::SVGDefsElement>(realm, document, move(qualified_name));
    if (local_name == SVG::TagNames::desc)
        return realm.heap().allocate<SVG::SVGDescElement>(realm, document, move(qualified_name));
    if (local_name == SVG::TagNames::ellipse)
        return realm.heap().allocate<SVG::SVGEllipseElement>(realm, document, move(qualified_name));
    if (local_name.equals_ignoring_ascii_case(SVG::TagNames::foreignObject))
        return realm.heap().allocate<SVG::SVGForeignObjectElement>(realm, document, move(qualified_name));
    if (local_name == SVG::TagNames::line)
        return realm.heap().allocate<SVG::SVGLineElement>(realm, document, move(qualified_name));
    if (local_name == SVG::TagNames::linearGradient)
        return realm.heap().allocate<SVG::SVGLinearGradientElement>(realm, document, move(qualified_name));
    if (local_name == SVG::TagNames::mask)
        return realm.heap().allocate<SVG::SVGMaskElement>(realm, document, move(qualified_name));
    if (local_name == SVG::TagNames::path)
        return realm.heap().allocate<SVG::SVGPathElement>(realm, document, move(qualified_name));
    if (local_name == SVG::TagNames::polygon)
        return realm.heap().allocate<SVG::SVGPolygonElement>(realm, document, move(qualified_name));
    if (local_name == SVG::TagNames::polyline)
        return realm.heap().allocate<SVG::SVGPolylineElement>(realm, document, move(qualified_name));
    if (local_name == SVG::TagNames::radialGradient)
        return realm.heap().allocate<SVG::SVGRadialGradientElement>(realm, document, move(qualified_name));
    if (local_name == SVG::TagNames::rect)
        return realm.heap().allocate<SVG::SVGRectElement>(realm, document, move(qualified_name));
    if (local_name == SVG::TagNames::g)
        return realm.heap().allocate<SVG::SVGGElement>(realm, document, move(qualified_name));
    if (local_name == SVG::TagNames::stop)
        return realm.heap().allocate<SVG::SVGStopElement>(realm, document, move(qualified_name));
    if (local_name == SVG::TagNames::style)
        return realm.heap().allocate<SVG::SVGStyleElement>(realm, document, move(qualified_name));
    if (local_name == SVG::TagNames::symbol)
        return realm.heap().allocate<SVG::SVGSymbolElement>(realm, document, move(qualified_name));
    if (local_name == SVG::TagNames::text)
        return realm.heap().allocate<SVG::SVGTextElement>(realm, document, move(qualified_name));
    if (local_name == SVG::TagNames::textPath)
        return realm.heap().allocate<SVG::SVGTextPathElement>(realm, document, move(qualified_name));
    if (local_name == SVG::TagNames::title)
        return realm.heap().allocate<SVG::SVGTitleElement>(realm, document, move(qualified_name));
    if (local_name == SVG::TagNames::tspan)
        return realm.heap().allocate<SVG::SVGTSpanElement>(realm, document, move(qualified_name));
    if (local_name == SVG::TagNames::use)
        return realm.heap().allocate<SVG::SVGUseElement>(realm, document, move(qualified_name));
    if (local_name == SVG::TagNames::script)
        return realm.heap().allocate<SVG::SVGScriptElement>(realm, document, move(qualified_name));
    if (local_name == SVG::TagNames::a)
        return realm.heap().allocate<SVG::SVGAElement>(realm, document, move(qualified_name));

    return nullptr;
}

static JS::GCPtr<MathML::MathMLElement> create_mathml_element(JS::Realm& realm, Document& document, QualifiedName qualified_name)
{
    auto const& local_name = qualified_name.local_name();

    if (local_name.is_one_of(MathML::TagNames::annotation, MathML::TagNames::annotation_xml, MathML::TagNames::maction, MathML::TagNames::math, MathML::TagNames::merror, MathML::TagNames::mfrac, MathML::TagNames::mi, MathML::TagNames::mmultiscripts, MathML::TagNames::mn, MathML::TagNames::mo, MathML::TagNames::mover, MathML::TagNames::mpadded, MathML::TagNames::mphantom, MathML::TagNames::mprescripts, MathML::TagNames::mroot, MathML::TagNames::mrow, MathML::TagNames::ms, MathML::TagNames::mspace, MathML::TagNames::msqrt, MathML::TagNames::mstyle, MathML::TagNames::msub, MathML::TagNames::msubsup, MathML::TagNames::msup, MathML::TagNames::mtable, MathML::TagNames::mtd, MathML::TagNames::mtext, MathML::TagNames::mtr, MathML::TagNames::munder, MathML::TagNames::munderover, MathML::TagNames::semantics))
        return realm.heap().allocate<MathML::MathMLElement>(realm, document, move(qualified_name));

    return nullptr;
}
// https://dom.spec.whatwg.org/#concept-create-element
WebIDL::ExceptionOr<JS::NonnullGCPtr<Element>> create_element(Document& document, FlyString local_name, Optional<FlyString> namespace_, Optional<FlyString> prefix, Optional<String> is_value, bool synchronous_custom_elements_flag)
{
    auto& realm = document.realm();

    // 1. If prefix was not given, let prefix be null.
    // NOTE: This is already taken care of by `prefix` having a default value.

    // 2. If is was not given, let is be null.
    // NOTE: This is already taken care of by `is` having a default value.

    // 3. Let result be null.
    // NOTE: We collapse this into just returning an element where necessary.

    // 4. Let definition be the result of looking up a custom element definition given document, namespace, localName, and is.
    auto definition = document.lookup_custom_element_definition(namespace_, local_name, is_value);

    // 5. If definition is non-null, and definition’s name is not equal to its local name (i.e., definition represents a customized built-in element), then:
    if (definition && definition->name() != definition->local_name()) {
        // 1. Let interface be the element interface for localName and the HTML namespace.
        // 2. Set result to a new element that implements interface, with no attributes, namespace set to the HTML namespace,
        //    namespace prefix set to prefix, local name set to localName, custom element state set to "undefined", custom element definition set to null,
        //    is value set to is, and node document set to document.
        auto element = create_html_element(realm, document, QualifiedName { local_name, prefix, Namespace::HTML });

        // 3. If the synchronous custom elements flag is set, then run this step while catching any exceptions:
        if (synchronous_custom_elements_flag) {
            // 1. Upgrade element using definition.
            auto upgrade_result = element->upgrade_element(*definition);

            // If this step threw an exception, then:
            if (upgrade_result.is_throw_completion()) {
                // 1. Report the exception.
                HTML::report_exception(upgrade_result, realm);

                // 2. Set result’s custom element state to "failed".
                element->set_custom_element_state(CustomElementState::Failed);
            }
        }

        // 4. Otherwise, enqueue a custom element upgrade reaction given result and definition.
        else {
            element->enqueue_a_custom_element_upgrade_reaction(*definition);
        }

        return element;
    }

    // 6. Otherwise, if definition is non-null, then:
    if (definition) {
        // 1. If the synchronous custom elements flag is set, then run these steps while catching any exceptions:
        if (synchronous_custom_elements_flag) {
            auto synchronously_upgrade_custom_element = [&]() -> JS::ThrowCompletionOr<JS::NonnullGCPtr<HTML::HTMLElement>> {
                auto& vm = document.vm();

                // 1. Let C be definition’s constructor.
                auto& constructor = definition->constructor();

                // 2. Set result to the result of constructing C, with no arguments.
                auto result = TRY(WebIDL::construct(constructor));

                // FIXME: 3. Assert: result’s custom element state and custom element definition are initialized.
                // FIXME: 4. Assert: result’s namespace is the HTML namespace.
                //        Spec Note: IDL enforces that result is an HTMLElement object, which all use the HTML namespace.
                // IDL does not currently convert the object for us, so we will have to do it here.

                if (!result.has_value() || !result->is_object() || !is<HTML::HTMLElement>(result->as_object()))
                    return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "HTMLElement"sv);

                JS::NonnullGCPtr<HTML::HTMLElement> element = verify_cast<HTML::HTMLElement>(result->as_object());

                // 5. If result’s attribute list is not empty, then throw a "NotSupportedError" DOMException.
                if (element->has_attributes())
                    return JS::throw_completion(WebIDL::NotSupportedError::create(realm, "Synchronously created custom element cannot have attributes"_fly_string));

                // 6. If result has children, then throw a "NotSupportedError" DOMException.
                if (element->has_children())
                    return JS::throw_completion(WebIDL::NotSupportedError::create(realm, "Synchronously created custom element cannot have children"_fly_string));

                // 7. If result’s parent is not null, then throw a "NotSupportedError" DOMException.
                if (element->parent())
                    return JS::throw_completion(WebIDL::NotSupportedError::create(realm, "Synchronously created custom element cannot have a parent"_fly_string));

                // 8. If result’s node document is not document, then throw a "NotSupportedError" DOMException.
                if (&element->document() != &document)
                    return JS::throw_completion(WebIDL::NotSupportedError::create(realm, "Synchronously created custom element must be in the same document that element creation was invoked in"_fly_string));

                // 9. If result’s local name is not equal to localName, then throw a "NotSupportedError" DOMException.
                if (element->local_name() != local_name)
                    return JS::throw_completion(WebIDL::NotSupportedError::create(realm, "Synchronously created custom element must have the same local name that element creation was invoked with"_fly_string));

                // 10. Set result’s namespace prefix to prefix.
                element->set_prefix(prefix);

                // 11. Set result’s is value to null.
                element->set_is_value(Optional<String> {});
                return element;
            };

            auto result = synchronously_upgrade_custom_element();

            // If any of these steps threw an exception, then:
            if (result.is_throw_completion()) {
                // 1. Report the exception.
                HTML::report_exception(result, realm);

                // 2. Set result to a new element that implements the HTMLUnknownElement interface, with no attributes, namespace set to the HTML namespace, namespace prefix set to prefix,
                //    local name set to localName, custom element state set to "failed", custom element definition set to null, is value set to null, and node document set to document.
                JS::NonnullGCPtr<Element> element = realm.heap().allocate<HTML::HTMLUnknownElement>(realm, document, QualifiedName { local_name, prefix, Namespace::HTML });
                element->set_custom_element_state(CustomElementState::Failed);
                return element;
            }

            return result.release_value();
        }

        // 2. Otherwise:
        // 1. Set result to a new element that implements the HTMLElement interface, with no attributes, namespace set to the HTML namespace, namespace prefix set to prefix,
        //    local name set to localName, custom element state set to "undefined", custom element definition set to null, is value set to null, and node document set to document.
        auto element = realm.heap().allocate<HTML::HTMLElement>(realm, document, QualifiedName { local_name, prefix, Namespace::HTML });
        element->set_custom_element_state(CustomElementState::Undefined);

        // 2. Enqueue a custom element upgrade reaction given result and definition.
        element->enqueue_a_custom_element_upgrade_reaction(*definition);
        return element;
    }

    // 7. Otherwise:
    //    1. Let interface be the element interface for localName and namespace.
    //    2. Set result to a new element that implements interface, with no attributes, namespace set to namespace, namespace prefix set to prefix,
    //       local name set to localName, custom element state set to "uncustomized", custom element definition set to null, is value set to is,
    //       and node document set to document.

    auto qualified_name = QualifiedName { local_name, prefix, namespace_ };

    if (namespace_ == Namespace::HTML) {
        auto element = create_html_element(realm, document, move(qualified_name));
        element->set_is_value(move(is_value));
        element->set_custom_element_state(CustomElementState::Uncustomized);

        // 3. If namespace is the HTML namespace, and either localName is a valid custom element name or is is non-null,
        //    then set result’s custom element state to "undefined".
        if (HTML::is_valid_custom_element_name(local_name) || is_value.has_value())
            element->set_custom_element_state(CustomElementState::Undefined);

        return element;
    }

    if (namespace_ == Namespace::SVG) {
        auto element = create_svg_element(realm, document, qualified_name);
        if (element) {
            element->set_is_value(move(is_value));
            element->set_custom_element_state(CustomElementState::Uncustomized);
            return JS::NonnullGCPtr<Element> { *element };
        }
    }

    if (namespace_ == Namespace::MathML) {
        auto element = create_mathml_element(realm, document, qualified_name);
        if (element) {
            element->set_is_value(move(is_value));
            element->set_custom_element_state(CustomElementState::Uncustomized);
            return JS::NonnullGCPtr<Element> { *element };
        }
    }

    // 8. Return result.
    // NOTE: See step 3.

    // https://dom.spec.whatwg.org/#concept-element-interface
    // The element interface for any name and namespace is Element, unless stated otherwise.
    dbgln("Potential FIXME: Creating unknown generic element '{}' in namespace '{}'", local_name, namespace_);
    auto element = realm.heap().allocate<DOM::Element>(realm, document, move(qualified_name));
    element->set_is_value(move(is_value));
    element->set_custom_element_state(CustomElementState::Uncustomized);
    return element;
}

}
