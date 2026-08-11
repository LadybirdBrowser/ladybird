/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Invalidation/ElementStateInvalidator.h>
#include <LibWeb/CSS/PseudoClass.h>
#include <LibWeb/CSS/SelectorMatching.h>
#include <LibWeb/CSS/StyleEngineInput.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/HTML/HTMLMediaElement.h>
#include <LibWeb/HTML/HTMLMeterElement.h>

namespace Web::CSS::Invalidation {

void invalidate_style_after_active_state_change(DOM::Element& element, bool is_active)
{
    record_element_state_changed(element, PseudoClass::Active, is_active);
}

void invalidate_style_after_modal_state_change(DOM::Element& element, bool is_modal)
{
    record_element_state_changed(element, PseudoClass::Modal, is_modal);
}

void invalidate_style_after_open_state_change(DOM::Element& element, bool is_open)
{
    record_element_state_changed(element, PseudoClass::Open, is_open);
}

void invalidate_style_after_option_selected_state_change(DOM::Element& element, bool is_selected)
{
    // An option's selectedness is what `:checked` tests on it, and `:unchecked` is its negation.
    record_element_state_changed(element, PseudoClass::Checked, is_selected);
    record_element_state_changed(element, PseudoClass::Unchecked, !is_selected);
}

void invalidate_style_after_input_open_state_change(DOM::Element& element, bool is_open)
{
    record_element_state_changed(element, PseudoClass::Open, is_open);
}

void invalidate_style_after_select_open_state_change(DOM::Element& element, bool is_open)
{
    record_element_state_changed(element, PseudoClass::Open, is_open);
}

void invalidate_style_after_indeterminate_state_change(DOM::Element& element, bool)
{
    record_element_state_changed(element, PseudoClass::Indeterminate, SelectorMatching::element_matches_state(element, PseudoClass::Indeterminate));
}

void invalidate_style_after_default_state_change(DOM::Element& element, bool was_default)
{
    auto is_default = SelectorMatching::element_matches_state(element, PseudoClass::Default);
    if (was_default == is_default)
        return;
    record_element_state_changed(element, PseudoClass::Default, is_default);
}

void invalidate_style_after_read_write_state_change(DOM::Element& element, bool was_read_write)
{
    auto is_read_write = SelectorMatching::element_matches_state(element, PseudoClass::ReadWrite);
    if (was_read_write == is_read_write)
        return;
    record_element_state_changed(element, PseudoClass::ReadOnly, !is_read_write);
    record_element_state_changed(element, PseudoClass::ReadWrite, is_read_write);
}

void invalidate_style_after_media_muted_state_change(HTML::HTMLMediaElement& element, bool is_muted)
{
    record_element_state_changed(element, PseudoClass::Muted, is_muted);
}

void invalidate_style_after_media_paused_state_change(HTML::HTMLMediaElement& element, bool is_paused)
{
    record_element_state_changed(element, PseudoClass::Paused, is_paused);
    record_element_state_changed(element, PseudoClass::Playing, !is_paused);
}

void invalidate_style_after_media_seeking_state_change(HTML::HTMLMediaElement& element, bool is_seeking)
{
    record_element_state_changed(element, PseudoClass::Seeking, is_seeking);
}

void invalidate_style_after_media_ready_state_change(HTML::HTMLMediaElement& element, bool was_buffering)
{
    auto is_buffering = element.blocked();
    if (was_buffering != is_buffering)
        record_element_state_changed(element, PseudoClass::Buffering, is_buffering);
}

void invalidate_style_after_meter_value_state_change(HTML::HTMLMeterElement& element)
{
    auto value_state = element.value_state();
    record_element_state_changed(element, PseudoClass::OptimalValue, value_state == HTML::HTMLMeterElement::ValueState::Optimal);
    record_element_state_changed(element, PseudoClass::SuboptimalValue, value_state == HTML::HTMLMeterElement::ValueState::Suboptimal);
    record_element_state_changed(element, PseudoClass::EvenLessGoodValue, value_state == HTML::HTMLMeterElement::ValueState::EvenLessGood);
    record_element_state_changed(element, PseudoClass::HighValue, element.value() > element.high());
    record_element_state_changed(element, PseudoClass::LowValue, element.value() < element.low());
}

}
