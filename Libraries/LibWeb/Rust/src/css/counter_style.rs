/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::counter_representation::{
    Algorithm, CounterStyle, ExtendedCjkStyle, GenericSystem, RangeEntry, RegisteredCounterStyles, Symbol, auto_range,
    decimal, symbol,
};
use crate::css::css_enums::{
    counter_style_system, keyword, keyword_from_ascii_case_insensitive, keyword_to_counter_style_name_keyword,
};
use crate::css::css_string::CssString;
use crate::css::descriptor_block::{DescriptorBlockData, FfiDescriptorBlock, rust_descriptor_block_retain};
use crate::css::ffi_support::{FfiUtf16View, ascii_lowercase};
use crate::css::parser::descriptor_parser::resolve_descriptor_integer;
use crate::css::parser::value_parser::equals_ascii_case_insensitive;
use crate::css::style_compute::FfiLengthResolutionContext;
use crate::css::style_value::{RetainedStyleValueData, StyleValueData};
use std::cell::RefCell;
use std::collections::HashMap;
use std::ffi::c_void;
use std::sync::Arc;

#[derive(Clone)]
pub struct CounterStyleData {
    pub(crate) name: CssString,
    pub(crate) descriptors: Arc<DescriptorBlockData>,
}

const _: () = {
    const fn assert_send_sync<T: Send + Sync>() {}
    assert_send_sync::<CounterStyleData>();
};

pub struct FfiCounterStyle {
    name: RefCell<CssString>,
    descriptors: FfiDescriptorBlock,
}

impl FfiCounterStyle {
    pub(crate) fn name(&self) -> CssString {
        self.name.borrow().clone()
    }

    pub(crate) fn descriptors(&self) -> Arc<DescriptorBlockData> {
        self.descriptors.data()
    }

