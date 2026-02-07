/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/ShadowRootPrototype.h>
#include <LibWeb/CSS/CSSStyleSheet.h>
#include <LibWeb/CSS/StyleSheetList.h>
#include <LibWeb/DOM/AdoptedStyleSheets.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/DocumentOrShadowRoot.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/DOM/SlotRegistry.h>
#include <LibWeb/HTML/HTMLSlotElement.h>
#include <LibWeb/HTML/HTMLTemplateElement.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>
#include <LibWeb/HTML/XMLSerializer.h>
#include <LibWeb/Layout/BlockContainer.h>
#include <LibWeb/TrustedTypes/RequireTrustedTypesForDirective.h>
#include <LibWeb/TrustedTypes/TrustedTypePolicy.h>

namespace Web::DOM {

GC_DEFINE_ALLOCATOR(ShadowRoot);

ShadowRoot::ShadowRoot(Document& document, Element& host, Bindings::ShadowRootMode mode)
    : DocumentFragment(document)
    , m_mode(mode)
    , m_style_scope(*this)
{
    document.register_shadow_root({}, *this);
    set_host(&host);
}

void ShadowRoot::finalize()
{
    Base::finalize();
    document().unregister_shadow_root({}, *this);
}

void ShadowRoot::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(ShadowRoot);
    Base::initialize(realm);
}

// https://dom.spec.whatwg.org/#dom-shadowroot-onslotchange
void ShadowRoot::set_onslotchange(WebIDL::CallbackType* event_handler)
{
    set_event_handler_attribute(HTML::EventNames::slotchange, event_handler);
}

// https://dom.spec.whatwg.org/#dom-shadowroot-onslotchange
WebIDL::CallbackType* ShadowRoot::onslotchange()
{
    return event_handler_attribute(HTML::EventNames::slotchange);
}

