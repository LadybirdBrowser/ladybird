/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The heap a parsed style sheet holds, per unit of CSS: per rule, selector, declaration by kind,
//! at-rule, keyframe and sheet. Each figure is the slope between two sheets that differ only in how
//! many of the unit they hold, so the fixed costs cancel; the bytes are what the allocator was
//! asked for and still holds once the parse returns, counted exactly by the test allocator.
//!
//! Each ceiling is what the unit may hold, which a change growing it has to raise, beside the
//! typical figure the unit is measured against.

use crate::css::css_tokenizer::TokenizerInput;
use crate::css::parser::syntax_parser::parse_stylesheet_uncached;
use crate::css::parser::value_parser::ParseContext;
use crate::css::rule::NativeRuleList;
use crate::css::style::pipeline::allocation_count::count_live_bytes;
use std::sync::Arc;

/// The bytes a parse of `text` leaves allocated, but for the text it keeps, and those the rules the
/// CSSOM materializes from it add. What the process interns or pools for every sheet, as names and common values, is the
/// process's: a parse of the same text before the one counted has it in place, so that what this
/// thread happens to grow or clear of those tables, beside the other test threads, is not counted.
fn parsed_bytes(text: &str) -> (i64, i64) {
    let units: Vec<u16> = text.encode_utf16().collect();
    let context = ParseContext::default();
    let parse = || Arc::new(parse_stylesheet_uncached(TokenizerInput::Utf16(&units), Some(&context)));
    let interned = parse();
    let (parsed, parse_bytes) = count_live_bytes(parse);
    let (list, native_bytes) = count_live_bytes(|| {
        let list = NativeRuleList::from_parsed(parsed.clone());
        for index in 0..list.len() {
            list.rule_at(index);
        }
        list
    });
    drop((list, parsed, interned));
    // NB: The sheet keeps its text, one byte per ASCII character, for DevTools, in place of the copy
    //     each document kept. The text is left out of every unit but the sheet's.
    assert!(text.is_ascii());
    (parse_bytes - text.len() as i64, native_bytes)
}

fn sheet(count: usize, unit: &dyn Fn(usize) -> String) -> String {
    (0..count).map(|index| unit(index) + "\n").collect()
}

/// The bytes each of the units `unit` makes adds to a parse, and to the rules the CSSOM materializes
/// from it.
fn slope(unit: &dyn Fn(usize) -> String) -> (f64, f64) {
    const COUNT: usize = 500;
    let small = parsed_bytes(&sheet(COUNT, unit));
    let large = parsed_bytes(&sheet(2 * COUNT, unit));
    (
        (large.0 - small.0) as f64 / COUNT as f64,
        (large.1 - small.1) as f64 / COUNT as f64,
    )
}

/// The bytes each declaration `declaration(rule, k)` adds to a rule, of `per_rule` and twice as
/// many in each of the rules.
fn declaration_slope(per_rule: usize, declaration: &dyn Fn(usize, usize) -> String) -> f64 {
    const COUNT: usize = 500;
    let rules = |declarations: usize| {
        sheet(COUNT, &|index| {
            // NB: A declaration of the rule's own keeps sheets from sharing one block between rules. Each
            //     declaration's value is its own as well, which the parse would otherwise share.
            let mut rule = format!(".c{index} {{ order: {index} ");
            for k in 0..declarations {
                rule += &format!("; {}", declaration(index, k));
            }
            rule + " }"
        })
    };
    let small = parsed_bytes(&rules(per_rule)).0;
    let large = parsed_bytes(&rules(2 * per_rule)).0;
    (large - small) as f64 / (COUNT * per_rule) as f64
}

struct Unit {
    name: &'static str,
    bytes: f64,
    ceiling: f64,
    typical: &'static str,
}

fn check(units: &[Unit]) {
    let mut over = Vec::new();
    for unit in units {
        eprintln!(
            "{:<44} {:>8.1} B  (ceiling {:>6}; {})",
            unit.name, unit.bytes, unit.ceiling, unit.typical
        );
        if unit.bytes > unit.ceiling {
            over.push(unit.name);
        }
    }
    assert!(over.is_empty(), "over their ceilings: {over:?}");
}

