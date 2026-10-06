/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Generating a counter representation: the pure function from a resolved counter style and an
//! integer to the text a marker or a `counter()` shows.
//!
//! The counter styles a tree scope registers are resolved from its `@counter-style` rules (see
//! `counter_style.rs`) and published here, per tree scope, as the registry a fallback chain is
//! looked up in.

use crate::css::css_enums::symbols_type;
use crate::css::css_string::CssString;
use crate::css::style_value::StyleValueData;
use std::collections::HashMap;
use std::ffi::c_void;
use std::sync::Arc;
use std::sync::LazyLock;

/// A counter symbol. The `<image>` half of `<symbol>` is not implemented, so every symbol is text.
pub(crate) type Symbol = Box<[u16]>;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum GenericSystem {
    Cyclic,
    Numeric,
    Alphabetic,
    Symbolic,
}

/// https://drafts.csswg.org/css-counter-styles-3/#complex-predefined-counters
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum ExtendedCjkStyle {
    SimpChineseInformal,
    SimpChineseFormal,
    TradChineseInformal,
    TradChineseFormal,
    JapaneseInformal,
    JapaneseFormal,
    KoreanHangulFormal,
    KoreanHanjaInformal,
    KoreanHanjaFormal,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) enum Algorithm {
    Additive(Vec<(i32, Symbol)>),
    Fixed {
        first_symbol: i32,
        symbols: Vec<Symbol>,
    },
    Generic {
        system: GenericSystem,
        symbols: Vec<Symbol>,
    },
    EthiopicNumeric,
    ExtendedCjk(ExtendedCjkStyle),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct RangeEntry {
    pub start: i32,
    pub end: i32,
}

/// A counter style with every descriptor resolved.
#[derive(Debug, PartialEq, Eq)]
pub(crate) struct CounterStyle {
    pub name: Symbol,
    pub algorithm: Algorithm,
    pub negative_prefix: Symbol,
    pub negative_suffix: Symbol,
    pub prefix: Symbol,
    pub suffix: Symbol,
    pub range: Vec<RangeEntry>,
    /// Every counter style but `decimal` has a fallback; a missing one is read as `decimal`.
    pub fallback: Option<Symbol>,
    pub pad_minimum_length: i32,
    pub pad_symbol: Symbol,
}

/// The counter styles one tree scope registers, by name. A C++ style scope holds its own as an
/// `Arc` pointer, and every document that defines none of its own shares the user agent's.
#[derive(Default, PartialEq)]
pub struct RegisteredCounterStyles(pub(crate) HashMap<Symbol, Arc<CounterStyle>>);

impl RegisteredCounterStyles {
    /// The style a name resolves to in the first of `scopes` that registers it, nearest first.
    /// https://drafts.csswg.org/css-shadow-1/#tree-scoped-name-global
    pub(crate) fn lookup<'a>(scopes: &[&'a Self], name: &[u16]) -> Option<&'a Arc<CounterStyle>> {
        scopes.iter().find_map(|scope| scope.0.get(name))
    }
}

/// The counter styles one tree scope registers, and the scope a name it does not register is
/// looked for in next. https://drafts.csswg.org/css-shadow-1/#tree-scoped-name-global
#[derive(Default)]
pub(crate) struct CounterStyleScope {
    pub(crate) parent: Option<u32>,
    pub(crate) styles: Arc<RegisteredCounterStyles>,
}

/// Every tree scope's registered counter styles, keyed by the tree scope's identity.
#[derive(Default)]
pub(crate) struct CounterStyleRegistry {
    scopes: HashMap<u32, CounterStyleScope>,
}

impl CounterStyleRegistry {
    pub(crate) fn publish_scope(&mut self, tree_scope: u32, scope: CounterStyleScope) {
        self.scopes.insert(tree_scope, scope);
    }

    /// The style a name resolves to from `tree_scope`: this scope's own registration, else the
    /// host's, recursively.
    pub(crate) fn lookup(&self, tree_scope: u32, name: &[u16]) -> Option<&Arc<CounterStyle>> {
        let mut scope_id = Some(tree_scope);
        // A malformed parent chain would otherwise spin; the depth bound is the shadow nesting depth.
        let mut remaining_hops = self.scopes.len() + 1;
        while let Some(current) = scope_id {
            let scope = self.scopes.get(&current)?;
            if let Some(style) = scope.styles.0.get(name) {
                return Some(style);
            }
            remaining_hops = remaining_hops.checked_sub(1)?;
            scope_id = scope.parent;
        }
        None
    }
}

pub(crate) fn symbol(text: &str) -> Symbol {
    text.encode_utf16().collect::<Vec<_>>().into_boxed_slice()
}

/// https://drafts.csswg.org/css-counter-styles-3/#decimal
pub(crate) fn decimal() -> &'static CounterStyle {
    static DECIMAL: LazyLock<CounterStyle> = LazyLock::new(|| CounterStyle {
        name: symbol("decimal"),
        algorithm: Algorithm::Generic {
            system: GenericSystem::Numeric,
            symbols: ["0", "1", "2", "3", "4", "5", "6", "7", "8", "9"]
                .iter()
                .map(|digit| symbol(digit))
                .collect(),
        },
        negative_prefix: symbol("-"),
        negative_suffix: symbol(""),
        prefix: symbol(""),
        suffix: symbol(". "),
        range: vec![RangeEntry {
            start: i32::MIN,
            end: i32::MAX,
        }],
        fallback: None,
        pad_minimum_length: 0,
        pad_symbol: symbol(""),
    });
    &DECIMAL
}

