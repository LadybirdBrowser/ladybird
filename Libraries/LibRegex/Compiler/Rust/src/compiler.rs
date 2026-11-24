/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Bytecode compiler: AST -> Program.
//!
//! Compiles the parsed regex AST into a concrete instruction stream for the
//! backtracking VM. The program and all the work data of the compile are in the
//! zone of the compile (see `ak::Zone`).
//!
//! Spec:
//! - <https://tc39.es/ecma262/#sec-pattern-semantics>
//! - <https://tc39.es/ecma262/#sec-compilepattern>
use core::cell::Cell;

use ak::Zone;
use ak::ZoneVec;
use libunicode_rust::character_types::ResolvedProperty;

use crate::ast::*;
use crate::parser::Error;
use crate::zone_program::*;

/// Resolve a Unicode property name/value pair to an ICU enum ID.
/// Returns None if the property could not be resolved (falls back to
/// string-based matching).
///
/// Spec source for the accepted names and values:
/// - <https://tc39.es/ecma262/#sec-runtime-semantics-unicodematchproperty-p>
/// - <https://tc39.es/ecma262/#sec-runtime-semantics-unicodematchpropertyvalue-p-v>
pub fn resolve_property(name: &str, value: Option<&str>) -> Option<ResolvedProperty> {
    libunicode_rust::character_types::resolve_property(name, value)
}

/// Compile a parsed pattern into a bytecode program, which is in `zone`.
///
/// Spec entry point: `CompilePattern`.
/// <https://tc39.es/ecma262/#sec-compilepattern>
pub fn compile<'z>(zone: &'z Zone, pattern: &Pattern<'z>) -> Result<ZoneProgram<'z>, Error<'z>> {
    let mut compiler = Compiler::new(zone, pattern);
    compiler.compile_pattern(pattern);
    if compiler.ran_out_of_stack.get() {
        return Err(Error::TooDeeplyNested);
    }
    Ok(ZoneProgram {
        instructions: compiler.instructions.into_slice(),
        capture_count: pattern.capture_count,
        register_count: compiler.register_count,
        unicode: compiler.unicode,
        unicode_sets: compiler.unicode_sets,
        ignore_case: pattern.flags.ignore_case,
        multiline: pattern.flags.multiline,
        dot_all: pattern.flags.dot_all,
        named_groups: pattern.named_groups,
    })
}

const fn char_range(start: u32, end: u32) -> CodePointRange {
    CodePointRange { start, end }
}

/// `\d`: <https://tc39.es/ecma262/#sec-compiletocharset>
const DIGIT_RANGES: &[CodePointRange] = &[char_range(0x30, 0x39)];
const NON_DIGIT_RANGES: &[CodePointRange] = &[char_range(0, 0x2F), char_range(0x3A, 0x10FFFF)];

/// `\w` without the u and v flags is the ASCII WordCharacters: <https://tc39.es/ecma262/#sec-wordcharacters>
const WORD_RANGES: &[CodePointRange] = &[
    char_range(0x30, 0x39),
    char_range(0x41, 0x5A),
    char_range(0x5F, 0x5F),
    char_range(0x61, 0x7A),
];
const NON_WORD_RANGES: &[CodePointRange] = &[
    char_range(0, 0x2F),
    char_range(0x3A, 0x40),
    char_range(0x5B, 0x5E),
    char_range(0x60, 0x60),
    char_range(0x7B, 0x10FFFF),
];

/// `\s` is WhiteSpace and LineTerminator: <https://tc39.es/ecma262/#sec-white-space>,
/// <https://tc39.es/ecma262/#sec-line-terminators>
const WHITESPACE_RANGES: &[CodePointRange] = &[
    char_range(0x09, 0x0D),
    char_range(0x20, 0x20),
    char_range(0xA0, 0xA0),
    char_range(0x1680, 0x1680),
    char_range(0x2000, 0x200A),
    char_range(0x2028, 0x2029),
    char_range(0x202F, 0x202F),
    char_range(0x205F, 0x205F),
    char_range(0x3000, 0x3000),
    char_range(0xFEFF, 0xFEFF),
];
const NON_WHITESPACE_RANGES: &[CodePointRange] = &[
    char_range(0, 0x08),
    char_range(0x0E, 0x1F),
    char_range(0x21, 0x9F),
    char_range(0xA1, 0x167F),
    char_range(0x1681, 0x1FFF),
    char_range(0x200B, 0x2027),
    char_range(0x202A, 0x202E),
    char_range(0x2030, 0x205E),
    char_range(0x2060, 0x2FFF),
    char_range(0x3001, 0xFEFE),
    char_range(0xFF00, 0x10FFFF),
];

/// Whether the character `cp` has a different lowercase and uppercase form, so that it matches more than itself under
/// case folding.
fn has_case(cp: u32) -> bool {
    let Some(ch) = char::from_u32(cp) else {
        return false;
    };
    ch.to_lowercase().next() != ch.to_uppercase().next()
}

/// Sorts `ranges` and merges the ones that overlap or touch, which the binary search of the VM relies on.
fn normalize_char_ranges(ranges: &mut ZoneVec<'_, CodePointRange>) {
    // NB: Ranges with the same start always merge, so their order does not matter.
    ranges.sort_unstable_by_key(|range| range.start);
    let mut merged_count = 0;
    for index in 0..ranges.len() {
        let range = ranges[index];
        if merged_count > 0 {
            let last = &mut ranges[merged_count - 1];
            if range.start <= last.end.saturating_add(1) {
                last.end = last.end.max(range.end);
                continue;
            }
        }
        ranges[merged_count] = range;
        merged_count += 1;
    }
    ranges.truncate(merged_count);
}

/// The multi-codepoint strings of a string property, from its packed data (see
/// `Compiler::string_property_data()`), in order.
fn packed_strings(data: &[u32]) -> impl Iterator<Item = &[u32]> {
    // Parse packed format: [count, len1, cp1..., len2, cp2..., ...]
    let mut remaining = data.first().map_or(0, |count| *count as usize);
    let mut offset = 1;
    core::iter::from_fn(move || {
        if remaining == 0 || offset >= data.len() {
            return None;
        }
        remaining -= 1;
        let len = data[offset] as usize;
        offset += 1;
        if offset + len > data.len() {
            remaining = 0;
            return None;
        }
        let string = &data[offset..offset + len];
        offset += len;
        Some(string)
    })
}

/// Adds `length` to `lengths`, which has each length one time.
fn add_length(lengths: &mut ZoneVec<'_, usize>, length: usize) {
    if !lengths.contains(&length) {
        lengths.push(length);
    }
}

struct Compiler<'z> {
    /// The zone that the program and the work data of the compile go in.
    zone: &'z Zone,
    /// The bytecode instructions.
    instructions: ZoneVec<'z, ZoneInstruction<'z>>,
    /// Total number of registers needed (2 per capture group + 2 for group 0, then the internal ones).
    register_count: u32,
    /// Whether Unicode mode is enabled (affects surrogate pair decoding).
    unicode: bool,
    /// Whether v-flag (unicode sets) mode is enabled.
    unicode_sets: bool,
    /// Effective flags (affected by modifier groups).
    effective_ignore_case: bool,
    effective_multiline: bool,
    effective_dot_all: bool,
    /// Next available register for internal use (repetition counters, progress checks).
    next_internal_reg: u32,
    /// True when compiling a lookbehind body (terms are reversed).
    backward: bool,
    /// Set when a pass stopped short of a nested group or class because the stack ran low, which fails the compile.
    ran_out_of_stack: Cell<bool>,
    /// Room for the capture registers of an atom, which each use clears.
    capture_registers: ZoneVec<'z, u32>,
    /// The packed strings of each string property that the compile has fetched, with its name.
    string_properties: ZoneVec<'z, (&'z str, &'z [u32])>,
}

impl<'z> Compiler<'z> {
    /// Whether a pass has to stop short of recursing into a nested group or class, which fails the compile.
    fn is_out_of_stack(&self) -> bool {
        if crate::stack::is_exhausted() {
            self.ran_out_of_stack.set(true);
        }
        self.ran_out_of_stack.get()
    }

