/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Bytecode compiler: AST -> Program.
//!
//! Compiles the parsed regex AST into a concrete instruction stream for the
//! backtracking VM.
//!
//! Spec:
//! - <https://tc39.es/ecma262/#sec-pattern-semantics>
//! - <https://tc39.es/ecma262/#sec-compilepattern>
use crate::ast::*;
use crate::bytecode::*;
use std::collections::BTreeSet;

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

/// Compile a parsed pattern into a bytecode program.
///
/// Spec entry point: `CompilePattern`.
/// <https://tc39.es/ecma262/#sec-compilepattern>
pub fn compile(pattern: &Pattern) -> Program {
    let mut compiler = Compiler::new(pattern);
    compiler.compile_pattern(pattern);
    compiler.program.named_groups = pattern
        .named_groups
        .iter()
        .map(|ng| NamedGroupEntry {
            name: ng.name.clone(),
            index: ng.index,
        })
        .collect();
    compiler.program
}

struct Compiler {
    program: Program,
    /// Effective flags (affected by modifier groups).
    effective_ignore_case: bool,
    effective_multiline: bool,
    effective_dot_all: bool,
    /// Next available register for internal use (repetition counters, progress checks).
    next_internal_reg: u32,
    /// True when compiling a lookbehind body (terms are reversed).
    backward: bool,
}

impl Compiler {
    fn append_ascii_char_range(char_ranges: &mut Vec<CharRange>, start: u8, end: u8) {
        char_ranges.push(CharRange {
            start: u32::from(start),
            end: u32::from(end),
        });
    }

    fn append_builtin_class_ranges_for_legacy_positive_class(
        char_ranges: &mut Vec<CharRange>,
        builtin_class: BuiltinCharacterClass,
    ) -> bool {
        match builtin_class {
            BuiltinCharacterClass::Digit => {
                Self::append_ascii_char_range(char_ranges, b'0', b'9');
                true
            }
            BuiltinCharacterClass::Word => {
                // Legacy `\w` is ASCII-only: `[A-Za-z0-9_]`.
                Self::append_ascii_char_range(char_ranges, b'0', b'9');
                Self::append_ascii_char_range(char_ranges, b'A', b'Z');
                Self::append_ascii_char_range(char_ranges, b'_', b'_');
                Self::append_ascii_char_range(char_ranges, b'a', b'z');
                true
            }
            _ => false,
        }
    }

    fn new(pattern: &Pattern) -> Self {
        // Registers: 0-1 for group 0, then 2 per capture group.
        let register_count = 2 + pattern.capture_count * 2;
        Self {
            program: Program {
                instructions: Vec::new(),
                capture_count: pattern.capture_count,
                register_count,
                unicode: pattern.flags.unicode || pattern.flags.unicode_sets,
                unicode_sets: pattern.flags.unicode_sets,
                ignore_case: pattern.flags.ignore_case,
                multiline: pattern.flags.multiline,
                dot_all: pattern.flags.dot_all,
                named_groups: Vec::new(),
            },
            effective_ignore_case: pattern.flags.ignore_case,
            effective_multiline: pattern.flags.multiline,
            effective_dot_all: pattern.flags.dot_all,
            next_internal_reg: register_count,
            backward: false,
        }
    }

    /// Fetch multi-codepoint strings for a Unicode string property via FFI.
    /// Returns empty Vec if the property is not a string property.
    ///
    /// These are the string-valued members that `/v` character classes can
    /// match atomically, as described by `CompileClassSetString`.
    /// <https://tc39.es/ecma262/#sec-compileclasssetstring>
    fn get_string_property_strings(name: &str) -> Vec<Vec<u32>> {
        let buf = libunicode_rust::character_types::get_string_property_data(name);
        if buf.is_empty() {
            return Vec::new();
        }

        // Parse packed format: [count, len1, cp1..., len2, cp2..., ...]
        let count = buf[0] as usize;
        let mut strings = Vec::with_capacity(count);
        let mut offset = 1;
        for _ in 0..count {
            if offset >= buf.len() {
                break;
            }
            let len = buf[offset] as usize;
            offset += 1;
            if offset + len > buf.len() {
                break;
            }
            strings.push(buf[offset..offset + len].to_vec());
            offset += len;
        }
        strings
    }

    fn get_string_property_strings_of_length(name: &str, len: usize) -> Vec<Vec<u32>> {
        Self::get_string_property_strings(name)
            .into_iter()
            .filter(|string| string.len() == len)
            .collect()
    }

