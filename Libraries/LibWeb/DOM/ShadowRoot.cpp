/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/Runtime/Iterator.h>
#include <LibWeb/Bindings/Sanitizer.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleSheetList.h>
#include <LibWeb/CSS/StyleSheetState.h>
#include <LibWeb/DOM/AdoptedStyleSheets.h>
#include <LibWeb/DOM/BindingsGlue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/DocumentOrShadowRoot.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/DOM/SlotRegistry.h>
#include <LibWeb/DOM/Utils.h>
#include <LibWeb/HTML/AttributeNames.h>
#include <LibWeb/HTML/CustomElements/CustomElementRegistry.h>
#include <LibWeb/HTML/HTMLInsertion.h>
#include <LibWeb/HTML/HTMLSlotElement.h>
#include <LibWeb/HTML/HTMLTemplateElement.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>
#include <LibWeb/HTML/RadioButtonGroupRegistry.h>
#include <LibWeb/HTML/XMLSerializer.h>
#include <LibWeb/Layout/LayoutRustFFI.h>
#include <LibWeb/Layout/NodeArena.h>
#include <LibWeb/TrustedTypes/RequireTrustedTypesForDirective.h>
#include <LibWeb/TrustedTypes/TrustedTypePolicy.h>

namespace Web::DOM {

GC_DEFINE_ALLOCATOR(ShadowRoot);

GC::Ref<ShadowRoot> ShadowRoot::create(Document& document, Element& host, ShadowRootMode mode)
{
    return GC::Heap::the().allocate<ShadowRoot>(document, host, mode);
}

ShadowRoot::ShadowRoot(Document& document, Element& host, ShadowRootMode mode)
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
    document().remove_shadow_root_needing_slot_assignment({}, *this);
}

void ShadowRoot::adopted_from(Document& old_document)
{
    Base::adopted_from(old_document);

    // The document tracks its shadow roots for document-wide walks (media invalidation, user style changes,
    // tree-scoped name lookups), so adoption has to move this shadow root to the new document's list.
    old_document.unregister_shadow_root({}, *this);
    document().register_shadow_root({}, *this);
    if (!m_slots_needing_assignment.is_empty()) {
        old_document.remove_shadow_root_needing_slot_assignment({}, *this);
        document().add_shadow_root_needing_slot_assignment({}, *this);
    }

    // Identities belong to one document's engine, so the root and its scope have to be minted
    // again in the new one. Its sheets are re-adopted through the CSSOM, which is what attaches
    // them to the new identities. Attaching a sheet names even a disconnected root, so the old
    // document's node table may still hold it.
    old_document.style_computer().unregister_style_node(style_node_id());
    set_style_node_id(0);
    m_style_engine_tree_scope = 0;
}

void ShadowRoot::set_style_node_id(CSS::StyleNodeID style_node_id)
{
    m_style_node_id = style_node_id;
    // The layout arena keys layout tree update marks by identity alone, so one handed to this root holds none, whatever
    // the node that held it before left behind.
    if (auto* arena = document().layout_node_arena_if_created(); arena && style_node_id != 0)
        Layout::RustFFI::render_state_clear_layout_tree_update_marks(arena->host(), style_node_id.value());
    // The style engine keeps the mark of the children that explicitly inherit by identity too.
    if (style_node_id != 0)
        publish_children_explicitly_inherit_mark();
}

// https://fullscreen.spec.whatwg.org/#dom-document-fullscreenelement
GC::Ptr<Element> ShadowRoot::retargeted_fullscreen_element() const
{
    // 1. If this is a shadow root and its host is not connected, then return null.
    if (!host() || !host()->is_connected())
        return nullptr;

    // 2. Let candidate be the result of retargeting fullscreen element against this.
    // NB: ShadowRoot does not have it's own top layer. But the algorithm says to get the fullscreen element from the
    //     top layer, so it's grabbed from this' document.
    auto* candidate = retarget(document().fullscreen_element().ptr(), const_cast<ShadowRoot*>(this));
    if (!candidate)
        return nullptr;

    // 3. If candidate and this are in the same tree, then return candidate.
    if (auto* retargeted_element = as<Element>(candidate); &retargeted_element->root() == &root())
        return retargeted_element;

    // 4. Return null.
    return nullptr;
}

