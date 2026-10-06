/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;

fn text(units: &[u16]) -> String {
    String::from_utf16(units).unwrap()
}

fn registered(styles: Vec<CounterStyle>) -> Arc<RegisteredCounterStyles> {
    Arc::new(RegisteredCounterStyles(
        styles
            .into_iter()
            .map(|style| (style.name.clone(), Arc::new(style)))
            .collect(),
    ))
}

fn registry_with(styles: Vec<CounterStyle>) -> CounterStyleRegistry {
    let mut registry = CounterStyleRegistry::default();
    registry.publish_scope(
        0,
        CounterStyleScope {
            parent: None,
            styles: registered(styles),
        },
    );
    registry
}

fn style(name: &str, algorithm: Algorithm) -> CounterStyle {
    CounterStyle {
        name: symbol(name),
        algorithm,
        negative_prefix: symbol("-"),
        negative_suffix: symbol(""),
        prefix: symbol(""),
        suffix: symbol(". "),
        range: vec![RangeEntry {
            start: i32::MIN,
            end: i32::MAX,
        }],
        fallback: Some(symbol("decimal")),
        pad_minimum_length: 0,
        pad_symbol: symbol(""),
    }
}

fn symbols(list: &[&str]) -> Vec<Symbol> {
    list.iter().map(|entry| symbol(entry)).collect()
}

fn generate(registry: &CounterStyleRegistry, style: &CounterStyle, value: i32) -> String {
    text(&generate_a_counter_representation(registry, 0, Some(style), value))
}

#[test]
fn an_unknown_counter_style_is_decimal() {
    let registry = CounterStyleRegistry::default();
    assert_eq!(text(&generate_a_counter_representation(&registry, 0, None, -17)), "-17");
    assert_eq!(text(&generate_a_counter_representation(&registry, 0, None, 0)), "0");
    assert_eq!(
        text(&generate_a_counter_representation(&registry, 0, None, i32::MIN)),
        "-2147483648"
    );
}

#[test]
fn the_generic_systems() {
    let registry = CounterStyleRegistry::default();
    let cyclic = style(
        "cyclic",
        Algorithm::Generic {
            system: GenericSystem::Cyclic,
            symbols: symbols(&["a", "b", "c"]),
        },
    );
    assert_eq!(generate(&registry, &cyclic, 1), "a");
    assert_eq!(generate(&registry, &cyclic, 3), "c");
    assert_eq!(generate(&registry, &cyclic, 4), "a");
    // Cyclic uses no negative sign, so a negative value wraps in the unsigned modulus C++ computes.
    assert_eq!(generate(&registry, &cyclic, -1), "c");

    let alphabetic = style(
        "alphabetic",
        Algorithm::Generic {
            system: GenericSystem::Alphabetic,
            symbols: symbols(&["a", "b", "c"]),
        },
    );
    assert_eq!(generate(&registry, &alphabetic, 1), "a");
    assert_eq!(generate(&registry, &alphabetic, 3), "c");
    assert_eq!(generate(&registry, &alphabetic, 4), "aa");
    assert_eq!(generate(&registry, &alphabetic, 12), "cc");
    assert_eq!(generate(&registry, &alphabetic, -4), "-aa");
    // Alphabetic has no representation for zero: the loop ends immediately.
    assert_eq!(generate(&registry, &alphabetic, 0), "");

    let symbolic = style(
        "symbolic",
        Algorithm::Generic {
            system: GenericSystem::Symbolic,
            symbols: symbols(&["*", "+"]),
        },
    );
    assert_eq!(generate(&registry, &symbolic, 1), "*");
    assert_eq!(generate(&registry, &symbolic, 2), "+");
    assert_eq!(generate(&registry, &symbolic, 3), "**");
    assert_eq!(generate(&registry, &symbolic, 4), "++");
    assert_eq!(generate(&registry, &symbolic, -3), "-**");

    let numeric = style(
        "numeric",
        Algorithm::Generic {
            system: GenericSystem::Numeric,
            symbols: symbols(&["0", "1", "2"]),
        },
    );
    assert_eq!(generate(&registry, &numeric, 0), "0");
    assert_eq!(generate(&registry, &numeric, 5), "12");
    assert_eq!(generate(&registry, &numeric, -5), "-12");
}