/// https://drafts.csswg.org/css-counter-styles-3/#counter-style-range
/// The range `auto` resolves to, which depends on the counter system. None for the complex
/// predefined styles, which define their range explicitly.
pub(crate) fn auto_range(algorithm: &Algorithm) -> Option<Vec<RangeEntry>> {
    let entry = match algorithm {
        // For additive systems, the range is 0 to positive infinity.
        Algorithm::Additive(_) => RangeEntry {
            start: 0,
            end: i32::MAX,
        },
        // For cyclic, numeric, and fixed systems, the range is negative infinity to positive infinity.
        Algorithm::Fixed { .. }
        | Algorithm::Generic {
            system: GenericSystem::Cyclic | GenericSystem::Numeric,
            ..
        } => RangeEntry {
            start: i32::MIN,
            end: i32::MAX,
        },
        // For alphabetic and symbolic systems, the range is 1 to positive infinity.
        Algorithm::Generic {
            system: GenericSystem::Alphabetic | GenericSystem::Symbolic,
            ..
        } => RangeEntry {
            start: 1,
            end: i32::MAX,
        },
        Algorithm::EthiopicNumeric | Algorithm::ExtendedCjk(_) => return None,
    };
    Some(vec![entry])
}

/// https://drafts.csswg.org/css-counter-styles-3/#symbols-function
/// The anonymous counter style a `symbols()` function defines: "a prefix of "" (empty string) and
/// suffix of " " (U+0020 SPACE), a range of auto, a fallback of decimal, a negative of "\2D"
/// ("-" hyphen-minus), a pad of 0 "", and a speak-as of auto."
fn symbols_function_counter_style(symbols_type: u8, symbols: &[CssString]) -> Arc<CounterStyle> {
    let symbol_list: Vec<Symbol> = symbols
        .iter()
        .map(|symbol| symbol.units().to_vec().into_boxed_slice())
        .collect();
    let algorithm = match symbols_type {
        symbols_type::CYCLIC => Algorithm::Generic {
            system: GenericSystem::Cyclic,
            symbols: symbol_list,
        },
        symbols_type::NUMERIC => Algorithm::Generic {
            system: GenericSystem::Numeric,
            symbols: symbol_list,
        },
        symbols_type::ALPHABETIC => Algorithm::Generic {
            system: GenericSystem::Alphabetic,
            symbols: symbol_list,
        },
        symbols_type::SYMBOLIC => Algorithm::Generic {
            system: GenericSystem::Symbolic,
            symbols: symbol_list,
        },
        // If the system is fixed, the first symbol value is 1.
        symbols_type::FIXED => Algorithm::Fixed {
            first_symbol: 1,
            symbols: symbol_list,
        },
        other => panic!("unknown symbols() type {other}"),
    };
    let range = auto_range(&algorithm).unwrap_or_default();
    Arc::new(CounterStyle {
        // NB: The empty string rather than no name cannot clash with an authored
        //     <counter-style-name>, and the name only shows up in serialization.
        name: symbol(""),
        algorithm,
        negative_prefix: symbol("-"),
        negative_suffix: symbol(""),
        prefix: symbol(""),
        suffix: symbol(" "),
        range,
        fallback: Some(symbol("decimal")),
        pad_minimum_length: 0,
        pad_symbol: symbol(""),
    })
}

/// The counter style a `<counter-style>` value names, resolved from `tree_scope`: the style the
/// registry holds under the name, or the anonymous style a `symbols()` function defines. `None`
/// for a name no scope in the chain registers, which reads as `decimal`.
pub(crate) fn resolve_counter_style_value(
    registry: &CounterStyleRegistry,
    tree_scope: u32,
    value: Option<&StyleValueData>,
) -> Option<Arc<CounterStyle>> {
    let Some(StyleValueData::CounterStyle {
        is_symbols,
        name,
        symbols_type,
        symbols,
    }) = value
    else {
        return None;
    };
    if *is_symbols {
        return Some(symbols_function_counter_style(*symbols_type, symbols.as_slice()));
    }
    registry.lookup(tree_scope, name.units()).cloned()
}

// C++ computes `value % symbol_list.size()` and `value - 1 % ...` in the unsigned type the size
// brings to the expression, so a negative value wraps rather than indexing backwards. Only the
// cyclic system reaches these with a negative value - every other one takes the absolute value
// first - but the wrap is observable there, so it is reproduced exactly.
fn wrapping_index(value: i64, modulus: usize) -> usize {
    ((value as u64) % (modulus as u64)) as usize
}

impl CounterStyle {
    /// Whether every counter value this style represents produces the same text, which is what
    /// makes a list marker built from it independent of its list item's counter value.
    pub(crate) fn representation_is_constant(&self) -> bool {
        let Algorithm::Generic {
            system: GenericSystem::Cyclic,
            symbols,
        } = &self.algorithm
        else {
            return false;
        };
        symbols.len() == 1 && self.range.len() == 1 && self.range[0].start == i32::MIN && self.range[0].end == i32::MAX
    }

