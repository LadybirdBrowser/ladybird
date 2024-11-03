/*
 * Copyright (c) 2020, the SerenityOS developers.
 * Copyright (c) 2023, Bastiaan van der Plaat <bastiaan.v.d.plaat@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/HTMLMeterElementPrototype.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/ElementFactory.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/HTML/HTMLMeterElement.h>
#include <LibWeb/HTML/Numbers.h>
#include <LibWeb/Namespace.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(HTMLMeterElement);

HTMLMeterElement::HTMLMeterElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLMeterElement::~HTMLMeterElement() = default;

void HTMLMeterElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLMeterElement);
}

void HTMLMeterElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_meter_value_element);
}

// https://html.spec.whatwg.org/multipage/form-elements.html#concept-meter-actual
double HTMLMeterElement::value() const
{
    // If the value attribute is specified and a value could be parsed out of it, then that value is the candidate actual value. Otherwise, the candidate actual value is zero.
    double candidate_value = 0.0;
    if (auto value_string = get_attribute(HTML::AttributeNames::value); value_string.has_value()) {
        if (auto value = parse_floating_point_number(*value_string); value.has_value())
            candidate_value = *value;
    }

    // If the candidate actual value is less than the minimum value, then the actual value is the minimum value.
    // Otherwise, if the candidate actual value is greater than the maximum value, then the actual value is the maximum value.
    // Otherwise, the actual value is the candidate actual value.
    return clamp(candidate_value, min(), max());
}

WebIDL::ExceptionOr<void> HTMLMeterElement::set_value(double value)
{
    TRY(set_attribute(HTML::AttributeNames::value, String::number(value)));
    update_meter_value_element();
    return {};
}

// https://html.spec.whatwg.org/multipage/form-elements.html#concept-meter-minimum
double HTMLMeterElement::min() const
{
    // If the min attribute is specified and a value could be parsed out of it, then the minimum value is that value. Otherwise, the minimum value is zero.
    if (auto min_string = get_attribute(HTML::AttributeNames::min); min_string.has_value()) {
        if (auto min = parse_floating_point_number(*min_string); min.has_value())
            return *min;
    }
    return 0;
}

WebIDL::ExceptionOr<void> HTMLMeterElement::set_min(double value)
{
    TRY(set_attribute(HTML::AttributeNames::min, String::number(value)));
    update_meter_value_element();
    return {};
}

// https://html.spec.whatwg.org/multipage/form-elements.html#concept-meter-maximum
double HTMLMeterElement::max() const
{
    // If the max attribute is specified and a value could be parsed out of it, then the candidate maximum value is that value. Otherwise, the candidate maximum value is 1.0.
    double candidate_max = 1.0;
    if (auto max_string = get_attribute(HTML::AttributeNames::max); max_string.has_value()) {
        if (auto max = parse_floating_point_number(*max_string); max.has_value())
            candidate_max = *max;
    }

    // If the candidate maximum value is greater than or equal to the minimum value, then the maximum value is the candidate maximum value. Otherwise, the maximum value is the same as the minimum value.
    return AK::max(candidate_max, min());
}

WebIDL::ExceptionOr<void> HTMLMeterElement::set_max(double value)
{
    TRY(set_attribute(HTML::AttributeNames::max, String::number(value)));
    update_meter_value_element();
    return {};
}

// https://html.spec.whatwg.org/multipage/form-elements.html#concept-meter-low
double HTMLMeterElement::low() const
{
    // If the low attribute is specified and a value could be parsed out of it, then the candidate low boundary is that value. Otherwise, the candidate low boundary is the same as the minimum value.
    double candidate_low = min();
    if (auto low_string = get_attribute(HTML::AttributeNames::low); low_string.has_value()) {
        if (auto low = parse_floating_point_number(*low_string); low.has_value())
            candidate_low = *low;
    }

    // If the candidate low boundary is less than the minimum value, then the low boundary is the minimum value.
    // Otherwise, if the candidate low boundary is greater than the maximum value, then the low boundary is the maximum value.
    // Otherwise, the low boundary is the candidate low boundary.
    return clamp(candidate_low, min(), max());
}

WebIDL::ExceptionOr<void> HTMLMeterElement::set_low(double value)
{
    TRY(set_attribute(HTML::AttributeNames::low, String::number(value)));
    update_meter_value_element();
    return {};
}

// https://html.spec.whatwg.org/multipage/form-elements.html#concept-meter-high
double HTMLMeterElement::high() const
{
    // If the high attribute is specified and a value could be parsed out of it, then the candidate high boundary is that value. Otherwise, the candidate high boundary is the same as the maximum value.
    double candidate_high = max();
    if (auto high_string = get_attribute(HTML::AttributeNames::high); high_string.has_value()) {
        if (auto high = parse_floating_point_number(*high_string); high.has_value())
            candidate_high = *high;
    }

    // If the candidate high boundary is less than the low boundary, then the high boundary is the low boundary.
    // Otherwise, if the candidate high boundary is greater than the maximum value, then the high boundary is the maximum value.
    // Otherwise, the high boundary is the candidate high boundary.
    return clamp(candidate_high, low(), max());
}

WebIDL::ExceptionOr<void> HTMLMeterElement::set_high(double value)
{
    TRY(set_attribute(HTML::AttributeNames::high, String::number(value)));
    update_meter_value_element();
    return {};
}

// https://html.spec.whatwg.org/multipage/form-elements.html#concept-meter-optimum
double HTMLMeterElement::optimum() const
{
    // If the optimum attribute is specified and a value could be parsed out of it, then the candidate optimum point is that value. Otherwise, the candidate optimum point is the midpoint between the minimum value and the maximum value.
    double candidate_optimum = (max() + min()) / 2;
    if (auto optimum_string = get_attribute(HTML::AttributeNames::optimum); optimum_string.has_value()) {
        if (auto optimum = parse_floating_point_number(*optimum_string); optimum.has_value())
            candidate_optimum = *optimum;
    }

    // If the candidate optimum point is less than the minimum value, then the optimum point is the minimum value.
    // Otherwise, if the candidate optimum point is greater than the maximum value, then the optimum point is the maximum value.
    // Otherwise, the optimum point is the candidate optimum point.
    return clamp(candidate_optimum, min(), max());
}

WebIDL::ExceptionOr<void> HTMLMeterElement::set_optimum(double value)
{
    TRY(set_attribute(HTML::AttributeNames::optimum, String::number(value)));
    update_meter_value_element();
    return {};
}

void HTMLMeterElement::inserted()
{
    create_shadow_tree_if_needed();
}

void HTMLMeterElement::removed_from(DOM::Node*)
{
    set_shadow_root(nullptr);
}

void HTMLMeterElement::create_shadow_tree_if_needed()
{
    if (shadow_root())
        return;

    auto shadow_root = heap().allocate<DOM::ShadowRoot>(realm(), document(), *this, Bindings::ShadowRootMode::Closed);
    set_shadow_root(shadow_root);

    auto meter_bar_element = MUST(DOM::create_element(document(), HTML::TagNames::div, Namespace::HTML));
    meter_bar_element->set_use_pseudo_element(CSS::Selector::PseudoElement::Type::MeterBar);
    MUST(shadow_root->append_child(*meter_bar_element));

    m_meter_value_element = MUST(DOM::create_element(document(), HTML::TagNames::div, Namespace::HTML));
    MUST(meter_bar_element->append_child(*m_meter_value_element));
    update_meter_value_element();
}

void HTMLMeterElement::update_meter_value_element()
{
    if (!m_meter_value_element)
        return;

    // UA requirements for regions of the gauge:
    double value = this->value();
    double min = this->min();
    double max = this->max();
    double low = this->low();
    double high = this->high();
    double optimum = this->optimum();

    // If the optimum point is equal to the low boundary or the high boundary, or anywhere in between them, then the region between the low and high boundaries of the gauge must be treated as the optimum region, and the low and high parts, if any, must be treated as suboptimal.
    if (optimum >= low && optimum <= high) {
        if (value >= low && value <= high)
            m_meter_value_element->set_use_pseudo_element(CSS::Selector::PseudoElement::Type::MeterOptimumValue);
        else
            m_meter_value_element->set_use_pseudo_element(CSS::Selector::PseudoElement::Type::MeterSuboptimumValue);
    }
    // Otherwise, if the optimum point is less than the low boundary, then the region between the minimum value and the low boundary must be treated as the optimum region, the region from the low boundary up to the high boundary must be treated as a suboptimal region, and the remaining region must be treated as an even less good region.
    else if (optimum < low) {
        if (value >= low && value <= high)
            m_meter_value_element->set_use_pseudo_element(CSS::Selector::PseudoElement::Type::MeterSuboptimumValue);
        else
            m_meter_value_element->set_use_pseudo_element(CSS::Selector::PseudoElement::Type::MeterEvenLessGoodValue);
    }
    // Finally, if the optimum point is higher than the high boundary, then the situation is reversed; the region between the high boundary and the maximum value must be treated as the optimum region, the region from the high boundary down to the low boundary must be treated as a suboptimal region, and the remaining region must be treated as an even less good region.
    else {
        if (value >= high && value <= max)
            m_meter_value_element->set_use_pseudo_element(CSS::Selector::PseudoElement::Type::MeterOptimumValue);
        else if (value >= low && value <= high)
            m_meter_value_element->set_use_pseudo_element(CSS::Selector::PseudoElement::Type::MeterSuboptimumValue);
        else
            m_meter_value_element->set_use_pseudo_element(CSS::Selector::PseudoElement::Type::MeterEvenLessGoodValue);
    }

    double position = (value - min) / (max - min) * 100;
    MUST(m_meter_value_element->style_for_bindings()->set_property(CSS::PropertyID::Width, MUST(String::formatted("{}%", position))));
}
}
