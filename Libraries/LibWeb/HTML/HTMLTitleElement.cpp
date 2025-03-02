/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/Timer.h>
#include <LibWeb/Bindings/HTMLTitleElementPrototype.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/HTMLTitleElement.h>
#include <LibWeb/HTML/TraversableNavigable.h>
#include <LibWeb/Page/Page.h>

static constexpr u32 timer_throttle_ms = 5;
static constexpr u32 timer_unconditional_update_ms = 15;

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLTitleElement);

HTMLTitleElement::HTMLTitleElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
    m_throttle_update_timer = Core::Timer::create_single_shot(timer_throttle_ms, [this] {
        this->propagate_title_update();
    });
}

HTMLTitleElement::~HTMLTitleElement() = default;

void HTMLTitleElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLTitleElement);
}

void HTMLTitleElement::children_changed(ChildrenChangedMetadata const* metadata)
{
    HTMLElement::children_changed(metadata);

    auto navigable = this->navigable();
    if (navigable && navigable->is_traversable()) {
        consider_propagate_title_update();
    }
}

// https://html.spec.whatwg.org/multipage/semantics.html#dom-title-text
String HTMLTitleElement::text() const
{
    // The text attribute's getter must return this title element's child text content.
    return child_text_content();
}

// https://html.spec.whatwg.org/multipage/semantics.html#dom-title-text
void HTMLTitleElement::set_text(String const& value)
{
    // The text attribute's setter must string replace all with the given value within this title element.
    string_replace_all(value);
}

void HTMLTitleElement::consider_propagate_title_update()
{
    if (text().is_empty()) {
        m_throttle_update_timer->start();
        return;
    }

    i64 const time_now = MonotonicTime::now_coarse().milliseconds();

    if (m_first_block_at_ms == 0) {
        m_first_block_at_ms = time_now;
    }

    if (time_now - m_first_block_at_ms > timer_unconditional_update_ms) {
        // we've exceeded the max ms without a title update, we'll propagate it immediately
        propagate_title_update();
        return;
    }

    // start a throttling timer. This is to not spam every update to the front-end.
    // furthermore, the first update is removing the title, so it gives us an erroneous empty title.
    m_throttle_update_timer->restart();
}

void HTMLTitleElement::propagate_title_update()
{
    m_first_block_at_ms = 0;
    m_throttle_update_timer->stop();
    navigable()->traversable_navigable()->page().client().page_did_change_title(document().title().to_byte_string());
}

}
