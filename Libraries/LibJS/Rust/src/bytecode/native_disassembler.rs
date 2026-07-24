/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/// One decoded native instruction. Offsets and lengths are relative to the
/// supplied code span, while `address` is its executable address.
pub struct DisassembledInstruction {
    pub offset: usize,
    pub address: u64,
    pub length: usize,
    pub text: String,
}

pub fn disassemble(code: &[u8], address: u64) -> Vec<DisassembledInstruction> {
    architecture::disassemble(code, address)
}

#[cfg(target_arch = "x86_64")]
mod architecture {
    use super::DisassembledInstruction;
    use iced_x86::{Decoder, DecoderOptions, Formatter, IntelFormatter};

    pub fn disassemble(code: &[u8], address: u64) -> Vec<DisassembledInstruction> {
        let mut decoder = Decoder::with_ip(64, code, address, DecoderOptions::NONE);
        let mut formatter = IntelFormatter::new();
        let mut instructions = Vec::new();

        while decoder.can_decode() {
            let instruction = decoder.decode();
            let mut text = String::new();
            formatter.format(&instruction, &mut text);
            instructions.push(DisassembledInstruction {
                offset: usize::try_from(instruction.ip() - address).expect("native instruction offset fits usize"),
                address: instruction.ip(),
                length: instruction.len(),
                text,
            });
        }

        instructions
    }
}

#[cfg(target_arch = "aarch64")]
mod architecture {
    use super::DisassembledInstruction;
    use farm64::format::{FmtFormatter, format_to_string};
    use farm64::{Decoder, DecoderOptions};

    pub fn disassemble(code: &[u8], address: u64) -> Vec<DisassembledInstruction> {
        let mut decoder = Decoder::new(code, address, DecoderOptions::default());
        let formatter = FmtFormatter::new();
        let mut instructions = Vec::new();

        while decoder.can_decode() {
            let offset = decoder.position();
            let instruction = decoder.decode();
            let instruction_address = address + offset as u64;
            let text = if instruction.is_invalid() {
                let opcode = u32::from_le_bytes(code[offset..offset + 4].try_into().unwrap());
                format!(".word 0x{opcode:08x}")
            } else {
                format_to_string(&formatter, &instruction)
            };
            instructions.push(DisassembledInstruction {
                offset,
                address: instruction_address,
                length: 4,
                text,
            });
        }

        instructions
    }
}

#[cfg(not(any(target_arch = "x86_64", target_arch = "aarch64")))]
mod architecture {
    use super::DisassembledInstruction;

    pub fn disassemble(_code: &[u8], _address: u64) -> Vec<DisassembledInstruction> {
        Vec::new()
    }
}

#[cfg(all(test, target_arch = "x86_64"))]
mod tests {
    use super::*;

    #[test]
    fn decodes_x86_64_code() {
        let instructions = disassemble(&[0x48, 0x89, 0xd8, 0xc3], 0x1000);

        assert_eq!(instructions.len(), 2);
        assert_eq!(instructions[0].offset, 0);
        assert_eq!(instructions[0].address, 0x1000);
        assert_eq!(instructions[0].length, 3);
        assert_eq!(instructions[0].text, "mov rax,rbx");
        assert_eq!(instructions[1].offset, 3);
        assert_eq!(instructions[1].address, 0x1003);
        assert_eq!(instructions[1].length, 1);
        assert_eq!(instructions[1].text, "ret");
    }
}

#[cfg(all(test, target_arch = "aarch64"))]
mod tests {
    use super::*;

    #[test]
    fn decodes_aarch64_code() {
        let instructions = disassemble(&[0x1f, 0x20, 0x03, 0xd5], 0x1000);

        assert_eq!(instructions.len(), 1);
        assert_eq!(instructions[0].offset, 0);
        assert_eq!(instructions[0].address, 0x1000);
        assert_eq!(instructions[0].length, 4);
        assert_eq!(instructions[0].text, "nop");
    }
}
