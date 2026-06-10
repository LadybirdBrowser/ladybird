/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2020-2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/ElementFactory.h>
#include <LibWeb/HTML/CustomElements/CustomElementAlgorithms.h>
#include <LibWeb/HTML/CustomElements/CustomElementDefinition.h>
#include <LibWeb/HTML/CustomElements/CustomElementName.h>
#include <LibWeb/HTML/CustomElements/CustomElementRegistry.h>
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
#include <LibWeb/HTML/HTMLSelectedContentElement.h>
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
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/WindowOrWorkerGlobalScope.h>
#include <LibWeb/Infra/Strings.h>
#include <LibWeb/MathML/MathMLElement.h>
#include <LibWeb/MathML/MathMLMiElement.h>
#include <LibWeb/MathML/MathMLMspaceElement.h>
#include <LibWeb/MathML/TagNames.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/SVG/SVGAElement.h>
#include <LibWeb/SVG/SVGCircleElement.h>
#include <LibWeb/SVG/SVGClipPathElement.h>
#include <LibWeb/SVG/SVGDefsElement.h>
#include <LibWeb/SVG/SVGDescElement.h>
#include <LibWeb/SVG/SVGEllipseElement.h>
#include <LibWeb/SVG/SVGFEBlendElement.h>
#include <LibWeb/SVG/SVGFEColorMatrixElement.h>
#include <LibWeb/SVG/SVGFEComponentTransferElement.h>
#include <LibWeb/SVG/SVGFECompositeElement.h>
#include <LibWeb/SVG/SVGFEDisplacementMapElement.h>
#include <LibWeb/SVG/SVGFEDropShadowElement.h>
#include <LibWeb/SVG/SVGFEFloodElement.h>
#include <LibWeb/SVG/SVGFEFuncAElement.h>
#include <LibWeb/SVG/SVGFEFuncBElement.h>
#include <LibWeb/SVG/SVGFEFuncGElement.h>
#include <LibWeb/SVG/SVGFEFuncRElement.h>
#include <LibWeb/SVG/SVGFEGaussianBlurElement.h>
#include <LibWeb/SVG/SVGFEImageElement.h>
#include <LibWeb/SVG/SVGFEMergeElement.h>
#include <LibWeb/SVG/SVGFEMergeNodeElement.h>
#include <LibWeb/SVG/SVGFEMorphologyElement.h>
#include <LibWeb/SVG/SVGFEOffsetElement.h>
#include <LibWeb/SVG/SVGFETurbulenceElement.h>
#include <LibWeb/SVG/SVGFilterElement.h>
#include <LibWeb/SVG/SVGForeignObjectElement.h>
#include <LibWeb/SVG/SVGGElement.h>
#include <LibWeb/SVG/SVGImageElement.h>
#include <LibWeb/SVG/SVGLineElement.h>
#include <LibWeb/SVG/SVGLinearGradientElement.h>
#include <LibWeb/SVG/SVGMaskElement.h>
#include <LibWeb/SVG/SVGMetadataElement.h>
#include <LibWeb/SVG/SVGPathElement.h>
#include <LibWeb/SVG/SVGPatternElement.h>
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
#include <LibWeb/SVG/SVGViewElement.h>
#include <LibWeb/SVG/TagNames.h>

