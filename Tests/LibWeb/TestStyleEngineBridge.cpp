/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Utf16FlyString.h>
#include <LibTest/TestCase.h>
#include <LibWeb/CSS/StyleEngineBridge.h>

static Web::CSS::StyleEngine::PublishedStyleDelta make_style_delta(Web::CSS::StyleEngineFFI::FfiStyleDeltaGap gap, u8 reaction, u8 inherited_style_groups)
{
    return {
        .style_node = 1,
        .match_answer = 0,
        .old_style_record = 1,
        .new_style_record = gap == Web::CSS::StyleEngineFFI::FfiStyleDeltaGap::None ? 2u : 0u,
        .damage = gap == Web::CSS::StyleEngineFFI::FfiStyleDeltaGap::None
            ? Web::CSS::StyleEngineFFI::FfiStyleDeltaDamage::Full
            : Web::CSS::StyleEngineFFI::FfiStyleDeltaDamage::None,
        .reaction = reaction,
        .inherited_style_groups = inherited_style_groups,
        .pseudo_kind = 0xff,
        .gap = gap,
    };
}

TEST_CASE(interned_atoms_are_released_with_the_style_engine)
{
    auto initial_fly_string_count = Utf16FlyString::number_of_utf16_fly_strings();
    {
        auto name = Utf16FlyString::from_utf8_without_validation("style-engine-atom-lifetime-test-name"sv);
        EXPECT_EQ(Utf16FlyString::number_of_utf16_fly_strings(), initial_fly_string_count + 1);

        Web::CSS::StyleEngine engine(Web::CSS::StyleEngine::DeviceClass::ForegroundDesktop);
        engine.intern_atom(name);
    }
    EXPECT_EQ(Utf16FlyString::number_of_utf16_fly_strings(), initial_fly_string_count);
}

TEST_CASE(direct_inherited_style_deltas_only_absorb_covered_reactions)
{
    using Web::CSS::StyleEngine;
    using Web::CSS::StyleEngineFFI::FfiStyleDeltaGap;

    auto direct_delta = make_style_delta(FfiStyleDeltaGap::None, StyleEngine::InheritedStyle, 0b0010);
    EXPECT(StyleEngine::published_style_delta_can_absorb_reaction(direct_delta, StyleEngine::InheritedStyle, 0b0010));
    EXPECT(StyleEngine::published_style_delta_can_absorb_reaction(direct_delta, StyleEngine::InheritedStyle, 0));
    EXPECT(!StyleEngine::published_style_delta_can_absorb_reaction(direct_delta, StyleEngine::InheritedStyle | StyleEngine::InheritedCustomProperties, 0b0010));
    EXPECT(!StyleEngine::published_style_delta_can_absorb_reaction(direct_delta, StyleEngine::InheritedStyle, 0b0110));
    EXPECT(!StyleEngine::published_style_delta_can_absorb_reaction(direct_delta, StyleEngine::RecomputeStyle, 0));

    auto materialized_delta = make_style_delta(FfiStyleDeltaGap::Materialize, StyleEngine::InheritedStyle, 0b0010);
    EXPECT(StyleEngine::published_style_delta_can_absorb_reaction(materialized_delta, StyleEngine::InheritedStyle | StyleEngine::InheritedCustomProperties, 0b0110));
}