#[test]
fn the_additive_and_fixed_systems() {
    let registry = CounterStyleRegistry::default();
    let mut roman = style(
        "upper-roman",
        Algorithm::Additive(vec![
            (1000, symbol("M")),
            (900, symbol("CM")),
            (500, symbol("D")),
            (400, symbol("CD")),
            (100, symbol("C")),
            (90, symbol("XC")),
            (50, symbol("L")),
            (40, symbol("XL")),
            (10, symbol("X")),
            (9, symbol("IX")),
            (5, symbol("V")),
            (4, symbol("IV")),
            (1, symbol("I")),
        ]),
    );
    roman.range = vec![RangeEntry { start: 1, end: 3999 }];
    assert_eq!(generate(&registry, &roman, 1), "I");
    assert_eq!(generate(&registry, &roman, 1994), "MCMXCIV");
    assert_eq!(generate(&registry, &roman, 3999), "MMMCMXCIX");
    // Out of range in both directions, so decimal represents it.
    assert_eq!(generate(&registry, &roman, 4000), "4000");
    assert_eq!(generate(&registry, &roman, 0), "0");

    let mut fixed = style(
        "fixed",
        Algorithm::Fixed {
            first_symbol: 2,
            symbols: symbols(&["x", "y"]),
        },
    );
    fixed.negative_prefix = symbol("!");
    assert_eq!(generate(&registry, &fixed, 2), "x");
    assert_eq!(generate(&registry, &fixed, 3), "y");
    // Past the end of the list, and before its start, the fallback represents the value.
    assert_eq!(generate(&registry, &fixed, 4), "4");
    // Fixed uses no negative sign, so the fallback sees the value as written.
    assert_eq!(generate(&registry, &fixed, -1), "-1");
}

#[test]
fn a_fallback_loop_ends_at_decimal() {
    let mut first = style(
        "first",
        Algorithm::Generic {
            system: GenericSystem::Cyclic,
            symbols: symbols(&["!"]),
        },
    );
    first.range = vec![RangeEntry { start: 1, end: 1 }];
    first.fallback = Some(symbol("second"));
    let mut second = style(
        "second",
        Algorithm::Generic {
            system: GenericSystem::Cyclic,
            symbols: symbols(&["?"]),
        },
    );
    second.range = vec![RangeEntry { start: 2, end: 2 }];
    second.fallback = Some(symbol("first"));

    let registry = registry_with(vec![first, second]);
    let first = registry.lookup(0, &symbol("first")).unwrap();
    assert_eq!(generate(&registry, first, 1), "!");
    assert_eq!(generate(&registry, first, 2), "?");
    assert_eq!(generate(&registry, first, 3), "3");

    // A fallback naming a style nothing registers is decimal too.
    let mut orphan = style(
        "orphan",
        Algorithm::Generic {
            system: GenericSystem::Cyclic,
            symbols: symbols(&["#"]),
        },
    );
    orphan.range = vec![RangeEntry { start: 1, end: 1 }];
    orphan.fallback = Some(symbol("nowhere"));
    let registry = registry_with(vec![orphan]);
    let orphan = registry.lookup(0, &symbol("orphan")).unwrap();
    assert_eq!(generate(&registry, orphan, 9), "9");
}

#[test]
fn the_pad_descriptor_counts_the_negative_sign() {
    let registry = CounterStyleRegistry::default();
    let mut padded = style(
        "padded",
        Algorithm::Generic {
            system: GenericSystem::Numeric,
            symbols: symbols(&["0", "1", "2", "3", "4", "5", "6", "7", "8", "9"]),
        },
    );
    padded.pad_minimum_length = 5;
    padded.pad_symbol = symbol("0");
    padded.negative_prefix = symbol("(");
    padded.negative_suffix = symbol(")");
    assert_eq!(generate(&registry, &padded, 7), "00007");
    assert_eq!(generate(&registry, &padded, 12345), "12345");
    assert_eq!(generate(&registry, &padded, 123456), "123456");
    // Five units minus the two sign units leaves room for three.
    assert_eq!(generate(&registry, &padded, -7), "(007)");
}

#[test]
fn a_name_resolves_through_the_host_scope() {
    let mut registry = CounterStyleRegistry::default();
    registry.publish_scope(
        0,
        CounterStyleScope {
            parent: None,
            styles: registered(vec![style(
                "shared",
                Algorithm::Generic {
                    system: GenericSystem::Cyclic,
                    symbols: symbols(&["outer"]),
                },
            )]),
        },
    );
    registry.publish_scope(
        7,
        CounterStyleScope {
            parent: Some(0),
            styles: registered(vec![style(
                "local",
                Algorithm::Generic {
                    system: GenericSystem::Cyclic,
                    symbols: symbols(&["inner"]),
                },
            )]),
        },
    );

    assert!(registry.lookup(7, &symbol("local")).is_some());
    assert!(registry.lookup(7, &symbol("shared")).is_some());
    assert!(registry.lookup(0, &symbol("local")).is_none());
    assert!(registry.lookup(9, &symbol("shared")).is_none());
}

