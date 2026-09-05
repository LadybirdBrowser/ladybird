/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/String.h>
#include <AK/Utf16FlyString.h>
#include <LibTest/TestCase.h>
#include <LibWeb/CSS/StyleEngineBridge.h>

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

TEST_CASE(unowned_atoms_are_released_in_one_transaction_batch)
{
    auto initial_fly_string_count = Utf16FlyString::number_of_utf16_fly_strings();
    Web::CSS::StyleEngine engine(Web::CSS::StyleEngine::DeviceClass::ForegroundDesktop);
    auto root = engine.allocate_style_node();
    for (size_t index = 0; index < 256; ++index) {
        auto name = MUST(String::formatted("style-engine-atom-churn-{}", index));
        engine.intern_atom(Utf16FlyString::from_utf8_without_validation(name));
    }
    EXPECT_EQ(Utf16FlyString::number_of_utf16_fly_strings(), initial_fly_string_count + 256);
    auto initial_generation = engine.atom_generation();
    (void)engine.take_style_transaction(root);
    EXPECT_EQ(engine.atom_generation(), initial_generation + 1);
    EXPECT_EQ(Utf16FlyString::number_of_utf16_fly_strings(), initial_fly_string_count);
}

TEST_CASE(flush_does_not_recycle_atoms_before_the_bridge_can_forget_them)
{
    Web::CSS::StyleEngine engine(Web::CSS::StyleEngine::DeviceClass::ForegroundDesktop);
    Vector<Utf16FlyString> names;
    Vector<Web::CSS::StyleAtomID> atoms;
    names.ensure_capacity(256);
    atoms.ensure_capacity(256);
    for (size_t index = 0; index < 256; ++index) {
        auto name = Utf16FlyString::from_utf8_without_validation(MUST(String::formatted("style-engine-flush-atom-{}", index)));
        atoms.unchecked_append(engine.intern_atom(name));
        names.unchecked_append(move(name));
    }

    engine.flush();
    auto new_atom = engine.intern_atom(Utf16FlyString::from_utf8_without_validation("style-engine-after-flush"sv));
    EXPECT(!atoms.contains_slow(new_atom));
    for (size_t index = 0; index < names.size(); ++index)
        EXPECT_EQ(engine.intern_atom(names[index]), atoms[index]);
}

static u64 counter_value(Web::CSS::StyleEngine const& engine, StringView expected_name)
{
    for (size_t index = 0;; ++index) {
        StringView name;
        u64 value = 0;
        if (!engine.counter(index, name, value))
            VERIFY_NOT_REACHED();
        if (name == expected_name)
            return value;
    }
}

TEST_CASE(reclaimed_language_atoms_republish_their_text)
{
    Web::CSS::StyleEngine engine(Web::CSS::StyleEngine::DeviceClass::ForegroundDesktop);
    auto root = engine.allocate_style_node();
    auto language = Utf16FlyString::from_utf8_without_validation("reclaimed-language"sv);
    engine.intern_language_atom(language.view());
    for (size_t index = 0; index < 255; ++index) {
        auto name = MUST(String::formatted("style-engine-language-sweep-{}", index));
        engine.intern_atom(Utf16FlyString::from_utf8_without_validation(name));
    }
    EXPECT_EQ(counter_value(engine, "languageTextsPublished"sv), 1ull);

    engine.flush();
    (void)engine.take_style_transaction(root);
    engine.intern_language_atom(language.view());

    EXPECT_EQ(counter_value(engine, "languageTextsPublished"sv), 2ull);
}
