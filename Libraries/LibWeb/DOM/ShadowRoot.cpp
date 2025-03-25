/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/ShadowRootPrototype.h>
#include <LibWeb/DOM/AdoptedStyleSheets.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/HTML/HTMLTemplateElement.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>
#include <LibWeb/Layout/BlockContainer.h>

namespace Web::DOM {

GC_DEFINE_ALLOCATOR(ShadowRoot);

ShadowRoot::ShadowRoot(Document& document, Element& host, Bindings::ShadowRootMode mode)
    : DocumentFragment(document)
    , m_mode(mode)
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
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(ShadowRoot);
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
WebIDL::ExceptionOr<String> ShadowRoot::inner_html() const
{
    return serialize_fragment(HTML::RequireWellFormed::Yes);
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#dom-shadowroot-innerhtml
WebIDL::ExceptionOr<void> ShadowRoot::set_inner_html(StringView value)
{
    // FIXME: 1. Let compliantString be the result of invoking the Get Trusted Type compliant string algorithm with TrustedHTML, this's relevant global object, the given value, "ShadowRoot innerHTML", and "script".

    // 2. Let context be this's host.
    auto context = this->host();
    VERIFY(context);

    // 3. Let fragment be the result of invoking the fragment parsing algorithm steps with context and compliantString. FIXME: Use compliantString instead of markup.
    auto fragment = TRY(context->parse_fragment(value));

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
WebIDL::ExceptionOr<void> ShadowRoot::set_html_unsafe(StringView html)
{
    // FIXME: 1. Let compliantHTML be the result of invoking the Get Trusted Type compliant string algorithm with TrustedHTML, this's relevant global object, html, "ShadowRoot setHTMLUnsafe", and "script".

    // 3. Unsafe set HTML given this, this's shadow host, and compliantHTML. FIXME: Use compliantHTML.
    TRY(unsafely_set_html(*this->host(), html));

    return {};
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
    visitor.visit(m_style_sheets);
    visitor.visit(m_adopted_style_sheets);
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

WebIDL::ExceptionOr<Vector<GC::Ref<Animations::Animation>>> ShadowRoot::get_animations()
{
    Vector<GC::Ref<Animations::Animation>> relevant_animations;
    TRY(for_each_child_of_type_fallible<Element>([&](auto& child) -> WebIDL::ExceptionOr<IterationDecision> {
        relevant_animations.extend(TRY(child.get_animations(Animations::GetAnimationsOptions { .subtree = true })));
        return IterationDecision::Continue;
    }));
    return relevant_animations;
}

ElementByIdMap& ShadowRoot::element_by_id() const
{
    if (!m_element_by_id)
        m_element_by_id = make<ElementByIdMap>();
    return *m_element_by_id;
}

}
