/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use bytecode_def::OpLayout;
use std::collections::HashMap;

#[derive(Debug, Clone)]
pub enum Operand {
    /// A register name (DSL or platform): t0, pc, values, etc.
    Register(String),
    /// An immediate integer value
    Immediate(i64),
    /// A named constant
    Constant(String),
    /// Memory access: [base, index, scale] or [base, offset] or [base]
    Memory {
        base: String,
        index: Option<String>,
        scale: Option<String>,
    },
    /// A label reference (.local or global)
    Label(String),
    /// A bytecode instruction field reference (e.g. m_dst, m_src).
    /// Resolved to the field's byte offset within the current handler's opcode.
    FieldRef(String),
}

#[derive(Debug, Clone)]
pub struct AsmInstruction {
    pub mnemonic: String,
    pub operands: Vec<Operand>,
}

#[derive(Debug, Clone)]
pub struct Handler {
    pub name: String,
    pub size: Option<u32>,
    pub instructions: Vec<AsmInstruction>,
}

#[derive(Debug, Clone)]
pub struct Macro {
    pub params: Vec<String>,
    pub body: Vec<AsmInstruction>,
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum ObjectFormat {
    MachO,
    Elf,
}

pub struct Program {
    pub constants: HashMap<String, i64>,
    pub macros: HashMap<String, Macro>,
    pub handlers: Vec<Handler>,
    /// Opcode layouts computed from Bytecode.def (field offsets and sizes).
    pub op_layouts: HashMap<String, OpLayout>,
    /// Ordered list of all opcode names from Bytecode.def.
    pub opcode_list: Vec<String>,
    /// Target object format.
    /// Affects backend details that differ between ELF and Mach-O, such as
    /// relocation syntax and which CFI directives assemblers accept.
    pub object_format: ObjectFormat,
    /// Whether the target has ARMv8.3 FEAT_JSCVT (fjcvtzs instruction).
    pub has_jscvt: bool,
}

pub fn parse(input: &str) -> Program {
    let mut constants = HashMap::new();
    let mut macros = HashMap::new();
    let mut handlers = Vec::new();

    let lines: Vec<&str> = input.lines().collect();
    let mut i = 0;

    while i < lines.len() {
        let line = lines[i].trim();

        // Skip empty lines and comments
        if line.is_empty() || line.starts_with('#') {
            i += 1;
            continue;
        }

        if let Some(rest) = line.strip_prefix("const ") {
            // const NAME = VALUE
            if let Some((name, value_str)) = rest.split_once('=') {
                let name = name.trim().to_string();
                let value_str = value_str.trim();
                let value = parse_integer(value_str);
                constants.insert(name, value);
            }
            i += 1;
        } else if let Some(rest) = line.strip_prefix("macro ") {
            // macro name(params)
            let (name, params) = parse_macro_signature(rest);
            i += 1;
            let mut body = Vec::new();
            while i < lines.len() {
                let l = lines[i].trim();
                if l == "end" {
                    break;
                }
                if !l.is_empty() && !l.starts_with('#') {
                    body.push(parse_asm_instruction(l));
                }
                i += 1;
            }
            macros.insert(name, Macro { params, body });
            i += 1;
        } else if let Some(rest) = line.strip_prefix("handler ") {
            // handler Name, size=N
            let (name, size) = parse_handler_header(rest);
            i += 1;
            let mut instructions = Vec::new();
            while i < lines.len() {
                let l = lines[i].trim();
                if l == "end" {
                    break;
                }
                if !l.is_empty() && !l.starts_with('#') {
                    instructions.push(parse_asm_instruction(l));
                }
                i += 1;
            }
            handlers.push(Handler {
                name,
                size,
                instructions,
            });
            i += 1;
        } else {
            i += 1;
        }
    }

    Program {
        constants,
        macros,
        handlers,
        op_layouts: HashMap::new(),
        opcode_list: Vec::new(),
        object_format: ObjectFormat::MachO,
        has_jscvt: false,
    }
}

fn parse_integer(s: &str) -> i64 {
    let s = s.trim();
    if let Some(hex) = s.strip_prefix("0x") {
        // Parse as u64 first to handle large hex constants, then reinterpret as i64
        u64::from_str_radix(hex, 16).unwrap() as i64
    } else if let Some(bin) = s.strip_prefix("0b") {
        u64::from_str_radix(bin, 2).unwrap() as i64
    } else {
        s.parse().unwrap()
    }
}

fn parse_macro_signature(s: &str) -> (String, Vec<String>) {
    let s = s.trim();
    if let Some(paren_start) = s.find('(') {
        let name = s[..paren_start].trim().to_string();
        let paren_end = s.find(')').unwrap_or(s.len());
        let params_str = &s[paren_start + 1..paren_end];
        let params: Vec<String> = if params_str.trim().is_empty() {
            vec![]
        } else {
            params_str
                .split(',')
                .map(|p| p.trim().to_string())
                .collect()
        };
        (name, params)
    } else {
        (s.to_string(), vec![])
    }
}

fn parse_handler_header(s: &str) -> (String, Option<u32>) {
    let parts: Vec<&str> = s.split(',').collect();
    let name = parts[0].trim().to_string();
    let mut size = None;
    for part in &parts[1..] {
        let part = part.trim();
        if let Some(val) = part.strip_prefix("size=") {
            size = Some(val.trim().parse().unwrap());
        }
    }
    (name, size)
}

fn parse_asm_instruction(line: &str) -> AsmInstruction {
    let line = line.trim();

    // Label definition (e.g. ".slow:")
    if let Some(label) = line.strip_suffix(':') {
        return AsmInstruction {
            mnemonic: "label".to_string(),
            operands: vec![Operand::Label(label.to_string())],
        };
    }

    // Split mnemonic and operands
    let (mnemonic, rest) = if let Some(space_idx) = line.find(|c: char| c.is_whitespace()) {
        (&line[..space_idx], line[space_idx..].trim())
    } else {
        (line, "")
    };

    let operands = if rest.is_empty() {
        vec![]
    } else {
        parse_operand_list(rest)
    };

    AsmInstruction {
        mnemonic: mnemonic.to_string(),
        operands,
    }
}

fn parse_operand_list(s: &str) -> Vec<Operand> {
    let mut operands = Vec::new();
    let mut depth = 0;
    let mut current = String::new();

    for ch in s.chars() {
        match ch {
            '[' => {
                depth += 1;
                current.push(ch);
            }
            ']' => {
                depth -= 1;
                current.push(ch);
            }
            ',' if depth == 0 => {
                operands.push(parse_single_operand(current.trim()));
                current.clear();
            }
            _ => current.push(ch),
        }
    }
    if !current.trim().is_empty() {
        operands.push(parse_single_operand(current.trim()));
    }
    operands
}

fn parse_single_operand(s: &str) -> Operand {
    let s = s.trim();

    // Memory operand: [base, index, scale] or [base, offset] or [base]
    if s.starts_with('[') && s.ends_with(']') {
        let inner = &s[1..s.len() - 1];
        let parts: Vec<&str> = inner.split(',').map(|p| p.trim()).collect();
        return Operand::Memory {
            base: parts[0].to_string(),
            index: parts.get(1).map(|s| s.to_string()),
            scale: parts.get(2).map(|s| s.to_string()),
        };
    }

    // Label reference (starts with .)
    if s.starts_with('.') {
        return Operand::Label(s.to_string());
    }

    // Try parsing as integer
    if s.starts_with("0x")
        || s.starts_with("0b")
        || s.starts_with('-')
        || s.chars().next().is_some_and(|c| c.is_ascii_digit())
    {
        let parsed = if let Some(hex) = s.strip_prefix("0x") {
            u64::from_str_radix(hex, 16).map(|v| v as i64)
        } else if let Some(bin) = s.strip_prefix("0b") {
            u64::from_str_radix(bin, 2).map(|v| v as i64)
        } else {
            s.parse()
        };
        if let Ok(val) = parsed {
            return Operand::Immediate(val);
        }
    }

    // Bytecode instruction field reference (e.g. m_dst, m_src)
    if s.starts_with("m_") {
        return Operand::FieldRef(s.to_string());
    }

    // Must be a register or constant name
    // If it contains only uppercase, underscores, and digits, treat as constant
    if s.chars()
        .all(|c| c.is_ascii_uppercase() || c == '_' || c.is_ascii_digit())
        && s.chars().next().is_some_and(|c| c.is_ascii_uppercase())
    {
        return Operand::Constant(s.to_string());
    }

    Operand::Register(s.to_string())
}