    /// Emit code for a Unicode string property match.
    /// Compiles as: (longest_multi_cp_string | ... | shortest_multi_cp_string | single_cp_property)
    /// This ensures longest match is tried first (v-flag semantics).
    /// <https://tc39.es/ecma262/#sec-compileclasssetstring>
    fn emit_string_property_match(&mut self, name: &str, value: Option<&str>) {
        let mut strings = Self::get_string_property_strings(name);
        if strings.is_empty() {
            // Not a string property or no multi-cp strings; just emit regular property match.
            self.emit_unicode_property(false, name, value);
            return;
        }

        // Sort longest-first for greedy longest-match semantics.
        strings.sort_by_key(|s| std::cmp::Reverse(s.len()));

        // Pack strings into a flat vec: [len, cp0, cp1, ..., len, cp0, ...]
        let mut packed = Vec::new();
        for string in &strings {
            let s = if self.backward {
                string.iter().rev().copied().collect::<Vec<_>>()
            } else {
                string.clone()
            };
            packed.push(s.len() as u32);
            packed.extend_from_slice(&s);
        }

        let property_data = UnicodePropertyData {
            negated: false,
            name: name.to_string(),
            value: value.map(|v| v.to_string()),
            resolved: None,
        };

        self.emit(Instruction::StringPropertyMatch {
            strings: packed.into_boxed_slice(),
            property: Box::new(property_data),
        });
    }

    fn emit_unicode_property(&mut self, negated: bool, name: &str, value: Option<&str>) {
        self.emit(Instruction::UnicodeProperty(Box::new(UnicodePropertyData {
            negated,
            name: name.to_string(),
            value: value.map(|v| v.to_string()),
            resolved: None,
        })));
    }

    fn emit_char_string(&mut self, chars: &[char]) {
        let iter: Box<dyn Iterator<Item = &char>> = if self.backward {
            Box::new(chars.iter().rev())
        } else {
            Box::new(chars.iter())
        };
        for c in iter {
            self.emit_char_maybe_case_fold(*c as u32);
        }
    }

    fn emit_code_point_string(&mut self, string: &[u32]) {
        let iter: Box<dyn Iterator<Item = &u32>> = if self.backward {
            Box::new(string.iter().rev())
        } else {
            Box::new(string.iter())
        };
        for cp in iter {
            self.emit_char_maybe_case_fold(*cp);
        }
    }

    fn singleton_length_set() -> BTreeSet<usize> {
        [1usize].into_iter().collect()
    }

    fn class_set_expression_lengths(&self, expr: &ClassSetExpression) -> BTreeSet<usize> {
        match expr {
            ClassSetExpression::Union(operands) => operands.iter().fold(BTreeSet::new(), |mut lengths, operand| {
                lengths.extend(self.class_set_operand_lengths(operand));
                lengths
            }),
            ClassSetExpression::Intersection(operands) => {
                let Some((first, rest)) = operands.split_first() else {
                    return BTreeSet::new();
                };
                let mut lengths = self.class_set_operand_lengths(first);
                for operand in rest {
                    let operand_lengths = self.class_set_operand_lengths(operand);
                    lengths.retain(|len| operand_lengths.contains(len));
                }
                lengths
            }
            ClassSetExpression::Subtraction(operands) => operands
                .first()
                .map(|operand| self.class_set_operand_lengths(operand))
                .unwrap_or_default(),
        }
    }