    /// https://drafts.csswg.org/css-counter-styles-3/#counter-style-negative
    /// Not all system values use a negative sign. In particular, a counter style uses a negative
    /// sign if its system value is symbolic, alphabetic, numeric, additive, or extends if the
    /// extended counter style itself uses a negative sign.
    // NB: We have resolved extends to the underlying algorithm before this is asked.
    fn uses_a_negative_sign(&self) -> bool {
        match &self.algorithm {
            Algorithm::Additive(_) => true,
            Algorithm::Fixed { .. } => false,
            Algorithm::Generic { system, .. } => !matches!(system, GenericSystem::Cyclic),
            // https://drafts.csswg.org/css-counter-styles-3/#complex-predefined-counters
            // All of the counter styles defined in this section have a spoken form of numbers, and
            // use a negative sign.
            Algorithm::EthiopicNumeric | Algorithm::ExtendedCjk(_) => true,
        }
    }

    /// Only a cyclic style with one distinct symbol represents every value alike, and only if no
    /// value is outside its range and left to the fallback style. Every other system counts, or
    /// leaves some value to the fallback style.
    fn representation_depends_on_value(&self) -> bool {
        let Algorithm::Generic {
            system: GenericSystem::Cyclic,
            symbols,
        } = &self.algorithm
        else {
            return true;
        };
        let Some((first, rest)) = symbols.split_first() else {
            return true;
        };
        let represents_every_value = self
            .range
            .iter()
            .any(|entry| entry.start == i32::MIN && entry.end == i32::MAX);
        !represents_every_value || rest.iter().any(|symbol| symbol != first)
    }

    /// The representation before the pad and the negative sign are applied, or nothing when this
    /// style cannot represent the value and the fallback style must.
    fn generate_an_initial_representation_for_the_counter_value(&self, mut value: i64) -> Option<Vec<u16>> {
        match &self.algorithm {
            // https://drafts.csswg.org/css-counter-styles-3/#additive-system
            Algorithm::Additive(symbol_list) => {
                // 1. Let value initially be the counter value, S initially be the empty string, and
                //    symbol list initially be the list of additive tuples.

                // 2. If value is zero:
                if value == 0 {
                    // 1. If symbol list contains a tuple with a weight of zero, append that tuple's
                    //    counter symbol to S and return S.
                    if let Some((_, symbol)) = symbol_list.iter().find(|(weight, _)| *weight == 0) {
                        return Some(symbol.to_vec());
                    }

                    // 2. Otherwise, the given counter value cannot be represented by this counter
                    //    style, and must instead be represented by the fallback counter style.
                    return None;
                }

                let mut representation = Vec::new();

                // 3. For each tuple in symbol list:
                for (weight, symbol) in symbol_list {
                    // 1. Let symbol and weight be tuple's counter symbol and weight, respectively.

                    // 2. If weight is zero, or weight is greater than value, continue.
                    let weight = i64::from(*weight);
                    if weight == 0 || weight > value {
                        continue;
                    }

                    // 3. Let reps be floor( value / weight ).
                    let reps = value / weight;

                    // 4. Append symbol to S reps times.
                    for _ in 0..reps {
                        representation.extend_from_slice(symbol);
                    }

                    // 5. Decrement value by weight * reps.
                    value -= weight * reps;

                    // 6. If value is zero, return S.
                    if value == 0 {
                        return Some(representation);
                    }
                }

                // The given counter value cannot be represented by this counter style, and must
                // instead be represented by the fallback counter style.
                None
            }
            // https://drafts.csswg.org/css-counter-styles-3/#fixed-system
            // The first counter symbol is the representation for the first symbol value, and
            // subsequent counter values are represented by subsequent counter symbols. Once the
            // list of counter symbols is exhausted, further values cannot be represented by this
            // counter style, and must instead be represented by the fallback counter style.
            Algorithm::Fixed { first_symbol, symbols } => {
                let index = value - i64::from(*first_symbol);
                if index < 0 || index >= symbols.len() as i64 {
                    return None;
                }
                Some(symbols[index as usize].to_vec())
            }
            Algorithm::Generic { system, symbols } => {
                // A system with no symbols parses as invalid, so the list is never empty; answering
                // with the fallback rather than dividing by zero keeps that assumption harmless.
                if symbols.is_empty() {
                    return None;
                }
                match system {
                    // https://drafts.csswg.org/css-counter-styles-3/#cyclic-system
                    // If there are N counter symbols and a representation is being constructed for
                    // the integer value, the representation is the counter symbol at index
                    // ( (value-1) mod N) of the list of counter symbols (0-indexed).
                    GenericSystem::Cyclic => Some(symbols[wrapping_index(value - 1, symbols.len())].to_vec()),
                    // https://drafts.csswg.org/css-counter-styles-3/#numeric-system
                    // If there are N counter symbols, the representation is a base N number using
                    // the counter symbols as digits.
                    GenericSystem::Numeric => {
                        // 1. If value is 0, append symbol(0) to S and return S.
                        if value == 0 {
                            return Some(symbols[0].to_vec());
                        }

                        let mut digits = Vec::new();

                        // 2. While value is not equal to 0:
                        while value != 0 {
                            // 1. Prepend symbol( value mod N ) to S.
                            digits.push(&symbols[wrapping_index(value, symbols.len())]);

                            // 2. Set value to floor( value / N ).
                            value /= symbols.len() as i64;
                        }

                        // 3. Return S.
                        Some(join_symbols_in_reverse(&digits))
                    }
                    // https://drafts.csswg.org/css-counter-styles-3/#alphabetic-system
                    // If there are N counter symbols, the representation is a base N alphabetic
                    // number using the counter symbols as digits.
                    GenericSystem::Alphabetic => {
                        let mut digits = Vec::new();

                        // While value is not equal to 0:
                        while value != 0 {
                            // 1. Set value to value - 1.
                            value -= 1;

                            // 2. Prepend symbol( value mod N ) to S.
                            digits.push(&symbols[wrapping_index(value, symbols.len())]);

                            // 3. Set value to floor( value / N ).
                            value /= symbols.len() as i64;
                        }

                        // Finally, return S.
                        Some(join_symbols_in_reverse(&digits))
                    }
                    // https://drafts.csswg.org/css-counter-styles-3/#symbolic-system
                    GenericSystem::Symbolic => {
                        // 1. Let the chosen symbol be symbol( (value - 1) mod N).
                        let chosen = &symbols[wrapping_index(value - 1, symbols.len())];

                        // 2. Let the representation length be ceil( value / N ).
                        let representation_length = (value as u64).div_ceil(symbols.len() as u64);

                        // 3. Append the chosen symbol to S a number of times equal to the
                        //    representation length. Finally, return S.
                        let mut representation = Vec::with_capacity(chosen.len() * representation_length as usize);
                        for _ in 0..representation_length {
                            representation.extend_from_slice(chosen);
                        }
                        Some(representation)
                    }
                }
            }
            Algorithm::EthiopicNumeric => Some(generate_an_ethiopic_numeric_representation(value)),
            Algorithm::ExtendedCjk(style) => Some(generate_an_extended_cjk_representation(value, *style)),
        }
    }
}