#[test]
fn the_ethiopic_numeric_style() {
    let registry = CounterStyleRegistry::default();
    let mut ethiopic = style("ethiopic-numeric", Algorithm::EthiopicNumeric);
    ethiopic.range = vec![RangeEntry {
        start: 1,
        end: i32::MAX,
    }];
    assert_eq!(generate(&registry, &ethiopic, 1), "\u{1369}");
    assert_eq!(generate(&registry, &ethiopic, 10), "\u{1372}");
    assert_eq!(generate(&registry, &ethiopic, 100), "\u{137b}");
    assert_eq!(generate(&registry, &ethiopic, 1000), "\u{1372}\u{137b}");
    assert_eq!(generate(&registry, &ethiopic, 10000), "\u{137c}");
    // Out of range, so decimal represents it.
    assert_eq!(generate(&registry, &ethiopic, 0), "0");
}

#[test]
fn the_extended_cjk_styles() {
    let registry = CounterStyleRegistry::default();
    let japanese = style(
        "japanese-informal",
        Algorithm::ExtendedCjk(ExtendedCjkStyle::JapaneseInformal),
    );
    assert_eq!(generate(&registry, &japanese, 0), "\u{3007}");
    assert_eq!(generate(&registry, &japanese, 1), "\u{4e00}");
    assert_eq!(generate(&registry, &japanese, 10), "\u{5341}");
    assert_eq!(generate(&registry, &japanese, 11), "\u{5341}\u{4e00}");
    assert_eq!(generate(&registry, &japanese, 20), "\u{4e8c}\u{5341}");
    assert_eq!(generate(&registry, &japanese, 100), "\u{767e}");

    let simp = style(
        "simp-chinese-informal",
        Algorithm::ExtendedCjk(ExtendedCjkStyle::SimpChineseInformal),
    );
    assert_eq!(generate(&registry, &simp, 10), "\u{5341}");
    assert_eq!(generate(&registry, &simp, 11), "\u{5341}\u{4e00}");
    assert_eq!(generate(&registry, &simp, 21), "\u{4e8c}\u{5341}\u{4e00}");
    assert_eq!(generate(&registry, &simp, 101), "\u{4e00}\u{767e}\u{96f6}\u{4e00}");
    assert_eq!(generate(&registry, &simp, 10000), "\u{4e00}\u{4e07}");

    let korean = style(
        "korean-hangul-formal",
        Algorithm::ExtendedCjk(ExtendedCjkStyle::KoreanHangulFormal),
    );
    // The Korean styles put a space between groups.
    assert_eq!(generate(&registry, &korean, 10001), "\u{c77c}\u{b9cc} \u{c77c}");
}

#[test]
fn a_marker_with_one_symbol_does_not_depend_on_the_value() {
    let depends_on_value = |algorithm| style("marker", algorithm).representation_depends_on_value();
    assert!(!depends_on_value(Algorithm::Generic {
        system: GenericSystem::Cyclic,
        symbols: symbols(&["\u{2022}"]),
    }));
    assert!(depends_on_value(Algorithm::Generic {
        system: GenericSystem::Numeric,
        symbols: symbols(&["0", "1", "2", "3", "4", "5", "6", "7", "8", "9"]),
    }));
}

#[test]
fn a_marker_that_leaves_values_to_the_fallback_depends_on_the_value() {
    let depends_on_value = |style: CounterStyle| style.representation_depends_on_value();

    // One symbol for every value in range, and the fallback's text outside it.
    let mut stars_for_two = style(
        "stars-for-two",
        Algorithm::Generic {
            system: GenericSystem::Cyclic,
            symbols: symbols(&["*"]),
        },
    );
    stars_for_two.range = vec![RangeEntry { start: 1, end: 2 }];
    assert!(depends_on_value(stars_for_two));

    // No symbol for 1, 2 or 3: those are all the fallback's text, but 7 and 8 are not.
    assert!(depends_on_value(style(
        "from-seven",
        Algorithm::Fixed {
            first_symbol: 7,
            symbols: symbols(&["x", "y"]),
        },
    )));

    // A cyclic style whose symbols all read the same is still one text.
    assert!(!depends_on_value(style(
        "stars",
        Algorithm::Generic {
            system: GenericSystem::Cyclic,
            symbols: symbols(&["*", "*"]),
        },
    )));
}
