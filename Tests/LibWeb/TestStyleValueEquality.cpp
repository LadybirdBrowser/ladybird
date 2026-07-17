/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibWeb/CSS/StyleValues/ColorFunctionStyleValue.h>
#include <LibWeb/CSS/StyleValues/ConicGradientStyleValue.h>
#include <LibWeb/CSS/StyleValues/CounterDefinitionsStyleValue.h>
#include <LibWeb/CSS/StyleValues/CustomIdentStyleValue.h>
#include <LibWeb/CSS/StyleValues/IntegerStyleValue.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/PositionStyleValue.h>
#include <LibWeb/CSS/StyleValues/RadialGradientStyleValue.h>
#include <LibWeb/CSS/StyleValues/RadialSizeStyleValue.h>
#include <LibWeb/CSS/StyleValues/UnresolvedStyleValue.h>

// These tests build separately-allocated style values with equal (or deliberately unequal)
// contents and check that StyleValue::equals() compares by value, not by pointer identity.
// Style value equality feeds restyle invalidation and transition change detection, so drift
// here does not show up in rendering tests until it causes stale styles or spurious
// transitions.

namespace Web::CSS {

TEST_CASE(counter_definitions_equality_is_deep)
{
    auto make_definitions = [](i32 value) {
        Vector<CounterDefinition> definitions;
        definitions.append(CounterDefinition {
            .name = "chapter"_utf16_fly_string,
            .is_reversed = false,
            .value = IntegerStyleValue::create(value),
        });
        return CounterDefinitionsStyleValue::create(move(definitions));
    };

    auto first = make_definitions(1);
    auto same_as_first = make_definitions(1);
    auto different = make_definitions(2);

    EXPECT(first->equals(same_as_first));
    EXPECT(!first->equals(different));
}

static NonnullRefPtr<RadialGradientStyleValue const> make_radial_gradient()
{
    Vector<ColorStopListElement> stops;
    stops.append(ColorStopListElement { .transition_hint = nullptr, .color_stop = { .color = KeywordStyleValue::create(Keyword::Currentcolor), .position = nullptr } });
    stops.append(ColorStopListElement { .transition_hint = nullptr, .color_stop = { .color = KeywordStyleValue::create(Keyword::None), .position = nullptr } });
    return RadialGradientStyleValue::create(
        RadialGradientStyleValue::EndingShape::Ellipse,
        KeywordStyleValue::create(Keyword::FarthestCorner),
        PositionStyleValue::create_center(),
        move(stops),
        GradientRepeating::No,
        nullptr);
}

TEST_CASE(radial_gradient_size_equality_is_deep)
{
    // The size sub-values are separate allocations with equal contents; the gradients must
    // still compare equal.
    EXPECT(make_radial_gradient()->equals(*make_radial_gradient()));
}

static NonnullRefPtr<ConicGradientStyleValue const> make_conic_gradient(ColorSyntax gradient_color_syntax)
{
    auto make_color = [](double r, double g, double b) {
        return ColorFunctionStyleValue::create(
            ColorStyleValue::ColorType::RGB,
            NumberStyleValue::create(r),
            NumberStyleValue::create(g),
            NumberStyleValue::create(b),
            NumberStyleValue::create(1),
            ColorSyntax::Legacy);
    };
    Vector<ColorStopListElement> stops;
    stops.append(ColorStopListElement { .transition_hint = nullptr, .color_stop = { .color = make_color(255, 0, 0), .position = nullptr } });
    stops.append(ColorStopListElement { .transition_hint = nullptr, .color_stop = { .color = make_color(0, 0, 255), .position = nullptr } });
    return ConicGradientStyleValue::create(nullptr, PositionStyleValue::create_center(), move(stops), GradientRepeating::No, nullptr, gradient_color_syntax);
}

TEST_CASE(conic_gradient_equality_considers_color_syntax)
{
    auto legacy = make_conic_gradient(ColorSyntax::Legacy);
    auto modern = make_conic_gradient(ColorSyntax::Modern);

    EXPECT(legacy->equals(*make_conic_gradient(ColorSyntax::Legacy)));
    EXPECT(!legacy->equals(*modern));
}

TEST_CASE(radial_size_equality_is_deep)
{
    auto make_size = [](double width, double height) {
        Vector<RadialSizeStyleValue::Component> components;
        components.append(NonnullRefPtr<StyleValue const> { NumberStyleValue::create(width) });
        components.append(NonnullRefPtr<StyleValue const> { NumberStyleValue::create(height) });
        return RadialSizeStyleValue::create(move(components));
    };

    EXPECT(make_size(50, 30)->equals(*make_size(50, 30)));
    EXPECT(!make_size(50, 30)->equals(*make_size(50, 40)));
}

TEST_CASE(unresolved_equality_trims_only_ascii_whitespace)
{
    auto make_unresolved = [](String source_text) {
        return UnresolvedStyleValue::create({}, {}, move(source_text));
    };

    // U+00A0 has the Unicode White_Space property but is not ASCII whitespace; values differing
    // by it must not compare equal, or custom-property change detection misses the update.
    auto plain = make_unresolved("foo"_string);
    auto with_leading_nbsp = make_unresolved("\u00A0foo"_string);

    EXPECT(plain->equals(*make_unresolved("foo"_string)));
    EXPECT(!plain->equals(*with_leading_nbsp));
}

}