fn join_symbols_in_reverse(symbols: &[&Symbol]) -> Vec<u16> {
    let mut representation = Vec::with_capacity(symbols.iter().map(|symbol| symbol.len()).sum());
    for symbol in symbols.iter().rev() {
        representation.extend_from_slice(symbol);
    }
    representation
}

/// https://drafts.csswg.org/css-counter-styles-3/#ethiopic-numeric-counter-style
/// The following algorithm converts decimal digits to ethiopic numbers.
fn generate_an_ethiopic_numeric_representation(mut value: i64) -> Vec<u16> {
    // 1. If the number is 1, return "፩" (U+1369).
    if value == 1 {
        return vec![0x1369];
    }

    // 2. Split the number into groups of two digits, starting with the least significant decimal digit.
    let mut groups = Vec::new();
    while value != 0 {
        groups.push((value % 100) as u8);
        value /= 100;
    }

    let mut representation = Vec::new();

    // 3. Index each group sequentially, starting from the least significant as group number zero.
    // NB: We iterate in descending order of significance, so we can append in order.
    for index in (0..groups.len()).rev() {
        let group_value = groups[index];

        // 4. If the group has the value zero, or if the group is the most significant one and has
        //    the value 1, or if the group has an odd index (as given in the previous step) and has
        //    the value 1, then remove the digits (but leave the group, so it still has a separator
        //    appended below).
        if group_value != 0 && !(index == groups.len() - 1 && group_value == 1) && !(index % 2 == 1 && group_value == 1)
        {
            // 5. For each remaining digit, substitute the relevant ethiopic character.
            // Tens: 10 ፲ U+1372 .. 90 ፺ U+137A
            let tens = group_value / 10;
            if tens != 0 {
                representation.push(0x1372 + u16::from(tens) - 1);
            }

            // Units: 1 ፩ U+1369 .. 9 ፱ U+1371
            let units = group_value % 10;
            if units != 0 {
                representation.push(0x1369 + u16::from(units) - 1);
            }
        }

        // 6. For each group with an odd index, except groups which originally had a value of zero,
        //    append ፻ U+137B.
        if index % 2 == 1 && group_value != 0 {
            representation.push(0x137B);
        }
        // 7. For each group with an even index, except the group with index 0, append ፼ U+137C.
        else if index % 2 == 0 && index != 0 {
            representation.push(0x137C);
        }
    }

    // 8. Concatenate the groups into one string, and return it.
    representation
}

