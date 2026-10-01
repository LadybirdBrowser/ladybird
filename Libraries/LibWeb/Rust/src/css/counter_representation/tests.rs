/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;

fn symbol(text: &str) -> Symbol {
    text.encode_utf16().collect::<Vec<_>>().into_boxed_slice()
}

fn symbols(list: &[&str]) -> Vec<Symbol> {
    list.iter().map(|entry| symbol(entry)).collect()
}

fn style(algorithm: Algorithm) -> CounterStyle {
    CounterStyle { algorithm }
}

fn initial(style: &CounterStyle, value: i64) -> Option<String> {
    style
        .generate_an_initial_representation_for_the_counter_value(value)
        .map(|units| String::from_utf16(&units).unwrap())
}

fn represented(style: &CounterStyle, value: i64) -> String {
    initial(style, value).expect("the style represents the value")
}

#[test]
fn the_generic_systems() {
    let cyclic = style(Algorithm::Generic {
        system: GenericSystem::Cyclic,
        symbols: symbols(&["a", "b", "c"]),
    });
    assert_eq!(represented(&cyclic, 1), "a");
    assert_eq!(represented(&cyclic, 3), "c");
    assert_eq!(represented(&cyclic, 4), "a");
    // Cyclic uses no negative sign, so a negative value wraps in the unsigned modulus C++ computes.
    assert_eq!(represented(&cyclic, -1), "c");

    let alphabetic = style(Algorithm::Generic {
        system: GenericSystem::Alphabetic,
        symbols: symbols(&["a", "b", "c"]),
    });
    assert_eq!(represented(&alphabetic, 1), "a");
    assert_eq!(represented(&alphabetic, 3), "c");
    assert_eq!(represented(&alphabetic, 4), "aa");
    assert_eq!(represented(&alphabetic, 12), "cc");
    // Alphabetic has no representation for zero: the loop ends immediately.
    assert_eq!(represented(&alphabetic, 0), "");

    let symbolic = style(Algorithm::Generic {
        system: GenericSystem::Symbolic,
        symbols: symbols(&["*", "+"]),
    });
    assert_eq!(represented(&symbolic, 1), "*");
    assert_eq!(represented(&symbolic, 2), "+");
    assert_eq!(represented(&symbolic, 3), "**");
    assert_eq!(represented(&symbolic, 4), "++");

    let numeric = style(Algorithm::Generic {
        system: GenericSystem::Numeric,
        symbols: symbols(&["0", "1", "2"]),
    });
    assert_eq!(represented(&numeric, 0), "0");
    assert_eq!(represented(&numeric, 5), "12");
}

#[test]
fn the_additive_and_fixed_systems() {
    let roman = style(Algorithm::Additive(vec![
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
    ]));
    assert_eq!(represented(&roman, 1), "I");
    assert_eq!(represented(&roman, 1994), "MCMXCIV");
    assert_eq!(represented(&roman, 3999), "MMMCMXCIX");
    // Without a tuple of weight zero, zero needs the fallback style.
    assert_eq!(initial(&roman, 0), None);

    let fixed = style(Algorithm::Fixed {
        first_symbol: 2,
        symbols: symbols(&["x", "y"]),
    });
    assert_eq!(represented(&fixed, 2), "x");
    assert_eq!(represented(&fixed, 3), "y");
    // Past the end of the list, and before its start, the fallback represents the value.
    assert_eq!(initial(&fixed, 4), None);
    assert_eq!(initial(&fixed, 1), None);
}

#[test]
fn the_ethiopic_numeric_style() {
    let ethiopic = style(Algorithm::EthiopicNumeric);
    assert_eq!(represented(&ethiopic, 1), "\u{1369}");
    assert_eq!(represented(&ethiopic, 10), "\u{1372}");
    assert_eq!(represented(&ethiopic, 100), "\u{137b}");
    assert_eq!(represented(&ethiopic, 1000), "\u{1372}\u{137b}");
    assert_eq!(represented(&ethiopic, 10000), "\u{137c}");
}

#[test]
fn the_extended_cjk_styles() {
    let japanese = style(Algorithm::ExtendedCjk(ExtendedCjkStyle::JapaneseInformal));
    assert_eq!(represented(&japanese, 0), "\u{3007}");
    assert_eq!(represented(&japanese, 1), "\u{4e00}");
    assert_eq!(represented(&japanese, 10), "\u{5341}");
    assert_eq!(represented(&japanese, 11), "\u{5341}\u{4e00}");
    assert_eq!(represented(&japanese, 20), "\u{4e8c}\u{5341}");
    assert_eq!(represented(&japanese, 100), "\u{767e}");

    let simp = style(Algorithm::ExtendedCjk(ExtendedCjkStyle::SimpChineseInformal));
    assert_eq!(represented(&simp, 10), "\u{5341}");
    assert_eq!(represented(&simp, 11), "\u{5341}\u{4e00}");
    assert_eq!(represented(&simp, 21), "\u{4e8c}\u{5341}\u{4e00}");
    assert_eq!(represented(&simp, 101), "\u{4e00}\u{767e}\u{96f6}\u{4e00}");
    assert_eq!(represented(&simp, 10000), "\u{4e00}\u{4e07}");

    let korean = style(Algorithm::ExtendedCjk(ExtendedCjkStyle::KoreanHangulFormal));
    // The Korean styles put a space between groups.
    assert_eq!(represented(&korean, 10001), "\u{c77c}\u{b9cc} \u{c77c}");
}

#[test]
fn a_marker_with_one_symbol_does_not_depend_on_the_value() {
    let depends_on_value = |algorithm| {
        let handle = FfiRegisteredCounterStyle(style(algorithm));
        // SAFETY: The handle is live for the duration of the call.
        unsafe { rust_counter_style_representation_depends_on_value(&handle) }
    };
    assert!(!depends_on_value(Algorithm::Generic {
        system: GenericSystem::Cyclic,
        symbols: symbols(&["\u{2022}"]),
    }));
    assert!(depends_on_value(Algorithm::Generic {
        system: GenericSystem::Numeric,
        symbols: symbols(&["0", "1", "2", "3", "4", "5", "6", "7", "8", "9"]),
    }));
}
