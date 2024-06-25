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
#include <LibWeb/DOMParsing/InnerHTML.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>
#include <LibWeb/Layout/BlockContainer.h>

namespace Web::DOM {

JS_DEFINE_ALLOCATOR(ShadowRoot);

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
        auto& events_first_invocation_target = verify_cast<Node>(*event.path().first().invocation_target);
        if (&events_first_invocation_target.root() == this)
            return nullptr;
    }

    return host();
}

// https://w3c.github.io/DOM-Parsing/#dom-innerhtml-innerhtml
WebIDL::ExceptionOr<String> ShadowRoot::inner_html() const
{
    return serialize_fragment(DOMParsing::RequireWellFormed::Yes);
}

// https://w3c.github.io/DOM-Parsing/#dom-innerhtml-innerhtml
WebIDL::ExceptionOr<void> ShadowRoot::set_inner_html(StringView markup)
{
    TRY(DOMParsing::inner_html_setter(*this, markup));

    set_needs_style_update(true);
    return {};
}

// https://html.spec.whatwg.org/#dom-element-gethtml
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

CSS::StyleSheetList& ShadowRoot::style_sheets()
{
    if (!m_style_sheets)
        m_style_sheets = CSS::StyleSheetList::create(document());
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

JS::NonnullGCPtr<WebIDL::ObservableArray> ShadowRoot::adopted_style_sheets() const
{
    if (!m_adopted_style_sheets)
        m_adopted_style_sheets = create_adopted_style_sheets_list(const_cast<Document&>(document()));
    return *m_adopted_style_sheets;
}

WebIDL::ExceptionOr<void> ShadowRoot::set_adopted_style_sheets(JS::Value new_value)
{
    if (!m_adopted_style_sheets)
        m_adopted_style_sheets = create_adopted_style_sheets_list(const_cast<Document&>(document()));

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

Vector<JS::NonnullGCPtr<Animations::Animation>> ShadowRoot::get_animations()
{
    Vector<JS::NonnullGCPtr<Animations::Animation>> relevant_animations;
    for_each_child_of_type<Element>([&](auto& child) {
        relevant_animations.extend(child.get_animations({ .subtree = true }));
        return IterationDecision::Continue;
    });
    return relevant_animations;
}

}