/// The characters one extended CJK style spells its digits, digit markers and group markers with.
struct ExtendedCjkCharacters {
    digits: [&'static [u16]; 10],
    digit_markers: [&'static [u16]; 3],
    group_markers: [&'static [u16]; 3],
}

// https://drafts.csswg.org/css-counter-styles-3/#extended-range-optional
// The tables below define the characters used in these styles.
fn extended_cjk_characters(style: ExtendedCjkStyle) -> ExtendedCjkCharacters {
    // | Values                | simp-chinese-informal | simp-chinese-formal | trad-chinese-informal | trad-chinese-formal
    // | Digit 0               | 零 U+96F6             | 零 U+96F6           | 零 U+96F6             | 零 U+96F6
    // | Digit 1               | 一 U+4E00             | 壹 U+58F9           | 一 U+4E00             | 壹 U+58F9
    // | Digit 2               | 二 U+4E8C             | 贰 U+8D30           | 二 U+4E8C             | 貳 U+8CB3
    // | Digit 3               | 三 U+4E09             | 叁 U+53C1           | 三 U+4E09             | 參 U+53C3
    // | Digit 4               | 四 U+56DB             | 肆 U+8086           | 四 U+56DB             | 肆 U+8086
    // | Digit 5               | 五 U+4E94             | 伍 U+4F0D           | 五 U+4E94             | 伍 U+4F0D
    // | Digit 6               | 六 U+516D             | 陆 U+9646           | 六 U+516D             | 陸 U+9678
    // | Digit 7               | 七 U+4E03             | 柒 U+67D2           | 七 U+4E03             | 柒 U+67D2
    // | Digit 8               | 八 U+516B             | 捌 U+634C           | 八 U+516B             | 捌 U+634C
    // | Digit 9               | 九 U+4E5D             | 玖 U+7396           | 九 U+4E5D             | 玖 U+7396
    // | Second Digit Marker   | 十 U+5341             | 拾 U+62FE           | 十 U+5341             | 拾 U+62FE
    // | Third Digit Marker    | 百 U+767E             | 佰 U+4F70           | 百 U+767E             | 佰 U+4F70
    // | Fourth Digit Marker   | 千 U+5343             | 仟 U+4EDF           | 千 U+5343             | 仟 U+4EDF
    // | Second Group Marker   | 万 U+4E07             | 万 U+4E07           | 萬 U+842C             | 萬 U+842C
    // | Third Group Marker    | 亿 U+4EBF             | 亿 U+4EBF           | 億 U+5104             | 億 U+5104
    // | Fourth Group Marker   | 万亿 U+4E07 U+4EBF    | 万亿 U+4E07 U+4EBF  | 兆 U+5146             | 兆 U+5146
    //
    // | Values                | japanese-informal | japanese-formal | korean-hangul-formal | korean-hanja-informal | korean-hanja-formal
    // | Digit 0               | 〇 U+3007         | 零 U+96F6       | 영 U+C601            | 零 U+96F6             | 零 U+96F6
    // | Digit 1               | 一 U+4E00         | 壱 U+58F1       | 일 U+C77C            | 一 U+4E00             | 壹 U+58F9
    // | Digit 2               | 二 U+4E8C         | 弐 U+5F10       | 이 U+C774            | 二 U+4E8C             | 貳 U+8CB3
    // | Digit 3               | 三 U+4E09         | 参 U+53C2       | 삼 U+C0BC            | 三 U+4E09             | 參 U+53C3
    // | Digit 4               | 四 U+56DB         | 四 U+56DB       | 사 U+C0AC            | 四 U+56DB             | 四 U+56DB
    // | Digit 5               | 五 U+4E94         | 伍 U+4F0D       | 오 U+C624            | 五 U+4E94             | 五 U+4E94
    // | Digit 6               | 六 U+516D         | 六 U+516D       | 육 U+C721            | 六 U+516D             | 六 U+516D
    // | Digit 7               | 七 U+4E03         | 七 U+4E03       | 칠 U+CE60            | 七 U+4E03             | 七 U+4E03
    // | Digit 8               | 八 U+516B         | 八 U+516B       | 팔 U+D314            | 八 U+516B             | 八 U+516B
    // | Digit 9               | 九 U+4E5D         | 九 U+4E5D       | 구 U+AD6C            | 九 U+4E5D             | 九 U+4E5D
    // | Second Digit Marker   | 十 U+5341         | 拾 U+62FE       | 십 U+C2ED            | 十 U+5341             | 拾 U+62FE
    // | Third Digit Marker    | 百 U+767E         | 百 U+767E       | 백 U+BC31            | 百 U+767E             | 百 U+767E
    // | Fourth Digit Marker   | 千 U+5343         | 阡 U+9621       | 천 U+CC9C            | 千 U+5343             | 仟 U+4EDF
    // | Second Group Marker   | 万 U+4E07         | 萬 U+842C       | 만 U+B9CC            | 萬 U+842C             | 萬 U+842C
    // | Third Group Marker    | 億 U+5104         | 億 U+5104       | 억 U+C5B5            | 億 U+5104             | 億 U+5104
    // | Fourth Group Marker   | 兆 U+5146         | 兆 U+5146       | 조 U+C870            | 兆 U+5146             | 兆 U+5146
    const SIMP_INFORMAL_DIGITS: [&[u16]; 10] = [
        &[0x96F6],
        &[0x4E00],
        &[0x4E8C],
        &[0x4E09],
        &[0x56DB],
        &[0x4E94],
        &[0x516D],
        &[0x4E03],
        &[0x516B],
        &[0x4E5D],
    ];
    const FORMAL_CHINESE_DIGIT_MARKERS: [&[u16]; 3] = [&[0x62FE], &[0x4F70], &[0x4EDF]];
    const INFORMAL_CHINESE_DIGIT_MARKERS: [&[u16]; 3] = [&[0x5341], &[0x767E], &[0x5343]];
    const SIMP_GROUP_MARKERS: [&[u16]; 3] = [&[0x4E07], &[0x4EBF], &[0x4E07, 0x4EBF]];
    const TRAD_GROUP_MARKERS: [&[u16]; 3] = [&[0x842C], &[0x5104], &[0x5146]];

    match style {
        ExtendedCjkStyle::SimpChineseInformal => ExtendedCjkCharacters {
            digits: SIMP_INFORMAL_DIGITS,
            digit_markers: INFORMAL_CHINESE_DIGIT_MARKERS,
            group_markers: SIMP_GROUP_MARKERS,
        },
        ExtendedCjkStyle::SimpChineseFormal => ExtendedCjkCharacters {
            digits: [
                &[0x96F6],
                &[0x58F9],
                &[0x8D30],
                &[0x53C1],
                &[0x8086],
                &[0x4F0D],
                &[0x9646],
                &[0x67D2],
                &[0x634C],
                &[0x7396],
            ],
            digit_markers: FORMAL_CHINESE_DIGIT_MARKERS,
            group_markers: SIMP_GROUP_MARKERS,
        },
        ExtendedCjkStyle::TradChineseInformal => ExtendedCjkCharacters {
            digits: SIMP_INFORMAL_DIGITS,
            digit_markers: INFORMAL_CHINESE_DIGIT_MARKERS,
            group_markers: TRAD_GROUP_MARKERS,
        },
        ExtendedCjkStyle::TradChineseFormal => ExtendedCjkCharacters {
            digits: [
                &[0x96F6],
                &[0x58F9],
                &[0x8CB3],
                &[0x53C3],
                &[0x8086],
                &[0x4F0D],
                &[0x9678],
                &[0x67D2],
                &[0x634C],
                &[0x7396],
            ],
            digit_markers: FORMAL_CHINESE_DIGIT_MARKERS,
            group_markers: TRAD_GROUP_MARKERS,
        },
        ExtendedCjkStyle::JapaneseInformal => ExtendedCjkCharacters {
            digits: [
                &[0x3007],
                &[0x4E00],
                &[0x4E8C],
                &[0x4E09],
                &[0x56DB],
                &[0x4E94],
                &[0x516D],
                &[0x4E03],
                &[0x516B],
                &[0x4E5D],
            ],
            digit_markers: INFORMAL_CHINESE_DIGIT_MARKERS,
            group_markers: [&[0x4E07], &[0x5104], &[0x5146]],
        },
        ExtendedCjkStyle::JapaneseFormal => ExtendedCjkCharacters {
            digits: [
                &[0x96F6],
                &[0x58F1],
                &[0x5F10],
                &[0x53C2],
                &[0x56DB],
                &[0x4F0D],
                &[0x516D],
                &[0x4E03],
                &[0x516B],
                &[0x4E5D],
            ],
            digit_markers: [&[0x62FE], &[0x767E], &[0x9621]],
            group_markers: TRAD_GROUP_MARKERS,
        },
        ExtendedCjkStyle::KoreanHangulFormal => ExtendedCjkCharacters {
            digits: [
                &[0xC601],
                &[0xC77C],
                &[0xC774],
                &[0xC0BC],
                &[0xC0AC],
                &[0xC624],
                &[0xC721],
                &[0xCE60],
                &[0xD314],
                &[0xAD6C],
            ],
            digit_markers: [&[0xC2ED], &[0xBC31], &[0xCC9C]],
            group_markers: [&[0xB9CC], &[0xC5B5], &[0xC870]],
        },
        ExtendedCjkStyle::KoreanHanjaInformal => ExtendedCjkCharacters {
            digits: SIMP_INFORMAL_DIGITS,
            digit_markers: INFORMAL_CHINESE_DIGIT_MARKERS,
            group_markers: TRAD_GROUP_MARKERS,
        },
        ExtendedCjkStyle::KoreanHanjaFormal => ExtendedCjkCharacters {
            digits: [
                &[0x96F6],
                &[0x58F9],
                &[0x8CB3],
                &[0x53C3],
                &[0x56DB],
                &[0x4E94],
                &[0x516D],
                &[0x4E03],
                &[0x516B],
                &[0x4E5D],
            ],
            // NB: The third digit marker is 百 U+767E here, not the 佰 U+4F70 the formal Chinese
            //     styles use.
            digit_markers: [&[0x62FE], &[0x767E], &[0x4EDF]],
            group_markers: TRAD_GROUP_MARKERS,
        },
    }
}

/// https://drafts.csswg.org/css-counter-styles-3/#extended-range-optional
/// All of the styles are defined by almost identical algorithms (specified as a single algorithm
/// here, with the differences called out when relevant), but use different sets of characters.
fn generate_an_extended_cjk_representation(mut value: i64, style: ExtendedCjkStyle) -> Vec<u16> {
    use ExtendedCjkStyle::*;

    let characters = extended_cjk_characters(style);

    // 1. If the counter value is 0, the representation is the character for 0 specified for the
    //    given counter style. Skip the rest of this algorithm.
    if value == 0 {
        return characters.digits[0].to_vec();
    }

    // 2. If the counter value is negative, instead use the absolute value of the counter value for
    //    the remaining steps of this algorithm.
    // NB: This is handled by the caller.

    // 3. Initially represent the counter value as a decimal number. Starting from the right (ones
    //    place), split the decimal number into groups of four digits.
    let mut groups = Vec::new();
    while value > 0 {
        groups.push((value % 10000) as u16);
        value /= 10000;
    }

    let mut representation = Vec::new();

    for group_index in (0..groups.len()).rev() {
        let group_value = groups[group_index];

        let mut digits = Vec::new();
        let mut remaining = group_value;
        while remaining > 0 {
            digits.push((remaining % 10) as u8);
            remaining /= 10;
        }

        // NB: Pad the group with zeroes up to four digits, unless this is the most significant group.
        if group_index != groups.len() - 1 {
            while digits.len() < 4 {
                digits.push(0);
            }
        }

        // NB: We move around the order of spec steps to work with a string builder rather than
        //     replacing characters in a string.
        for digit_index in (0..digits.len()).rev() {
            let digit_value = digits[digit_index];

            let mut should_drop_digit = false;
            // 6. Drop ones:
            //  - For the Chinese informal styles, for any group with a value between ten and
            //    nineteen, remove the tens digit (leave the digit marker).
            if matches!(style, SimpChineseInformal | TradChineseInformal) {
                should_drop_digit |= (10..20).contains(&group_value) && digit_index == 1;
            }

            //  - For the Japanese informal and Korean informal styles, if any of the digit markers
            //    are preceded by the digit 1, and that digit is not the first digit of the group,
            //    remove the digit (leave the digit marker).
            if matches!(style, JapaneseInformal | KoreanHanjaInformal) {
                should_drop_digit |= digit_value == 1 && digit_index != 0;
            }

            //  - For Korean informal styles, if the value of the ten-thousands group is 1, drop the
            //    digit (leave the digit marker).
            if style == KoreanHanjaInformal {
                should_drop_digit |= group_index == 1 && group_value == 1 && digit_index == 0;
            }

            // 7. Drop zeros:
            //  - For the Japanese and Korean styles, drop all zero digits.
            if matches!(
                style,
                JapaneseInformal | JapaneseFormal | KoreanHangulFormal | KoreanHanjaInformal | KoreanHanjaFormal
            ) {
                should_drop_digit |= digit_value == 0;
            }

            //  - For the Chinese styles, drop any trailing zeros for all non-zero groups and
            //    collapse (across groups) each remaining consecutive group of zeros into a single
            //    zero digit.
            if matches!(
                style,
                SimpChineseInformal | SimpChineseFormal | TradChineseInformal | TradChineseFormal
            ) {
                let is_trailing_zero = digits[..=digit_index].iter().all(|digit| *digit == 0);
                should_drop_digit |= is_trailing_zero;

                // NB: We don't need to worry about collapsing across groups since dropping trailing
                //     zeroes above means that a run of zeroes can't occur at the end of a group.
                should_drop_digit |= digit_value == 0 && digit_index != 3 && digits[digit_index + 1] == 0;
            }

            // 9. Replace the digits 0-9 with the appropriate character for the given counter style.
            if !should_drop_digit {
                representation.extend_from_slice(characters.digits[usize::from(digit_value)]);
            }

            // 5. Within each group, for each digit that is not 0, append the appropriate digit
            //    marker to the digit. The ones digit of each group has no marker.
            if digit_value != 0 && digit_index != 0 {
                representation.extend_from_slice(characters.digit_markers[digit_index - 1]);
            }
        }

        // 4. For each group with a non-zero value, append the appropriate group marker to the
        //    group. The ones group has no marker.
        if group_value != 0 && group_index != 0 {
            representation.extend_from_slice(characters.group_markers[group_index - 1]);
        }

        // 8. For the Korean styles, insert a space (" " U+0020) between each group.
        if matches!(style, KoreanHangulFormal | KoreanHanjaInformal | KoreanHanjaFormal) && group_index != 0 {
            representation.push(u16::from(b' '));
        }
    }

    // 10. If the counter value was negative, prepend the appropriate negative sign character.
    // NB: This is handled by the caller.

    // 11. Return the resultant string as the representation of the counter value.
    representation
}

/// https://drafts.csswg.org/css-counter-styles-3/#generate-a-counter
/// When asked to generate a counter representation using a particular counter style for a
/// particular counter value, follow these steps.
pub(crate) fn generate_a_counter_representation<'a>(
    registry: &'a CounterStyleRegistry,
    tree_scope: u32,
    counter_style: Option<&'a CounterStyle>,
    value: i32,
) -> Vec<u16> {
    // https://drafts.csswg.org/css-counter-styles-3/#counter-style-fallback
    // If the value of the fallback descriptor isn't the name of any defined counter style, the used
    // value of the fallback descriptor is decimal instead. Similarly, while following fallbacks to
    // find a counter style that can render the given counter value, if a loop in the specified
    // fallbacks is detected, the decimal style must be used instead.
    let mut fallback_history: Vec<&'a [u16]> = Vec::new();

    // 1. If the counter style is unknown, exit this algorithm and instead generate a counter
    //    representation using the decimal style and the same counter value.
    let mut counter_style = counter_style.unwrap_or(decimal());

    // Each hop either lands on `decimal`, which represents every value and so is the last one, or
    // on a style whose name the history does not hold yet, so the chain is bounded by the registry.
    // The bound is a backstop for a registry that changed under a chain, not a rule of the algorithm.
    for _ in 0..=registry
        .scopes
        .values()
        .map(|scope| scope.styles.0.len())
        .sum::<usize>()
        + 2
    {
        let mut fall_back_to = |style: &'a CounterStyle| -> &'a CounterStyle {
            let Some(fallback_name) = style.fallback.as_deref() else {
                return decimal();
            };
            let Some(fallback) = registry.lookup(tree_scope, fallback_name) else {
                return decimal();
            };
            if fallback_history.contains(&fallback_name) {
                return decimal();
            }
            fallback_history.push(&style.name);
            fallback.as_ref()
        };

        // 2. If the counter value is outside the range of the counter style, exit this algorithm
        //    and instead generate a counter representation using the counter style's fallback style
        //    and the same counter value.
        if !counter_style
            .range
            .iter()
            .any(|entry| value >= entry.start && value <= entry.end)
        {
            counter_style = fall_back_to(counter_style);
            continue;
        }

        let value_is_negative_and_uses_negative_sign = value < 0 && counter_style.uses_a_negative_sign();

        // 3. Using the counter value and the counter algorithm for the counter style, generate an
        //    initial representation for the counter value. If the counter value is negative and the
        //    counter style uses a negative sign, instead generate an initial representation using
        //    the absolute value of the counter value.
        let initial_value = if value_is_negative_and_uses_negative_sign {
            i64::from(value).abs()
        } else {
            i64::from(value)
        };

        // AD-HOC: Algorithms are sometimes unable to produce a representation and require us to use
        //         the fallback.
        let Some(mut representation) =
            counter_style.generate_an_initial_representation_for_the_counter_value(initial_value)
        else {
            counter_style = fall_back_to(counter_style);
            continue;
        };

        // 4. Prepend symbols to the representation as specified in the pad descriptor.
        // https://drafts.csswg.org/css-counter-styles-3/#counter-style-pad
        // Let difference be the provided <integer> minus the number of grapheme clusters in the
        // initial representation for the counter value.
        // FIXME: We should be counting grapheme clusters here.
        let mut difference = counter_style.pad_minimum_length - representation.len() as i32;

        // If the counter value is negative and the counter style uses a negative sign, further
        // reduce difference by the number of grapheme clusters in the counter style's negative
        // descriptor's <symbol>(s).
        // FIXME: We should be counting grapheme clusters here.
        if value_is_negative_and_uses_negative_sign {
            difference -= counter_style.negative_prefix.len() as i32 + counter_style.negative_suffix.len() as i32;
        }

        // If difference is greater than zero, prepend difference copies of the specified <symbol>
        // to the representation.
        if difference > 0 {
            let mut padded =
                Vec::with_capacity(counter_style.pad_symbol.len() * difference as usize + representation.len());
            for _ in 0..difference {
                padded.extend_from_slice(&counter_style.pad_symbol);
            }
            padded.append(&mut representation);
            representation = padded;
        }

        // 5. If the counter value is negative and the counter style uses a negative sign, wrap the
        //    representation in the counter style's negative sign as specified in the negative
        //    descriptor.
        if value_is_negative_and_uses_negative_sign {
            let mut signed = Vec::with_capacity(
                counter_style.negative_prefix.len() + representation.len() + counter_style.negative_suffix.len(),
            );
            signed.extend_from_slice(&counter_style.negative_prefix);
            signed.append(&mut representation);
            signed.extend_from_slice(&counter_style.negative_suffix);
            representation = signed;
        }

        // 6. Return the representation.
        return representation;
    }

    Vec::new()
}

// The FFI half: C++ style scopes hold their registered counter styles as `Arc` pointers, publish
// them to the layout node arena, and ask what a list marker shows.

/// Takes another reference to a scope's registered counter styles.
///
/// # Safety
///
/// `styles` must be a live reference from `rust_counter_styles_resolve`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_counter_styles_retain(
    styles: *const RegisteredCounterStyles,
) -> *const RegisteredCounterStyles {
    // SAFETY: The caller holds a reference, so the allocation is live.
    unsafe { Arc::increment_strong_count(styles) };
    styles
}

/// # Safety
///
/// `styles` must be null or a reference from `rust_counter_styles_resolve` or
/// `rust_counter_styles_retain` that has not been released.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_counter_styles_release(styles: *const RegisteredCounterStyles) {
    if !styles.is_null() {
        // SAFETY: The caller gives up the reference it holds.
        drop(unsafe { Arc::from_raw(styles) });
    }
}