const KEYWORDS: [&str; 8] = [
    "display: block",
    "position: relative",
    "float: left",
    "clear: both",
    "visibility: hidden",
    "text-align: center",
    "box-sizing: border-box",
    "text-transform: uppercase",
];

const LENGTH_PROPERTIES: [&str; 8] = [
    "margin-left",
    "margin-right",
    "margin-top",
    "margin-bottom",
    "padding-left",
    "padding-right",
    "padding-top",
    "padding-bottom",
];

const COLOR_PROPERTIES: [&str; 8] = [
    "color",
    "background-color",
    "border-left-color",
    "border-right-color",
    "border-top-color",
    "border-bottom-color",
    "outline-color",
    "caret-color",
];

const URL_PROPERTIES: [&str; 4] = [
    "background-image",
    "list-style-image",
    "border-image-source",
    "mask-image",
];

const SHORTHANDS: [&str; 4] = ["margin", "padding", "inset", "scroll-margin"];

#[test]
fn parsed_sheet_bytes_per_unit() {
    let rule = slope(&|index| format!(".c{index} {{ color: red }}"));
    // NB: Pooled values in pairs no other rule repeats, so that each rule parses a block of its own.
    let own_block = slope(&|index| format!(".c{index} {{ z-index: {}; order: {} }}", index % 100, index / 100));
    let style_rule =
        |selector: &dyn Fn(usize) -> String| slope(&|index| format!("{} {{ color: red }}", selector(index)));
    let compound = style_rule(&|index| format!("div.c{index}.d{index}[x]"));
    let complex = style_rule(&|index| format!(".a{index} .b{index} > .c{index}"));
    let list = style_rule(&|index| format!(".a{index}, .b{index}"));
    let argument_list = style_rule(&|index| format!(":is(.x{index}, .y{index}) .z{index}"));
    let wrapped = |wrap: &dyn Fn(usize) -> String| {
        slope(&|index| format!("{} {{ .c{index} {{ color: red }} }}", wrap(index))).0 - rule.0
    };
    let keyframes = |count: usize| {
        slope(&|index| {
            let mut rule = format!("@keyframes k{index} {{");
            for k in 0..count {
                rule += &format!(" {}% {{ opacity: 0.{index} }}", k * 100 / count);
            }
            rule + " }"
        })
        .0
    };
    let (two_keyframes, four_keyframes) = (keyframes(2), keyframes(4));
    let sheets = {
        const COUNT: usize = 200;
        let bytes: i64 = (0..COUNT)
            .map(|index| parsed_bytes(&format!(".s{index} {{ color: red }}")).0)
            .sum();
        bytes as f64 / COUNT as f64 - rule.0
    };

    check(&[
        Unit {
            name: "sheet, fixed",
            bytes: sheets,
            // NB: The arena holds the sheet's `@namespace` declarations and the rank of its compounds.
            ceiling: 920.0,
            typical: "about 600-1000 B",
        },
        Unit {
            name: "style rule `.c` with a shared block",
            bytes: rule.0,
            ceiling: 144.0,
            typical: "24 B + 16 B selector + 16 B facts of its compound",
        },
        Unit {
            name: "declaration block of a rule's own, fixed",
            bytes: own_block.0 - rule.0 - 2.0 * 10.0,
            ceiling: 24.0,
            typical: "8 B + 16 B per declaration",
        },
        Unit {
            name: "style rule, materialized for the CSSOM",
            bytes: rule.1,
            ceiling: 160.0,
            typical: "the CSSOM wrapper only",
        },
        Unit {
            name: "simple selector, in a compound",
            bytes: (compound.0 - rule.0) / 3.0,
            ceiling: 24.0,
            typical: "16 B",
        },
        Unit {
            name: "compound selector, after a combinator",
            bytes: (complex.0 - rule.0) / 2.0,
            ceiling: 32.0,
            typical: "16 B + 16 B compound facts",
        },
        Unit {
            name: "complex selector, in a list",
            bytes: list.0 - rule.0,
            ceiling: 32.0,
            typical: "16 B + 16 B compound facts",
        },
        Unit {
            name: ":is() of two classes, left of a class",
            bytes: argument_list.0 - rule.0,
            ceiling: 88.0,
            typical: "an argument list of two complex selectors and the compound it is in, with their compound facts",
        },
        Unit {
            name: "declaration, keyword",
            bytes: declaration_slope(4, &|_, k| KEYWORDS[k].to_owned()),
            ceiling: 10.0,
            typical: "10 B",
        },
        Unit {
            name: "declaration, length",
            bytes: declaration_slope(4, &|index, k| format!("{}: {index}.{k}75px", LENGTH_PROPERTIES[k])),
            ceiling: 34.0,
            typical: "26 B",
        },
        Unit {
            name: "declaration, rgb() color",
            bytes: declaration_slope(4, &|index, k| {
                format!(
                    "{}: rgb({}, {}, {k})",
                    COLOR_PROPERTIES[k],
                    index % 256,
                    index / 256 % 256
                )
            }),
            ceiling: 98.0,
            typical: "10 B, cached",
        },
        Unit {
            name: "declaration, rgb() color another property repeats",
            bytes: declaration_slope(4, &|index, k| {
                format!(
                    "{}: rgb({}, {}, 0)",
                    COLOR_PROPERTIES[k],
                    index % 256,
                    index / 256 % 256
                )
            }),
            ceiling: 10.0,
            typical: "10 B, cached",
        },
        Unit {
            name: "declaration, url()",
            bytes: declaration_slope(2, &|index, k| format!("{}: url(a{index}-{k}.png)", URL_PROPERTIES[k])),
            ceiling: 128.0,
            typical: "16 + 40 B + text",
        },
        Unit {
            name: "declaration, var()",
            bytes: declaration_slope(4, &|index, k| format!("{}: var(--v{index}-{k})", LENGTH_PROPERTIES[k])),
            ceiling: 98.0,
            typical: "about 30 B + text",
        },
        Unit {
            name: "declaration, shorthand of 4 longhands",
            bytes: declaration_slope(2, &|index, k| format!("{}: {index}.5px {k}px", SHORTHANDS[k])),
            ceiling: 88.0,
            typical: "40 B",
        },
        Unit {
            name: "declaration, custom property",
            bytes: declaration_slope(4, &|index, k| format!("--p{k}: {index}.{k}px solid")),
            ceiling: 80.0,
            typical: "about 36 B + text",
        },
        Unit {
            name: "@media around a rule",
            bytes: wrapped(&|index| format!("@media (min-width: {index}px)")),
            ceiling: 274.0,
            typical: "about 112 B + expressions",
        },
        Unit {
            name: "@supports around a rule",
            bytes: wrapped(&|index| format!("@supports (z-index: {index})")),
            ceiling: 300.0,
            typical: "about 40 B + condition",
        },
        Unit {
            name: "@container around a rule",
            bytes: wrapped(&|index| format!("@container (min-width: {index}px)")),
            ceiling: 286.0,
            typical: "about 120 B",
        },
        Unit {
            name: "@layer block around a rule",
            bytes: wrapped(&|index| format!("@layer l{index}")),
            ceiling: 172.0,
            typical: "about 40 B",
        },
        Unit {
            name: "@keyframes with 2 keyframes",
            bytes: two_keyframes,
            ceiling: 500.0,
            typical: "about 40 + 2 x (32 + block) B",
        },
        Unit {
            name: "keyframe",
            bytes: (four_keyframes - two_keyframes) / 2.0,
            ceiling: 168.0,
            typical: "about 32 B + block",
        },
        Unit {
            name: "@font-face",
            bytes: slope(&|index| format!("@font-face {{ font-family: f{index}; src: url(f{index}.woff) }}")).0,
            ceiling: 505.0,
            typical: "about 16 B + block",
        },
        Unit {
            name: "@property",
            bytes: slope(&|index| {
                format!("@property --p{index} {{ syntax: \"<length>\"; inherits: false; initial-value: {index}px }}")
            })
            .0,
            ceiling: 659.0,
            typical: "about 24 B + block",
        },
    ]);
}
