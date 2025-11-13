/*
 * Copyright (c) 2023, Jonah Shafran <jonahshafran@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Bindings/MathMLElementPrototype.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/StyleValues/IntegerStyleValue.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/MathDepthStyleValue.h>
#include <LibWeb/HTML/Numbers.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>
#include <LibWeb/Layout/MathMLBox.h>
#include <LibWeb/MathML/AttributeNames.h>
#include <LibWeb/MathML/MathMLElement.h>
#include <LibWeb/MathML/TagNames.h>

namespace Web::MathML {

GC_DEFINE_ALLOCATOR(MathMLElement);

MathMLElement::~MathMLElement() = default;

MathMLElement::MathMLElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : DOM::Element(document, move(qualified_name))
{
}

void MathMLElement::attribute_changed(FlyString const& local_name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_)
{
    Base::attribute_changed(local_name, old_value, value, namespace_);
    HTMLOrSVGElement::attribute_changed(local_name, old_value, value, namespace_);
}

WebIDL::ExceptionOr<void> MathMLElement::cloned(DOM::Node& node, bool clone_children) const
{
    TRY(Base::cloned(node, clone_children));
    TRY(HTMLOrSVGElement::cloned(node, clone_children));
    return {};
}

void MathMLElement::inserted()
{
    Base::inserted();
    HTMLOrSVGElement::inserted();
}

void MathMLElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(MathMLElement);
    Base::initialize(realm);
}

Optional<ARIA::Role> MathMLElement::default_role() const
{
    // https://www.w3.org/TR/html-aria/#el-math
    if (local_name() == TagNames::math)
        return ARIA::Role::math;
    return {};
}

GC::Ptr<Layout::Node> MathMLElement::create_layout_node(GC::Ref<CSS::ComputedProperties> style)
{
    // By default, create a MathMLBox for all MathML elements
    // Specific element types can override this to create specialized boxes
    return heap().allocate<Layout::MathMLBox>(document(), *this, move(style));
}

void MathMLElement::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    HTMLOrSVGElement::visit_edges(visitor);
}

bool MathMLElement::is_presentational_hint(FlyString const& name) const
{
    return first_is_one_of(name, AttributeNames::dir, AttributeNames::mathcolor, AttributeNames::mathbackground,
        AttributeNames::mathsize, AttributeNames::displaystyle, AttributeNames::scriptlevel);
}

void MathMLElement::apply_presentational_hints(GC::Ref<CSS::CascadedProperties> cascaded_properties) const
{
    for_each_attribute([&](auto& name, auto& value) {
        if (name == AttributeNames::dir) {
            // https://w3c.github.io/mathml-core/#attributes-common-to-html-and-mathml-elements
            // The dir attribute, if present, must be an ASCII case-insensitive match to ltr or rtl. In that case, the
            // user agent is expected to treat the attribute as a presentational hint setting the element's direction
            // property to the corresponding value. More precisely, an ASCII case-insensitive match to rtl is mapped to
            // rtl while an ASCII case-insensitive match to ltr is mapped to ltr.
            if (value.equals_ignoring_ascii_case("ltr"sv))
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::Direction, CSS::KeywordStyleValue::create(CSS::Keyword::Ltr));
            else if (value.equals_ignoring_ascii_case("rtl"sv))
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::Direction, CSS::KeywordStyleValue::create(CSS::Keyword::Rtl));
        } else if (name == AttributeNames::mathcolor) {
            // https://w3c.github.io/mathml-core/#legacy-mathml-style-attributes
            // The mathcolor and mathbackground attributes, if present, must have a value that is a <color>. In that case,
            // the user agent is expected to treat these attributes as a presentational hint setting the element's color
            // and background-color properties to the corresponding values.
            if (auto parsed_value = parse_css_value(CSS::Parser::ParsingParams { document() }, value, CSS::PropertyID::Color))
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::Color, parsed_value.release_nonnull());
        } else if (name == AttributeNames::mathbackground) {
            if (auto parsed_value = parse_css_value(CSS::Parser::ParsingParams { document() }, value, CSS::PropertyID::BackgroundColor))
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::BackgroundColor, parsed_value.release_nonnull());
        } else if (name == AttributeNames::mathsize) {
            // https://w3c.github.io/mathml-core/#dfn-mathsize
            // The mathsize attribute, if present, must have a value that is a valid <length-percentage>.
            // In that case, the user agent is expected to treat the attribute as a presentational hint setting the
            // element's font-size property to the corresponding value.
            if (auto parsed_value = HTML::parse_dimension_value(value))
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::FontSize, parsed_value.release_nonnull());
        } else if (name == AttributeNames::displaystyle) {
            // https://w3c.github.io/mathml-core/#dfn-displaystyle
            // The displaystyle attribute, if present, must have a value that is a boolean. In that case, the user agent
            // is expected to treat the attribute as a presentational hint setting the element's math-style property to
            // the corresponding value. More precisely, an ASCII case-insensitive match to true is mapped to normal while
            // an ASCII case-insensitive match to false is mapped to compact.
            if (value.equals_ignoring_ascii_case("true"sv))
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::MathStyle, CSS::KeywordStyleValue::create(CSS::Keyword::Normal));
            else if (value.equals_ignoring_ascii_case("false"sv))
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::MathStyle, CSS::KeywordStyleValue::create(CSS::Keyword::Compact));
        } else if (name == AttributeNames::scriptlevel) {
            // https://w3c.github.io/mathml-core/#dfn-scriptlevel
            // The scriptlevel attribute, if present, must have value +<U>, -<U> or <U> where <U> is an unsigned-integer.
            // In that case the user agent is expected to treat the scriptlevel attribute as a presentational hint
            // setting the element's math-depth property to the corresponding value. More precisely, +<U>, -<U> and <U>
            // are respectively mapped to add(<U>) add(<-U>) and <U>.
            if (Optional<StringView> parsed_value = HTML::parse_integer_digits(value); parsed_value.has_value()) {
                auto string_value = parsed_value.value();
                if (auto value = parsed_value->to_number<i32>(TrimWhitespace::No); value.has_value()) {
                    auto style_value = string_value[0] == '+' || string_value[0] == '-' ? CSS::MathDepthStyleValue::create_add(CSS::IntegerStyleValue::create(value.release_value()))
                                                                                        : CSS::MathDepthStyleValue::create_integer(CSS::IntegerStyleValue::create(value.release_value()));
                    cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::MathDepth, style_value);
                }
            }
        }
    });
}

}