/// Whether the marker text of a list item whose `list-style-type` is `list_style_type` depends on
/// its counter value, with counter style names looked up in `scopes`, nearest first.
///
/// # Safety
///
/// `list_style_type` must point to a live style value, and `scopes` must address `scope_count`
/// live registrations.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_list_style_type_depends_on_counter_value(
    list_style_type: *const c_void,
    scopes: *const &RegisteredCounterStyles,
    scope_count: usize,
) -> bool {
    // SAFETY: Guaranteed by the caller.
    let (list_style_type, scopes) = unsafe {
        (
            &*list_style_type.cast::<StyleValueData>(),
            std::slice::from_raw_parts(scopes, scope_count),
        )
    };
    match list_style_type {
        // `none`, and a string marker, are the same for every item.
        StyleValueData::Keyword { .. } | StyleValueData::String { .. } => false,
        StyleValueData::CounterStyle {
            is_symbols: true,
            symbols_type,
            symbols,
            ..
        } => symbols_function_counter_style(*symbols_type, symbols.as_slice()).representation_depends_on_value(),
        // A name no scope registers counts in decimal.
        StyleValueData::CounterStyle { name, .. } => RegisteredCounterStyles::lookup(scopes, name.units())
            .is_none_or(|style| style.representation_depends_on_value()),
        _ => true,
    }
}

/// Replaces one tree scope's registered counter styles, and names the scope a name it does not
/// register is looked for in next.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread, and `styles` null, for a scope
/// that registers none, or a live reference from `rust_counter_styles_resolve`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_publish_counter_styles(
    host: &crate::render_state::DocumentHost,
    tree_scope: u32,
    parent_tree_scope: u32,
    has_parent_tree_scope: bool,
    styles: *const RegisteredCounterStyles,
) {
    let scope = CounterStyleScope {
        parent: has_parent_tree_scope.then_some(parent_tree_scope),
        // SAFETY: The caller keeps its own reference.
        styles: if styles.is_null() {
            Arc::default()
        } else {
            unsafe { Arc::from_raw(rust_counter_styles_retain(styles)) }
        },
    };
    host.queue_change(crate::render_state::ArenaChange::Layout(
        crate::layout::layout_changes::LayoutChange::CounterStyles { tree_scope, scope },
    ));
}

#[cfg(test)]
mod tests;
