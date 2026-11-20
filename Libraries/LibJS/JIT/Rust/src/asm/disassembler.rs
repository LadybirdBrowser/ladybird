/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Disassemblers for `dump-asm` and for checking encodings in tests.
//!
//! Tests can decode both architectures on any host. Other builds only have
//! the decoder of the host architecture; code for the other one is listed as
//! raw bytes.

use super::Architecture;

/// One instruction of a listing, at `offset` from the start of the code.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DisassembledInstruction {
    pub offset: usize,
    pub length: usize,
    pub text: String,
    /// The 64-bit constant the instruction materializes, if any (an
    /// immediate move, or the last `movz`/`movk` of a sequence on AArch64,
    /// or a load from the constant pool).
    pub constant: Option<u64>,
    /// The offset a PC-relative memory operand refers to, if any.
    pub pc_relative_target: Option<usize>,
}

/// Calls `visit` with each instruction of `code` (assumed to start at offset 0, so branch targets are code offsets)
/// for `architecture`, in order: those up to `data_offset`, then the 64-bit constants they load (see
/// `PortableMacroAssembler::finish`). Loads of a constant list it as their `constant`.
pub fn for_each_instruction(
    architecture: Architecture,
    code: &[u8],
    data_offset: usize,
    mut visit: impl FnMut(&DisassembledInstruction),
) {
    let constant_at = |target: usize| {
        let bytes = code.get(target..target + 8)?;
        (target >= data_offset).then(|| u64::from_le_bytes(bytes.try_into().expect("eight bytes")))
    };
    let mut visit_instruction = |mut instruction: DisassembledInstruction| {
        if let Some(target) = instruction.pc_relative_target {
            instruction.constant = instruction.constant.or_else(|| constant_at(target));
        }
        visit(&instruction);
    };
    match architecture {
        Architecture::X86_64 => x86_64_instructions(&code[..data_offset], &mut visit_instruction),
        Architecture::AArch64 => aarch64_instructions(&code[..data_offset], &mut visit_instruction),
    }
    let first_constant = data_offset.next_multiple_of(8);
    for offset in (first_constant..code.len()).step_by(8) {
        let value = constant_at(offset).expect("constants fill the data");
        visit_instruction(DisassembledInstruction {
            offset,
            length: 8,
            text: format!(".quad {value:#x}"),
            constant: Some(value),
            pc_relative_target: None,
        });
    }
}

/// The bytes of code that no decoder in this build understands.
#[cfg(not(test))]
fn raw_instructions(code: &[u8], visit: &mut impl FnMut(DisassembledInstruction)) {
    use std::fmt::Write;

    for (index, chunk) in code.chunks(8).enumerate() {
        let mut text = String::from(".byte");
        for byte in chunk {
            write!(text, " {byte:02x}").unwrap();
        }
        visit(DisassembledInstruction {
            offset: 8 * index,
            length: chunk.len(),
            text,
            constant: None,
            pc_relative_target: None,
        });
    }
}

#[cfg(not(any(test, target_arch = "x86_64")))]
fn x86_64_instructions(code: &[u8], visit: &mut impl FnMut(DisassembledInstruction)) {
    raw_instructions(code, visit);
}

#[cfg(not(any(test, target_arch = "aarch64")))]
fn aarch64_instructions(code: &[u8], visit: &mut impl FnMut(DisassembledInstruction)) {
    raw_instructions(code, visit);
}

#[cfg(any(test, target_arch = "x86_64"))]
fn x86_64_formatter() -> iced_x86::IntelFormatter {
    use iced_x86::Formatter;
    use iced_x86::MemorySizeOptions;

    let mut formatter = iced_x86::IntelFormatter::new();
    let options = formatter.options_mut();
    options.set_space_after_operand_separator(true);
    options.set_hex_prefix("0x");
    options.set_hex_suffix("");
    options.set_uppercase_hex(false);
    options.set_branch_leading_zeros(false);
    options.set_memory_size_options(MemorySizeOptions::Always);
    formatter
}

#[cfg(any(test, target_arch = "x86_64"))]
fn x86_64_instructions(code: &[u8], visit: &mut impl FnMut(DisassembledInstruction)) {
    use iced_x86::Decoder;
    use iced_x86::DecoderOptions;
    use iced_x86::Formatter;
    use iced_x86::OpKind;

    let mut decoder = Decoder::with_ip(64, code, 0, DecoderOptions::NONE);
    let mut formatter = x86_64_formatter();
    while decoder.can_decode() {
        let instruction = decoder.decode();
        let offset = instruction.ip() as usize;
        let mut text = String::new();
        if instruction.is_invalid() {
            text = format!(".byte {:02x}", code[offset]);
        } else {
            formatter.format(&instruction, &mut text);
        }
        let constant = (0..instruction.op_count())
            .find(|operand| instruction.op_kind(*operand) == OpKind::Immediate64)
            .map(|operand| instruction.immediate(operand));
        let pc_relative_target = instruction
            .is_ip_rel_memory_operand()
            .then(|| instruction.ip_rel_memory_address() as usize);
        visit(DisassembledInstruction {
            offset,
            length: instruction.len(),
            text,
            constant,
            pc_relative_target,
        });
    }
}