// https://dom.spec.whatwg.org/#ref-for-get-the-parent%E2%91%A6
EventTarget* ShadowRoot::get_parent(Event const& event)
{
    if (!event.composed()) {
        auto& events_first_invocation_target = as<Node>(*event.path().first().invocation_target);
        if (&events_first_invocation_target.root() == this)
            return nullptr;
    }

    return host();
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#dom-shadowroot-innerhtml
WebIDL::ExceptionOr<TrustedTypes::TrustedHTMLOrString> ShadowRoot::inner_html() const
{
    return TRY(serialize_fragment(HTML::RequireWellFormed::Yes));
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#dom-shadowroot-innerhtml
WebIDL::ExceptionOr<void> ShadowRoot::set_inner_html(TrustedTypes::TrustedHTMLOrString const& value)
{
    // 1. Let compliantString be the result of invoking the Get Trusted Type compliant string algorithm with
    //    TrustedHTML, this's relevant global object, the given value, "ShadowRoot innerHTML", and "script".
    auto const compliant_string = TRY(TrustedTypes::get_trusted_type_compliant_string(
        TrustedTypes::TrustedTypeName::TrustedHTML,
        HTML::relevant_global_object(*this),
        value,
        TrustedTypes::InjectionSink::ShadowRoot_innerHTML,
        TrustedTypes::Script.to_string()));

    // 2. Let context be this's host.
    auto context = this->host();
    VERIFY(context);

    // 3. Let fragment be the result of invoking the fragment parsing algorithm steps with context and compliantString.
    auto fragment = TRY(context->parse_fragment(compliant_string.to_utf8_but_should_be_ported_to_utf16()));

    // 4. Replace all with fragment within this.
    this->replace_all(fragment);

    // NOTE: We don't invalidate style & layout for <template> elements since they don't affect rendering.
    if (!is<HTML::HTMLTemplateElement>(*this)) {
        this->set_needs_style_update(true);

        if (this->is_connected()) {
            // NOTE: Since the DOM has changed, we have to rebuild the layout tree.
            this->document().invalidate_layout_tree(InvalidateLayoutTreeReason::ShadowRootSetInnerHTML);
        }
    }

    set_needs_style_update(true);
    return {};
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#dom-element-gethtml
WebIDL::ExceptionOr<String> ShadowRoot::get_html(GetHTMLOptions const& options) const
{
    // ShadowRoot's getHTML(options) method steps are to return the result
    // of HTML fragment serialization algorithm with this,
    // options["serializableShadowRoots"], and options["shadowRoots"].
    return HTML::HTMLParser::serialize_html_fragment(
        *this,
        options.serializable_shadow_roots ? HTML::HTMLParser::SerializableShadowRoots::Yes : HTML::HTMLParser::SerializableShadowRoots::No,
        options.shadow_roots);
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#dom-shadowroot-sethtmlunsafe
WebIDL::ExceptionOr<void> ShadowRoot::set_html_unsafe(TrustedTypes::TrustedHTMLOrString const& html)
{
    // 1. Let compliantHTML be the result of invoking the Get Trusted Type compliant string algorithm with
    //    TrustedHTML, this's relevant global object, html, "ShadowRoot setHTMLUnsafe", and "script".
    auto const compliant_html = TRY(TrustedTypes::get_trusted_type_compliant_string(
        TrustedTypes::TrustedTypeName::TrustedHTML,
        HTML::relevant_global_object(*this),
        html,
        TrustedTypes::InjectionSink::ShadowRoot_setHTMLUnsafe,
        TrustedTypes::Script.to_string()));

    // 2. Unsafely set HTML given this, this's shadow host, and compliantHTML.
    TRY(unsafely_set_html(*this->host(), compliant_html.to_utf8_but_should_be_ported_to_utf16()));

    return {};
}

GC::Ptr<Element> ShadowRoot::active_element()
{
    return calculate_active_element(*this);
}

CSS::StyleSheetList& ShadowRoot::style_sheets()
{
    if (!m_style_sheets)
        m_style_sheets = CSS::StyleSheetList::create(*this);
    return *m_style_sheets;
}

CSS::StyleSheetList const& ShadowRoot::style_sheets() const
{
    return const_cast<ShadowRoot*>(this)->style_sheets();
}

void ShadowRoot::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    m_style_scope.visit_edges(visitor);
    visitor.visit(m_style_sheets);
    visitor.visit(m_adopted_style_sheets);
    for (auto const& [key, elements] : m_part_element_map) {
        for (auto const& element : elements)
            element.visit(visitor);
    }
}

GC::Ref<WebIDL::ObservableArray> ShadowRoot::adopted_style_sheets() const
{
    if (!m_adopted_style_sheets)
        m_adopted_style_sheets = create_adopted_style_sheets_list(const_cast<ShadowRoot&>(*this));
    return *m_adopted_style_sheets;
}

WebIDL::ExceptionOr<void> ShadowRoot::set_adopted_style_sheets(JS::Value new_value)
{
    if (!m_adopted_style_sheets)
        m_adopted_style_sheets = create_adopted_style_sheets_list(*this);

    m_adopted_style_sheets->clear();
    auto iterator_record = TRY(get_iterator(vm(), new_value, JS::IteratorHint::Sync));
    while (true) {
        auto next = TRY(iterator_step_value(vm(), iterator_record));
        if (!next.has_value())
            break;
        TRY(m_adopted_style_sheets->append(*next));
    }

    return {};
}

void ShadowRoot::for_each_css_style_sheet(Function<void(CSS::CSSStyleSheet&)>&& callback) const
{
    for (auto& style_sheet : style_sheets().sheets())
        callback(*style_sheet);

    if (m_adopted_style_sheets) {
        m_adopted_style_sheets->for_each<CSS::CSSStyleSheet>([&](auto& style_sheet) {
            callback(style_sheet);
        });
    }
}

void ShadowRoot::for_each_active_css_style_sheet(Function<void(CSS::CSSStyleSheet&)> const& callback) const
{
    for (auto& style_sheet : style_sheets().sheets()) {
        if (!style_sheet->disabled())
            callback(*style_sheet);
    }

    if (m_adopted_style_sheets) {
        m_adopted_style_sheets->for_each<CSS::CSSStyleSheet>([&](auto& style_sheet) {
            if (!style_sheet.disabled())
                callback(style_sheet);
        });
    }
}

WebIDL::ExceptionOr<Vector<GC::Ref<Animations::Animation>>> ShadowRoot::get_animations()
{
    document().update_style();
    return calculate_get_animations(*this);
}

ElementByIdMap& ShadowRoot::element_by_id() const
{
    if (!m_element_by_id)
        m_element_by_id = make<ElementByIdMap>();
    return *m_element_by_id;
}

void ShadowRoot::register_slot(HTML::HTMLSlotElement& slot)
{
    if (!m_slot_registry)
        m_slot_registry = make<SlotRegistry>();
    m_slot_registry->add(slot);
}

void ShadowRoot::unregister_slot(HTML::HTMLSlotElement& slot)
{
    if (m_slot_registry)
        m_slot_registry->remove(slot);
}

GC::Ptr<HTML::HTMLSlotElement> ShadowRoot::first_slot_with_name(FlyString const& name) const
{
    if (!m_slot_registry)
        return nullptr;
    return m_slot_registry->first_slot_with_name(name);
}

// https://drafts.csswg.org/css-shadow-1/#shadow-root-part-element-map
ShadowRoot::PartElementMap const& ShadowRoot::part_element_map() const
{
    // FIXME: dom_tree_version() is crude and invalidates more than necessary.
    //        Come up with a smarter way of invalidating this if it turns out to be slow.
    if (m_dom_tree_version_when_calculated_part_element_map < document().dom_tree_version()) {
        const_cast<ShadowRoot*>(this)->calculate_part_element_map();
        m_dom_tree_version_when_calculated_part_element_map = document().dom_tree_version();
    }
    return m_part_element_map;
}

// https://drafts.csswg.org/css-shadow-1/#calculate-the-part-element-map
void ShadowRoot::calculate_part_element_map()
{
    // To calculate the part element map of a shadow root, outerRoot:

    m_part_element_map.clear();

    // 1. For each descendant el within outerRoot:
    for_each_in_subtree_of_type<Element>([this](Element const& element) {
        // 1. For each name in el’s part name list, append el to outerRoot’s part element map[name].
        for (auto const& name : element.part_names())
            m_part_element_map.ensure(name).set({ const_cast<Element&>(element), {} });

        // FIXME: The rest of this concerns forwarded part names, which we don't implement yet.

        // 2. If el is a shadow host itself then let innerRoot be its shadow root.
        // 3. Calculate innerRoot’s part element map.
        // 4. For each innerName/outerName in el’s forwarded part name list:
        {
            // 1. If innerName is an ident:
            {
                // 1. Let innerParts be innerRoot’s part element map[innerName]
                // 2. Append the elements in innerParts to outerRoot’s part element map[outerName]
            }
            // 2. If innerName is a pseudo-element name:
            {
                // 1. Append innerRoot’s pseudo-element(s) with that name to outerRoot’s part element map[outerName].
            }
        }
        return TraversalDecision::Continue;
    });
}

}