namespace Web::DOM {

template<typename ElementType>
static GC::Ref<ElementType> create_element_on_heap(Document& document, QualifiedName qualified_name)
{
    auto element = GC::Heap::the().allocate<ElementType>(document, move(qualified_name));
    static_cast<Element&>(*element).initialize_element();
    return element;
}

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
    if (html_element_interface_name == "HTMLFieldSetElement"sv)
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
    if (html_element_interface_name == "HTMLLegendElement"sv)
        return FixedArray<FlyString>::create({ HTML::TagNames::legend });
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
    if (html_element_interface_name == "HTMLMetaElement"sv)
        return FixedArray<FlyString>::create({ HTML::TagNames::meta });
    if (html_element_interface_name == "HTMLMeterElement"sv)
        return FixedArray<FlyString>::create({ HTML::TagNames::meter });
    if (html_element_interface_name == "HTMLModElement"sv)
        return FixedArray<FlyString>::create({ HTML::TagNames::ins, HTML::TagNames::del });
    if (html_element_interface_name == "HTMLOListElement"sv)
        return FixedArray<FlyString>::create({ HTML::TagNames::ol });
    if (html_element_interface_name == "HTMLObjectElement"sv)
        return FixedArray<FlyString>::create({ HTML::TagNames::object });
    if (html_element_interface_name == "HTMLOptGroupElement"sv)
        return FixedArray<FlyString>::create({ HTML::TagNames::optgroup });
    if (html_element_interface_name == "HTMLOptionElement"sv)
        return FixedArray<FlyString>::create({ HTML::TagNames::option });
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
    if (html_element_interface_name == "HTMLSelectedContentElement"sv)
        return FixedArray<FlyString>::create({ HTML::TagNames::selectedcontent });
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
    if (html_element_interface_name == "HTMLTableRowElement"sv)
        return FixedArray<FlyString>::create({ HTML::TagNames::tr });
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
        return FixedArray<FlyString>::create({ HTML::TagNames::article, HTML::TagNames::search, HTML::TagNames::section, HTML::TagNames::nav, HTML::TagNames::aside, HTML::TagNames::hgroup, HTML::TagNames::header, HTML::TagNames::footer, HTML::TagNames::address, HTML::TagNames::dt, HTML::TagNames::dd, HTML::TagNames::figure, HTML::TagNames::figcaption, HTML::TagNames::main, HTML::TagNames::em, HTML::TagNames::strong, HTML::TagNames::small, HTML::TagNames::s, HTML::TagNames::cite, HTML::TagNames::dfn, HTML::TagNames::abbr, HTML::TagNames::ruby, HTML::TagNames::rt, HTML::TagNames::rp, HTML::TagNames::code, HTML::TagNames::var, HTML::TagNames::samp, HTML::TagNames::kbd, HTML::TagNames::sub, HTML::TagNames::sup, HTML::TagNames::i, HTML::TagNames::b, HTML::TagNames::u, HTML::TagNames::mark, HTML::TagNames::bdi, HTML::TagNames::bdo, HTML::TagNames::wbr, HTML::TagNames::summary, HTML::TagNames::noscript, HTML::TagNames::acronym, HTML::TagNames::basefont, HTML::TagNames::big, HTML::TagNames::center, HTML::TagNames::nobr, HTML::TagNames::noembed, HTML::TagNames::noframes, HTML::TagNames::plaintext, HTML::TagNames::rb, HTML::TagNames::rtc, HTML::TagNames::strike, HTML::TagNames::tt });
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
#define __ENUMERATE_HTML_TAG(name, tag)   \
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

// https://html.spec.whatwg.org/multipage/dom.html#elements-in-the-dom%3Aelement-interface
static GC::Ref<Element> create_html_element(Document& document, QualifiedName qualified_name)
{
    FlyString tag_name = qualified_name.local_name();

    if (tag_name == HTML::TagNames::a)
        return create_element_on_heap<HTML::HTMLAnchorElement>(document, move(qualified_name));
    if (tag_name == HTML::TagNames::area)
        return create_element_on_heap<HTML::HTMLAreaElement>(document, move(qualified_name));
    if (tag_name == HTML::TagNames::audio)
        return create_element_on_heap<HTML::HTMLAudioElement>(document, move(qualified_name));
    if (tag_name == HTML::TagNames::base)
        return create_element_on_heap<HTML::HTMLBaseElement>(document, move(qualified_name));
    if (tag_name == HTML::TagNames::body)
        return create_element_on_heap<HTML::HTMLBodyElement>(document, move(qualified_name));
    if (tag_name == HTML::TagNames::br)
        return create_element_on_heap<HTML::HTMLBRElement>(document, move(qualified_name));
    if (tag_name == HTML::TagNames::button)
        return create_element_on_heap<HTML::HTMLButtonElement>(document, move(qualified_name));
    if (tag_name == HTML::TagNames::canvas)
        return create_element_on_heap<HTML::HTMLCanvasElement>(document, move(qualified_name));
    if (tag_name == HTML::TagNames::data)
        return create_element_on_heap<HTML::HTMLDataElement>(document, move(qualified_name));
    if (tag_name == HTML::TagNames::datalist)
        return create_element_on_heap<HTML::HTMLDataListElement>(document, move(qualified_name));
    if (tag_name == HTML::TagNames::details)
        return create_element_on_heap<HTML::HTMLDetailsElement>(document, move(qualified_name));
    if (tag_name == HTML::TagNames::dialog)
        return create_element_on_heap<HTML::HTMLDialogElement>(document, move(qualified_name));
    if (tag_name == HTML::TagNames::dir)
        return create_element_on_heap<HTML::HTMLDirectoryElement>(document, move(qualified_name));
    if (tag_name == HTML::TagNames::div)
        return create_element_on_heap<HTML::HTMLDivElement>(document, move(qualified_name));
    if (tag_name == HTML::TagNames::dl)
        return create_element_on_heap<HTML::HTMLDListElement>(document, move(qualified_name));
    if (tag_name == HTML::TagNames::embed)
        return create_element_on_heap<HTML::HTMLEmbedElement>(document, move(qualified_name));
    if (tag_name == HTML::TagNames::fieldset)
        return create_element_on_heap<HTML::HTMLFieldSetElement>(document, move(qualified_name));
    if (tag_name == HTML::TagNames::font)
        return create_element_on_heap<HTML::HTMLFontElement>(document, move(qualified_name));
    if (tag_name == HTML::TagNames::form)
        return create_element_on_heap<HTML::HTMLFormElement>(document, move(qualified_name));
    if (tag_name == HTML::TagNames::frame)
        return create_element_on_heap<HTML::HTMLFrameElement>(document, move(qualified_name));
    if (tag_name == HTML::TagNames::frameset)
        return create_element_on_heap<HTML::HTMLFrameSetElement>(document, move(qualified_name));
    if (tag_name == HTML::TagNames::head)
        return create_element_on_heap<HTML::HTMLHeadElement>(document, move(qualified_name));
    if (tag_name.is_one_of(HTML::TagNames::h1, HTML::TagNames::h2, HTML::TagNames::h3, HTML::TagNames::h4, HTML::TagNames::h5, HTML::TagNames::h6))
        return create_element_on_heap<HTML::HTMLHeadingElement>(document, move(qualified_name));
    if (tag_name == HTML::TagNames::hr)
        return create_element_on_heap<HTML::HTMLHRElement>(document, move(qualified_name));
    if (tag_name == HTML::TagNames::html)
        return create_element_on_heap<HTML::HTMLHtmlElement>(document, move(qualified_name));
    if (tag_name == HTML::TagNames::iframe)
        return create_element_on_heap<HTML::HTMLIFrameElement>(document, move(qualified_name));
    if (tag_name == HTML::TagNames::img)
        return create_element_on_heap<HTML::HTMLImageElement>(document, move(qualified_name));
    if (tag_name == HTML::TagNames::input)
        return create_element_on_heap<HTML::HTMLInputElement>(document, move(qualified_name));
    if (tag_name == HTML::TagNames::label)
        return create_element_on_heap<HTML::HTMLLabelElement>(document, move(qualified_name));
    if (tag_name == HTML::TagNames::legend)
        return create_element_on_heap<HTML::HTMLLegendElement>(document, move(qualified_name));
    if (tag_name == HTML::TagNames::li)
        return create_element_on_heap<HTML::HTMLLIElement>(document, move(qualified_name));
    if (tag_name == HTML::TagNames::link)
        return create_element_on_heap<HTML::HTMLLinkElement>(document, move(qualified_name));
    if (tag_name == HTML::TagNames::map)
        return create_element_on_heap<HTML::HTMLMapElement>(document, move(qualified_name));
    if (tag_name == HTML::TagNames::marquee)
        return create_element_on_heap<HTML::HTMLMarqueeElement>(document, move(qualified_name));
    if (tag_name == HTML::TagNames::menu)
        return create_element_on_heap<HTML::HTMLMenuElement>(document, move(qualified_name));
    if (tag_name == HTML::TagNames::meta)
        return create_element_on_heap<HTML::HTMLMetaElement>(document, move(qualified_name));
    if (tag_name == HTML::TagNames::meter)
        return create_element_on_heap<HTML::HTMLMeterElement>(document, move(qualified_name));
    if (tag_name.is_one_of(HTML::TagNames::ins, HTML::TagNames::del))
        return create_element_on_heap<HTML::HTMLModElement>(document, move(qualified_name));
    if (tag_name == HTML::TagNames::object)
        return create_element_on_heap<HTML::HTMLObjectElement>(document, move(qualified_name));
    if (tag_name == HTML::TagNames::ol)
        return create_element_on_heap<HTML::HTMLOListElement>(document, move(qualified_name));
    if (tag_name == HTML::TagNames::optgroup)
        return create_element_on_heap<HTML::HTMLOptGroupElement>(document, move(qualified_name));
    if (tag_name == HTML::TagNames::option)
        return create_element_on_heap<HTML::HTMLOptionElement>(document, move(qualified_name));
    if (tag_name == HTML::TagNames::output)
        return create_element_on_heap<HTML::HTMLOutputElement>(document, move(qualified_name));
    if (tag_name == HTML::TagNames::p)
        return create_element_on_heap<HTML::HTMLParagraphElement>(document, move(qualified_name));
    if (tag_name == HTML::TagNames::param)
        return create_element_on_heap<HTML::HTMLParamElement>(document, move(qualified_name));
    if (tag_name == HTML::TagNames::picture)
        return create_element_on_heap<HTML::HTMLPictureElement>(document, move(qualified_name));
    // NOTE: The obsolete elements "listing" and "xmp" are explicitly mapped to HTMLPreElement in the specification.
    if (tag_name.is_one_of(HTML::TagNames::pre, HTML::TagNames::listing, HTML::TagNames::xmp))
        return create_element_on_heap<HTML::HTMLPreElement>(document, move(qualified_name));
    if (tag_name == HTML::TagNames::progress)
        return create_element_on_heap<HTML::HTMLProgressElement>(document, move(qualified_name));
    if (tag_name.is_one_of(HTML::TagNames::blockquote, HTML::TagNames::q))
        return create_element_on_heap<HTML::HTMLQuoteElement>(document, move(qualified_name));
    if (tag_name == HTML::TagNames::script)
        return create_element_on_heap<HTML::HTMLScriptElement>(document, move(qualified_name));
    if (tag_name == HTML::TagNames::selectedcontent)
        return create_element_on_heap<HTML::HTMLSelectedContentElement>(document, move(qualified_name));
    if (tag_name == HTML::TagNames::select)
        return create_element_on_heap<HTML::HTMLSelectElement>(document, move(qualified_name));
    if (tag_name == HTML::TagNames::slot)
        return create_element_on_heap<HTML::HTMLSlotElement>(document, move(qualified_name));
    if (tag_name == HTML::TagNames::source)
        return create_element_on_heap<HTML::HTMLSourceElement>(document, move(qualified_name));
    if (tag_name == HTML::TagNames::span)
        return create_element_on_heap<HTML::HTMLSpanElement>(document, move(qualified_name));
    if (tag_name == HTML::TagNames::style)
        return create_element_on_heap<HTML::HTMLStyleElement>(document, move(qualified_name));
    if (tag_name == HTML::TagNames::summary)
        return create_element_on_heap<HTML::HTMLSummaryElement>(document, move(qualified_name));
    if (tag_name == HTML::TagNames::caption)
        return create_element_on_heap<HTML::HTMLTableCaptionElement>(document, move(qualified_name));
    if (tag_name.is_one_of(Web::HTML::TagNames::td, Web::HTML::TagNames::th))
        return create_element_on_heap<HTML::HTMLTableCellElement>(document, move(qualified_name));
    if (tag_name.is_one_of(HTML::TagNames::colgroup, HTML::TagNames::col))
        return create_element_on_heap<HTML::HTMLTableColElement>(document, move(qualified_name));
    if (tag_name == HTML::TagNames::table)
        return create_element_on_heap<HTML::HTMLTableElement>(document, move(qualified_name));
    if (tag_name == HTML::TagNames::tr)
        return create_element_on_heap<HTML::HTMLTableRowElement>(document, move(qualified_name));
    if (tag_name.is_one_of(HTML::TagNames::tbody, HTML::TagNames::thead, HTML::TagNames::tfoot))
        return create_element_on_heap<HTML::HTMLTableSectionElement>(document, move(qualified_name));
    if (tag_name == HTML::TagNames::template_)
        return create_element_on_heap<HTML::HTMLTemplateElement>(document, move(qualified_name));
    if (tag_name == HTML::TagNames::textarea)
        return create_element_on_heap<HTML::HTMLTextAreaElement>(document, move(qualified_name));
    if (tag_name == HTML::TagNames::time)
        return create_element_on_heap<HTML::HTMLTimeElement>(document, move(qualified_name));
    if (tag_name == HTML::TagNames::title)
        return create_element_on_heap<HTML::HTMLTitleElement>(document, move(qualified_name));
    if (tag_name == HTML::TagNames::track)
        return create_element_on_heap<HTML::HTMLTrackElement>(document, move(qualified_name));
    if (tag_name == HTML::TagNames::ul)
        return create_element_on_heap<HTML::HTMLUListElement>(document, move(qualified_name));
    if (tag_name == HTML::TagNames::video)
        return create_element_on_heap<HTML::HTMLVideoElement>(document, move(qualified_name));
    if (tag_name.is_one_of(
            HTML::TagNames::article, HTML::TagNames::search, HTML::TagNames::section, HTML::TagNames::nav, HTML::TagNames::aside, HTML::TagNames::hgroup, HTML::TagNames::header, HTML::TagNames::footer, HTML::TagNames::address, HTML::TagNames::dt, HTML::TagNames::dd, HTML::TagNames::figure, HTML::TagNames::figcaption, HTML::TagNames::main, HTML::TagNames::em, HTML::TagNames::strong, HTML::TagNames::small, HTML::TagNames::s, HTML::TagNames::cite, HTML::TagNames::dfn, HTML::TagNames::abbr, HTML::TagNames::ruby, HTML::TagNames::rt, HTML::TagNames::rp, HTML::TagNames::code, HTML::TagNames::var, HTML::TagNames::samp, HTML::TagNames::kbd, HTML::TagNames::sub, HTML::TagNames::sup, HTML::TagNames::i, HTML::TagNames::b, HTML::TagNames::u, HTML::TagNames::mark, HTML::TagNames::bdi, HTML::TagNames::bdo, HTML::TagNames::wbr, HTML::TagNames::noscript,
            // Obsolete
            HTML::TagNames::acronym, HTML::TagNames::basefont, HTML::TagNames::big, HTML::TagNames::center, HTML::TagNames::nobr, HTML::TagNames::noembed, HTML::TagNames::noframes, HTML::TagNames::plaintext, HTML::TagNames::rb, HTML::TagNames::rtc, HTML::TagNames::strike, HTML::TagNames::tt))
        return create_element_on_heap<HTML::HTMLElement>(document, move(qualified_name));
    if (HTML::is_valid_custom_element_name(qualified_name.local_name()))
        return create_element_on_heap<HTML::HTMLElement>(document, move(qualified_name));

    return create_element_on_heap<HTML::HTMLUnknownElement>(document, move(qualified_name));
}

static GC::Ref<SVG::SVGElement> create_svg_element(Document& document, QualifiedName qualified_name)
{
    auto const& local_name = qualified_name.local_name();

    if (local_name == SVG::TagNames::svg)
        return create_element_on_heap<SVG::SVGSVGElement>(document, move(qualified_name));
    // FIXME: Support SVG's mixedCase tag names properly.
    if (local_name.equals_ignoring_ascii_case(SVG::TagNames::clipPath))
        return create_element_on_heap<SVG::SVGClipPathElement>(document, move(qualified_name));
    if (local_name == SVG::TagNames::circle)
        return create_element_on_heap<SVG::SVGCircleElement>(document, move(qualified_name));
    if (local_name.equals_ignoring_ascii_case(SVG::TagNames::defs))
        return create_element_on_heap<SVG::SVGDefsElement>(document, move(qualified_name));
    if (local_name == SVG::TagNames::desc)
        return create_element_on_heap<SVG::SVGDescElement>(document, move(qualified_name));
    if (local_name == SVG::TagNames::ellipse)
        return create_element_on_heap<SVG::SVGEllipseElement>(document, move(qualified_name));
    if (local_name == SVG::TagNames::feBlend)
        return create_element_on_heap<SVG::SVGFEBlendElement>(document, move(qualified_name));
    if (local_name == SVG::TagNames::feColorMatrix)
        return create_element_on_heap<SVG::SVGFEColorMatrixElement>(document, move(qualified_name));
    if (local_name == SVG::TagNames::feComponentTransfer)
        return create_element_on_heap<SVG::SVGFEComponentTransferElement>(document, move(qualified_name));
    if (local_name == SVG::TagNames::feComposite)
        return create_element_on_heap<SVG::SVGFECompositeElement>(document, move(qualified_name));
    if (local_name == SVG::TagNames::feDisplacementMap)
        return create_element_on_heap<SVG::SVGFEDisplacementMapElement>(document, move(qualified_name));
    if (local_name == SVG::TagNames::feDropShadow)
        return create_element_on_heap<SVG::SVGFEDropShadowElement>(document, move(qualified_name));
    if (local_name == SVG::TagNames::feFlood)
        return create_element_on_heap<SVG::SVGFEFloodElement>(document, move(qualified_name));
    if (local_name == SVG::TagNames::feFuncA)
        return create_element_on_heap<SVG::SVGFEFuncAElement>(document, move(qualified_name));
    if (local_name == SVG::TagNames::feFuncB)
        return create_element_on_heap<SVG::SVGFEFuncBElement>(document, move(qualified_name));
    if (local_name == SVG::TagNames::feFuncG)
        return create_element_on_heap<SVG::SVGFEFuncGElement>(document, move(qualified_name));
    if (local_name == SVG::TagNames::feFuncR)
        return create_element_on_heap<SVG::SVGFEFuncRElement>(document, move(qualified_name));
    if (local_name == SVG::TagNames::feGaussianBlur)
        return create_element_on_heap<SVG::SVGFEGaussianBlurElement>(document, move(qualified_name));
    if (local_name == SVG::TagNames::feImage)
        return create_element_on_heap<SVG::SVGFEImageElement>(document, move(qualified_name));
    if (local_name == SVG::TagNames::feMerge)
        return create_element_on_heap<SVG::SVGFEMergeElement>(document, move(qualified_name));
    if (local_name == SVG::TagNames::feMergeNode)
        return create_element_on_heap<SVG::SVGFEMergeNodeElement>(document, move(qualified_name));
    if (local_name == SVG::TagNames::feMorphology)
        return create_element_on_heap<SVG::SVGFEMorphologyElement>(document, move(qualified_name));
    if (local_name == SVG::TagNames::feOffset)
        return create_element_on_heap<SVG::SVGFEOffsetElement>(document, move(qualified_name));
    if (local_name == SVG::TagNames::feTurbulence)
        return create_element_on_heap<SVG::SVGFETurbulenceElement>(document, move(qualified_name));
    if (local_name == SVG::TagNames::filter)
        return create_element_on_heap<SVG::SVGFilterElement>(document, move(qualified_name));
    if (local_name.equals_ignoring_ascii_case(SVG::TagNames::foreignObject))
        return create_element_on_heap<SVG::SVGForeignObjectElement>(document, move(qualified_name));
    if (local_name == SVG::TagNames::line)
        return create_element_on_heap<SVG::SVGLineElement>(document, move(qualified_name));
    if (local_name == SVG::TagNames::linearGradient)
        return create_element_on_heap<SVG::SVGLinearGradientElement>(document, move(qualified_name));
    if (local_name == SVG::TagNames::mask)
        return create_element_on_heap<SVG::SVGMaskElement>(document, move(qualified_name));
    if (local_name == SVG::TagNames::metadata)
        return create_element_on_heap<SVG::SVGMetadataElement>(document, move(qualified_name));
    if (local_name == SVG::TagNames::path)
        return create_element_on_heap<SVG::SVGPathElement>(document, move(qualified_name));
    if (local_name == SVG::TagNames::pattern)
        return create_element_on_heap<SVG::SVGPatternElement>(document, move(qualified_name));
    if (local_name == SVG::TagNames::polygon)
        return create_element_on_heap<SVG::SVGPolygonElement>(document, move(qualified_name));
    if (local_name == SVG::TagNames::polyline)
        return create_element_on_heap<SVG::SVGPolylineElement>(document, move(qualified_name));
    if (local_name == SVG::TagNames::radialGradient)
        return create_element_on_heap<SVG::SVGRadialGradientElement>(document, move(qualified_name));
    if (local_name == SVG::TagNames::rect)
        return create_element_on_heap<SVG::SVGRectElement>(document, move(qualified_name));
    if (local_name == SVG::TagNames::g)
        return create_element_on_heap<SVG::SVGGElement>(document, move(qualified_name));
    if (local_name == SVG::TagNames::stop)
        return create_element_on_heap<SVG::SVGStopElement>(document, move(qualified_name));
    if (local_name == SVG::TagNames::style)
        return create_element_on_heap<SVG::SVGStyleElement>(document, move(qualified_name));
    if (local_name == SVG::TagNames::symbol)
        return create_element_on_heap<SVG::SVGSymbolElement>(document, move(qualified_name));
    if (local_name == SVG::TagNames::text)
        return create_element_on_heap<SVG::SVGTextElement>(document, move(qualified_name));
    if (local_name == SVG::TagNames::textPath)
        return create_element_on_heap<SVG::SVGTextPathElement>(document, move(qualified_name));
    if (local_name == SVG::TagNames::title)
        return create_element_on_heap<SVG::SVGTitleElement>(document, move(qualified_name));
    if (local_name == SVG::TagNames::tspan)
        return create_element_on_heap<SVG::SVGTSpanElement>(document, move(qualified_name));
    if (local_name == SVG::TagNames::use)
        return create_element_on_heap<SVG::SVGUseElement>(document, move(qualified_name));
    if (local_name == SVG::TagNames::script)
        return create_element_on_heap<SVG::SVGScriptElement>(document, move(qualified_name));
    if (local_name == SVG::TagNames::view)
        return create_element_on_heap<SVG::SVGViewElement>(document, move(qualified_name));
    if (local_name == SVG::TagNames::a)
        return create_element_on_heap<SVG::SVGAElement>(document, move(qualified_name));
    if (local_name == SVG::TagNames::image)
        return create_element_on_heap<SVG::SVGImageElement>(document, move(qualified_name));

    // https://svgwg.org/svg2-draft/types.html#ElementsInTheSVGDOM
    // Elements in the SVG namespace whose local name does not match an element defined in any
    // specification supported by the software must nonetheless implement the SVGElement interface.
    return create_element_on_heap<SVG::SVGElement>(document, move(qualified_name));
}

static GC::Ref<MathML::MathMLElement> create_mathml_element(Document& document, QualifiedName qualified_name)
{

    auto const& local_name = qualified_name.local_name();
    if (local_name == MathML::TagNames::mi)
        return create_element_on_heap<MathML::MathMLMiElement>(document, move(qualified_name));
    if (local_name == MathML::TagNames::mspace)
        return create_element_on_heap<MathML::MathMLMspaceElement>(document, move(qualified_name));

    // https://w3c.github.io/mathml-core/#dom-and-javascript
    // All the nodes representing MathML elements in the DOM must implement, and expose to scripts,
    // the following MathMLElement interface.

    // https://w3c.github.io/mathml-core/#mathml-elements-and-attributes
    // The term MathML element refers to any element in the MathML namespace.

    return create_element_on_heap<MathML::MathMLElement>(document, move(qualified_name));
}

// https://dom.spec.whatwg.org/#create-an-element-internal
template<typename Interface>
GC::Ref<Element> create_element_internal(Document& document, Interface interface, FlyString local_name, Optional<FlyString> namespace_, Optional<FlyString> prefix, CustomElementState custom_element_state, Optional<String> is_value, GC::Ptr<HTML::CustomElementRegistry> registry)
{
    // 1. Let element be a new element that implements interface, with namespace set to namespace, namespace prefix set
    //    to prefix, local name set to localName, custom element registry set to registry, custom element state set to
    //    state, custom element definition set to null, is value set to is, and node document set to document.
    auto qualified_name = QualifiedName { local_name, prefix, namespace_ };
    auto element = interface(document, qualified_name);
    element->set_custom_element_registry(registry);
    element->set_custom_element_state(custom_element_state);
    element->set_is_value(move(is_value));
    // NB: Document is set in the call to `interface()` above. Custom element definition is null by default.

    // 2. Assert: element’s attribute list is empty.
    VERIFY(!element->has_attributes());

    // 3. Return element.
    return element;
}

// https://dom.spec.whatwg.org/#concept-create-element
WebIDL::ExceptionOr<GC::Ref<Element>> create_element(Document& document, FlyString local_name, Optional<FlyString> namespace_, Optional<FlyString> prefix, Optional<String> is_value, bool synchronous_custom_elements_flag, Variant<GC::Ptr<HTML::CustomElementRegistry>, Default> initial_registry)
{
    // 1. Let result be null.
    GC::Ptr<Element> result;

    // 2. If registry is "default", then set registry to the result of looking up a custom element registry given
    //    document.
    GC::Ptr<HTML::CustomElementRegistry> registry = initial_registry.visit(
        [&document](Default const&) {
            return HTML::look_up_a_custom_element_registry(document);
        },
        [](GC::Ptr<HTML::CustomElementRegistry> pointer) {
            return pointer;
        });

    // 3. Let definition be the result of looking up a custom element definition given registry, namespace, localName,
    //    and is.
    auto definition = HTML::look_up_a_custom_element_definition(registry, namespace_, local_name, is_value);

    // 4. If definition is non-null, and definition’s name is not equal to its local name (i.e., definition represents
    //    a customized built-in element):
    if (definition && definition->name() != definition->local_name()) {
        // 1. Let interface be the element interface for localName and the HTML namespace.
        // 2. Set result to the result of creating an element internal given document, interface, localName, the HTML
        //    namespace, prefix, "undefined", is, and registry.
        result = create_element_internal(document, create_html_element, local_name, Namespace::HTML, prefix, CustomElementState::Undefined, is_value, registry);

        // 3. If synchronousCustomElements is true, then run this step while catching any exceptions:
        if (synchronous_custom_elements_flag) {
            // 1. Upgrade result using definition.
            auto upgrade_result = Bindings::upgrade_custom_element(*result, *definition);

            // If this step threw an exception exception:
            if (upgrade_result.is_throw_completion()) {
                // 1. Report exception for definition’s constructor’s corresponding JavaScript object’s associated
                //    realm’s global object.
                auto& window_or_worker = HTML::relevant_window_or_worker_global_scope(definition->constructor().callback);
                window_or_worker.report_an_exception(upgrade_result.error_value());

                // 2. Set result’s custom element state to "failed".
                result->set_custom_element_state(CustomElementState::Failed);
            }
        }

        // 4. Otherwise, enqueue a custom element upgrade reaction given result and definition.
        else {
            result->enqueue_a_custom_element_upgrade_reaction(*definition);
        }
    }

    // 5. Otherwise, if definition is non-null, then:
    else if (definition) {
        // 1. If synchronousCustomElements is true:
        if (synchronous_custom_elements_flag) {
            // 3. Run these steps while catching any exceptions:
            auto constructed_element = Bindings::construct_autonomous_custom_element(document, local_name, prefix, registry, *definition);

            // If any of these steps threw an exception, then:
            if (constructed_element.is_throw_completion()) {
                // 1. Report exception for definition’s constructor’s corresponding JavaScript object’s associated
                //    realm’s global object.
                auto& window_or_worker = HTML::relevant_window_or_worker_global_scope(definition->constructor().callback);
                window_or_worker.report_an_exception(constructed_element.error_value());

                // 2. Set result to the result of creating an element internal given document, HTMLUnknownElement,
                //    localName, the HTML namespace, prefix, "failed", null, and registry.
                result = create_element_internal(document, [](auto& document, auto qualified_name) { return create_element_on_heap<HTML::HTMLUnknownElement>(document, qualified_name); }, local_name, Namespace::HTML, prefix, CustomElementState::Failed, {}, registry);
            }

            else {
                result = constructed_element.release_value();
            }
        }

        // 2. Otherwise:
        else {
            // 1. Set result to the result of creating an element internal given document, HTMLElement, localName, the HTML
            //    namespace, prefix, "undefined", null, and registry.
            result = create_element_internal(document, [](auto& document, auto qualified_name) { return create_element_on_heap<HTML::HTMLElement>(document, qualified_name); }, local_name, Namespace::HTML, prefix, CustomElementState::Undefined, {}, registry);

            // 2. Enqueue a custom element upgrade reaction given result and definition.
            result->enqueue_a_custom_element_upgrade_reaction(*definition);
        }
    }

    // 6. Otherwise:
    else {
        // 1. Let interface be the element interface for localName and namespace.
        // 2. Set result to the result of creating an element internal given document, interface, localName, namespace,
        //    prefix, "uncustomized", is, and registry.
        if (namespace_ == Namespace::HTML) {
            result = create_element_internal(document, create_html_element, local_name, namespace_, prefix, CustomElementState::Uncustomized, is_value, registry);

            // 3. If namespace is the HTML namespace, and either localName is a valid custom element name or is is
            //    non-null, then set result’s custom element state to "undefined".
            if (HTML::is_valid_custom_element_name(local_name) || is_value.has_value())
                result->set_custom_element_state(CustomElementState::Undefined);
        }

        else if (namespace_ == Namespace::SVG) {
            result = create_element_internal(document, create_svg_element, local_name, namespace_, prefix, CustomElementState::Uncustomized, is_value, registry);
        }

        else if (namespace_ == Namespace::MathML) {
            result = create_element_internal(document, create_mathml_element, local_name, namespace_, prefix, CustomElementState::Uncustomized, is_value, registry);
        }

        else {
            // https://dom.spec.whatwg.org/#concept-element-interface
            // The element interface for any name and namespace is Element, unless stated otherwise.
            result = create_element_internal(document, [](auto& document, auto qualified_name) { return create_element_on_heap<DOM::Element>(document, qualified_name); }, local_name, namespace_, prefix, CustomElementState::Uncustomized, is_value, registry);
        }
    }

    // 7. Return result.
    return GC::Ref { *result };
}

}