/// The words of `text`, with one space between each two.
#[cfg(any(test, target_arch = "aarch64"))]
fn single_spaced(text: &str) -> String {
    let mut spaced = String::with_capacity(text.len());
    for word in text.split_whitespace() {
        if !spaced.is_empty() {
            spaced.push(' ');
        }
        spaced.push_str(word);
    }
    spaced
}

#[cfg(any(test, target_arch = "aarch64"))]
fn aarch64_instructions(code: &[u8], visit: &mut impl FnMut(DisassembledInstruction)) {
    use farm64::format::FmtFormatter;
    use farm64::format::format_to_string;

    let mut decoder = farm64::Decoder::new(code, 0, farm64::DecoderOptions::default());
    let formatter = FmtFormatter::new();
    // The value each register holds after a `movz`/`movk` sequence.
    let mut register_values = [None::<u64>; 32];
    while decoder.can_decode() {
        let offset = decoder.position();
        let instruction = decoder.decode();
        let word = u32::from_le_bytes(code[offset..offset + 4].try_into().unwrap());
        let text = if instruction.is_invalid() {
            format!(".word {word:#010x}")
        } else {
            single_spaced(&format_to_string(&formatter, &instruction))
        };
        let register = (word & 0x1f) as usize;
        let shift = 16 * ((word >> 21) & 3);
        let immediate = u64::from((word >> 5) & 0xffff) << shift;
        let constant = match word & 0xff80_0000 {
            // movz Xd, #imm16, lsl #shift
            0xd280_0000 => {
                register_values[register] = Some(immediate);
                Some(immediate)
            }
            // movk Xd, #imm16, lsl #shift
            0xf280_0000 => {
                let value = register_values[register].map(|value| (value & !(0xffff << shift)) | immediate);
                register_values[register] = value;
                value
            }
            _ => None,
        };
        // LDR (literal) of a 64-bit general or FP register.
        let pc_relative_target = (word & 0xff00_0000 == 0x5800_0000 || word & 0xff00_0000 == 0x5c00_0000).then(|| {
            let imm19 = ((((word >> 5) & 0x7ffff) << 13) as i32) >> 13;
            (offset as i64 + 4 * i64::from(imm19)) as usize
        });
        visit(DisassembledInstruction {
            offset,
            length: 4,
            text,
            constant,
            pc_relative_target,
        });
    }
}

#[cfg(test)]
pub(crate) use listings::aarch64;
#[cfg(test)]
pub(crate) use listings::aarch64_range;
#[cfg(test)]
pub(crate) use listings::assert_encodings;
#[cfg(test)]
pub(crate) use listings::listing;
#[cfg(test)]
pub(crate) use listings::listing_with_data;
#[cfg(test)]
pub(crate) use listings::x86_64;

/// Listings as lists, for tests.
#[cfg(test)]
pub(crate) mod listings {
    use super::*;

    /// The instructions of `code` (assumed to start at offset 0, so branch targets are code offsets) for
    /// `architecture`.
    pub fn listing(architecture: Architecture, code: &[u8]) -> Vec<DisassembledInstruction> {
        listing_with_data(architecture, code, code.len())
    }

    /// Like `listing()`, for instructions up to `data_offset` followed by the 64-bit constants they load.
    pub fn listing_with_data(
        architecture: Architecture,
        code: &[u8],
        data_offset: usize,
    ) -> Vec<DisassembledInstruction> {
        let mut instructions = Vec::new();
        for_each_instruction(architecture, code, data_offset, |instruction| {
            instructions.push(instruction.clone())
        });
        instructions
    }

    /// One line per instruction, Intel syntax, code assumed to start at 0.
    pub fn x86_64(code: &[u8]) -> Vec<String> {
        listing(Architecture::X86_64, code)
            .into_iter()
            .map(|instruction| {
                assert!(
                    !instruction.text.starts_with(".byte"),
                    "invalid x86-64 encoding in {code:02x?}"
                );
                instruction.text
            })
            .collect()
    }

    /// One line per instruction, code assumed to start at 0.
    pub fn aarch64(code: &[u8]) -> Vec<String> {
        aarch64_range(code, 0, code.len())
    }