    /// The code point ranges of a builtin class inside a character class of a pattern without the u and v flags, if
    /// the class can match it as plain ranges.
    ///
    /// Without ignore-case, every builtin is a fixed set of code points. With ignore-case, \d and \w are still exact in
    /// a positive class, since Canonicalize never maps a code point outside them onto one inside them in non-unicode
    /// mode. But matching a class with ranges then checks every code point that is not in them against the case
    /// closure of each range, which would make the large ranges of \s, of the negated builtins, and of negated classes
    /// slower than matching the builtins themselves.
    fn legacy_builtin_class_ranges(
        &self,
        class: BuiltinCharacterClass,
        negated: bool,
    ) -> Option<&'static [CodePointRange]> {
        if self.unicode || self.unicode_sets {
            return None;
        }
        if self.effective_ignore_case
            && (negated || !matches!(class, BuiltinCharacterClass::Digit | BuiltinCharacterClass::Word))
        {
            return None;
        }
        Some(match class {
            BuiltinCharacterClass::Digit => DIGIT_RANGES,
            BuiltinCharacterClass::NonDigit => NON_DIGIT_RANGES,
            BuiltinCharacterClass::Word => WORD_RANGES,
            BuiltinCharacterClass::NonWord => NON_WORD_RANGES,
            BuiltinCharacterClass::Whitespace => WHITESPACE_RANGES,
            BuiltinCharacterClass::NonWhitespace => NON_WHITESPACE_RANGES,
        })
    }

    fn new(zone: &'z Zone, pattern: &Pattern<'z>) -> Self {
        // Registers: 0-1 for group 0, then 2 per capture group.
        let register_count = 2 + pattern.capture_count * 2;
        Self {
            zone,
            instructions: ZoneVec::new_in(zone),
            register_count,
            unicode: pattern.flags.unicode || pattern.flags.unicode_sets,
            unicode_sets: pattern.flags.unicode_sets,
            effective_ignore_case: pattern.flags.ignore_case,
            effective_multiline: pattern.flags.multiline,
            effective_dot_all: pattern.flags.dot_all,
            next_internal_reg: register_count,
            backward: false,
            ran_out_of_stack: Cell::new(false),
            capture_registers: ZoneVec::new_in(zone),
            string_properties: ZoneVec::new_in(zone),
        }
    }

    /// Fetch the multi-codepoint strings of a Unicode string property via FFI, packed as
    /// [count, len1, cp1..., len2, cp2..., ...] (see `packed_strings()`).
    /// Returns nothing if the property is not a string property.
    ///
    /// These are the string-valued members that `/v` character classes can
    /// match atomically, as described by `CompileClassSetString`.
    /// <https://tc39.es/ecma262/#sec-compileclasssetstring>
    fn string_property_data(&mut self, name: &'z str) -> &'z [u32] {
        if let Some((_, data)) = self.string_properties.iter().find(|(known, _)| *known == name) {
            return data;
        }
        let needed = libunicode_rust::character_types::get_string_property_data(name, &mut []);
        let buffer = self.zone.alloc_slice_fill(needed, 0);
        let written = libunicode_rust::character_types::get_string_property_data(name, buffer);
        let data: &'z [u32] = if written == 0 || written > needed {
            &[]
        } else {
            &buffer[..written]
        };
        self.string_properties.push((name, data));
        data
    }

    /// Emit code for a Unicode string property match.
    /// Compiles as: (longest_multi_cp_string | ... | shortest_multi_cp_string | single_cp_property)
    /// This ensures longest match is tried first (v-flag semantics).
    /// <https://tc39.es/ecma262/#sec-compileclasssetstring>
    fn emit_string_property_match(&mut self, name: &'z str, value: Option<&'z str>) {
        let data = self.string_property_data(name);
        let mut lengths = ZoneVec::new_in(self.zone);
        for string in packed_strings(data) {
            add_length(&mut lengths, string.len());
        }
        if lengths.is_empty() {
            // Not a string property or no multi-cp strings; just emit regular property match.
            self.emit_unicode_property(false, name, value);
            return;
        }

        // Sort longest-first for greedy longest-match semantics. Strings of the same length keep their order.
        lengths.sort_unstable_by(|a, b| b.cmp(a));

        // Pack strings into a flat list: [len, cp0, cp1, ..., len, cp0, ...]
        let packed_len = packed_strings(data).map(|string| string.len() + 1).sum();
        let mut packed = ZoneVec::with_capacity_in(packed_len, self.zone);
        for length in &lengths {
            for string in packed_strings(data).filter(|string| string.len() == *length) {
                packed.push(string.len() as u32);
                if self.backward {
                    packed.extend(string.iter().rev());
                } else {
                    packed.extend_from_slice(string);
                }
            }
        }

        self.emit(ZoneInstruction::StringPropertyMatch {
            strings: packed.into_slice(),
            property: self.zone.alloc(UnicodeProperty {
                negated: false,
                name,
                value,
            }),
        });
    }

    fn emit_unicode_property(&mut self, negated: bool, name: &'z str, value: Option<&'z str>) {
        let property = self.zone.alloc(UnicodeProperty { negated, name, value });
        self.emit(ZoneInstruction::UnicodeProperty(property));
    }

    fn emit_char_string(&mut self, chars: &[char]) {
        if self.backward {
            for c in chars.iter().rev() {
                self.emit_char_maybe_case_fold(*c as u32);
            }
        } else {
            for c in chars {
                self.emit_char_maybe_case_fold(*c as u32);
            }
        }
    }

    fn emit_code_point_string(&mut self, string: &[u32]) {
        if self.backward {
            for cp in string.iter().rev() {
                self.emit_char_maybe_case_fold(*cp);
            }
        } else {
            for cp in string {
                self.emit_char_maybe_case_fold(*cp);
            }
        }
    }

    /// Whether `expr` matches strings of `length` code points.
    fn class_set_expression_has_length(&mut self, expr: &ClassSetExpression<'z>, length: usize) -> bool {
        if self.is_out_of_stack() {
            return false;
        }
        match expr {
            ClassSetExpression::Union(operands) => operands
                .iter()
                .any(|operand| self.class_set_operand_has_length(operand, length)),
            ClassSetExpression::Intersection(operands) => {
                !operands.is_empty()
                    && operands
                        .iter()
                        .all(|operand| self.class_set_operand_has_length(operand, length))
            }
            ClassSetExpression::Subtraction(operands) => operands
                .first()
                .is_some_and(|operand| self.class_set_operand_has_length(operand, length)),
        }
    }

    /// Whether `operand` matches strings of `length` code points.
    fn class_set_operand_has_length(&mut self, operand: &ClassSetOperand<'z>, length: usize) -> bool {
        match operand {
            ClassSetOperand::Char(_) | ClassSetOperand::Range(_, _) | ClassSetOperand::BuiltinClass(_) => length == 1,
            ClassSetOperand::NestedClass(cc) => {
                if cc.negated {
                    return length == 1;
                }
                match &cc.body {
                    CharacterClassBody::Ranges(_) => length == 1,
                    CharacterClassBody::UnicodeSet(expr) => self.class_set_expression_has_length(expr, length),
                }
            }
            ClassSetOperand::UnicodeProperty(up) => {
                if self.unicode_sets && !up.negated && libunicode_rust::character_types::is_string_property(up.name) {
                    return length == 1
                        || packed_strings(self.string_property_data(up.name)).any(|string| string.len() == length);
                }
                length == 1
            }
            ClassSetOperand::StringLiteral(chars) => chars.len() == length,
        }
    }

    /// Adds the lengths of the strings that `expr` matches to `lengths`, which has each length one time.
    fn collect_class_set_expression_lengths(
        &mut self,
        expr: &ClassSetExpression<'z>,
        lengths: &mut ZoneVec<'z, usize>,
    ) {
        if self.is_out_of_stack() {
            return;
        }
        match expr {
            ClassSetExpression::Union(operands) => {
                for operand in *operands {
                    self.collect_class_set_operand_lengths(operand, lengths);
                }
            }
            ClassSetExpression::Intersection(operands) => {
                let Some((first, rest)) = operands.split_first() else {
                    return;
                };
                // NB: A length that is in `lengths` already stays, as the union with the lengths of the intersection.
                let first_new = lengths.len();
                self.collect_class_set_operand_lengths(first, lengths);
                let mut index = first_new;
                while index < lengths.len() {
                    let length = lengths[index];
                    if rest
                        .iter()
                        .all(|operand| self.class_set_operand_has_length(operand, length))
                    {
                        index += 1;
                    } else {
                        lengths.swap_remove(index);
                    }
                }
            }
            ClassSetExpression::Subtraction(operands) => {
                if let Some(first) = operands.first() {
                    self.collect_class_set_operand_lengths(first, lengths);
                }
            }
        }
    }

    /// Adds the lengths of the strings that `operand` matches to `lengths`, which has each length one time.
    fn collect_class_set_operand_lengths(&mut self, operand: &ClassSetOperand<'z>, lengths: &mut ZoneVec<'z, usize>) {
        match operand {
            ClassSetOperand::Char(_) | ClassSetOperand::Range(_, _) | ClassSetOperand::BuiltinClass(_) => {
                add_length(lengths, 1);
            }
            ClassSetOperand::NestedClass(cc) => {
                if cc.negated {
                    add_length(lengths, 1);
                    return;
                }
                match &cc.body {
                    CharacterClassBody::Ranges(_) => add_length(lengths, 1),
                    CharacterClassBody::UnicodeSet(expr) => self.collect_class_set_expression_lengths(expr, lengths),
                }
            }
            ClassSetOperand::UnicodeProperty(up) => {
                add_length(lengths, 1);
                if self.unicode_sets && !up.negated && libunicode_rust::character_types::is_string_property(up.name) {
                    for string in packed_strings(self.string_property_data(up.name)) {
                        add_length(lengths, string.len());
                    }
                }
            }
            ClassSetOperand::StringLiteral(chars) => add_length(lengths, chars.len()),
        }
    }

    /// Emit a split/jump chain for N items, calling `compile_one` for each.
    /// This is the standard pattern for disjunctions in the bytecode:
    ///   Split prefer=alt_0, other=next_split
    ///   alt_0: ... Jump(end)
    ///   Split prefer=alt_1, other=next_split
    ///   alt_1: ... Jump(end)
    ///   ...
    ///   alt_n: ...
    ///   end:
    fn emit_split_chain<T>(&mut self, items: &[T], mut compile_one: impl FnMut(&mut Self, &T)) {
        if items.len() == 1 {
            compile_one(self, &items[0]);
            return;
        }

        // NB: Until the end is known, each jump to the end jumps to the one before it, and the first one to u32::MAX.
        let mut last_jump_to_end = u32::MAX;
        for (i, item) in items.iter().enumerate() {
            if i < items.len() - 1 {
                let split = self.emit(ZoneInstruction::Split {
                    prefer: self.current_offset() + 1,
                    other: u32::MAX,
                });
                compile_one(self, item);
                last_jump_to_end = self.emit(ZoneInstruction::Jump(last_jump_to_end));
                let next = self.current_offset();
                self.instructions[split as usize] = ZoneInstruction::Split {
                    prefer: split + 1,
                    other: next,
                };
            } else {
                compile_one(self, item);
            }
        }
        let end = self.current_offset();
        while last_jump_to_end != u32::MAX {
            let ZoneInstruction::Jump(previous_jump_to_end) = self.instructions[last_jump_to_end as usize] else {
                unreachable!("a jump to the end of a split chain is a jump");
            };
            self.instructions[last_jump_to_end as usize] = ZoneInstruction::Jump(end);
            last_jump_to_end = previous_jump_to_end;
        }
    }

    /// Emit a Char or CharNoCase instruction for a single code point,
    /// depending on whether case-insensitive mode is active.
    fn emit_char_maybe_case_fold(&mut self, cp: u32) {
        if self.effective_ignore_case && has_case(cp) {
            self.emit(ZoneInstruction::CharNoCase(cp, cp));
        } else {
            self.emit(ZoneInstruction::Char(cp));
        }
    }

    fn alloc_register(&mut self) -> u32 {
        let reg = self.next_internal_reg;
        self.next_internal_reg += 1;
        self.register_count = self.next_internal_reg;
        reg
    }

    fn emit(&mut self, inst: ZoneInstruction<'z>) -> u32 {
        let idx = self.current_offset();
        self.instructions.push(inst);
        idx
    }

    fn current_offset(&self) -> u32 {
        self.instructions.len() as u32
    }

    /// Collect the capture register indices (start and end) of all capture groups inside an atom (recursively).
    fn collect_capture_registers(&self, atom: &Atom<'z>, out: &mut ZoneVec<'z, u32>) {
        match atom {
            Atom::Group(g) => {
                out.push(g.index * 2);
                out.push(g.index * 2 + 1);
                self.collect_capture_registers_in_disjunction(&g.body, out);
            }
            Atom::NonCapturingGroup(g) => {
                self.collect_capture_registers_in_disjunction(&g.body, out);
            }
            Atom::ModifierGroup(mg) => {
                self.collect_capture_registers_in_disjunction(&mg.body, out);
            }
            Atom::Lookaround(la) => {
                self.collect_capture_registers_in_disjunction(&la.body, out);
            }
            _ => {}
        }
    }

    fn collect_capture_registers_in_disjunction(&self, disj: &Disjunction<'z>, out: &mut ZoneVec<'z, u32>) {
        if self.is_out_of_stack() {
            return;
        }
        for alt in disj.alternatives {
            for term in alt.terms {
                self.collect_capture_registers(&term.atom, out);
            }
        }
    }

    /// Emit `ClearRegister` instructions for all captures inside an atom.
    /// Per ECMA-262 RepeatMatcher step 2.d, captures must be reset to
    /// undefined at the start of each quantifier iteration.
    /// <https://tc39.es/ecma262/#sec-runtime-semantics-repeatmatcher-abstract-operation>
    fn emit_clear_captures(&mut self, atom: &Atom<'z>) {
        let mut registers = self.capture_registers.take();
        registers.clear();
        if !matches!(atom, Atom::Lookaround(_) | Atom::Assertion(_)) {
            self.collect_capture_registers(atom, &mut registers);
        }
        for reg in &registers {
            self.emit(ZoneInstruction::ClearRegister(*reg));
        }
        self.capture_registers = registers;
    }

    /// Collect all capture register indices (start and end) for captures inside an atom.
    /// Returns empty for intrinsically zero-width atoms (lookarounds, assertions)
    /// since their captures should NOT be cleared on zero-width ProgressCheck.
    fn capture_registers(&self, atom: &Atom<'z>) -> &'z [u32] {
        if matches!(atom, Atom::Lookaround(_) | Atom::Assertion(_)) {
            return &[];
        }
        let mut regs = ZoneVec::new_in(self.zone);
        self.collect_capture_registers(atom, &mut regs);
        regs.into_slice()
    }

    fn disjunction_can_be_zero_width(&self, disjunction: &Disjunction<'z>) -> bool {
        if self.is_out_of_stack() {
            return true;
        }
        disjunction
            .alternatives
            .iter()
            .any(|alternative| self.alternative_can_be_zero_width(alternative))
    }

    fn alternative_can_be_zero_width(&self, alternative: &Alternative<'z>) -> bool {
        alternative.terms.iter().all(|term| self.term_can_be_zero_width(term))
    }

    fn term_can_be_zero_width(&self, term: &Term<'z>) -> bool {
        match &term.quantifier {
            Some(quantifier) if quantifier.min == 0 => true,
            _ => self.atom_can_be_zero_width(&term.atom),
        }
    }

    /// Returns true if the given atom could potentially match zero characters.
    fn atom_can_be_zero_width(&self, atom: &Atom<'z>) -> bool {
        match atom {
            Atom::Literal(_)
            | Atom::Dot
            | Atom::BuiltinCharacterClass(_)
            | Atom::CharacterClass(_)
            | Atom::UnicodeProperty { .. } => false,
            Atom::Group(group) => self.disjunction_can_be_zero_width(&group.body),
            Atom::NonCapturingGroup(group) => self.disjunction_can_be_zero_width(&group.body),
            Atom::ModifierGroup(group) => self.disjunction_can_be_zero_width(&group.body),
            Atom::Backreference(_) | Atom::Lookaround(_) | Atom::Assertion(_) => true,
        }
    }

    /// Lower `Pattern` to the concrete program entry sequence.
    /// <https://tc39.es/ecma262/#sec-compilepattern>
    fn compile_pattern(&mut self, pattern: &Pattern<'z>) {
        // Save start of match (group 0).
        self.emit(ZoneInstruction::Save(0));
        self.compile_disjunction(&pattern.disjunction);
        // Save end of match (group 0).
        self.emit(ZoneInstruction::Save(1));
        self.emit(ZoneInstruction::Match);
    }

    /// Lower `Disjunction` to a chain of split/jump choice points.
    /// <https://tc39.es/ecma262/#sec-compilesubpattern>
    fn compile_disjunction(&mut self, disj: &Disjunction<'z>) {
        if self.is_out_of_stack() {
            return;
        }
        if let Some(term) = self.try_merge_optional_alternatives(disj) {
            self.compile_term(&term);
            return;
        }
        self.emit_split_chain(disj.alternatives, |s, alt| s.compile_alternative(alt));
    }

    /// Lower `Alternative` in the current direction.
    /// <https://tc39.es/ecma262/#sec-compilesubpattern>
    fn compile_alternative(&mut self, alt: &Alternative<'z>) {
        if self.backward {
            for term in alt.terms.iter().rev() {
                self.compile_term(term);
            }
            return;
        }
        let mut terms = alt.terms;
        while let Some(term) = terms.first() {
            let run = terms
                .iter()
                .take_while(|term| self.literal_code_unit(term).is_some())
                .count();
            if run >= 2 {
                self.emit_literal(&terms[..run]);
                terms = &terms[run..];
            } else {
                self.compile_term(term);
                terms = &terms[1..];
            }
        }
    }

    /// The code unit that `term` matches, if it is a single literal character that the VM can match as one code unit
    /// of a `Literal` instruction: a character of the BMP, and not a surrogate in Unicode mode, where a surrogate of
    /// the pattern must not match half of a surrogate pair of the input.
    fn literal_code_unit(&self, term: &Term<'z>) -> Option<u16> {
        if term.quantifier.is_some() {
            return None;
        }
        let Atom::Literal(code_point) = term.atom else {
            return None;
        };
        let code_unit = u16::try_from(code_point).ok()?;
        if self.unicode && (0xD800..=0xDFFF).contains(&code_unit) {
            return None;
        }
        Some(code_unit)
    }

    /// Emit one `Literal` instruction for a run of forward literal terms, which would otherwise be a `Char` or
    /// `CharNoCase` instruction each.
    fn emit_literal(&mut self, terms: &[Term<'z>]) {
        let mut code_units = ZoneVec::with_capacity_in(terms.len(), self.zone);
        code_units.extend(terms.iter().filter_map(|term| self.literal_code_unit(term)));
        // NB: A literal of characters without case compares as plain code units, as their `Char` instructions would,
        //     which keeps it visible to the analyses that only look at case-sensitive literals.
        let ignore_case = self.effective_ignore_case && code_units.iter().any(|&code_unit| has_case(code_unit.into()));
        self.emit(ZoneInstruction::Literal {
            code_units: code_units.into_slice(),
            ignore_case,
        });
    }

    /// Lower `Term`, delegating to either `CompileAtom` or the quantified path.
    /// <https://tc39.es/ecma262/#sec-compilesubpattern>
    fn compile_term(&mut self, term: &Term<'z>) {
        match &term.quantifier {
            None => self.compile_atom(&term.atom),
            Some(q) => self.compile_quantified(&term.atom, q),
        }
    }

    /// Try to extract a ZoneSimpleMatch for an atom (for optimized greedy/lazy loops).
    fn try_simple_match(&self, atom: &Atom<'z>) -> Option<ZoneSimpleMatch<'z>> {
        match atom {
            Atom::Dot => Some(ZoneSimpleMatch::AnyChar {
                dot_all: self.effective_dot_all,
            }),
            Atom::Literal(c) => {
                // Don't use simple match for supplementary chars in non-unicode mode
                // (they need surrogate pair splitting).
                if *c > 0xFFFF && !self.unicode && !self.unicode_sets {
                    return None;
                }
                if self.effective_ignore_case {
                    let lo = *c;
                    let hi = *c; // VM will use case_fold_eq
                    Some(ZoneSimpleMatch::CharNoCase(lo, hi))
                } else {
                    Some(ZoneSimpleMatch::Char(*c))
                }
            }
            Atom::BuiltinCharacterClass(class) => Some(ZoneSimpleMatch::BuiltinClass(*class)),
            Atom::CharacterClass(cc) => match &cc.body {
                CharacterClassBody::Ranges(ranges_vec) => self.try_simple_match_for_ranges(ranges_vec, cc.negated),
                CharacterClassBody::UnicodeSet(expr) => self.try_simple_match_for_unicode_set(expr, cc.negated),
            },
            Atom::UnicodeProperty(up) => {
                // String properties (e.g. Basic_Emoji) can match multi-character
                // sequences and cannot use simple matching.
                if self.unicode_sets && libunicode_rust::character_types::is_string_property(up.name) {
                    return None;
                }
                Some(ZoneSimpleMatch::UnicodeProperty(self.zone.alloc(*up)))
            }
            _ => None,
        }
    }

    /// Collapse simple disjunctions like `a|a?` into a single greedy optional
    /// term. This preserves semantics for single-term simple-match
    /// alternatives while avoiding exponential backtracking in quantified
    /// contexts such as `^(a|a?)+$`.
    fn try_merge_optional_alternatives(&self, disj: &Disjunction<'z>) -> Option<Term<'z>> {
        let [first, second] = disj.alternatives else {
            return None;
        };
        let [first_term] = first.terms else {
            return None;
        };
        let [second_term] = second.terms else {
            return None;
        };

        let optional = Quantifier::zero_or_one(true);
        match (&first_term.quantifier, &second_term.quantifier) {
            (None, Some(q)) | (Some(q), None) if *q == optional => {}
            _ => return None,
        }

        let first_match = self.try_simple_match(&first_term.atom)?;
        let second_match = self.try_simple_match(&second_term.atom)?;
        if first_match != second_match {
            return None;
        }

        Some(Term {
            atom: first_term.atom,
            quantifier: Some(optional),
        })
    }

    /// Lower a quantified atom using the same observable behavior as
    /// `CompileQuantifier` plus `RepeatMatcher`.
    /// - <https://tc39.es/ecma262/#sec-compilequantifier>
    /// - <https://tc39.es/ecma262/#sec-runtime-semantics-repeatmatcher-abstract-operation>
    fn compile_quantified(&mut self, atom: &Atom<'z>, q: &Quantifier) {
        // Try to use optimized GreedyLoop/LazyLoop for simple character matchers.
        if let Some(matcher) = self.try_simple_match(atom) {
            match q.max {
                Some(max) if max == q.min && q.min <= 100_000 => {
                    for _ in 0..q.min {
                        self.compile_atom(atom);
                    }
                    return;
                }
                _ => {
                    if q.greedy {
                        self.emit(ZoneInstruction::GreedyLoop {
                            matcher,
                            min: q.min,
                            max: q.max,
                        });
                    } else {
                        self.emit(ZoneInstruction::LazyLoop {
                            matcher,
                            min: q.min,
                            max: q.max,
                        });
                    }
                    return;
                }
            }
        }

        // For large repetition counts, use a counted loop (RepeatStart/RepeatCheck)
        // instead of unrolling, to avoid generating millions of instructions.
        let max_for_limit = q.max.unwrap_or(q.min);
        if max_for_limit > 100_000 {
            let counter_reg = self.alloc_register();
            let progress_reg = if self.atom_can_be_zero_width(atom) {
                Some(self.alloc_register())
            } else {
                None
            };

            self.emit(ZoneInstruction::RepeatStart { counter_reg });
            let body_start = self.current_offset();
            if let Some(reg) = progress_reg {
                self.emit(ZoneInstruction::Save(reg));
            }
            self.emit_clear_captures(atom);
            self.compile_atom(atom);
            if let Some(reg) = progress_reg {
                self.emit(ZoneInstruction::ProgressCheck {
                    reg,
                    clear_captures: self.capture_registers(atom),
                });
            }
            self.emit(ZoneInstruction::RepeatCheck {
                counter_reg,
                min: q.min,
                max: q.max,
                body: body_start,
                greedy: q.greedy,
            });
            return;
        }

        // Emit `min` required copies.
        // Per ECMA-262, captures inside must be cleared at the start of each iteration.
        let can_be_zero_width = self.atom_can_be_zero_width(atom);
        for i in 0..q.min {
            if i > 0 {
                self.emit_clear_captures(atom);
            }
            self.compile_atom(atom);
        }

        match q.max {
            Some(max) if max == q.min => {
                // Exact count {n} — already emitted above.
            }
            Some(max) => {
                // Bounded: {min, max} — emit (max - min) optional copies.
                // Per ECMA-262 22.2.2.5.1 RepeatMatcher step 2.b: if the body matches
                // zero-width and min is 0, the quantifier should act as if it matched 0
                // times. The check must happen AFTER the body so it can explore non-empty
                // alternatives through backtracking before being rejected.
                let optional_count = max - q.min;
                if !can_be_zero_width {
                    self.compile_counted_optional_repetitions(atom, optional_count, q.greedy);
                    return;
                }
                for _ in 0..optional_count {
                    let progress_reg = Some(self.alloc_register());
                    if q.greedy {
                        // Split: prefer body, other skip.
                        let split = self.emit(ZoneInstruction::Split {
                            prefer: self.current_offset() + 1,
                            other: u32::MAX,
                        });
                        if let Some(reg) = progress_reg {
                            self.emit(ZoneInstruction::Save(reg));
                        }
                        self.emit_clear_captures(atom);
                        self.compile_atom(atom);
                        if let Some(reg) = progress_reg {
                            self.emit(ZoneInstruction::ProgressCheck {
                                reg,
                                clear_captures: self.capture_registers(atom),
                            });
                        }
                        let after = self.current_offset();
                        self.instructions[split as usize] = ZoneInstruction::Split {
                            prefer: split + 1,
                            other: after,
                        };
                    } else {
                        // Lazy: prefer skip, other body.
                        let split = self.emit(ZoneInstruction::Split {
                            prefer: u32::MAX,
                            other: self.current_offset() + 1,
                        });
                        if let Some(reg) = progress_reg {
                            self.emit(ZoneInstruction::Save(reg));
                        }
                        self.emit_clear_captures(atom);
                        self.compile_atom(atom);
                        if let Some(reg) = progress_reg {
                            self.emit(ZoneInstruction::ProgressCheck {
                                reg,
                                clear_captures: self.capture_registers(atom),
                            });
                        }
                        let after = self.current_offset();
                        self.instructions[split as usize] = ZoneInstruction::Split {
                            prefer: after,
                            other: split + 1,
                        };
                    }
                }
            }
            None => {
                // Unbounded: {min,} — emit a loop.
                // We need a progress check to prevent infinite loops on zero-width matches.
                // Per ECMA-262 22.2.2.5.1 RepeatMatcher step 2.b: the check happens AFTER
                // the body matches, so the body gets a chance to explore non-empty alternatives
                // before being rejected for zero-width.
                let can_be_zero_width = self.atom_can_be_zero_width(atom);
                let progress_reg = self.alloc_register();
                let loop_start = self.current_offset();

                if q.greedy {
                    let split = self.emit(ZoneInstruction::Split {
                        prefer: self.current_offset() + 1,
                        other: u32::MAX,
                    });
                    if can_be_zero_width {
                        self.emit(ZoneInstruction::Save(progress_reg));
                    }
                    self.emit_clear_captures(atom);
                    self.compile_atom(atom);
                    if can_be_zero_width {
                        self.emit(ZoneInstruction::ProgressCheck {
                            reg: progress_reg,
                            clear_captures: self.capture_registers(atom),
                        });
                    }
                    self.emit(ZoneInstruction::Jump(loop_start));
                    let after = self.current_offset();
                    self.instructions[split as usize] = ZoneInstruction::Split {
                        prefer: split + 1,
                        other: after,
                    };
                } else {
                    let split = self.emit(ZoneInstruction::Split {
                        prefer: u32::MAX,
                        other: self.current_offset() + 1,
                    });
                    if can_be_zero_width {
                        self.emit(ZoneInstruction::Save(progress_reg));
                    }
                    self.emit_clear_captures(atom);
                    self.compile_atom(atom);
                    if can_be_zero_width {
                        self.emit(ZoneInstruction::ProgressCheck {
                            reg: progress_reg,
                            clear_captures: self.capture_registers(atom),
                        });
                    }
                    self.emit(ZoneInstruction::Jump(loop_start));
                    let after = self.current_offset();
                    self.instructions[split as usize] = ZoneInstruction::Split {
                        prefer: after,
                        other: split + 1,
                    };
                }
            }
        }
    }

    fn compile_counted_optional_repetitions(&mut self, atom: &Atom<'z>, optional_count: u32, greedy: bool) {
        let counter_reg = self.alloc_register();
        self.emit(ZoneInstruction::RepeatStart { counter_reg });

        // min is always 0 here: this function only emits the optional tail of a {min,max}
        // quantifier; the required `min` repetitions have already been emitted by the caller.
        let check = self.emit(ZoneInstruction::RepeatCheck {
            counter_reg,
            min: 0,
            max: Some(optional_count),
            body: u32::MAX,
            greedy,
        });

        let skip_body = self.emit(ZoneInstruction::Jump(u32::MAX));
        let body_start = self.current_offset();
        self.emit_clear_captures(atom);
        self.compile_atom(atom);
        self.emit(ZoneInstruction::Jump(check));

        self.instructions[check as usize] = ZoneInstruction::RepeatCheck {
            counter_reg,
            min: 0,
            max: Some(optional_count),
            body: body_start,
            greedy,
        };
        self.instructions[skip_body as usize] = ZoneInstruction::Jump(self.current_offset());
    }

    /// Lower an `Atom` or assertion-like atom to bytecode.
    /// - <https://tc39.es/ecma262/#sec-compileatom>
    /// - <https://tc39.es/ecma262/#sec-compileassertion>
    fn compile_atom(&mut self, atom: &Atom<'z>) {
        match atom {
            Atom::Literal(c) => {
                // In non-unicode mode, supplementary characters (>0xFFFF) must be
                // split into their UTF-16 surrogate pair halves.
                if *c > 0xFFFF && !self.unicode && !self.unicode_sets {
                    let hi = ((*c - 0x10000) >> 10) + 0xD800;
                    let lo = ((*c - 0x10000) & 0x3FF) + 0xDC00;
                    self.emit(ZoneInstruction::Char(hi));
                    self.emit(ZoneInstruction::Char(lo));
                } else {
                    self.emit_char_maybe_case_fold(*c);
                }
            }

            Atom::Dot => {
                self.emit(ZoneInstruction::AnyChar {
                    dot_all: self.effective_dot_all,
                });
            }

            Atom::CharacterClass(cc) => {
                self.compile_character_class(cc);
            }

            Atom::BuiltinCharacterClass(bc) => {
                self.emit(ZoneInstruction::BuiltinClass(*bc));
            }

            Atom::UnicodeProperty(up) => {
                if self.unicode_sets && !up.negated && libunicode_rust::character_types::is_string_property(up.name) {
                    self.emit_string_property_match(up.name, up.value);
                    return;
                }
                self.emit_unicode_property(up.negated, up.name, up.value);
            }

            Atom::Group(g) => {
                let save_start = g.index * 2;
                let save_end = g.index * 2 + 1;
                // Clear captures inside this group before entering
                // (ECMA-262 requires captures to reset on each iteration).
                self.emit(ZoneInstruction::Save(save_start));
                self.compile_disjunction(&g.body);
                self.emit(ZoneInstruction::Save(save_end));
            }

            Atom::NonCapturingGroup(g) => {
                self.compile_disjunction(&g.body);
            }

            Atom::Lookaround(la) => {
                let (positive, forward) = match la.kind {
                    LookaroundKind::LookaheadPositive => (true, true),
                    LookaroundKind::LookaheadNegative => (false, true),
                    LookaroundKind::LookbehindPositive => (true, false),
                    LookaroundKind::LookbehindNegative => (false, false),
                };
                let look_start = self.emit(ZoneInstruction::LookStart {
                    positive,
                    forward,
                    end: u32::MAX,
                });
                let saved_backward = self.backward;
                // Compile lookbehind bodies in reverse so the VM can keep using
                // the same primitive character-matching instructions in both
                // directions.
                self.backward = !forward;
                self.compile_disjunction(&la.body);
                self.backward = saved_backward;
                self.emit(ZoneInstruction::LookEnd);
                let end = self.current_offset();
                // Patch the end offset.
                self.instructions[look_start as usize] = ZoneInstruction::LookStart { positive, forward, end };
            }

            Atom::Backreference(br) => match br {
                Backreference::Index(n) => {
                    self.emit(ZoneInstruction::Backref(*n));
                }
                Backreference::Named(name) => {
                    self.emit(ZoneInstruction::BackrefNamed(name));
                }
            },

            Atom::Assertion(kind) => match kind {
                AssertionKind::StartOfInput => {
                    self.emit(ZoneInstruction::AssertStart {
                        multiline: self.effective_multiline,
                    });
                }
                AssertionKind::EndOfInput => {
                    self.emit(ZoneInstruction::AssertEnd {
                        multiline: self.effective_multiline,
                    });
                }
                AssertionKind::WordBoundary => {
                    self.emit(ZoneInstruction::AssertWordBoundary);
                }
                AssertionKind::NonWordBoundary => {
                    self.emit(ZoneInstruction::AssertNonWordBoundary);
                }
            },

            Atom::ModifierGroup(mg) => {
                let ignore_case = if mg.add_flags.ignore_case {
                    Some(true)
                } else if mg.remove_flags.ignore_case {
                    Some(false)
                } else {
                    None
                };
                let multiline = if mg.add_flags.multiline {
                    Some(true)
                } else if mg.remove_flags.multiline {
                    Some(false)
                } else {
                    None
                };
                let dot_all = if mg.add_flags.dot_all {
                    Some(true)
                } else if mg.remove_flags.dot_all {
                    Some(false)
                } else {
                    None
                };
                self.emit(ZoneInstruction::PushModifiers {
                    ignore_case,
                    multiline,
                    dot_all,
                });
                // Track effective flags for compile-time decisions.
                let saved_ignore_case = self.effective_ignore_case;
                let saved_multiline = self.effective_multiline;
                let saved_dot_all = self.effective_dot_all;
                if let Some(v) = ignore_case {
                    self.effective_ignore_case = v;
                }
                if let Some(v) = multiline {
                    self.effective_multiline = v;
                }
                if let Some(v) = dot_all {
                    self.effective_dot_all = v;
                }
                self.compile_disjunction(&mg.body);
                self.effective_ignore_case = saved_ignore_case;
                self.effective_multiline = saved_multiline;
                self.effective_dot_all = saved_dot_all;
                self.emit(ZoneInstruction::PopModifiers);
            }
        }
    }

    /// Lower `CharacterClass`.
    /// <https://tc39.es/ecma262/#sec-compilecharacterclass>
    fn compile_character_class(&mut self, cc: &CharacterClass<'z>) {
        match &cc.body {
            CharacterClassBody::Ranges(ranges) => {
                let mut char_ranges = ZoneVec::new_in(self.zone);
                let mut has_builtin = false;

                let split_surrogates = !self.unicode && !self.unicode_sets;
                for r in *ranges {
                    match r {
                        CharacterClassRange::Single(cp) => {
                            if split_surrogates && *cp > 0xFFFF {
                                let hi = ((*cp - 0x10000) >> 10) + 0xD800;
                                let lo = ((*cp - 0x10000) & 0x3FF) + 0xDC00;
                                char_ranges.push(CodePointRange { start: hi, end: hi });
                                char_ranges.push(CodePointRange { start: lo, end: lo });
                            } else {
                                char_ranges.push(CodePointRange { start: *cp, end: *cp });
                            }
                        }
                        CharacterClassRange::Range(lo, hi) => {
                            char_ranges.push(CodePointRange { start: *lo, end: *hi });
                        }
                        CharacterClassRange::BuiltinClass(class)
                            if let Some(builtin_ranges) = self.legacy_builtin_class_ranges(*class, cc.negated) =>
                        {
                            char_ranges.extend_from_slice(builtin_ranges);
                        }
                        CharacterClassRange::BuiltinClass(_) | CharacterClassRange::UnicodeProperty(_) => {
                            has_builtin = true;
                        }
                    }
                }

                if has_builtin {
                    // Complex class with builtins: emit as disjunction of
                    // individual matchers.
                    self.compile_complex_class(ranges, cc.negated);
                } else {
                    // Sort and merge ranges for binary search in the VM.
                    normalize_char_ranges(&mut char_ranges);
                    self.emit(ZoneInstruction::CharClass {
                        ranges: char_ranges.into_slice(),
                        negated: cc.negated,
                    });
                }
            }
            CharacterClassBody::UnicodeSet(expr) => {
                self.compile_unicode_set_class(expr, cc.negated);
            }
        }
    }

    fn compile_complex_class(&mut self, ranges: &[CharacterClassRange<'z>], negated: bool) {
        // For classes containing builtin escapes like [\d\w], we emit
        // a split-based disjunction of each component.
        // For negated classes with a single builtin, emit the negated builtin directly.
        if negated {
            // Optimization: [^\s] → \S, [^\d] → \D, [^\w] → \W
            if ranges.len() == 1
                && let CharacterClassRange::BuiltinClass(bc) = &ranges[0]
            {
                self.emit(ZoneInstruction::BuiltinClass(bc.negated()));
                return;
            }
            // General case: [^\d\w] = (?!\d|\w).
            let forward = !self.backward;
            let look_start = self.emit(ZoneInstruction::LookStart {
                positive: false,
                forward,
                end: u32::MAX,
            });
            self.compile_class_components_as_disjunction(ranges);
            self.emit(ZoneInstruction::LookEnd);
            let end = self.current_offset();
            self.instructions[look_start as usize] = ZoneInstruction::LookStart {
                positive: false,
                forward,
                end,
            };
            self.emit(ZoneInstruction::AnyChar { dot_all: true });
        } else {
            self.compile_class_components_as_disjunction(ranges);
        }
    }

    fn compile_class_components_as_disjunction(&mut self, ranges: &[CharacterClassRange<'z>]) {
        self.emit_split_chain(ranges, |s, r| s.compile_class_component(r));
    }

    fn compile_class_component(&mut self, range: &CharacterClassRange<'z>) {
        match range {
            CharacterClassRange::Single(cp) => {
                self.emit_char_maybe_case_fold(*cp);
            }
            CharacterClassRange::Range(lo, hi) => {
                self.emit(ZoneInstruction::CharClass {
                    ranges: self.zone.alloc_slice_copy(&[CodePointRange { start: *lo, end: *hi }]),
                    negated: false,
                });
            }
            CharacterClassRange::BuiltinClass(bc) => {
                self.emit(ZoneInstruction::BuiltinClass(*bc));
            }
            CharacterClassRange::UnicodeProperty(up) => {
                self.emit_unicode_property(up.negated, up.name, up.value);
            }
        }
    }

    /// Try to extract a `ZoneSimpleMatch` for a single `ClassSetOperand` in the context of
    /// a `LazyLoop`/`GreedyLoop`. Returns `None` for operands that require multi-character
    /// matching (string literals) or set operations that can't be represented as a simple test.
    fn try_simple_match_for_operand(&self, operand: &ClassSetOperand<'z>) -> Option<ZoneSimpleMatch<'z>> {
        if crate::stack::is_exhausted() {
            return None;
        }
        match operand {
            ClassSetOperand::Char(c) => Some(ZoneSimpleMatch::Char(*c)),
            ClassSetOperand::Range(lo, hi) => Some(ZoneSimpleMatch::CharClass {
                ranges: self.zone.alloc_slice_copy(&[CodePointRange { start: *lo, end: *hi }]),
                negated: false,
            }),
            ClassSetOperand::BuiltinClass(bc) => Some(ZoneSimpleMatch::BuiltinClass(*bc)),
            ClassSetOperand::UnicodeProperty(up) => {
                if libunicode_rust::character_types::is_string_property(up.name) {
                    return None;
                }
                Some(ZoneSimpleMatch::UnicodeProperty(self.zone.alloc(*up)))
            }
            ClassSetOperand::NestedClass(cc) => match &cc.body {
                CharacterClassBody::Ranges(ranges_vec) => {
                    let mut ranges = ZoneVec::new_in(self.zone);
                    for r in *ranges_vec {
                        match r {
                            CharacterClassRange::Single(cp) => {
                                ranges.push(CodePointRange { start: *cp, end: *cp });
                            }
                            CharacterClassRange::Range(lo, hi) => {
                                ranges.push(CodePointRange { start: *lo, end: *hi });
                            }
                            _ => return None,
                        }
                    }
                    normalize_char_ranges(&mut ranges);
                    Some(ZoneSimpleMatch::CharClass {
                        ranges: ranges.into_slice(),
                        negated: cc.negated,
                    })
                }
                CharacterClassBody::UnicodeSet(inner_expr) => {
                    let ClassSetExpression::Union(inner_operands) = inner_expr else {
                        return None;
                    };
                    if cc.negated {
                        // NB: Only a single matcher can be negated.
                        let [inner_operand] = inner_operands else {
                            return None;
                        };
                        self.negate_simple_match(self.try_simple_match_for_operand(inner_operand)?)
                    } else {
                        self.try_simple_match_for_union(inner_operands)
                    }
                }
            },
            ClassSetOperand::StringLiteral(_) => None,
        }
    }

    /// The union of the simple matchers of `operands`, folded from the left, if each operand has one.
    fn try_simple_match_for_union(&self, operands: &[ClassSetOperand<'z>]) -> Option<ZoneSimpleMatch<'z>> {
        let mut union = None;
        for operand in operands {
            let matcher = self.try_simple_match_for_operand(operand)?;
            union = Some(self.add_to_union(union, matcher));
        }
        union
    }

    /// `matcher`, or the union of `union` and `matcher` if there is a union so far.
    fn add_to_union(&self, union: Option<ZoneSimpleMatch<'z>>, matcher: ZoneSimpleMatch<'z>) -> ZoneSimpleMatch<'z> {
        match union {
            Some(union) => ZoneSimpleMatch::Union(self.zone.alloc(union), self.zone.alloc(matcher)),
            None => matcher,
        }
    }

    fn negate_simple_match(&self, matcher: ZoneSimpleMatch<'z>) -> Option<ZoneSimpleMatch<'z>> {
        match matcher {
            ZoneSimpleMatch::CharClass { ranges, negated } => Some(ZoneSimpleMatch::CharClass {
                ranges,
                negated: !negated,
            }),
            ZoneSimpleMatch::UnicodeProperty(data) => {
                Some(ZoneSimpleMatch::UnicodeProperty(self.zone.alloc(UnicodeProperty {
                    negated: !data.negated,
                    ..*data
                })))
            }
            _ => None,
        }
    }

    /// Try to build a `ZoneSimpleMatch` for a `CharacterClassBody::Ranges`.
    ///
    /// If all elements are plain chars/ranges, returns a single `CharClass`.
    /// If the class is non-negated and contains builtins or Unicode properties,
    /// collects plain ranges into one `CharClass` and each builtin/property into its
    /// own `ZoneSimpleMatch`, then folds them into a `Union`.
    fn try_simple_match_for_ranges(
        &self,
        ranges_vec: &[CharacterClassRange<'z>],
        negated: bool,
    ) -> Option<ZoneSimpleMatch<'z>> {
        let has_complex = ranges_vec.iter().any(|r| match r {
            CharacterClassRange::BuiltinClass(class) => self.legacy_builtin_class_ranges(*class, negated).is_none(),
            CharacterClassRange::UnicodeProperty(_) => true,
            CharacterClassRange::Single(_) | CharacterClassRange::Range(..) => false,
        });

        if !has_complex {
            let mut ranges = ZoneVec::new_in(self.zone);
            for r in ranges_vec {
                match r {
                    CharacterClassRange::Single(c) => {
                        if *c > 0xFFFF && !self.unicode && !self.unicode_sets {
                            return None;
                        }
                        ranges.push(CodePointRange { start: *c, end: *c });
                    }
                    CharacterClassRange::Range(lo, hi) => {
                        ranges.push(CodePointRange { start: *lo, end: *hi });
                    }
                    CharacterClassRange::BuiltinClass(class) => {
                        ranges.extend_from_slice(self.legacy_builtin_class_ranges(*class, negated)?);
                    }
                    CharacterClassRange::UnicodeProperty(_) => return None,
                }
            }
            normalize_char_ranges(&mut ranges);
            return Some(ZoneSimpleMatch::CharClass {
                ranges: ranges.into_slice(),
                negated,
            });
        }

        if negated {
            return None;
        }

        let mut plain_ranges = ZoneVec::new_in(self.zone);
        let mut union = None;

        for r in ranges_vec {
            match r {
                CharacterClassRange::Single(c) => {
                    if *c > 0xFFFF && !self.unicode && !self.unicode_sets {
                        return None;
                    }
                    plain_ranges.push(CodePointRange { start: *c, end: *c });
                }
                CharacterClassRange::Range(lo, hi) => {
                    plain_ranges.push(CodePointRange { start: *lo, end: *hi });
                }
                CharacterClassRange::BuiltinClass(bc) => {
                    union = Some(self.add_to_union(union, ZoneSimpleMatch::BuiltinClass(*bc)));
                }
                CharacterClassRange::UnicodeProperty(up) => {
                    if libunicode_rust::character_types::is_string_property(up.name) {
                        return None;
                    }
                    union = Some(self.add_to_union(union, ZoneSimpleMatch::UnicodeProperty(self.zone.alloc(*up))));
                }
            }
        }

        if !plain_ranges.is_empty() {
            normalize_char_ranges(&mut plain_ranges);
            let plain = ZoneSimpleMatch::CharClass {
                ranges: plain_ranges.into_slice(),
                negated: false,
            };
            union = Some(self.add_to_union(union, plain));
        }

        union
    }

    /// Try to build a `ZoneSimpleMatch` for a `/v`-mode character class `UnicodeSet` body.
    ///
    /// For unions that can't be collapsed into a flat range set, we try to build a `ZoneSimpleMatch::Union`
    /// so that quantifiers can still use the optimized `GreedyLoop`/`LazyLoop` path.
    fn try_simple_match_for_unicode_set(
        &self,
        expr: &ClassSetExpression<'z>,
        negated: bool,
    ) -> Option<ZoneSimpleMatch<'z>> {
        if let Some(ranges) = self.try_extract_union_ranges(expr) {
            return Some(ZoneSimpleMatch::CharClass {
                ranges: ranges.into_slice(),
                negated,
            });
        }

        if negated {
            return None;
        }

        let ClassSetExpression::Union(operands) = expr else {
            return None;
        };

        if operands.is_empty() {
            return None;
        }

        self.try_simple_match_for_union(operands)
    }

    /// The sorted and merged ranges of a union of plain characters and ranges.
    fn try_extract_union_ranges(&self, expr: &ClassSetExpression<'z>) -> Option<ZoneVec<'z, CodePointRange>> {
        let mut ranges = ZoneVec::new_in(self.zone);
        if !Self::collect_union_ranges(expr, &mut ranges) {
            return None;
        }
        normalize_char_ranges(&mut ranges);
        Some(ranges)
    }

    /// Adds the ranges of a union of plain characters and ranges to `ranges`, and returns whether `expr` is one.
    fn collect_union_ranges(expr: &ClassSetExpression<'z>, ranges: &mut ZoneVec<'z, CodePointRange>) -> bool {
        if crate::stack::is_exhausted() {
            return false;
        }
        let ClassSetExpression::Union(operands) = expr else {
            return false;
        };

        for operand in *operands {
            match operand {
                ClassSetOperand::Char(c) => {
                    ranges.push(CodePointRange { start: *c, end: *c });
                }
                ClassSetOperand::Range(lo, hi) => {
                    ranges.push(CodePointRange { start: *lo, end: *hi });
                }
                ClassSetOperand::NestedClass(cc) => match &cc.body {
                    CharacterClassBody::Ranges(class_ranges) => {
                        for r in *class_ranges {
                            match r {
                                CharacterClassRange::Single(cp) => {
                                    ranges.push(CodePointRange { start: *cp, end: *cp });
                                }
                                CharacterClassRange::Range(lo, hi) => {
                                    ranges.push(CodePointRange { start: *lo, end: *hi });
                                }
                                CharacterClassRange::BuiltinClass(_) | CharacterClassRange::UnicodeProperty(_) => {
                                    return false;
                                }
                            }
                        }
                        if cc.negated {
                            return false;
                        }
                    }
                    CharacterClassBody::UnicodeSet(inner_expr) => {
                        if cc.negated {
                            return false;
                        }
                        if !Self::collect_union_ranges(inner_expr, ranges) {
                            return false;
                        }
                    }
                },
                ClassSetOperand::BuiltinClass(_)
                | ClassSetOperand::UnicodeProperty(_)
                | ClassSetOperand::StringLiteral(_) => {
                    return false;
                }
            }
        }

        true
    }

    /// Lower a `/v` character class expression.
    ///
    /// Negation is expressed as a negative lookahead plus `AnyChar` because
    /// once class operands may match strings, complementing them is no longer
    /// a simple single-code-point set operation.
    /// <https://tc39.es/ecma262/#sec-compilecharacterclass>
    fn compile_unicode_set_class(&mut self, expr: &ClassSetExpression<'z>, negated: bool) {
        if matches!(expr, ClassSetExpression::Union(operands) if operands.is_empty()) {
            if negated {
                self.emit(ZoneInstruction::AnyChar { dot_all: true });
            } else {
                self.emit(ZoneInstruction::Fail);
            }
            return;
        }

        if let Some(ranges) = self.try_extract_union_ranges(expr) {
            self.emit(ZoneInstruction::CharClass {
                ranges: ranges.into_slice(),
                negated,
            });
            return;
        }

        if negated {
            let forward = !self.backward;
            let look_start = self.emit(ZoneInstruction::LookStart {
                positive: false,
                forward,
                end: u32::MAX,
            });
            self.compile_class_set_expression(expr);
            self.emit(ZoneInstruction::LookEnd);
            let end = self.current_offset();
            self.instructions[look_start as usize] = ZoneInstruction::LookStart {
                positive: false,
                forward,
                end,
            };
            self.emit(ZoneInstruction::AnyChar { dot_all: true });
        } else {
            self.compile_class_set_expression(expr);
        }
    }

    /// Lower `ClassSetExpression`.
    /// - <https://tc39.es/ecma262/#sec-compilecharacterclass>
    /// - <https://tc39.es/ecma262/#sec-compileclasssetstring>
    fn compile_class_set_expression(&mut self, expr: &ClassSetExpression<'z>) {
        // NB: Compile each exact match length separately, longest first, so
        // intersection/subtraction only compare equal-length alternatives.
        let mut lengths = ZoneVec::new_in(self.zone);
        self.collect_class_set_expression_lengths(expr, &mut lengths);
        lengths.sort_unstable_by(|a, b| b.cmp(a));

        if lengths.is_empty() {
            self.emit(ZoneInstruction::Fail);
            return;
        }

        self.emit_split_chain(&lengths, |s, len| s.compile_class_set_expression_at_length(expr, *len));
    }

    fn compile_class_set_expression_at_length(&mut self, expr: &ClassSetExpression<'z>, length: usize) {
        if self.is_out_of_stack() {
            return;
        }
        match expr {
            ClassSetExpression::Union(operands) => {
                let mut filtered = ZoneVec::new_in(self.zone);
                for operand in *operands {
                    if self.class_set_operand_has_length(operand, length) {
                        filtered.push(operand);
                    }
                }
                if filtered.is_empty() {
                    self.emit(ZoneInstruction::Fail);
                    return;
                }
                self.emit_split_chain(&filtered, |s, operand| {
                    s.compile_class_set_operand_at_length(operand, length);
                });
            }
            ClassSetExpression::Intersection(operands) => {
                let Some((first, rest)) = operands.split_first() else {
                    self.emit(ZoneInstruction::Fail);
                    return;
                };
                self.compile_class_set_operand_at_length(first, length);
                for operand in rest {
                    if !self.class_set_operand_has_length(operand, length) {
                        self.emit(ZoneInstruction::Fail);
                        return;
                    }
                    // NB: In backward mode the already-consumed text sits to
                    // the right of the current position, so these checks need
                    // to flip from lookbehind to lookahead.
                    let forward = self.backward;
                    let look_start = self.emit(ZoneInstruction::LookStart {
                        positive: true,
                        forward,
                        end: u32::MAX,
                    });
                    let saved_backward = self.backward;
                    self.backward = !forward;
                    self.compile_class_set_operand_at_length(operand, length);
                    self.backward = saved_backward;
                    self.emit(ZoneInstruction::LookEnd);
                    let end = self.current_offset();
                    self.instructions[look_start as usize] = ZoneInstruction::LookStart {
                        positive: true,
                        forward,
                        end,
                    };
                }
            }
            ClassSetExpression::Subtraction(operands) => {
                let Some((first, rest)) = operands.split_first() else {
                    self.emit(ZoneInstruction::Fail);
                    return;
                };
                self.compile_class_set_operand_at_length(first, length);
                for operand in rest {
                    if !self.class_set_operand_has_length(operand, length) {
                        continue;
                    }
                    let forward = self.backward;
                    let look_start = self.emit(ZoneInstruction::LookStart {
                        positive: false,
                        forward,
                        end: u32::MAX,
                    });
                    let saved_backward = self.backward;
                    self.backward = !forward;
                    self.compile_class_set_operand_at_length(operand, length);
                    self.backward = saved_backward;
                    self.emit(ZoneInstruction::LookEnd);
                    let end = self.current_offset();
                    self.instructions[look_start as usize] = ZoneInstruction::LookStart {
                        positive: false,
                        forward,
                        end,
                    };
                }
            }
        }
    }

    fn compile_class_set_operand_at_length(&mut self, operand: &ClassSetOperand<'z>, length: usize) {
        match operand {
            ClassSetOperand::Char(c) => {
                if length != 1 {
                    self.emit(ZoneInstruction::Fail);
                    return;
                }
                self.emit_char_maybe_case_fold(*c);
            }
            ClassSetOperand::Range(lo, hi) => {
                if length != 1 {
                    self.emit(ZoneInstruction::Fail);
                    return;
                }
                self.emit(ZoneInstruction::CharClass {
                    ranges: self.zone.alloc_slice_copy(&[CodePointRange { start: *lo, end: *hi }]),
                    negated: false,
                });
            }
            ClassSetOperand::NestedClass(cc) => {
                if cc.negated {
                    if length != 1 {
                        self.emit(ZoneInstruction::Fail);
                        return;
                    }
                    self.compile_character_class(cc);
                    return;
                }

                match &cc.body {
                    CharacterClassBody::Ranges(_) => {
                        if length != 1 {
                            self.emit(ZoneInstruction::Fail);
                            return;
                        }
                        self.compile_character_class(cc);
                    }
                    CharacterClassBody::UnicodeSet(expr) => {
                        self.compile_class_set_expression_at_length(expr, length);
                    }
                }
            }
            ClassSetOperand::BuiltinClass(bc) => {
                if length != 1 {
                    self.emit(ZoneInstruction::Fail);
                    return;
                }
                self.emit(ZoneInstruction::BuiltinClass(*bc));
            }
            ClassSetOperand::UnicodeProperty(up) => {
                if self.unicode_sets && !up.negated && libunicode_rust::character_types::is_string_property(up.name) {
                    if length == 1 {
                        self.emit_unicode_property(up.negated, up.name, up.value);
                    } else {
                        let data = self.string_property_data(up.name);
                        let strings = ZoneVec::from_iter_in(
                            packed_strings(data).filter(|string| string.len() == length),
                            self.zone,
                        );
                        if strings.is_empty() {
                            self.emit(ZoneInstruction::Fail);
                            return;
                        }
                        self.emit_split_chain(&strings, |s, string| s.emit_code_point_string(string));
                    }
                    return;
                }
                if length != 1 {
                    self.emit(ZoneInstruction::Fail);
                    return;
                }
                self.emit_unicode_property(up.negated, up.name, up.value);
            }
            ClassSetOperand::StringLiteral(chars) => {
                if chars.len() != length {
                    self.emit(ZoneInstruction::Fail);
                    return;
                }
                self.emit_char_string(chars);
            }
        }
    }
}