// https://w3c.github.io/picture-in-picture/#dom-documentorshadowroot-pictureinpictureelement
GC::Ptr<Element> ShadowRoot::retargeted_picture_in_picture_element() const
{
    // 1. If this is a shadow root and its host is not connected, return null and abort these steps.
    if (!host() || !host()->is_connected())
        return nullptr;

    // 2. Let candidate be the result of retargeting Picture-in-Picture element against this.
    // NB: Only a document has its Picture-in-Picture element set, so this retargets the one of this' document.
    auto* candidate = as_if<Element>(retarget(document().picture_in_picture_element().ptr(), const_cast<ShadowRoot*>(this)));
    if (!candidate)
        return nullptr;

    // 3. If candidate and this are in the same tree, return candidate and abort these steps.
    if (&candidate->root() == &root())
        return candidate;

    // 4. Return null.
    return nullptr;
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
WebIDL::ExceptionOr<Utf16String> ShadowRoot::inner_html() const
{
    return TRY(serialize_fragment(HTML::RequireWellFormed::Yes));
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#dom-shadowroot-innerhtml
WebIDL::ExceptionOr<void> ShadowRoot::set_inner_html(Utf16View html, GC::Ptr<TrustedTypes::TrustedHTMLParserOptions> trusted_options)
{
    // 2. Let context be this's host.
    auto context = this->host();
    VERIFY(context);

    // 3. Let fragment be the result of invoking the fragment parsing algorithm steps with context and compliantString.
    // Keep the shadow root as the fragment parsing target so custom element
    // creation uses this shadow tree's registry. The host is still used as
    // the parser context by the fragment parsing algorithm.
    auto fragment = TRY(Element::parse_fragment(Variant<GC::Ref<Element>, GC::Ref<DocumentFragment>> { *this }, html, HTML::ParserScriptingMode::Inert, HTML::FragmentFastPath::Allow, trusted_options));

    // 4. Replace all with fragment within this.
    this->replace_all(fragment);

    // NOTE: We don't invalidate style & layout for <template> elements since they don't affect rendering.
    if (!is<HTML::HTMLTemplateElement>(*this)) {
        if (this->is_connected()) {
            // NOTE: Since the DOM has changed, we have to rebuild this shadow root's layout subtree.
            this->set_needs_layout_tree_update(true, SetNeedsLayoutTreeUpdateReason::ShadowRootSetInnerHTML);
        }
    }

    return {};
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#dom-element-gethtml
WebIDL::ExceptionOr<Utf16String> ShadowRoot::get_html(HTMLSerializationOptions const& options) const
{
    // ShadowRoot's getHTML(options) method steps are to return the result
    // of HTML fragment serialization algorithm with this,
    // options["serializableShadowRoots"], and options["shadowRoots"].
    return HTML::HTMLParser::serialize_html_fragment(
        *this,
        options.serializable_shadow_roots ? HTML::HTMLParser::SerializableShadowRoots::Yes : HTML::HTMLParser::SerializableShadowRoots::No,
        options.shadow_roots);
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#dom-shadowroot-sethtml
WebIDL::ExceptionOr<void> ShadowRoot::set_html(Utf16String const& html, Bindings::SetHTMLOptions const& options)
{
    // ShadowRoot's setHTML(html, options) method steps are to filter and set HTML given this, html, options, and Safe.
    return HTML::filter_and_set_html(GC::Ref<DocumentFragment> { *this }, html.utf16_view(), &options, HTML::FragmentParserMode::Safe);
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#dom-shadowroot-appendhtml
WebIDL::ExceptionOr<void> ShadowRoot::append_html(Utf16String const& html, Bindings::SetHTMLOptions const& options)
{
    // ShadowRoot's appendHTML(html, options) method steps are to filter and pre-insert HTML given this, null, html,
    // options, and Safe.
    return HTML::filter_and_pre_insert_html(GC::Ref<DocumentFragment> { *this }, nullptr, html.utf16_view(), &options, HTML::FragmentParserMode::Safe);
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#dom-shadowroot-prependhtml
WebIDL::ExceptionOr<void> ShadowRoot::prepend_html(Utf16String const& html, Bindings::SetHTMLOptions const& options)
{
    // ShadowRoot's prependHTML(html, options) method steps are to filter and pre-insert HTML given this, this's first
    // child, html, options, and Safe.
    return HTML::filter_and_pre_insert_html(GC::Ref<DocumentFragment> { *this }, first_child(), html.utf16_view(), &options, HTML::FragmentParserMode::Safe);
}

GC::Ptr<Element> ShadowRoot::active_element()
{
    return calculate_active_element(*this);
}

CSS::StyleSheetList& ShadowRoot::style_sheets()
{
    if (!m_style_sheets)
        m_style_sheets = CSS::StyleSheetList::create(m_style_scope);
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
    m_anchor_name_map.visit_edges(visitor);
    for (auto const& [key, elements] : m_part_element_map) {
        for (auto const& element : elements)
            element.visit(visitor);
    }
    visitor.visit(m_custom_element_registry);
    visitor.visit(m_radio_button_group_registry);
    visitor.visit(m_slots_needing_assignment);
}

HTML::RadioButtonGroupRegistry& ShadowRoot::ensure_radio_button_group_registry()
{
    if (!m_radio_button_group_registry)
        m_radio_button_group_registry = heap().allocate<HTML::RadioButtonGroupRegistry>();
    return *m_radio_button_group_registry;
}

GC::Ref<WebIDL::ObservableArray> ShadowRoot::adopted_style_sheets() const
{
    if (!m_adopted_style_sheets)
        m_adopted_style_sheets = create_adopted_style_sheets_list(const_cast<ShadowRoot&>(*this));
    return *m_adopted_style_sheets;
}

void ShadowRoot::for_each_css_style_sheet(Function<void(CSS::StyleSheetState&)>&& callback) const
{
    for (auto& style_sheet : m_style_scope.style_sheets())
        callback(*style_sheet);

    if (m_adopted_style_sheets) {
        for_each_adopted_style_sheet(*m_adopted_style_sheets, [&](auto& style_sheet) {
            callback(style_sheet);
        });
    }
}

void ShadowRoot::for_each_active_css_style_sheet(Function<void(CSS::StyleSheetState&)> const& callback) const
{
    for (auto& style_sheet : m_style_scope.style_sheets()) {
        if (!style_sheet->disabled())
            callback(*style_sheet);
    }

    if (m_adopted_style_sheets) {
        for_each_adopted_style_sheet(*m_adopted_style_sheets, [&](auto& style_sheet) {
            if (!style_sheet.disabled())
                callback(style_sheet);
        });
    }
}

WebIDL::ExceptionOr<GC::RootVector<GC::Ref<Animations::Animation>>> ShadowRoot::get_animations()
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

GC::Ptr<HTML::HTMLSlotElement> ShadowRoot::first_slot_with_name(Utf16View name) const
{
    if (!m_slot_registry)
        return nullptr;
    return m_slot_registry->first_slot_with_name(name);
}

void ShadowRoot::set_needs_slot_assignment(HTML::HTMLSlotElement& slot)
{
    if (m_slots_needing_assignment.contains_slow(GC::Ref { slot }))
        return;
    if (m_slots_needing_assignment.is_empty())
        document().add_shadow_root_needing_slot_assignment({}, *this);
    m_slots_needing_assignment.append(slot);
}

void ShadowRoot::assign_slottables_if_needed()
{
    document().remove_shadow_root_needing_slot_assignment({}, *this);
    GC::RootVector slots { move(m_slots_needing_assignment) };
    for (auto slot : slots)
        assign_slottables(slot, SignalSlotChange::No);
}

// https://drafts.csswg.org/css-shadow-1/#shadow-root-part-element-map
ShadowRoot::PartElementMap const& ShadowRoot::part_element_map() const
{
    // FIXME: dom_tree_version() is crude and invalidates more than necessary.
    //        Come up with a smarter way of invalidating this if it turns out to be slow.
    if (m_dom_tree_version_when_calculated_part_element_map != dom_tree_version()) {
        const_cast<ShadowRoot*>(this)->calculate_part_element_map();
        m_dom_tree_version_when_calculated_part_element_map = dom_tree_version();
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

        // 2. If el is a shadow host itself then let innerRoot be its shadow root.
        if (element.is_shadow_host()) {
            auto inner_root = element.shadow_root();

            // 3. Calculate innerRoot’s part element map.
            auto const& inner_map = inner_root->part_element_map();

            // 4. For each innerName/outerName in el’s forwarded part name list:
            element.for_each_exported_part([&](Utf16View inner_name_view, Utf16View outer_name_view) {
                // 1. If innerName is an ident:
                if (auto it = inner_map.find(inner_name_view); it != inner_map.end()) {
                    // 1. Let innerParts be innerRoot’s part element map[innerName]
                    // 2. Append the elements in innerParts to outerRoot’s part element map[outerName]
                    if (inner_name_view == outer_name_view) {
                        for (auto const& abstract_el : it->value)
                            m_part_element_map.ensure(it->key).set(abstract_el);
                    } else {
                        auto outer_name = Utf16FlyString::from_utf16(outer_name_view);
                        for (auto const& abstract_el : it->value)
                            m_part_element_map.ensure(outer_name).set(abstract_el);
                    }
                }
                // FIXME: 2. If innerName is a pseudo-element name, append innerRoot’s
                //        pseudo-element(s) with that name to outerRoot’s part element map[outerName].
            });
        }

        return TraversalDecision::Continue;
    });
}

// https://dom.spec.whatwg.org/#dom-documentorshadowroot-customelementregistry
GC::Ptr<HTML::CustomElementRegistry> ShadowRoot::custom_element_registry() const
{
    // 1. If this is a document, then return this’s custom element registry.
    // NB: Impossible.

    // 2. Assert: this is a ShadowRoot node.
    // 3. Return this’s custom element registry.
    return m_custom_element_registry;
}

}

namespace Web::Bindings {

WebIDL::ExceptionOr<TrustedTypes::TrustedHTMLOrString> inner_html(DOM::ShadowRoot& shadow_root)
{
    return TRY(shadow_root.inner_html());
}

WebIDL::ExceptionOr<void> set_inner_html(DOM::ShadowRoot& shadow_root, TrustedTypes::TrustedHTMLOrString const& value)
{
    // 1. Let (compliantString, compliantOptions) be the result of invoking the get trusted type compliant input
    //    algorithm with this's relevant global object, the given value, a new SetHTMLUnsafeOptions dictionary,
    //    "ShadowRoot innerHTML", this's node document's type, and false.
    auto const [compliant_string, trusted_options] = TRY(HTML::get_trusted_type_compliant_input_for_legacy_sink(shadow_root, value, TrustedTypes::InjectionSink::ShadowRoot_innerHTML));

    return shadow_root.set_inner_html(compliant_string, trusted_options);
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#dom-shadowroot-sethtmlunsafe
WebIDL::ExceptionOr<void> set_html_unsafe(DOM::ShadowRoot& shadow_root, Variant<GC::Ref<TrustedTypes::TrustedHTML>, Utf16String> const& html, Variant<SetHTMLUnsafeOptions, GC::Ref<TrustedTypes::TrustedHTMLParserOptions>> const& options)
{
    // 1. Let (compliantHTML, compliantOptions) be the result of invoking the get trusted type compliant input algorithm
    //    with this's relevant global object, html, options, and "ShadowRoot setHTMLUnsafe".
    auto const [compliant_html, compliant_options] = TRY(TrustedTypes::get_trusted_type_compliant_input(
        HTML::relevant_global_object(shadow_root),
        html,
        options,
        TrustedTypes::InjectionSink::ShadowRoot_setHTMLUnsafe));

    // 2. Filter and set HTML given this, compliantHTML, compliantOptions, and Unsafe.
    return HTML::filter_and_set_html(GC::Ref<DOM::DocumentFragment> { shadow_root }, compliant_html.utf16_view(), HTML::to_sanitizer_options(compliant_options), HTML::FragmentParserMode::Unsafe);
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#dom-shadowroot-appendhtmlunsafe
WebIDL::ExceptionOr<void> append_html_unsafe(DOM::ShadowRoot& shadow_root, Variant<GC::Ref<TrustedTypes::TrustedHTML>, Utf16String> const& html, Variant<SetHTMLUnsafeOptions, GC::Ref<TrustedTypes::TrustedHTMLParserOptions>> const& options)
{
    // 1. Let (compliantHTML, compliantOptions) be the result of invoking the get trusted type compliant input algorithm
    //    with this's relevant global object, html, options, and "ShadowRoot appendHTMLUnsafe".
    auto const [compliant_html, compliant_options] = TRY(TrustedTypes::get_trusted_type_compliant_input(
        HTML::relevant_global_object(shadow_root),
        html,
        options,
        TrustedTypes::InjectionSink::ShadowRoot_appendHTMLUnsafe));

    // 2. Filter and pre-insert HTML given this, null, compliantHTML, compliantOptions, and Unsafe.
    return HTML::filter_and_pre_insert_html(GC::Ref<DOM::DocumentFragment> { shadow_root }, nullptr, compliant_html.utf16_view(), HTML::to_sanitizer_options(compliant_options), HTML::FragmentParserMode::Unsafe);
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#dom-shadowroot-prependhtmlunsafe
WebIDL::ExceptionOr<void> prepend_html_unsafe(DOM::ShadowRoot& shadow_root, Variant<GC::Ref<TrustedTypes::TrustedHTML>, Utf16String> const& html, Variant<SetHTMLUnsafeOptions, GC::Ref<TrustedTypes::TrustedHTMLParserOptions>> const& options)
{
    // 1. Let (compliantHTML, compliantOptions) be the result of invoking the get trusted type compliant input algorithm
    //    with this's relevant global object, html, options, and "ShadowRoot prependHTMLUnsafe".
    auto const [compliant_html, compliant_options] = TRY(TrustedTypes::get_trusted_type_compliant_input(
        HTML::relevant_global_object(shadow_root),
        html,
        options,
        TrustedTypes::InjectionSink::ShadowRoot_prependHTMLUnsafe));

    // 2. Filter and pre-insert HTML given this, this's first child, compliantHTML, compliantOptions, and Unsafe.
    return HTML::filter_and_pre_insert_html(GC::Ref<DOM::DocumentFragment> { shadow_root }, shadow_root.first_child(), compliant_html.utf16_view(), HTML::to_sanitizer_options(compliant_options), HTML::FragmentParserMode::Unsafe);
}

GC::Ref<CSS::StyleSheetList> style_sheets(DOM::ShadowRoot& shadow_root)
{
    return shadow_root.style_sheets();
}

JS::Value adopted_style_sheets(DOM::ShadowRoot& shadow_root)
{
    return DOM::AdoptedStyleSheetsAccess::adopted_style_sheets(shadow_root)->array_object();
}

WebIDL::ExceptionOr<void> set_adopted_style_sheets(DOM::ShadowRoot& shadow_root, JS::Value new_value)
{
    auto adopted_style_sheets = DOM::AdoptedStyleSheetsAccess::adopted_style_sheets(shadow_root);
    adopted_style_sheets->clear();

    auto iterator_record = TRY(JS::get_iterator(shadow_root.vm(), new_value, JS::IteratorHint::Sync));
    while (true) {
        auto next = TRY(JS::iterator_step_value(shadow_root.vm(), iterator_record));
        if (!next.has_value())
            break;
        TRY(adopted_style_sheets->append(*next));
    }

    return {};
}

}