    /// Disassembles `code[start..end]`, with addresses relative to `code`.
    pub fn aarch64_range(code: &[u8], start: usize, end: usize) -> Vec<String> {
        use farm64::format::FmtFormatter;
        use farm64::format::format_to_string;

        let code_start = start;
        let code = &code[start..end];
        assert!(code.len().is_multiple_of(4));
        let mut decoder = farm64::Decoder::new(code, code_start as u64, farm64::DecoderOptions::default());
        let formatter = FmtFormatter::new();
        let mut lines = Vec::new();
        while decoder.can_decode() {
            let offset = decoder.position();
            let instruction = decoder.decode();
            let word = u32::from_le_bytes(code[offset..offset + 4].try_into().unwrap());
            assert!(!instruction.is_invalid(), "invalid AArch64 encoding {word:#010x}");
            let text = format_to_string(&formatter, &instruction);
            lines.push(single_spaced(&text));
        }
        lines
    }

    /// Where the instructions end in the code that an assembler finished: at the end of the code of an assembler, and
    /// at the data offset that a macro assembler gives.
    pub(crate) trait InstructionsEnd {
        fn instructions_end(&self) -> usize;
    }

    impl InstructionsEnd for &[u8] {
        fn instructions_end(&self) -> usize {
            self.len()
        }
    }

    impl InstructionsEnd for usize {
        fn instructions_end(&self) -> usize {
            *self
        }
    }

    /// Checks a table of `"expected; disassembly" => |assembler| emission;`
    /// entries, reporting every mismatch at once.
    macro_rules! assert_encodings {
        ($assembler:ty, $disassemble:path, { $($expected:expr => |$a:ident| $body:expr;)* }) => {{
            let mut failures: Vec<String> = Vec::new();
            $(
                let (mut code, work) = (crate::asm::test_code(), crate::asm::test_work());
                let mut $a = <$assembler>::new(&mut code, &work);
                $body;
                let end = crate::asm::disassembler::listings::InstructionsEnd::instructions_end(
                    &$a.finish().expect("assembling failed"),
                );
                let actual = $disassemble(&code[..end]).join("; ");
                if actual != $expected {
                    failures.push(format!("{}\n    expected: {}\n    actual:   {}", stringify!($body), $expected, actual));
                }
            )*
            assert!(failures.is_empty(), "{} mismatches:\n{}", failures.len(), failures.join("\n"));
        }};
    }

    pub(crate) use assert_encodings;
}

#[cfg(test)]
mod tests {
    use super::super::Gpr;
    use super::super::PortableMacroAssembler;
    use super::super::aarch64;
    use super::*;

    #[test]
    fn lists_x86_64_code_with_constants() {
        // mov r11, 0x123456789abcdef0; call r11; ret
        let code = [
            0x49, 0xbb, 0xf0, 0xde, 0xbc, 0x9a, 0x78, 0x56, 0x34, 0x12, 0x41, 0xff, 0xd3, 0xc3,
        ];
        let listing = listing(Architecture::X86_64, &code);
        let lines = listing
            .iter()
            .map(|instruction| (instruction.offset, instruction.text.as_str(), instruction.constant))
            .collect::<Vec<_>>();
        assert_eq!(
            lines,
            [
                (0, "mov r11, 0x123456789abcdef0", Some(0x1234_5678_9abc_def0)),
                (10, "call r11", None),
                (13, "ret", None),
            ]
        );
    }

    #[test]
    fn lists_aarch64_code_with_constants() {
        let (mut code, work) = (crate::asm::test_code(), crate::asm::test_work());
        let mut masm = aarch64::MacroAssembler::new(&mut code, &work);
        masm.move_imm64(Gpr(9), 0x1234_0000_0000_def0);
        masm.finish().unwrap();
        let listing = listing(Architecture::AArch64, &code);
        assert_eq!(listing.last().unwrap().constant, Some(0x1234_0000_0000_def0));
        assert!(listing[0].text.starts_with("mov"), "{listing:?}");
    }

    #[test]
    fn lists_loads_from_the_constant_pool() {
        let (mut code, work) = (crate::asm::test_code(), crate::asm::test_work());
        let mut masm = aarch64::MacroAssembler::new(&mut code, &work);
        masm.move_imm64(Gpr(9), 0x1234_5678_9abc_def0);
        let data_offset = masm.finish().unwrap();
        let listing = listing_with_data(Architecture::AArch64, &code, data_offset);
        assert!(listing[0].text.starts_with("ldr"), "{listing:?}");
        assert_eq!(listing[0].constant, Some(0x1234_5678_9abc_def0));
        assert_eq!(listing.last().unwrap().text, ".quad 0x123456789abcdef0");

        let mut masm = super::super::x86_64::MacroAssembler::new(&mut code, &work);
        masm.move_imm64(Gpr(0), 0x1234_5678_9abc_def0);
        let data_offset = masm.finish().unwrap();
        let listing = listing_with_data(Architecture::X86_64, &code, data_offset);
        assert_eq!(listing[0].constant, Some(0x1234_5678_9abc_def0), "{listing:?}");
    }
}