    fn class_set_operand_lengths(&self, operand: &ClassSetOperand) -> BTreeSet<usize> {
        match operand {
            ClassSetOperand::Char(_) | ClassSetOperand::Range(_, _) | ClassSetOperand::BuiltinClass(_) => {
                Self::singleton_length_set()
            }
            ClassSetOperand::NestedClass(cc) => {
                if cc.negated {
                    return Self::singleton_length_set();
                }
                match &cc.body {
                    CharacterClassBody::Ranges(_) => Self::singleton_length_set(),
                    CharacterClassBody::UnicodeSet(expr) => self.class_set_expression_lengths(expr),
                }
            }
            ClassSetOperand::UnicodeProperty(up) => {
                if self.program.unicode_sets
                    && !up.negated
                    && libunicode_rust::character_types::is_string_property(&up.name)
                {
                    let mut lengths = Self::singleton_length_set();
                    for string in Self::get_string_property_strings(&up.name) {
                        lengths.insert(string.len());
                    }
                    return lengths;
                }
                Self::singleton_length_set()
            }
            ClassSetOperand::StringLiteral(chars) => [chars.len()].into_iter().collect(),
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

        let mut jump_to_end = Vec::new();
        for (i, item) in items.iter().enumerate() {
            if i < items.len() - 1 {
                let split = self.emit(Instruction::Split {
                    prefer: self.current_offset() + 1,
                    other: u32::MAX,
                });
                compile_one(self, item);
                let jmp = self.emit(Instruction::Jump(u32::MAX));
                jump_to_end.push(jmp);
                let next = self.current_offset();
                self.program.instructions[split as usize] = Instruction::Split {
                    prefer: split + 1,
                    other: next,
                };
            } else {
                compile_one(self, item);
            }
        }
        let end = self.current_offset();
        for jmp in jump_to_end {
            self.program.instructions[jmp as usize] = Instruction::Jump(end);
        }
    }

    /// Emit a Char or CharNoCase instruction for a single code point,
    /// depending on whether case-insensitive mode is active.
    fn emit_char_maybe_case_fold(&mut self, cp: u32) {
        if self.effective_ignore_case
            && let Some(ch) = char::from_u32(cp)
        {
            let lower = ch.to_lowercase().next().unwrap_or(ch) as u32;
            let upper = ch.to_uppercase().next().unwrap_or(ch) as u32;
            if lower != upper {
                self.emit(Instruction::CharNoCase(cp, cp));
                return;
            }
        }
        self.emit(Instruction::Char(cp));
    }

    fn alloc_register(&mut self) -> u32 {
        let reg = self.next_internal_reg;
        self.next_internal_reg += 1;
        self.program.register_count = self.next_internal_reg;
        reg
    }

    fn emit(&mut self, inst: Instruction) -> u32 {
        self.program.emit(inst)
    }

    fn current_offset(&self) -> u32 {
        self.program.current_offset()
    }

    /// Collect all capture group indices inside an atom (recursively).
    fn collect_captures(atom: &Atom, out: &mut Vec<u32>) {
        match atom {
            Atom::Group(g) => {
                out.push(g.index);
                Self::collect_captures_in_disjunction(&g.body, out);
            }
            Atom::NonCapturingGroup(g) => {
                Self::collect_captures_in_disjunction(&g.body, out);
            }
            Atom::ModifierGroup(mg) => {
                Self::collect_captures_in_disjunction(&mg.body, out);
            }
            Atom::Lookaround(la) => {
                Self::collect_captures_in_disjunction(&la.body, out);
            }
            _ => {}
        }
    }

    fn collect_captures_in_disjunction(disj: &Disjunction, out: &mut Vec<u32>) {
        for alt in &disj.alternatives {
            for term in &alt.terms {
                Self::collect_captures(&term.atom, out);
            }
        }
    }

    /// Emit `ClearRegister` instructions for all captures inside an atom.
    /// Per ECMA-262 RepeatMatcher step 2.d, captures must be reset to
    /// undefined at the start of each quantifier iteration.
    /// <https://tc39.es/ecma262/#sec-runtime-semantics-repeatmatcher-abstract-operation>
    fn emit_clear_captures(&mut self, atom: &Atom) {
        for idx in Self::capture_registers(atom) {
            self.emit(Instruction::ClearRegister(idx));
        }
    }

    /// Collect all capture register indices (start and end) for captures inside an atom.
    /// Returns empty for intrinsically zero-width atoms (lookarounds, assertions)
    /// since their captures should NOT be cleared on zero-width ProgressCheck.
    fn capture_registers(atom: &Atom) -> Vec<u32> {
        if matches!(atom, Atom::Lookaround(_) | Atom::Assertion(_)) {
            return Vec::new();
        }
        let mut captures = Vec::new();
        Self::collect_captures(atom, &mut captures);
        let mut regs = Vec::new();
        for idx in captures {
            regs.push(idx * 2);
            regs.push(idx * 2 + 1);
        }
        regs
    }

    fn disjunction_can_be_zero_width(&self, disjunction: &Disjunction) -> bool {
        disjunction
            .alternatives
            .iter()
            .any(|alternative| self.alternative_can_be_zero_width(alternative))
    }

    fn alternative_can_be_zero_width(&self, alternative: &Alternative) -> bool {
        alternative.terms.iter().all(|term| self.term_can_be_zero_width(term))
    }

    fn term_can_be_zero_width(&self, term: &Term) -> bool {
        match &term.quantifier {
            Some(quantifier) if quantifier.min == 0 => true,
            _ => self.atom_can_be_zero_width(&term.atom),
        }
    }

    /// Returns true if the given atom could potentially match zero characters.
    fn atom_can_be_zero_width(&self, atom: &Atom) -> bool {
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
    fn compile_pattern(&mut self, pattern: &Pattern) {
        // Save start of match (group 0).
        self.emit(Instruction::Save(0));
        self.compile_disjunction(&pattern.disjunction);
        // Save end of match (group 0).
        self.emit(Instruction::Save(1));
        self.emit(Instruction::Match);
    }

    /// Lower `Disjunction` to a chain of split/jump choice points.
    /// <https://tc39.es/ecma262/#sec-compilesubpattern>
    fn compile_disjunction(&mut self, disj: &Disjunction) {
        if let Some(term) = self.try_merge_optional_alternatives(disj) {
            self.compile_term(&term);
            return;
        }
        self.emit_split_chain(&disj.alternatives, |s, alt| s.compile_alternative(alt));
    }

    /// Lower `Alternative` in the current direction.
    /// <https://tc39.es/ecma262/#sec-compilesubpattern>
    fn compile_alternative(&mut self, alt: &Alternative) {
        if self.backward {
            for term in alt.terms.iter().rev() {
                self.compile_term(term);
            }
        } else {
            for term in &alt.terms {
                self.compile_term(term);
            }
        }
    }

    /// Lower `Term`, delegating to either `CompileAtom` or the quantified path.
    /// <https://tc39.es/ecma262/#sec-compilesubpattern>
    fn compile_term(&mut self, term: &Term) {
        match &term.quantifier {
            None => self.compile_atom(&term.atom),
            Some(q) => self.compile_quantified(&term.atom, q),
        }
    }

    /// Try to extract a SimpleMatch for an atom (for optimized greedy/lazy loops).
    fn try_simple_match(&self, atom: &Atom) -> Option<SimpleMatch> {
        match atom {
            Atom::Dot => Some(SimpleMatch::AnyChar {
                dot_all: self.effective_dot_all,
            }),
            Atom::Literal(c) => {
                // Don't use simple match for supplementary chars in non-unicode mode
                // (they need surrogate pair splitting).
                if *c > 0xFFFF && !self.program.unicode && !self.program.unicode_sets {
                    return None;
                }
                if self.effective_ignore_case {
                    let lo = *c;
                    let hi = *c; // VM will use case_fold_eq
                    Some(SimpleMatch::CharNoCase(lo, hi))
                } else {
                    Some(SimpleMatch::Char(*c))
                }
            }
            Atom::BuiltinCharacterClass(class) => Some(SimpleMatch::BuiltinClass(*class)),
            Atom::CharacterClass(cc) => {
                // Only use simple match for character classes without v-flag set operations.
                let ranges_vec = match &cc.body {
                    CharacterClassBody::Ranges(ranges) => ranges,
                    CharacterClassBody::UnicodeSet(_) => return None,
                };
                let mut ranges = Vec::new();
                for r in ranges_vec {
                    match r {
                        CharacterClassRange::Single(c) => {
                            if *c > 0xFFFF && !self.program.unicode && !self.program.unicode_sets {
                                return None;
                            }
                            ranges.push(CharRange { start: *c, end: *c });
                        }
                        CharacterClassRange::Range(lo, hi) => {
                            ranges.push(CharRange { start: *lo, end: *hi });
                        }
                        _ => return None, // BuiltinClass or UnicodeProperty in class
                    }
                }
                // Sort ranges by start code point for binary search in the VM.
                ranges.sort_by_key(|r| r.start);
                Some(SimpleMatch::CharClass {
                    ranges,
                    negated: cc.negated,
                })
            }
            Atom::UnicodeProperty(up) => {
                // String properties (e.g. Basic_Emoji) can match multi-character
                // sequences and cannot use simple matching.
                if self.program.unicode_sets && libunicode_rust::character_types::is_string_property(&up.name) {
                    return None;
                }
                Some(SimpleMatch::UnicodeProperty(Box::new(UnicodePropertyData {
                    negated: up.negated,
                    name: up.name.clone(),
                    value: up.value.clone(),
                    resolved: None, // Will be populated after compilation.
                })))
            }
            _ => None,
        }
    }

    /// Collapse simple disjunctions like `a|a?` into a single greedy optional
    /// term. This preserves semantics for single-term simple-match
    /// alternatives while avoiding exponential backtracking in quantified
    /// contexts such as `^(a|a?)+$`.
    fn try_merge_optional_alternatives(&self, disj: &Disjunction) -> Option<Term> {
        let [first, second] = disj.alternatives.as_slice() else {
            return None;
        };
        let [first_term] = first.terms.as_slice() else {
            return None;
        };
        let [second_term] = second.terms.as_slice() else {
            return None;
        };

        let first_match = self.try_simple_match(&first_term.atom)?;
        let second_match = self.try_simple_match(&second_term.atom)?;
        if first_match != second_match {
            return None;
        }

        let optional = Quantifier::zero_or_one(true);
        match (&first_term.quantifier, &second_term.quantifier) {
            (None, Some(q)) | (Some(q), None) if *q == optional => Some(Term {
                atom: first_term.atom.clone(),
                quantifier: Some(optional),
            }),
            _ => None,
        }
    }

    /// Lower a quantified atom using the same observable behavior as
    /// `CompileQuantifier` plus `RepeatMatcher`.
    /// - <https://tc39.es/ecma262/#sec-compilequantifier>
    /// - <https://tc39.es/ecma262/#sec-runtime-semantics-repeatmatcher-abstract-operation>
    fn compile_quantified(&mut self, atom: &Atom, q: &Quantifier) {
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
                        self.emit(Instruction::GreedyLoop {
                            matcher,
                            min: q.min,
                            max: q.max,
                        });
                    } else {
                        self.emit(Instruction::LazyLoop {
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

            self.emit(Instruction::RepeatStart { counter_reg });
            let body_start = self.current_offset();
            if let Some(reg) = progress_reg {
                self.emit(Instruction::Save(reg));
            }
            self.emit_clear_captures(atom);
            self.compile_atom(atom);
            if let Some(reg) = progress_reg {
                self.emit(Instruction::ProgressCheck {
                    reg,
                    clear_captures: Self::capture_registers(atom),
                });
            }
            self.emit(Instruction::RepeatCheck {
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
                if !self.atom_can_be_zero_width(atom) {
                    self.compile_counted_optional_repetitions(atom, optional_count, q.greedy);
                    return;
                }
                let needs_progress_check = q.min == 0 && self.atom_can_be_zero_width(atom);
                for _ in 0..optional_count {
                    let progress_reg = if needs_progress_check {
                        Some(self.alloc_register())
                    } else {
                        None
                    };
                    if q.greedy {
                        // Split: prefer body, other skip.
                        let split = self.emit(Instruction::Split {
                            prefer: self.current_offset() + 1,
                            other: u32::MAX,
                        });
                        if let Some(reg) = progress_reg {
                            self.emit(Instruction::Save(reg));
                        }
                        self.emit_clear_captures(atom);
                        self.compile_atom(atom);
                        if let Some(reg) = progress_reg {
                            self.emit(Instruction::ProgressCheck {
                                reg,
                                clear_captures: Self::capture_registers(atom),
                            });
                        }
                        let after = self.current_offset();
                        self.program.instructions[split as usize] = Instruction::Split {
                            prefer: split + 1,
                            other: after,
                        };
                    } else {
                        // Lazy: prefer skip, other body.
                        let split = self.emit(Instruction::Split {
                            prefer: u32::MAX,
                            other: self.current_offset() + 1,
                        });
                        if let Some(reg) = progress_reg {
                            self.emit(Instruction::Save(reg));
                        }
                        self.emit_clear_captures(atom);
                        self.compile_atom(atom);
                        if let Some(reg) = progress_reg {
                            self.emit(Instruction::ProgressCheck {
                                reg,
                                clear_captures: Self::capture_registers(atom),
                            });
                        }
                        let after = self.current_offset();
                        self.program.instructions[split as usize] = Instruction::Split {
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
                    let split = self.emit(Instruction::Split {
                        prefer: self.current_offset() + 1,
                        other: u32::MAX,
                    });
                    if can_be_zero_width {
                        self.emit(Instruction::Save(progress_reg));
                    }
                    self.emit_clear_captures(atom);
                    self.compile_atom(atom);
                    if can_be_zero_width {
                        self.emit(Instruction::ProgressCheck {
                            reg: progress_reg,
                            clear_captures: Self::capture_registers(atom),
                        });
                    }
                    self.emit(Instruction::Jump(loop_start));
                    let after = self.current_offset();
                    self.program.instructions[split as usize] = Instruction::Split {
                        prefer: split + 1,
                        other: after,
                    };
                } else {
                    let split = self.emit(Instruction::Split {
                        prefer: u32::MAX,
                        other: self.current_offset() + 1,
                    });
                    if can_be_zero_width {
                        self.emit(Instruction::Save(progress_reg));
                    }
                    self.emit_clear_captures(atom);
                    self.compile_atom(atom);
                    if can_be_zero_width {
                        self.emit(Instruction::ProgressCheck {
                            reg: progress_reg,
                            clear_captures: Self::capture_registers(atom),
                        });
                    }
                    self.emit(Instruction::Jump(loop_start));
                    let after = self.current_offset();
                    self.program.instructions[split as usize] = Instruction::Split {
                        prefer: after,
                        other: split + 1,
                    };
                }
            }
        }
    }

    fn compile_counted_optional_repetitions(&mut self, atom: &Atom, optional_count: u32, greedy: bool) {
        let counter_reg = self.alloc_register();
        self.emit(Instruction::RepeatStart { counter_reg });

        // min is always 0 here: this function only emits the optional tail of a {min,max}
        // quantifier; the required `min` repetitions have already been emitted by the caller.
        let check = self.emit(Instruction::RepeatCheck {
            counter_reg,
            min: 0,
            max: Some(optional_count),
            body: u32::MAX,
            greedy,
        });

        let skip_body = self.emit(Instruction::Jump(u32::MAX));
        let body_start = self.current_offset();
        self.emit_clear_captures(atom);
        self.compile_atom(atom);
        self.emit(Instruction::Jump(check));

        self.program.instructions[check as usize] = Instruction::RepeatCheck {
            counter_reg,
            min: 0,
            max: Some(optional_count),
            body: body_start,
            greedy,
        };
        self.program.instructions[skip_body as usize] = Instruction::Jump(self.current_offset());
    }

    /// Lower an `Atom` or assertion-like atom to bytecode.
    /// - <https://tc39.es/ecma262/#sec-compileatom>
    /// - <https://tc39.es/ecma262/#sec-compileassertion>
    fn compile_atom(&mut self, atom: &Atom) {
        match atom {
            Atom::Literal(c) => {
                // In non-unicode mode, supplementary characters (>0xFFFF) must be
                // split into their UTF-16 surrogate pair halves.
                if *c > 0xFFFF && !self.program.unicode && !self.program.unicode_sets {
                    let hi = ((*c - 0x10000) >> 10) + 0xD800;
                    let lo = ((*c - 0x10000) & 0x3FF) + 0xDC00;
                    self.emit(Instruction::Char(hi));
                    self.emit(Instruction::Char(lo));
                } else {
                    self.emit_char_maybe_case_fold(*c);
                }
            }

            Atom::Dot => {
                self.emit(Instruction::AnyChar {
                    dot_all: self.effective_dot_all,
                });
            }

            Atom::CharacterClass(cc) => {
                self.compile_character_class(cc);
            }

            Atom::BuiltinCharacterClass(bc) => {
                self.emit(Instruction::BuiltinClass(*bc));
            }

            Atom::UnicodeProperty(up) => {
                if self.program.unicode_sets
                    && !up.negated
                    && libunicode_rust::character_types::is_string_property(&up.name)
                {
                    self.emit_string_property_match(&up.name, up.value.as_deref());
                    return;
                }
                self.emit_unicode_property(up.negated, &up.name, up.value.as_deref());
            }

            Atom::Group(g) => {
                let save_start = g.index * 2;
                let save_end = g.index * 2 + 1;
                // Clear captures inside this group before entering
                // (ECMA-262 requires captures to reset on each iteration).
                self.emit(Instruction::Save(save_start));
                self.compile_disjunction(&g.body);
                self.emit(Instruction::Save(save_end));
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
                let look_start = self.emit(Instruction::LookStart {
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
                self.emit(Instruction::LookEnd);
                let end = self.current_offset();
                // Patch the end offset.
                self.program.instructions[look_start as usize] = Instruction::LookStart { positive, forward, end };
            }

            Atom::Backreference(br) => match br {
                Backreference::Index(n) => {
                    self.emit(Instruction::Backref(*n));
                }
                Backreference::Named(name) => {
                    self.emit(Instruction::BackrefNamed(name.clone()));
                }
            },

            Atom::Assertion(kind) => match kind {
                AssertionKind::StartOfInput => {
                    self.emit(Instruction::AssertStart {
                        multiline: self.effective_multiline,
                    });
                }
                AssertionKind::EndOfInput => {
                    self.emit(Instruction::AssertEnd {
                        multiline: self.effective_multiline,
                    });
                }
                AssertionKind::WordBoundary => {
                    self.emit(Instruction::AssertWordBoundary);
                }
                AssertionKind::NonWordBoundary => {
                    self.emit(Instruction::AssertNonWordBoundary);
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
                self.emit(Instruction::PushModifiers {
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
                self.emit(Instruction::PopModifiers);
            }
        }
    }

    /// Lower `CharacterClass`.
    /// <https://tc39.es/ecma262/#sec-compilecharacterclass>
    fn compile_character_class(&mut self, cc: &CharacterClass) {
        match &cc.body {
            CharacterClassBody::Ranges(ranges) => {
                let mut char_ranges = Vec::new();
                let mut has_builtin = false;

                let split_surrogates = !self.program.unicode && !self.program.unicode_sets;
                let can_inline_builtin_ranges = !cc.negated && !self.program.unicode && !self.program.unicode_sets;
                for r in ranges {
                    match r {
                        CharacterClassRange::Single(cp) => {
                            if split_surrogates && *cp > 0xFFFF {
                                let hi = ((*cp - 0x10000) >> 10) + 0xD800;
                                let lo = ((*cp - 0x10000) & 0x3FF) + 0xDC00;
                                char_ranges.push(CharRange { start: hi, end: hi });
                                char_ranges.push(CharRange { start: lo, end: lo });
                            } else {
                                char_ranges.push(CharRange { start: *cp, end: *cp });
                            }
                        }
                        CharacterClassRange::Range(lo, hi) => {
                            char_ranges.push(CharRange { start: *lo, end: *hi });
                        }
                        CharacterClassRange::BuiltinClass(class)
                            if can_inline_builtin_ranges
                                && Self::append_builtin_class_ranges_for_legacy_positive_class(
                                    &mut char_ranges,
                                    *class,
                                ) => {}
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
                    // Sort ranges by start code point for binary search in the VM.
                    char_ranges.sort_by_key(|r| r.start);
                    self.emit(Instruction::CharClass {
                        ranges: char_ranges,
                        negated: cc.negated,
                    });
                }
            }
            CharacterClassBody::UnicodeSet(expr) => {
                self.compile_unicode_set_class(expr, cc.negated);
            }
        }
    }

    fn compile_complex_class(&mut self, ranges: &[CharacterClassRange], negated: bool) {
        // For classes containing builtin escapes like [\d\w], we emit
        // a split-based disjunction of each component.
        // For negated classes with a single builtin, emit the negated builtin directly.
        if negated {
            // Optimization: [^\s] → \S, [^\d] → \D, [^\w] → \W
            if ranges.len() == 1
                && let CharacterClassRange::BuiltinClass(bc) = &ranges[0]
            {
                self.emit(Instruction::BuiltinClass(bc.negated()));
                return;
            }
            // General case: [^\d\w] = (?!\d|\w).
            let forward = !self.backward;
            let look_start = self.emit(Instruction::LookStart {
                positive: false,
                forward,
                end: u32::MAX,
            });
            self.compile_class_components_as_disjunction(ranges);
            self.emit(Instruction::LookEnd);
            let end = self.current_offset();
            self.program.instructions[look_start as usize] = Instruction::LookStart {
                positive: false,
                forward,
                end,
            };
            self.emit(Instruction::AnyChar { dot_all: true });
        } else {
            self.compile_class_components_as_disjunction(ranges);
        }
    }

    fn compile_class_components_as_disjunction(&mut self, ranges: &[CharacterClassRange]) {
        self.emit_split_chain(ranges, |s, r| s.compile_class_component(r));
    }

    fn compile_class_component(&mut self, range: &CharacterClassRange) {
        match range {
            CharacterClassRange::Single(cp) => {
                self.emit_char_maybe_case_fold(*cp);
            }
            CharacterClassRange::Range(lo, hi) => {
                self.emit(Instruction::CharClass {
                    ranges: vec![CharRange { start: *lo, end: *hi }],
                    negated: false,
                });
            }
            CharacterClassRange::BuiltinClass(bc) => {
                self.emit(Instruction::BuiltinClass(*bc));
            }
            CharacterClassRange::UnicodeProperty(up) => {
                self.emit_unicode_property(up.negated, &up.name, up.value.as_deref());
            }
        }
    }

    /// Lower a `/v` character class expression.
    ///
    /// Negation is expressed as a negative lookahead plus `AnyChar` because
    /// once class operands may match strings, complementing them is no longer
    /// a simple single-code-point set operation.
    /// <https://tc39.es/ecma262/#sec-compilecharacterclass>
    fn compile_unicode_set_class(&mut self, expr: &ClassSetExpression, negated: bool) {
        if matches!(expr, ClassSetExpression::Union(operands) if operands.is_empty()) {
            if negated {
                self.emit(Instruction::AnyChar { dot_all: true });
            } else {
                self.emit(Instruction::Fail);
            }
            return;
        }

        // For now, compile unicode set classes as disjunctions of their operands.
        // This is correct but not optimal — future optimization can merge ranges.
        if negated {
            let forward = !self.backward;
            let look_start = self.emit(Instruction::LookStart {
                positive: false,
                forward,
                end: u32::MAX,
            });
            self.compile_class_set_expression(expr);
            self.emit(Instruction::LookEnd);
            let end = self.current_offset();
            self.program.instructions[look_start as usize] = Instruction::LookStart {
                positive: false,
                forward,
                end,
            };
            self.emit(Instruction::AnyChar { dot_all: true });
        } else {
            self.compile_class_set_expression(expr);
        }
    }

    /// Lower `ClassSetExpression`.
    /// - <https://tc39.es/ecma262/#sec-compilecharacterclass>
    /// - <https://tc39.es/ecma262/#sec-compileclasssetstring>
    fn compile_class_set_expression(&mut self, expr: &ClassSetExpression) {
        // NB: Compile each exact match length separately, longest first, so
        // intersection/subtraction only compare equal-length alternatives.
        let mut lengths: Vec<_> = self.class_set_expression_lengths(expr).into_iter().collect();
        lengths.sort_unstable_by(|a, b| b.cmp(a));

        if lengths.is_empty() {
            self.emit(Instruction::Fail);
            return;
        }

        self.emit_split_chain(&lengths, |s, len| s.compile_class_set_expression_at_length(expr, *len));
    }

    fn compile_class_set_expression_at_length(&mut self, expr: &ClassSetExpression, length: usize) {
        match expr {
            ClassSetExpression::Union(operands) => {
                let filtered: Vec<&ClassSetOperand> = operands
                    .iter()
                    .filter(|operand| self.class_set_operand_lengths(operand).contains(&length))
                    .collect();
                if filtered.is_empty() {
                    self.emit(Instruction::Fail);
                    return;
                }
                self.emit_split_chain(&filtered, |s, operand| {
                    s.compile_class_set_operand_at_length(operand, length)
                });
            }
            ClassSetExpression::Intersection(operands) => {
                let Some((first, rest)) = operands.split_first() else {
                    self.emit(Instruction::Fail);
                    return;
                };
                self.compile_class_set_operand_at_length(first, length);
                for operand in rest {
                    if !self.class_set_operand_lengths(operand).contains(&length) {
                        self.emit(Instruction::Fail);
                        return;
                    }
                    // NB: In backward mode the already-consumed text sits to
                    // the right of the current position, so these checks need
                    // to flip from lookbehind to lookahead.
                    let forward = self.backward;
                    let look_start = self.emit(Instruction::LookStart {
                        positive: true,
                        forward,
                        end: u32::MAX,
                    });
                    let saved_backward = self.backward;
                    self.backward = !forward;
                    self.compile_class_set_operand_at_length(operand, length);
                    self.backward = saved_backward;
                    self.emit(Instruction::LookEnd);
                    let end = self.current_offset();
                    self.program.instructions[look_start as usize] = Instruction::LookStart {
                        positive: true,
                        forward,
                        end,
                    };
                }
            }
            ClassSetExpression::Subtraction(operands) => {
                let Some((first, rest)) = operands.split_first() else {
                    self.emit(Instruction::Fail);
                    return;
                };
                self.compile_class_set_operand_at_length(first, length);
                for operand in rest {
                    if !self.class_set_operand_lengths(operand).contains(&length) {
                        continue;
                    }
                    let forward = self.backward;
                    let look_start = self.emit(Instruction::LookStart {
                        positive: false,
                        forward,
                        end: u32::MAX,
                    });
                    let saved_backward = self.backward;
                    self.backward = !forward;
                    self.compile_class_set_operand_at_length(operand, length);
                    self.backward = saved_backward;
                    self.emit(Instruction::LookEnd);
                    let end = self.current_offset();
                    self.program.instructions[look_start as usize] = Instruction::LookStart {
                        positive: false,
                        forward,
                        end,
                    };
                }
            }
        }
    }

    fn compile_class_set_operand_at_length(&mut self, operand: &ClassSetOperand, length: usize) {
        match operand {
            ClassSetOperand::Char(c) => {
                if length != 1 {
                    self.emit(Instruction::Fail);
                    return;
                }
                self.emit_char_maybe_case_fold(*c as u32);
            }
            ClassSetOperand::Range(lo, hi) => {
                if length != 1 {
                    self.emit(Instruction::Fail);
                    return;
                }
                self.emit(Instruction::CharClass {
                    ranges: vec![CharRange {
                        start: *lo as u32,
                        end: *hi as u32,
                    }],
                    negated: false,
                });
            }
            ClassSetOperand::NestedClass(cc) => {
                if cc.negated {
                    if length != 1 {
                        self.emit(Instruction::Fail);
                        return;
                    }
                    self.compile_character_class(cc);
                    return;
                }

                match &cc.body {
                    CharacterClassBody::Ranges(_) => {
                        if length != 1 {
                            self.emit(Instruction::Fail);
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
                    self.emit(Instruction::Fail);
                    return;
                }
                self.emit(Instruction::BuiltinClass(*bc));
            }
            ClassSetOperand::UnicodeProperty(up) => {
                if self.program.unicode_sets
                    && !up.negated
                    && libunicode_rust::character_types::is_string_property(&up.name)
                {
                    if length == 1 {
                        self.emit_unicode_property(up.negated, &up.name, up.value.as_deref());
                    } else {
                        let strings = Self::get_string_property_strings_of_length(&up.name, length);
                        if strings.is_empty() {
                            self.emit(Instruction::Fail);
                            return;
                        }
                        self.emit_split_chain(&strings, |s, string| s.emit_code_point_string(string));
                    }
                    return;
                }
                if length != 1 {
                    self.emit(Instruction::Fail);
                    return;
                }
                self.emit_unicode_property(up.negated, &up.name, up.value.as_deref());
            }
            ClassSetOperand::StringLiteral(chars) => {
                if chars.len() != length {
                    self.emit(Instruction::Fail);
                    return;
                }
                self.emit_char_string(chars);
            }
        }
    }
}