    pub(crate) fn new(data: Arc<CounterStyleData>) -> Self {
        Self {
            name: RefCell::new(data.name.clone()),
            descriptors: FfiDescriptorBlock::new(data.descriptors.clone()),
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_counter_style_descriptors(rule: &FfiCounterStyle) -> *mut FfiDescriptorBlock {
    rust_descriptor_block_retain(&rule.descriptors)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_counter_style_name(rule: &FfiCounterStyle) -> FfiUtf16View {
    let name = rule.name.borrow();
    let name = name.units();
    FfiUtf16View {
        ascii: std::ptr::null(),
        utf16: name.as_ptr(),
        length: name.len(),
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_counter_style_set_name(rule: &FfiCounterStyle, name: FfiUtf16View) -> bool {
    let Some(mut name) = (unsafe { name.to_utf16() }) else {
        return false;
    };
    // https://drafts.csswg.org/css-counter-styles-3/#dom-csscounterstylerule-name
    // 1. If the value is an ASCII case-insensitive match for "none" or one of the non-overridable counter-style names, do nothing and return.
    let keyword = keyword_from_ascii_case_insensitive(&name);
    if keyword == Some(crate::css::css_enums::keyword::NONE)
        || [
            b"decimal".as_slice(),
            b"disc",
            b"square",
            b"circle",
            b"disclosure-open",
            b"disclosure-closed",
        ]
        .iter()
        .any(|reserved| {
            name.iter()
                .copied()
                .map(ascii_lowercase)
                .eq(reserved.iter().copied().map(u16::from))
        })
    {
        return false;
    }
    // 2. If the value is an ASCII case-insensitive match for any of the predefined counter styles, lowercase it.
    if keyword.and_then(keyword_to_counter_style_name_keyword).is_some() {
        name.iter_mut().for_each(|unit| *unit = ascii_lowercase(*unit));
    }
    // 3. Replace the associated rule’s name with an identifier equal to the value.
    *rule.name.borrow_mut() = CssString::from_utf16(&name);
    true
}

/// https://drafts.csswg.org/css-counter-styles-3/#counter-style-system
enum System {
    Algorithm(Algorithm),
    Extends(Symbol),
}

/// https://drafts.csswg.org/css-counter-styles-3/#counter-style-range
enum Range {
    Auto,
    Entries(Vec<RangeEntry>),
}

/// What a `@counter-style` rule defines. A descriptor it leaves out comes from the style it
/// extends, or takes its initial value.
struct Definition {
    system: System,
    negative: Option<(Symbol, Symbol)>,
    prefix: Option<Symbol>,
    suffix: Option<Symbol>,
    range: Option<Range>,
    fallback: Option<Symbol>,
    pad: Option<(i32, Symbol)>,
}

/// A `<symbol>`: https://drafts.csswg.org/css-counter-styles-3/#typedef-symbol
fn text(value: &StyleValueData) -> Option<Symbol> {
    match value {
        StyleValueData::String { string, .. } => Some(string.units().into()),
        StyleValueData::CustomIdent { custom_ident } => Some(custom_ident.units().into()),
        _ => None,
    }
}

fn list(value: &StyleValueData) -> &[RetainedStyleValueData] {
    match value {
        StyleValueData::ValueList { values, .. } => values.as_slice(),
        _ => &[],
    }
}

/// How many symbols the `symbols` and the `additive-symbols` descriptor each need for a rule with
/// this `system` to define a counter style; the one its system does not read needs none. None for
/// `extends`, which must have neither.
/// https://drafts.csswg.org/css-counter-styles-3/#counter-style-symbols
fn symbol_counts_needed(system: Option<&StyleValueData>) -> Option<(usize, usize)> {
    match system {
        Some(StyleValueData::CounterStyleSystem { kind: 2, .. }) => None,
        Some(StyleValueData::CounterStyleSystem {
            kind: 0,
            system: counter_style_system::ADDITIVE,
            ..
        }) => Some((0, 1)),
        Some(StyleValueData::CounterStyleSystem {
            kind: 0,
            system: counter_style_system::ALPHABETIC | counter_style_system::NUMERIC,
            ..
        }) => Some((2, 0)),
        // Cyclic, symbolic and fixed systems need one symbol, and the initial system is symbolic.
        _ => Some((1, 0)),
    }
}

impl Definition {
    /// None when the rule does not define a counter style, though it is still a valid rule.
    fn from_descriptors(
        descriptors: &DescriptorBlockData,
        length_context: Option<&FfiLengthResolutionContext>,
    ) -> Option<Self> {
        let descriptor = |name: &str| {
            descriptors
                .descriptors
                .iter()
                .find(|descriptor| equals_ascii_case_insensitive(descriptor.name.units(), name.as_bytes()))
                .map(|descriptor| &*descriptor.value)
        };
        let integer = |value: &RetainedStyleValueData| resolve_descriptor_integer(value.data(), length_context);
        let pair = |value: &StyleValueData| match list(value) {
            [integer_value, symbol] => Some((integer(integer_value)?, text(symbol.data())?)),
            _ => None,
        };
        // https://drafts.csswg.org/css-counter-styles-3/#counter-style-symbols
        // The @counter-style rule must have a valid symbols descriptor if the counter system is
        // cyclic, numeric, alphabetic, symbolic, or fixed, or a valid additive-symbols descriptor if
        // the counter system is additive; otherwise, the @counter-style does not define a counter
        // style (but is still a valid at-rule).
        let (symbols_needed, additive_symbols_needed) = symbol_counts_needed(descriptor("system")).unwrap_or_default();
        let symbols = || {
            let symbols = list(descriptor("symbols")?)
                .iter()
                .map(|symbol| text(symbol.data()))
                .collect::<Option<Vec<_>>>()?;
            (symbols.len() >= symbols_needed).then_some(symbols)
        };
        let system = match descriptor("system") {
            Some(StyleValueData::CounterStyleSystem { kind: 2, name, .. }) => System::Extends(name.units().into()),
            // https://drafts.csswg.org/css-counter-styles-3/#fixed-system
            Some(StyleValueData::CounterStyleSystem {
                kind: 1, first_symbol, ..
            }) => System::Algorithm(Algorithm::Fixed {
                // If it is omitted, the first symbol value is 1.
                first_symbol: match first_symbol.optional_data() {
                    Some(_) => integer(first_symbol)?,
                    None => 1,
                },
                symbols: symbols()?,
            }),
            // https://drafts.csswg.org/css-counter-styles-3/#additive-system
            Some(StyleValueData::CounterStyleSystem {
                system: counter_style_system::ADDITIVE,
                ..
            }) => System::Algorithm(Algorithm::Additive(
                list(descriptor("additive-symbols")?)
                    .iter()
                    .map(|tuple| pair(tuple.data()))
                    .collect::<Option<Vec<_>>>()
                    .filter(|tuples| tuples.len() >= additive_symbols_needed)?,
            )),
            // The initial value of system is symbolic.
            system => {
                let system = match system {
                    Some(StyleValueData::CounterStyleSystem { system, .. }) => *system,
                    _ => counter_style_system::SYMBOLIC,
                };
                let system = match system {
                    counter_style_system::CYCLIC => GenericSystem::Cyclic,
                    counter_style_system::NUMERIC => GenericSystem::Numeric,
                    counter_style_system::ALPHABETIC => GenericSystem::Alphabetic,
                    _ => GenericSystem::Symbolic,
                };
                System::Algorithm(Algorithm::Generic {
                    system,
                    symbols: symbols()?,
                })
            }
        };
        // The parser only keeps well-formed values, so the rest read as left out if one is not.
        let range = descriptor("range").and_then(|value| {
            if matches!(value, StyleValueData::Keyword { keyword } if *keyword == keyword::AUTO) {
                return Some(Range::Auto);
            }
            // If infinite is used as the first value in a range, it represents negative
            // infinity; if used as the second value, it represents positive infinity.
            let bound = |value: &RetainedStyleValueData, infinite| match value.data() {
                StyleValueData::Keyword { keyword } if *keyword == keyword::INFINITE => Some(infinite),
                _ => integer(value),
            };
            list(value)
                .iter()
                .map(|entry| match list(entry.data()) {
                    [start, end] => Some(RangeEntry {
                        start: bound(start, i32::MIN)?,
                        end: bound(end, i32::MAX)?,
                    }),
                    _ => None,
                })
                .collect::<Option<_>>()
                .map(Range::Entries)
        });
        let negative = descriptor("negative").and_then(|value| match list(value) {
            [prefix] => Some((text(prefix.data())?, Symbol::default())),
            [prefix, suffix] => Some((text(prefix.data())?, text(suffix.data())?)),
            _ => None,
        });
        Some(Self {
            system,
            negative,
            prefix: descriptor("prefix").and_then(text),
            suffix: descriptor("suffix").and_then(text),
            range,
            fallback: descriptor("fallback").and_then(text),
            pad: descriptor("pad").and_then(pair),
        })
    }

    /// The counter style this definition describes under `name`, given `base`: the style its
    /// `extends` names, or for any other system `decimal`, whose descriptors are the initial values.
    fn resolve(&self, name: &[u16], base: &CounterStyle) -> CounterStyle {
        let algorithm = match &self.system {
            System::Algorithm(algorithm) => algorithm.clone(),
            System::Extends(_) => base.algorithm.clone(),
        };
        let range = match (&self.range, &self.system) {
            (Some(Range::Entries(entries)), _) => entries.clone(),
            (None, System::Extends(_)) => base.range.clone(),
            _ => auto_range(&algorithm).unwrap_or_else(|| base.range.clone()),
        };
        let (negative_prefix, negative_suffix) = self
            .negative
            .clone()
            .unwrap_or_else(|| (base.negative_prefix.clone(), base.negative_suffix.clone()));
        let (pad_minimum_length, pad_symbol) = self
            .pad
            .clone()
            .unwrap_or_else(|| (base.pad_minimum_length, base.pad_symbol.clone()));
        CounterStyle {
            name: name.into(),
            algorithm,
            negative_prefix,
            negative_suffix,
            prefix: self.prefix.as_ref().unwrap_or(&base.prefix).clone(),
            suffix: self.suffix.as_ref().unwrap_or(&base.suffix).clone(),
            range,
            fallback: Some(
                self.fallback
                    .as_ref()
                    .or(base.fallback.as_ref())
                    .map_or_else(|| decimal().name.clone(), Clone::clone),
            ),
            pad_minimum_length,
            pad_symbol,
        }
    }
}

/// https://drafts.csswg.org/css-counter-styles-3/#complex-predefined-counters
/// "While authors may define their own counter styles using the @counter-style rule or rely on the
/// set of predefined counter styles, a few counter styles are described by rules that are too
/// complex to be captured by the predefined algorithms."
fn complex_predefined_definitions() -> impl Iterator<Item = (Symbol, Definition)> {
    let definition = |algorithm, negative: Option<&str>, suffix, range, fallback: Option<&str>| Definition {
        system: System::Algorithm(algorithm),
        negative: negative.map(|negative| (symbol(negative), Symbol::default())),
        prefix: None,
        suffix: Some(symbol(suffix)),
        range: Some(Range::Entries(vec![range])),
        fallback: fallback.map(symbol),
        pad: None,
    };
    // https://drafts.csswg.org/css-counter-styles-3/#limited-chinese
    // https://drafts.csswg.org/css-counter-styles-3/#limited-japanese
    // https://drafts.csswg.org/css-counter-styles-3/#limited-korean
    // The fallback is cjk-decimal, and each style has its own negative sign. The Chinese and
    // Japanese styles have the suffix "、" U+3001, the Korean ones ", ".
    // https://drafts.csswg.org/css-counter-styles-3/#extended-range-optional
    // The range is calc(-1 * pow(10, 16) + 1) calc(pow(10, 16) - 1), which an <integer> clamps to
    // its own range.
    use ExtendedCjkStyle::*;
    const SIMPLIFIED: &str = "\u{8D1F}";
    const TRADITIONAL: &str = "\u{8CA0}";
    const JAPANESE: &str = "\u{30DE}\u{30A4}\u{30CA}\u{30B9}";
    const KOREAN: &str = "\u{B9C8}\u{C774}\u{B108}\u{C2A4} ";
    const CJK: [(&str, ExtendedCjkStyle, &str); 10] = [
        ("simp-chinese-informal", SimpChineseInformal, SIMPLIFIED),
        ("simp-chinese-formal", SimpChineseFormal, SIMPLIFIED),
        ("trad-chinese-informal", TradChineseInformal, TRADITIONAL),
        ("trad-chinese-formal", TradChineseFormal, TRADITIONAL),
        // https://drafts.csswg.org/css-counter-styles-3/#cjk-ideographic
        ("cjk-ideographic", TradChineseInformal, TRADITIONAL),
        ("japanese-informal", JapaneseInformal, JAPANESE),
        ("japanese-formal", JapaneseFormal, JAPANESE),
        ("korean-hangul-formal", KoreanHangulFormal, KOREAN),
        ("korean-hanja-informal", KoreanHanjaInformal, KOREAN),
        ("korean-hanja-formal", KoreanHanjaFormal, KOREAN),
    ];
    let every_value = RangeEntry {
        start: i32::MIN,
        end: i32::MAX,
    };
    // https://drafts.csswg.org/css-counter-styles-3/#ethiopic-numeric-counter-style
    // For this system, the name is "ethiopic-numeric", the range is 1 infinite, the suffix is "/ "
    // (U+002F SOLIDUS followed by a U+0020 SPACE), and the rest of the descriptors have their
    // initial value.
    let ethiopic = definition(
        Algorithm::EthiopicNumeric,
        None,
        "/ ",
        RangeEntry {
            start: 1,
            end: i32::MAX,
        },
        None,
    );
    std::iter::once((symbol("ethiopic-numeric"), ethiopic)).chain(CJK.into_iter().map(
        move |(name, style, negative)| {
            let suffix = if negative == KOREAN { ", " } else { "\u{3001}" };
            let definition = definition(
                Algorithm::ExtendedCjk(style),
                Some(negative),
                suffix,
                every_value,
                Some("cjk-decimal"),
            );
            (symbol(name), definition)
        },
    ))
}

/// Whether following `extends` from `name` through a scope's own definitions comes back to a name
/// it passed: such a style extends `decimal` instead.
/// https://drafts.csswg.org/css-counter-styles-3/#extends-system
fn extends_into_a_cycle(name: &[u16], definitions: &HashMap<Symbol, Definition>) -> bool {
    let mut visited = Vec::new();
    let mut current = name;
    while let Some(Definition {
        system: System::Extends(extended),
        ..
    }) = definitions.get(current)
    {
        if visited.contains(&current) {
            return true;
        }
        visited.push(current);
        current = extended;
    }
    false
}

/// Registers the style `definitions` holds for `name`, after the one it extends.
fn register(
    name: &[u16],
    definitions: &HashMap<Symbol, Definition>,
    outer_scopes: &[&RegisteredCounterStyles],
    registered: &mut HashMap<Symbol, Arc<CounterStyle>>,
) -> Arc<CounterStyle> {
    if let Some(style) = registered.get(name) {
        return style.clone();
    }
    let definition = &definitions[name];
    let style = match &definition.system {
        System::Algorithm(_) => definition.resolve(name, decimal()),
        System::Extends(extended) => {
            let decimal_name = &*decimal().name;
            let extended = if extends_into_a_cycle(name, definitions) {
                decimal_name
            } else {
                extended
            };
            // A name the scope does not define is looked up in the outer scopes, and one no scope
            // defines is decimal.
            let base = [extended, decimal_name]
                .into_iter()
                .filter(|candidate| *candidate != name)
                .find_map(|candidate| {
                    if definitions.contains_key(candidate) {
                        return Some(register(candidate, definitions, outer_scopes, registered));
                    }
                    RegisteredCounterStyles::lookup(outer_scopes, candidate).cloned()
                });
            definition.resolve(name, base.as_deref().unwrap_or(decimal()))
        }
    };
    let style = Arc::new(style);
    registered.insert(name.into(), style.clone());
    style
}

/// One `@counter-style` rule in effect in a tree scope.
#[repr(C)]
pub struct FfiCounterStyleRule {
    pub name: FfiUtf16View,
    pub descriptors: *const FfiDescriptorBlock,
    /// The precedence of the rule's cascade origin: 0 user agent, 1 user, 2 author.
    pub origin: u8,
    /// The precedence of the rule's cascade layer within its origin.
    pub layer: u32,
}

/// Resolves the counter styles a tree scope registers from the `@counter-style` rules in effect in
/// it, in cascade order, and the complex predefined styles when `registers_predefined_styles`. A
/// name the scope does not register is looked up in `outer_scopes`, nearest first. Null stands for
/// a scope that registers none, and a scope that registers the styles `previous` holds gets a new
/// reference to it.
///
/// # Safety
///
/// `rules` and `outer_scopes` must address their stated number of live elements, `previous` must be
/// null or a live reference from this function, and `length_resolution_context` null or a live
/// `FfiLengthResolutionContext`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_counter_styles_resolve(
    rules: *const FfiCounterStyleRule,
    rule_count: usize,
    registers_predefined_styles: bool,
    outer_scopes: *const &RegisteredCounterStyles,
    outer_scope_count: usize,
    previous: *const RegisteredCounterStyles,
    length_resolution_context: *const c_void,
) -> *const RegisteredCounterStyles {
    // SAFETY: Guaranteed by the caller.
    let (rules, outer_scopes, length_context) = unsafe {
        (
            std::slice::from_raw_parts(rules, rule_count),
            std::slice::from_raw_parts(outer_scopes, outer_scope_count),
            length_resolution_context.cast::<FfiLengthResolutionContext>().as_ref(),
        )
    };
    // A rule wins over the ones before it unless their origin or layer takes precedence, and a
    // complex predefined style is a user agent one.
    let mut winners: HashMap<Symbol, ((u8, u32), Definition)> = HashMap::new();
    if registers_predefined_styles {
        winners.extend(complex_predefined_definitions().map(|(name, definition)| (name, ((0, 0), definition))));
    }
    for rule in rules {
        // SAFETY: The caller keeps the block alive for this synchronous call.
        let descriptors = unsafe { &*rule.descriptors }.data();
        let Some(definition) = Definition::from_descriptors(&descriptors, length_context) else {
            continue;
        };
        // SAFETY: The caller keeps the name alive for this synchronous call.
        let Some(name) = (unsafe { rule.name.to_utf16() }) else {
            continue;
        };
        let precedence = (rule.origin, rule.layer);
        if winners
            .get(name.as_slice())
            .is_some_and(|(winner, _)| *winner > precedence)
        {
            continue;
        }
        winners.insert(name.into_boxed_slice(), (precedence, definition));
    }
    let definitions: HashMap<Symbol, Definition> = winners
        .into_iter()
        .map(|(name, (_, definition))| (name, definition))
        .collect();
    let mut registered = HashMap::with_capacity(definitions.len());
    for name in definitions.keys() {
        register(name, &definitions, outer_scopes, &mut registered);
    }
    if registered.is_empty() {
        return std::ptr::null();
    }
    let registered = RegisteredCounterStyles(registered);
    // SAFETY: Guaranteed by the caller.
    if unsafe { previous.as_ref() }.is_some_and(|previous| *previous == registered) {
        return unsafe { crate::css::counter_representation::rust_counter_styles_retain(previous) };
    }
    Arc::into_raw(Arc::new(registered))
}

// https://drafts.csswg.org/css-counter-styles-3/#the-csscounterstylerule-interface
// What a CSSCounterStyleRule setter checks before it sets a descriptor.

/// Whether a rule whose `system` is `system`, null when it has none, still defines a counter style
/// once its `symbols` descriptor, or its `additive-symbols` one when `additive`, lists `count`
/// entries.
///
/// # Safety
///
/// `system` must be null or point to a live style value.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_counter_style_system_accepts_symbols(
    system: *const c_void,
    additive: bool,
    count: usize,
) -> bool {
    // SAFETY: Guaranteed by the caller.
    let system = unsafe { system.cast::<StyleValueData>().as_ref() };
    symbol_counts_needed(system)
        .is_some_and(|(symbols, additive_symbols)| count >= if additive { additive_symbols } else { symbols })
}

/// Whether setting a rule's `system` from `current` to `new` changes the algorithm it uses: the
/// system itself, the first symbol of a fixed one or the style an extending one extends.
///
/// # Safety
///
/// `current` and `new` must point to live style values.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_counter_style_system_changes_algorithm(
    current: *const c_void,
    new: *const c_void,
) -> bool {
    use StyleValueData::CounterStyleSystem;
    // SAFETY: Guaranteed by the caller.
    let (current, new) = unsafe { (&*current.cast::<StyleValueData>(), &*new.cast::<StyleValueData>()) };
    match (current, new) {
        (
            CounterStyleSystem { kind: 0, system, .. },
            CounterStyleSystem {
                kind: 0, system: new, ..
            },
        ) => system != new,
        (
            CounterStyleSystem {
                kind: 1, first_symbol, ..
            },
            CounterStyleSystem {
                kind: 1,
                first_symbol: new,
                ..
            },
        ) => {
            // An omitted first symbol is 1.
            let one = StyleValueData::Integer { value: 1 };
            first_symbol.optional_data().unwrap_or(&one) != new.optional_data().unwrap_or(&one)
        }
        (CounterStyleSystem { kind: 2, name, .. }, CounterStyleSystem { kind: 2, name: new, .. }) => name != new,
        _ => true,
    }
}
