/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// https://w3c.github.io/mathml-core/#layout-algorithms

use super::*;
use libgfx_rust::math_table::{FfiMathData, MathConstant, StretchAxis};

/// The layout algorithm a MathML element's tag name selects when its computed display is block math or inline math.
/// Mirrors the C++ `MathMLLayoutKind`, which the element's construction facts carry.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(u8)]
pub(crate) enum MathLayoutKind {
    #[default]
    NotMathML,
    Mrow,
    Math,
    Token,
    Mo,
    Mspace,
    Mfrac,
    Msqrt,
    Mroot,
    Msub,
    Msup,
    Msubsup,
    Munder,
    Mover,
    Munderover,
    Mmultiscripts,
    Mprescripts,
    None,
    Mpadded,
    Mtext,
}

impl MathLayoutKind {
    pub(crate) fn from_construction_facts(facts: u32) -> Self {
        use crate::css::style::bridge::element_construction_fact::{MATHML_LAYOUT_KIND_MASK, MATHML_LAYOUT_KIND_SHIFT};
        match (facts & MATHML_LAYOUT_KIND_MASK) >> MATHML_LAYOUT_KIND_SHIFT {
            1 => Self::Mrow,
            2 => Self::Math,
            3 => Self::Token,
            4 => Self::Mo,
            5 => Self::Mspace,
            6 => Self::Mfrac,
            7 => Self::Msqrt,
            8 => Self::Mroot,
            9 => Self::Msub,
            10 => Self::Msup,
            11 => Self::Msubsup,
            12 => Self::Munder,
            13 => Self::Mover,
            14 => Self::Munderover,
            15 => Self::Mmultiscripts,
            16 => Self::Mprescripts,
            17 => Self::None,
            18 => Self::Mpadded,
            19 => Self::Mtext,
            _ => Self::NotMathML,
        }
    }

    /// Whether boxes of this kind lay out their children with a MathML layout algorithm. Token elements lay out their
    /// text as a block box.
    pub(crate) fn uses_math_layout(self) -> bool {
        !matches!(self, Self::NotMathML | Self::Token | Self::Mo | Self::Mtext)
    }

    // https://w3c.github.io/mathml-core/#dfn-grouping-elements
    fn is_grouping_element(self) -> bool {
        matches!(self, Self::Mrow | Self::Math | Self::Mprescripts)
    }

    // https://w3c.github.io/mathml-core/#dfn-scripted-elements
    fn is_scripted_element(self) -> bool {
        matches!(
            self,
            Self::Mmultiscripts
                | Self::Mover
                | Self::Msub
                | Self::Msubsup
                | Self::Msup
                | Self::Munder
                | Self::Munderover
        )
    }
}

/// The unit of a MathML attribute value that is a `<length-percentage>`. Mirrors the C++ `MathLengthUnit`, where 0
/// stands for an attribute that is absent or invalid.
pub(crate) mod math_length_unit {
    pub(crate) const PX: u8 = 1;
    pub(crate) const EM: u8 = 2;
    pub(crate) const EX: u8 = 3;
    pub(crate) const PERCENTAGE: u8 = 4;
}

/// A MathML attribute value that is a `<length-percentage>`, as the host parsed it. Font-relative lengths are kept
/// in their unit, as they resolve against the font of the box that reads them.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
#[repr(C)]
pub struct FfiMathLength {
    pub unit: u8,
    pub value: f32,
}

impl FfiMathLength {
    fn is_percentage(self) -> bool {
        self.unit == math_length_unit::PERCENTAGE
    }

    /// Resolves the length against the font of `style`, and a percentage against `percentage_basis`.
    fn resolve(self, style: &StyleValues<'_>, percentage_basis: CssPixels) -> Option<CssPixels> {
        let value = f64::from(self.value);
        match self.unit {
            math_length_unit::PX => Some(CssPixels::nearest_value_for(value)),
            math_length_unit::EM => Some(style.font_size().scaled(value)),
            math_length_unit::EX => Some(CssPixels::nearest_value_for(f64::from(style.font_x_height()) * value)),
            math_length_unit::PERCENTAGE => Some(percentage_basis.scaled(value / 100.0)),
            _ => None,
        }
    }
}

/// The value of a MathML attribute that is a boolean. Mirrors the C++ `MathBoolean`, where 0 stands for an attribute
/// that is absent or invalid and 2 for false.
pub(crate) mod math_boolean {
    pub(crate) const TRUE: u8 = 1;
    pub(crate) const FALSE: u8 = 2;
}

/// The value of a boolean MathML attribute, or `default` if it is absent or invalid.
fn math_boolean_or(value: u8, default: bool) -> bool {
    match value {
        math_boolean::TRUE => true,
        math_boolean::FALSE => false,
        _ => default,
    }
}

/// The MathML attributes of an element that its layout reads, as the host parsed them. Every element publishes all
/// of them; the layout of its box reads those its tag name gives a meaning to.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
#[repr(C)]
pub struct FfiMathAttributeFacts {
    pub width: FfiMathLength,
    pub height: FfiMathLength,
    pub depth: FfiMathLength,
    pub lspace: FfiMathLength,
    pub rspace: FfiMathLength,
    pub voffset: FfiMathLength,
    pub linethickness: FfiMathLength,
    pub minsize: FfiMathLength,
    pub maxsize: FfiMathLength,
    /// The form attribute: 1 for infix, 2 for prefix and 3 for postfix, or 0 if it is absent or invalid.
    pub form: u8,
    pub stretchy: u8,
    pub symmetric: u8,
    pub largeop: u8,
    pub movablelimits: u8,
    pub accent: u8,
    pub accentunder: u8,
}

// https://w3c.github.io/mathml-core/#dfn-form
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
enum OperatorForm {
    Infix,
    Prefix,
    Postfix,
}

impl OperatorForm {
    fn from_attribute(value: u8) -> Option<Self> {
        match value {
            1 => Some(Self::Infix),
            2 => Some(Self::Prefix),
            3 => Some(Self::Postfix),
            _ => Option::None,
        }
    }
}

// https://w3c.github.io/mathml-core/#operator-dictionary-categories-values
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum OperatorCategory {
    Default,
    ForceDefault,
    A,
    B,
    C,
    D,
    E,
    F,
    G,
    H,
    I,
    J,
    K,
    L,
    M,
}

/// The boolean properties of an operator, which its category and its attributes set.
// https://w3c.github.io/mathml-core/#dictionary-based-attributes
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
struct OperatorFlags {
    stretchy: bool,
    symmetric: bool,
    largeop: bool,
    movablelimits: bool,
}

/// The properties of an embellished operator that its layout and the layout of its parent read.
#[derive(Clone, Copy)]
struct OperatorProperties {
    core_operator: Node,
    lspace: CssPixels,
    rspace: CssPixels,
    flags: OperatorFlags,
}

impl OperatorCategory {
    /// The properties the category lists.
    // https://w3c.github.io/mathml-core/#operator-dictionary-categories-values
    fn flags(self) -> OperatorFlags {
        match self {
            Self::A | Self::I => OperatorFlags {
                stretchy: true,
                ..OperatorFlags::default()
            },
            Self::F | Self::G => OperatorFlags {
                stretchy: true,
                symmetric: true,
                ..OperatorFlags::default()
            },
            Self::H => OperatorFlags {
                symmetric: true,
                largeop: true,
                ..OperatorFlags::default()
            },
            Self::J => OperatorFlags {
                symmetric: true,
                largeop: true,
                movablelimits: true,
                ..OperatorFlags::default()
            },
            _ => OperatorFlags::default(),
        }
    }

    /// The lspace and rspace of the category, in eighteenths of an em.
    fn spacing_in_eighteenths_of_em(self) -> (u8, u8) {
        match self {
            Self::Default | Self::ForceDefault | Self::A => (5, 5),
            Self::B => (4, 4),
            Self::C | Self::H | Self::J => (3, 3),
            Self::D | Self::E | Self::F | Self::G | Self::I | Self::K => (0, 0),
            Self::L => (3, 0),
            Self::M => (0, 3),
        }
    }
}

// https://w3c.github.io/mathml-core/#operator-dictionary-compact-special-tables
// Operators_2_ascii_chars
const OPERATORS_2_ASCII_CHARS: [&[u8; 2]; 18] = [
    b"!!", b"!=", b"&&", b"**", b"*=", b"++", b"+=", b"--", b"-=", b"->", b"//", b"/=", b":=", b"<=", b"<>", b"==",
    b">=", b"||",
];

// https://w3c.github.io/mathml-core/#operator-dictionary-compact-dictionary
// The (Content, Form) keys of each category, as ranges of characters sorted by form and range start.
static OPERATOR_CATEGORIES: [(OperatorForm, u32, u32, OperatorCategory); 234] = [
    (OperatorForm::Infix, 0x0025, 0x0025, OperatorCategory::C),
    (OperatorForm::Infix, 0x002A, 0x002A, OperatorCategory::C),
    (OperatorForm::Infix, 0x002B, 0x002B, OperatorCategory::B),
    (OperatorForm::Infix, 0x002C, 0x002C, OperatorCategory::M),
    (OperatorForm::Infix, 0x002D, 0x002D, OperatorCategory::B),
    (OperatorForm::Infix, 0x002E, 0x002E, OperatorCategory::C),
    (OperatorForm::Infix, 0x002F, 0x002F, OperatorCategory::K),
    (OperatorForm::Infix, 0x003A, 0x003A, OperatorCategory::M),
    (OperatorForm::Infix, 0x003B, 0x003B, OperatorCategory::M),
    (OperatorForm::Infix, 0x003F, 0x0040, OperatorCategory::C),
    (OperatorForm::Infix, 0x005C, 0x005C, OperatorCategory::K),
    (OperatorForm::Infix, 0x005E, 0x005E, OperatorCategory::C),
    (OperatorForm::Infix, 0x005F, 0x005F, OperatorCategory::K),
    (OperatorForm::Infix, 0x00B1, 0x00B1, OperatorCategory::B),
    (OperatorForm::Infix, 0x00B7, 0x00B7, OperatorCategory::C),
    (OperatorForm::Infix, 0x00D7, 0x00D7, OperatorCategory::C),
    (OperatorForm::Infix, 0x00F7, 0x00F7, OperatorCategory::B),
    (OperatorForm::Infix, 0x0322, 0x0322, OperatorCategory::B),
    (OperatorForm::Infix, 0x0323, 0x0323, OperatorCategory::C),
    (OperatorForm::Infix, 0x032E, 0x032E, OperatorCategory::C),
    (OperatorForm::Infix, 0x2022, 0x2022, OperatorCategory::C),
    (OperatorForm::Infix, 0x2043, 0x2043, OperatorCategory::C),
    (OperatorForm::Infix, 0x2044, 0x2044, OperatorCategory::B),
    (OperatorForm::Infix, 0x2061, 0x2064, OperatorCategory::K),
    (OperatorForm::Infix, 0x2190, 0x2195, OperatorCategory::A),
    (OperatorForm::Infix, 0x219A, 0x21AE, OperatorCategory::A),
    (OperatorForm::Infix, 0x21B0, 0x21B5, OperatorCategory::A),
    (OperatorForm::Infix, 0x21B9, 0x21B9, OperatorCategory::A),
    (OperatorForm::Infix, 0x21BC, 0x21D5, OperatorCategory::A),
    (OperatorForm::Infix, 0x21DA, 0x21F0, OperatorCategory::A),
    (OperatorForm::Infix, 0x21F3, 0x21FF, OperatorCategory::A),
    (OperatorForm::Infix, 0x2206, 0x2206, OperatorCategory::K),
    (OperatorForm::Infix, 0x2212, 0x2216, OperatorCategory::B),
    (OperatorForm::Infix, 0x2217, 0x2219, OperatorCategory::C),
    (OperatorForm::Infix, 0x2227, 0x222A, OperatorCategory::B),
    (OperatorForm::Infix, 0x2236, 0x2236, OperatorCategory::B),
    (OperatorForm::Infix, 0x2238, 0x2238, OperatorCategory::B),
    (OperatorForm::Infix, 0x2240, 0x2240, OperatorCategory::C),
    (OperatorForm::Infix, 0x228C, 0x228E, OperatorCategory::B),
    (OperatorForm::Infix, 0x2293, 0x2296, OperatorCategory::B),
    (OperatorForm::Infix, 0x2297, 0x2297, OperatorCategory::C),
    (OperatorForm::Infix, 0x2298, 0x2298, OperatorCategory::B),
    (OperatorForm::Infix, 0x2299, 0x229B, OperatorCategory::C),
    (OperatorForm::Infix, 0x229D, 0x229F, OperatorCategory::B),
    (OperatorForm::Infix, 0x22A0, 0x22A1, OperatorCategory::C),
    (OperatorForm::Infix, 0x22BA, 0x22BA, OperatorCategory::C),
    (OperatorForm::Infix, 0x22BB, 0x22BD, OperatorCategory::B),
    (OperatorForm::Infix, 0x22C4, 0x22C7, OperatorCategory::C),
    (OperatorForm::Infix, 0x22C9, 0x22CC, OperatorCategory::C),
    (OperatorForm::Infix, 0x22CE, 0x22CF, OperatorCategory::B),
    (OperatorForm::Infix, 0x22D2, 0x22D3, OperatorCategory::B),
    (OperatorForm::Infix, 0x2305, 0x2306, OperatorCategory::C),
    (OperatorForm::Infix, 0x2794, 0x2794, OperatorCategory::A),
    (OperatorForm::Infix, 0x2795, 0x2797, OperatorCategory::B),
    (OperatorForm::Infix, 0x2799, 0x2799, OperatorCategory::A),
    (OperatorForm::Infix, 0x279B, 0x27A1, OperatorCategory::A),
    (OperatorForm::Infix, 0x27A5, 0x27A6, OperatorCategory::A),
    (OperatorForm::Infix, 0x27A8, 0x27AF, OperatorCategory::A),
    (OperatorForm::Infix, 0x27B1, 0x27B1, OperatorCategory::A),
    (OperatorForm::Infix, 0x27B3, 0x27B3, OperatorCategory::A),
    (OperatorForm::Infix, 0x27B5, 0x27B5, OperatorCategory::A),
    (OperatorForm::Infix, 0x27B8, 0x27B8, OperatorCategory::A),
    (OperatorForm::Infix, 0x27BA, 0x27BE, OperatorCategory::A),
    (OperatorForm::Infix, 0x27CB, 0x27CB, OperatorCategory::C),
    (OperatorForm::Infix, 0x27CD, 0x27CD, OperatorCategory::C),
    (OperatorForm::Infix, 0x27F0, 0x27F1, OperatorCategory::A),
    (OperatorForm::Infix, 0x27F4, 0x27FF, OperatorCategory::A),
    (OperatorForm::Infix, 0x2900, 0x2920, OperatorCategory::A),
    (OperatorForm::Infix, 0x2934, 0x2937, OperatorCategory::A),
    (OperatorForm::Infix, 0x2942, 0x2975, OperatorCategory::A),
    (OperatorForm::Infix, 0x297C, 0x297F, OperatorCategory::A),
    (OperatorForm::Infix, 0x29B8, 0x29B8, OperatorCategory::B),
    (OperatorForm::Infix, 0x29BC, 0x29BC, OperatorCategory::B),
    (OperatorForm::Infix, 0x29C4, 0x29C5, OperatorCategory::B),
    (OperatorForm::Infix, 0x29C6, 0x29C8, OperatorCategory::C),
    (OperatorForm::Infix, 0x29D4, 0x29D7, OperatorCategory::C),
    (OperatorForm::Infix, 0x29E2, 0x29E2, OperatorCategory::C),
    (OperatorForm::Infix, 0x29F5, 0x29FB, OperatorCategory::B),
    (OperatorForm::Infix, 0x2A1D, 0x2A1E, OperatorCategory::C),
    (OperatorForm::Infix, 0x2A1F, 0x2A2E, OperatorCategory::B),
    (OperatorForm::Infix, 0x2A2F, 0x2A37, OperatorCategory::C),
    (OperatorForm::Infix, 0x2A38, 0x2A3A, OperatorCategory::B),
    (OperatorForm::Infix, 0x2A3B, 0x2A3D, OperatorCategory::C),
    (OperatorForm::Infix, 0x2A3E, 0x2A3E, OperatorCategory::B),
    (OperatorForm::Infix, 0x2A3F, 0x2A3F, OperatorCategory::C),
    (OperatorForm::Infix, 0x2A40, 0x2A4F, OperatorCategory::B),
    (OperatorForm::Infix, 0x2A50, 0x2A50, OperatorCategory::C),
    (OperatorForm::Infix, 0x2A51, 0x2A63, OperatorCategory::B),
    (OperatorForm::Infix, 0x2A64, 0x2A65, OperatorCategory::C),
    (OperatorForm::Infix, 0x2ADB, 0x2ADB, OperatorCategory::B),
    (OperatorForm::Infix, 0x2ADC, 0x2ADD, OperatorCategory::C),
    (OperatorForm::Infix, 0x2AF6, 0x2AF6, OperatorCategory::B),
    (OperatorForm::Infix, 0x2AFB, 0x2AFB, OperatorCategory::B),
    (OperatorForm::Infix, 0x2AFD, 0x2AFD, OperatorCategory::B),
    (OperatorForm::Infix, 0x2AFE, 0x2AFE, OperatorCategory::C),
    (OperatorForm::Infix, 0x2B04, 0x2B07, OperatorCategory::A),
    (OperatorForm::Infix, 0x2B0C, 0x2B11, OperatorCategory::A),
    (OperatorForm::Infix, 0x2B30, 0x2B3E, OperatorCategory::A),
    (OperatorForm::Infix, 0x2B40, 0x2B4C, OperatorCategory::A),
    (OperatorForm::Infix, 0x2B60, 0x2B65, OperatorCategory::A),
    (OperatorForm::Infix, 0x2B6A, 0x2B6D, OperatorCategory::A),
    (OperatorForm::Infix, 0x2B70, 0x2B73, OperatorCategory::A),
    (OperatorForm::Infix, 0x2B7A, 0x2B7D, OperatorCategory::A),
    (OperatorForm::Infix, 0x2B80, 0x2B87, OperatorCategory::A),
    (OperatorForm::Infix, 0x2B95, 0x2B95, OperatorCategory::A),
    (OperatorForm::Infix, 0x2BA0, 0x2BAF, OperatorCategory::A),
    (OperatorForm::Infix, 0x2BB8, 0x2BB8, OperatorCategory::A),
    (OperatorForm::Prefix, 0x0021, 0x0021, OperatorCategory::D),
    (OperatorForm::Prefix, 0x0028, 0x0028, OperatorCategory::F),
    (OperatorForm::Prefix, 0x002B, 0x002B, OperatorCategory::D),
    (OperatorForm::Prefix, 0x002D, 0x002D, OperatorCategory::D),
    (OperatorForm::Prefix, 0x005B, 0x005B, OperatorCategory::F),
    (OperatorForm::Prefix, 0x007B, 0x007B, OperatorCategory::F),
    (OperatorForm::Prefix, 0x007C, 0x007C, OperatorCategory::F),
    (OperatorForm::Prefix, 0x00AC, 0x00AC, OperatorCategory::D),
    (OperatorForm::Prefix, 0x00B1, 0x00B1, OperatorCategory::D),
    (OperatorForm::Prefix, 0x0331, 0x0331, OperatorCategory::D),
    (OperatorForm::Prefix, 0x2016, 0x2016, OperatorCategory::F),
    (OperatorForm::Prefix, 0x2018, 0x2018, OperatorCategory::D),
    (OperatorForm::Prefix, 0x201C, 0x201C, OperatorCategory::D),
    (OperatorForm::Prefix, 0x2145, 0x2146, OperatorCategory::L),
    (OperatorForm::Prefix, 0x2200, 0x2201, OperatorCategory::D),
    (OperatorForm::Prefix, 0x2202, 0x2202, OperatorCategory::L),
    (OperatorForm::Prefix, 0x2203, 0x2204, OperatorCategory::D),
    (OperatorForm::Prefix, 0x2207, 0x2207, OperatorCategory::D),
    (OperatorForm::Prefix, 0x220F, 0x2211, OperatorCategory::J),
    (OperatorForm::Prefix, 0x2212, 0x2213, OperatorCategory::D),
    (OperatorForm::Prefix, 0x221A, 0x221C, OperatorCategory::L),
    (OperatorForm::Prefix, 0x221F, 0x2222, OperatorCategory::D),
    (OperatorForm::Prefix, 0x222B, 0x2233, OperatorCategory::H),
    (OperatorForm::Prefix, 0x2234, 0x2235, OperatorCategory::D),
    (OperatorForm::Prefix, 0x223C, 0x223C, OperatorCategory::D),
    (OperatorForm::Prefix, 0x22BE, 0x22BF, OperatorCategory::D),
    (OperatorForm::Prefix, 0x22C0, 0x22C3, OperatorCategory::J),
    (OperatorForm::Prefix, 0x2308, 0x2308, OperatorCategory::F),
    (OperatorForm::Prefix, 0x230A, 0x230A, OperatorCategory::F),
    (OperatorForm::Prefix, 0x2310, 0x2310, OperatorCategory::D),
    (OperatorForm::Prefix, 0x2319, 0x2319, OperatorCategory::D),
    (OperatorForm::Prefix, 0x2329, 0x2329, OperatorCategory::F),
    (OperatorForm::Prefix, 0x2772, 0x2772, OperatorCategory::F),
    (OperatorForm::Prefix, 0x2795, 0x2796, OperatorCategory::D),
    (OperatorForm::Prefix, 0x27C0, 0x27C0, OperatorCategory::D),
    (OperatorForm::Prefix, 0x27E6, 0x27E6, OperatorCategory::F),
    (OperatorForm::Prefix, 0x27E8, 0x27E8, OperatorCategory::F),
    (OperatorForm::Prefix, 0x27EA, 0x27EA, OperatorCategory::F),
    (OperatorForm::Prefix, 0x27EC, 0x27EC, OperatorCategory::F),
    (OperatorForm::Prefix, 0x27EE, 0x27EE, OperatorCategory::F),
    (OperatorForm::Prefix, 0x2980, 0x2980, OperatorCategory::F),
    (OperatorForm::Prefix, 0x2983, 0x2983, OperatorCategory::F),
    (OperatorForm::Prefix, 0x2985, 0x2985, OperatorCategory::F),
    (OperatorForm::Prefix, 0x2987, 0x2987, OperatorCategory::F),
    (OperatorForm::Prefix, 0x2989, 0x2989, OperatorCategory::F),
    (OperatorForm::Prefix, 0x298B, 0x298B, OperatorCategory::F),
    (OperatorForm::Prefix, 0x298D, 0x298D, OperatorCategory::F),
    (OperatorForm::Prefix, 0x298F, 0x298F, OperatorCategory::F),
    (OperatorForm::Prefix, 0x2991, 0x2991, OperatorCategory::F),
    (OperatorForm::Prefix, 0x2993, 0x2993, OperatorCategory::F),
    (OperatorForm::Prefix, 0x2995, 0x2995, OperatorCategory::F),
    (OperatorForm::Prefix, 0x2997, 0x2997, OperatorCategory::F),
    (OperatorForm::Prefix, 0x2999, 0x2999, OperatorCategory::F),
    (OperatorForm::Prefix, 0x299B, 0x29AF, OperatorCategory::D),
    (OperatorForm::Prefix, 0x29D8, 0x29D8, OperatorCategory::F),
    (OperatorForm::Prefix, 0x29DA, 0x29DA, OperatorCategory::F),
    (OperatorForm::Prefix, 0x29FC, 0x29FC, OperatorCategory::F),
    (OperatorForm::Prefix, 0x2A00, 0x2A0A, OperatorCategory::J),
    (OperatorForm::Prefix, 0x2A0B, 0x2A1C, OperatorCategory::H),
    (OperatorForm::Prefix, 0x2A1D, 0x2A1E, OperatorCategory::J),
    (OperatorForm::Prefix, 0x2AEC, 0x2AED, OperatorCategory::D),
    (OperatorForm::Prefix, 0x2AFC, 0x2AFC, OperatorCategory::J),
    (OperatorForm::Prefix, 0x2AFF, 0x2AFF, OperatorCategory::J),
    (OperatorForm::Postfix, 0x0021, 0x0022, OperatorCategory::E),
    (OperatorForm::Postfix, 0x0025, 0x0027, OperatorCategory::E),
    (OperatorForm::Postfix, 0x0029, 0x0029, OperatorCategory::G),
    (OperatorForm::Postfix, 0x005D, 0x005D, OperatorCategory::G),
    (OperatorForm::Postfix, 0x005E, 0x005F, OperatorCategory::I),
    (OperatorForm::Postfix, 0x0060, 0x0060, OperatorCategory::E),
    (OperatorForm::Postfix, 0x007C, 0x007C, OperatorCategory::G),
    (OperatorForm::Postfix, 0x007D, 0x007D, OperatorCategory::G),
    (OperatorForm::Postfix, 0x007E, 0x007E, OperatorCategory::I),
    (OperatorForm::Postfix, 0x00A8, 0x00A8, OperatorCategory::E),
    (OperatorForm::Postfix, 0x00AF, 0x00AF, OperatorCategory::I),
    (OperatorForm::Postfix, 0x00B0, 0x00B0, OperatorCategory::E),
    (OperatorForm::Postfix, 0x00B2, 0x00B4, OperatorCategory::E),
    (OperatorForm::Postfix, 0x00B8, 0x00B9, OperatorCategory::E),
    (OperatorForm::Postfix, 0x02C6, 0x02C7, OperatorCategory::I),
    (OperatorForm::Postfix, 0x02C9, 0x02C9, OperatorCategory::I),
    (OperatorForm::Postfix, 0x02CA, 0x02CB, OperatorCategory::E),
    (OperatorForm::Postfix, 0x02CD, 0x02CD, OperatorCategory::I),
    (OperatorForm::Postfix, 0x02D8, 0x02DA, OperatorCategory::E),
    (OperatorForm::Postfix, 0x02DC, 0x02DC, OperatorCategory::I),
    (OperatorForm::Postfix, 0x02DD, 0x02DD, OperatorCategory::E),
    (OperatorForm::Postfix, 0x02F7, 0x02F7, OperatorCategory::I),
    (OperatorForm::Postfix, 0x0302, 0x0302, OperatorCategory::I),
    (OperatorForm::Postfix, 0x0311, 0x0311, OperatorCategory::E),
    (OperatorForm::Postfix, 0x0320, 0x0320, OperatorCategory::E),
    (OperatorForm::Postfix, 0x0325, 0x0325, OperatorCategory::E),
    (OperatorForm::Postfix, 0x0327, 0x0327, OperatorCategory::E),
    (OperatorForm::Postfix, 0x0331, 0x0331, OperatorCategory::E),
    (OperatorForm::Postfix, 0x2016, 0x2016, OperatorCategory::G),
    (OperatorForm::Postfix, 0x2019, 0x201B, OperatorCategory::E),
    (OperatorForm::Postfix, 0x201D, 0x201F, OperatorCategory::E),
    (OperatorForm::Postfix, 0x2032, 0x2037, OperatorCategory::E),
    (OperatorForm::Postfix, 0x203E, 0x203E, OperatorCategory::I),
    (OperatorForm::Postfix, 0x2057, 0x2057, OperatorCategory::E),
    (OperatorForm::Postfix, 0x20DB, 0x20DC, OperatorCategory::E),
    (OperatorForm::Postfix, 0x2309, 0x2309, OperatorCategory::G),
    (OperatorForm::Postfix, 0x230B, 0x230B, OperatorCategory::G),
    (OperatorForm::Postfix, 0x2322, 0x2323, OperatorCategory::I),
    (OperatorForm::Postfix, 0x232A, 0x232A, OperatorCategory::G),
    (OperatorForm::Postfix, 0x23B4, 0x23B5, OperatorCategory::I),
    (OperatorForm::Postfix, 0x23CD, 0x23CD, OperatorCategory::E),
    (OperatorForm::Postfix, 0x23DC, 0x23E1, OperatorCategory::I),
    (OperatorForm::Postfix, 0x2773, 0x2773, OperatorCategory::G),
    (OperatorForm::Postfix, 0x27E7, 0x27E7, OperatorCategory::G),
    (OperatorForm::Postfix, 0x27E9, 0x27E9, OperatorCategory::G),
    (OperatorForm::Postfix, 0x27EB, 0x27EB, OperatorCategory::G),
    (OperatorForm::Postfix, 0x27ED, 0x27ED, OperatorCategory::G),
    (OperatorForm::Postfix, 0x27EF, 0x27EF, OperatorCategory::G),
    (OperatorForm::Postfix, 0x2980, 0x2980, OperatorCategory::G),
    (OperatorForm::Postfix, 0x2984, 0x2984, OperatorCategory::G),
    (OperatorForm::Postfix, 0x2986, 0x2986, OperatorCategory::G),
    (OperatorForm::Postfix, 0x2988, 0x2988, OperatorCategory::G),
    (OperatorForm::Postfix, 0x298A, 0x298A, OperatorCategory::G),
    (OperatorForm::Postfix, 0x298C, 0x298C, OperatorCategory::G),
    (OperatorForm::Postfix, 0x298E, 0x298E, OperatorCategory::G),
    (OperatorForm::Postfix, 0x2990, 0x2990, OperatorCategory::G),
    (OperatorForm::Postfix, 0x2992, 0x2992, OperatorCategory::G),
    (OperatorForm::Postfix, 0x2994, 0x2994, OperatorCategory::G),
    (OperatorForm::Postfix, 0x2996, 0x2996, OperatorCategory::G),
    (OperatorForm::Postfix, 0x2998, 0x2998, OperatorCategory::G),
    (OperatorForm::Postfix, 0x2999, 0x2999, OperatorCategory::G),
    (OperatorForm::Postfix, 0x29D9, 0x29D9, OperatorCategory::G),
    (OperatorForm::Postfix, 0x29DB, 0x29DB, OperatorCategory::G),
    (OperatorForm::Postfix, 0x29FD, 0x29FD, OperatorCategory::G),
];

// https://w3c.github.io/mathml-core/#dfn-algorithm-to-determine-the-category-of-an-operator
fn operator_category(content: &[u16], form: OperatorForm) -> OperatorCategory {
    // 1. If Content as an UTF-16 string does not have length or 1 or 2 then exit with category Default.
    if content.is_empty() || content.len() > 2 {
        return OperatorCategory::Default;
    }
    let mut character = u32::from(content[0]);
    // 2. If Content is a single character in the range U+0320–U+03FF then exit with category Default. Otherwise, if
    //    it has two characters:
    if content.len() == 1 {
        if (0x0320..=0x03FF).contains(&character) {
            return OperatorCategory::Default;
        }
    } else {
        let second = content[1];
        // - If Content is the surrogate pairs corresponding to U+1EEF0 ARABIC MATHEMATICAL OPERATOR MEEM WITH HAH
        //   WITH TATWEEL or U+1EEF1 ARABIC MATHEMATICAL OPERATOR HAH WITH DAL and Form is postfix, exit with category
        //   I.
        if form == OperatorForm::Postfix && content[0] == 0xD83B && (second == 0xDEF0 || second == 0xDEF1) {
            return OperatorCategory::I;
        }
        // - If the second character is U+0338 COMBINING LONG SOLIDUS OVERLAY or U+20D2 COMBINING LONG VERTICAL LINE
        //   OVERLAY then replace Content with the first character and move to step 3.
        if second == 0x0338 || second == 0x20D2 {
        }
        // - Otherwise, if Content is listed in Operators_2_ascii_chars then replace Content with the Unicode character
        //   "U+0320 plus the index of Content in Operators_2_ascii_chars" and move to step 3.
        else if let Some(index) = OPERATORS_2_ASCII_CHARS
            .iter()
            .position(|pair| content[0] == u16::from(pair[0]) && second == u16::from(pair[1]))
        {
            character = 0x0320 + index as u32;
        }
        // - Otherwise exit with category Default.
        else {
            return OperatorCategory::Default;
        }
    }
    // 3. If Form is infix and Content corresponds to one of U+007C VERTICAL LINE or U+223C TILDE OPERATOR then exit
    //    with category ForceDefault.
    if form == OperatorForm::Infix && (character == 0x007C || character == 0x223C) {
        return OperatorCategory::ForceDefault;
    }
    // NB: The remaining steps search the compact encoding of the dictionary. Content outside the ranges
    //     U+0000–U+03FF and U+2000–U+2BFF has category Default, and so does any key the dictionary does not list.
    if !(character <= 0x03FF || (0x2000..=0x2BFF).contains(&character)) {
        return OperatorCategory::Default;
    }
    let index =
        OPERATOR_CATEGORIES.partition_point(|&(entry_form, start, _, _)| (entry_form, start) <= (form, character));
    match index.checked_sub(1).map(|index| OPERATOR_CATEGORIES[index]) {
        Some((entry_form, start, end, category)) if entry_form == form && (start..=end).contains(&character) => {
            category
        }
        _ => OperatorCategory::Default,
    }
}

// https://w3c.github.io/mathml-core/#dfn-intrinsic-stretch-axis
// The code points corresponding to operators with inline stretch axis.
static INLINE_STRETCH_AXIS_CODE_POINTS: [u32; 246] = [
    0x003D, 0x005E, 0x005F, 0x007E, 0x00AF, 0x02C6, 0x02C7, 0x02C9, 0x02CD, 0x02DC, 0x02F7, 0x0302, 0x0332, 0x203E,
    0x20D0, 0x20D1, 0x20D6, 0x20D7, 0x20E1, 0x2190, 0x2192, 0x2194, 0x2198, 0x2199, 0x219A, 0x219B, 0x219C, 0x219D,
    0x219E, 0x21A0, 0x21A2, 0x21A3, 0x21A4, 0x21A6, 0x21A9, 0x21AA, 0x21AB, 0x21AC, 0x21AD, 0x21AE, 0x21B4, 0x21B9,
    0x21BC, 0x21BD, 0x21C0, 0x21C1, 0x21C4, 0x21C6, 0x21C7, 0x21C9, 0x21CB, 0x21CC, 0x21CD, 0x21CE, 0x21CF, 0x21D0,
    0x21D2, 0x21D4, 0x21DA, 0x21DB, 0x21DC, 0x21DD, 0x21E0, 0x21E2, 0x21E4, 0x21E5, 0x21E6, 0x21E8, 0x21F0, 0x21F4,
    0x21F6, 0x21F7, 0x21F8, 0x21F9, 0x21FA, 0x21FB, 0x21FC, 0x21FD, 0x21FE, 0x21FF, 0x2322, 0x2323, 0x23B4, 0x23B5,
    0x23DC, 0x23DD, 0x23DE, 0x23DF, 0x23E0, 0x23E1, 0x2500, 0x2794, 0x2799, 0x279B, 0x279C, 0x279D, 0x279E, 0x279F,
    0x27A0, 0x27A1, 0x27A5, 0x27A6, 0x27A8, 0x27A9, 0x27AA, 0x27AB, 0x27AC, 0x27AD, 0x27AE, 0x27AF, 0x27B1, 0x27B3,
    0x27B5, 0x27B8, 0x27BA, 0x27BB, 0x27BC, 0x27BD, 0x27BE, 0x27F4, 0x27F5, 0x27F6, 0x27F7, 0x27F8, 0x27F9, 0x27FA,
    0x27FB, 0x27FC, 0x27FD, 0x27FE, 0x27FF, 0x2900, 0x2901, 0x2902, 0x2903, 0x2904, 0x2905, 0x2906, 0x2907, 0x290C,
    0x290D, 0x290E, 0x290F, 0x2910, 0x2911, 0x2914, 0x2915, 0x2916, 0x2917, 0x2918, 0x2919, 0x291A, 0x291B, 0x291C,
    0x291D, 0x291E, 0x291F, 0x2920, 0x2942, 0x2943, 0x2944, 0x2945, 0x2946, 0x2947, 0x2948, 0x294A, 0x294B, 0x294E,
    0x2950, 0x2952, 0x2953, 0x2956, 0x2957, 0x295A, 0x295B, 0x295E, 0x295F, 0x2962, 0x2964, 0x2966, 0x2967, 0x2968,
    0x2969, 0x296A, 0x296B, 0x296C, 0x296D, 0x2970, 0x2971, 0x2972, 0x2973, 0x2974, 0x2975, 0x297C, 0x297D, 0x2B04,
    0x2B05, 0x2B0C, 0x2B30, 0x2B31, 0x2B32, 0x2B33, 0x2B34, 0x2B35, 0x2B36, 0x2B37, 0x2B38, 0x2B39, 0x2B3A, 0x2B3B,
    0x2B3C, 0x2B3D, 0x2B3E, 0x2B40, 0x2B41, 0x2B42, 0x2B43, 0x2B44, 0x2B45, 0x2B46, 0x2B47, 0x2B48, 0x2B49, 0x2B4A,
    0x2B4B, 0x2B4C, 0x2B60, 0x2B62, 0x2B64, 0x2B6A, 0x2B6C, 0x2B70, 0x2B72, 0x2B7A, 0x2B7C, 0x2B80, 0x2B82, 0x2B84,
    0x2B86, 0x2B95, 0xFE35, 0xFE36, 0xFE37, 0xFE38, 0x1EEF0, 0x1EEF1,
];

/// The intrinsic stretch axis of a Unicode character.
// https://w3c.github.io/mathml-core/#dfn-intrinsic-stretch-axis
fn intrinsic_stretch_axis(code_point: u32) -> StretchAxis {
    // The intrinsic stretch axis a Unicode character c is inline if it belongs to the list below. Otherwise, the
    // intrinsic stretch axis of c is block.
    if INLINE_STRETCH_AXIS_CODE_POINTS.binary_search(&code_point).is_ok() {
        StretchAxis::Inline
    } else {
        StretchAxis::Block
    }
}

/// An in-flow child box after its layout. The ascent and descent are measured from the alphabetic baseline to the
/// line-over and line-under edges of the child's margin box.
#[derive(Clone, Copy)]
struct Child {
    node: Node,
    inline_size: CssPixels,
    ascent: CssPixels,
    descent: CssPixels,
}

/// Where a child's margin box goes: its line-left offset, and how far its alphabetic baseline is shifted from the
/// alphabetic baseline of the math content towards the line-under.
#[derive(Clone, Copy, Default)]
struct ChildPosition {
    line_left_offset: CssPixels,
    baseline_shift: CssPixels,
}

/// The metrics of a math content box and the positions of its in-flow children.
#[derive(Default)]
struct MathContent {
    inline_size: CssPixels,
    ascent: CssPixels,
    descent: CssPixels,
    positions: Vec<ChildPosition>,
}

/// The layout constants of the first available font of a box.
// https://w3c.github.io/mathml-core/#layout-constants-mathconstants
struct MathConstants {
    data: FfiMathData,
    default_rule_thickness: CssPixels,
    x_height: CssPixels,
    em: CssPixels,
}

impl MathConstants {
    fn for_style(style: &StyleValues<'_>) -> Self {
        let data = *style.first_available_font().math_data();
        let em = style.font_size();
        // Default rule thickness: post.underlineThickness or Default fallback constant if the constant is not
        // available.
        // AD-HOC: Use 0.05em, a typical underline thickness, rather than the Default fallback constant of 0, which
        //         would make every gap based on the rule thickness disappear.
        let default_rule_thickness = data
            .underline_thickness()
            .map_or_else(|| em.scaled(0.05), CssPixels::nearest_value_for_f32);
        Self {
            data,
            default_rule_thickness,
            x_height: CssPixels::nearest_value_for_f32(style.font_x_height()),
            em,
        }
    }

    /// MATH.MathConstants.`constant`, or `fallback` if the constant is not available.
    fn constant_or(&self, constant: MathConstant, fallback: CssPixels) -> CssPixels {
        self.data
            .constant(constant)
            .map_or(fallback, CssPixels::nearest_value_for_f32)
    }

    /// MATH.MathConstants.`constant`, or the Default fallback constant of 0 if the constant is not available.
    fn constant_or_zero(&self, constant: MathConstant) -> CssPixels {
        self.constant_or(constant, CssPixels::default())
    }

    // axisHeight: MATH.MathConstants.axisHeight or half OS/2.sxHeight if the constant is not available.
    fn axis_height(&self) -> CssPixels {
        self.constant_or(MathConstant::AxisHeight, self.x_height / 2)
    }

    // accentBaseHeight: MATH.MathConstants.accentBaseHeight or OS/2.sxHeight if the constant is not available.
    fn accent_base_height(&self) -> CssPixels {
        self.constant_or(MathConstant::AccentBaseHeight, self.x_height)
    }

    // subscriptShiftDown: MATH.MathConstants.subscriptShiftDown or OS/2.ySubscriptYOffset if the constant is not
    // available.
    // FIXME: Fall back to OS/2.ySubscriptYOffset rather than 0.
    fn subscript_shift_down(&self) -> CssPixels {
        self.constant_or_zero(MathConstant::SubscriptShiftDown)
    }

    // subscriptTopMax: MATH.MathConstants.subscriptTopMax or ⅘ × OS/2.sxHeight if the constant is not available.
    fn subscript_top_max(&self) -> CssPixels {
        self.constant_or(MathConstant::SubscriptTopMax, self.x_height.scaled(0.8))
    }

    // subscriptBaselineDropMin: MATH.MathConstants.subscriptBaselineDropMin or Default fallback constant if the
    // constant is not available.
    fn subscript_baseline_drop_min(&self) -> CssPixels {
        self.constant_or_zero(MathConstant::SubscriptBaselineDropMin)
    }

    // superscriptShiftUp: MATH.MathConstants.superscriptShiftUp or OS/2.ySuperscriptYOffset if the constant is not
    // available.
    // FIXME: Fall back to OS/2.ySuperscriptYOffset rather than 0.
    fn superscript_shift_up(&self) -> CssPixels {
        self.constant_or_zero(MathConstant::SuperscriptShiftUp)
    }

    // superscriptShiftUpCramped: MATH.MathConstants.superscriptShiftUpCramped or Default fallback constant if the
    // constant is not available.
    fn superscript_shift_up_cramped(&self) -> CssPixels {
        self.constant_or_zero(MathConstant::SuperscriptShiftUpCramped)
    }

    // superscriptBottomMin: MATH.MathConstants.superscriptBottomMin or ¼ × OS/2.sxHeight if the constant is not
    // available.
    fn superscript_bottom_min(&self) -> CssPixels {
        self.constant_or(MathConstant::SuperscriptBottomMin, self.x_height.scaled(0.25))
    }

    // superscriptBaselineDropMax: MATH.MathConstants.superscriptBaselineDropMax or Default fallback constant if the
    // constant is not available.
    fn superscript_baseline_drop_max(&self) -> CssPixels {
        self.constant_or_zero(MathConstant::SuperscriptBaselineDropMax)
    }

    // subSuperscriptGapMin: MATH.MathConstants.subSuperscriptGapMin or 4 × default rule thickness if the constant is
    // not available.
    fn sub_superscript_gap_min(&self) -> CssPixels {
        self.constant_or(MathConstant::SubSuperscriptGapMin, self.default_rule_thickness * 4usize)
    }

    // superscriptBottomMaxWithSubscript: MATH.MathConstants.superscriptBottomMaxWithSubscript or ⅘ × OS/2.sxHeight if
    // the constant is not available.
    fn superscript_bottom_max_with_subscript(&self) -> CssPixels {
        self.constant_or(
            MathConstant::SuperscriptBottomMaxWithSubscript,
            self.x_height.scaled(0.8),
        )
    }

    // spaceAfterScript: MATH.MathConstants.spaceAfterScript or 1/24em if the constant is not available.
    fn space_after_script(&self) -> CssPixels {
        self.constant_or(MathConstant::SpaceAfterScript, self.em.scaled(1.0 / 24.0))
    }

    // upperLimitGapMin: MATH.MathConstants.upperLimitGapMin or Default fallback constant if the constant is not
    // available.
    fn upper_limit_gap_min(&self) -> CssPixels {
        self.constant_or_zero(MathConstant::UpperLimitGapMin)
    }

    // upperLimitBaselineRiseMin: MATH.MathConstants.upperLimitBaselineRiseMin or Default fallback constant if the
    // constant is not available.
    fn upper_limit_baseline_rise_min(&self) -> CssPixels {
        self.constant_or_zero(MathConstant::UpperLimitBaselineRiseMin)
    }

    // lowerLimitGapMin: MATH.MathConstants.lowerLimitGapMin or Default fallback constant if the constant is not
    // available.
    fn lower_limit_gap_min(&self) -> CssPixels {
        self.constant_or_zero(MathConstant::LowerLimitGapMin)
    }

    // lowerLimitBaselineDropMin: MATH.MathConstants.lowerLimitBaselineDropMin or Default fallback constant if the
    // constant is not available.
    fn lower_limit_baseline_drop_min(&self) -> CssPixels {
        self.constant_or_zero(MathConstant::LowerLimitBaselineDropMin)
    }

    // stackTopShiftUp: MATH.MathConstants.stackTopShiftUp or Default fallback constant if the constant is not
    // available.
    fn stack_top_shift_up(&self) -> CssPixels {
        self.constant_or_zero(MathConstant::StackTopShiftUp)
    }

    // stackTopDisplayStyleShiftUp: MATH.MathConstants.stackTopDisplayStyleShiftUp or Default fallback constant if the
    // constant is not available.
    fn stack_top_display_style_shift_up(&self) -> CssPixels {
        self.constant_or_zero(MathConstant::StackTopDisplayStyleShiftUp)
    }

    // stackBottomShiftDown: MATH.MathConstants.stackBottomShiftDown or Default fallback constant if the constant is
    // not available.
    fn stack_bottom_shift_down(&self) -> CssPixels {
        self.constant_or_zero(MathConstant::StackBottomShiftDown)
    }

    // stackBottomDisplayStyleShiftDown: MATH.MathConstants.stackBottomDisplayStyleShiftDown or Default fallback
    // constant if the constant is not available.
    fn stack_bottom_display_style_shift_down(&self) -> CssPixels {
        self.constant_or_zero(MathConstant::StackBottomDisplayStyleShiftDown)
    }

    // stackGapMin: MATH.MathConstants.stackGapMin or 3 × default rule thickness if the constant is not available.
    fn stack_gap_min(&self) -> CssPixels {
        self.constant_or(MathConstant::StackGapMin, self.default_rule_thickness * 3usize)
    }

    // stackDisplayStyleGapMin: MATH.MathConstants.stackDisplayStyleGapMin or 7 × default rule thickness if the
    // constant is not available.
    fn stack_display_style_gap_min(&self) -> CssPixels {
        self.constant_or(
            MathConstant::StackDisplayStyleGapMin,
            self.default_rule_thickness * 7usize,
        )
    }

    // stretchStackTopShiftUp: MATH.MathConstants.stretchStackTopShiftUp or Default fallback constant if the constant
    // is not available.
    fn stretch_stack_top_shift_up(&self) -> CssPixels {
        self.constant_or_zero(MathConstant::StretchStackTopShiftUp)
    }

    // stretchStackBottomShiftDown: MATH.MathConstants.stretchStackBottomShiftDown or Default fallback constant if the
    // constant is not available.
    fn stretch_stack_bottom_shift_down(&self) -> CssPixels {
        self.constant_or_zero(MathConstant::StretchStackBottomShiftDown)
    }

    // stretchStackGapAboveMin: MATH.MathConstants.stretchStackGapAboveMin or Default fallback constant if the
    // constant is not available.
    fn stretch_stack_gap_above_min(&self) -> CssPixels {
        self.constant_or_zero(MathConstant::StretchStackGapAboveMin)
    }

    // stretchStackGapBelowMin: MATH.MathConstants.stretchStackGapBelowMin or Default fallback constant if the
    // constant is not available.
    fn stretch_stack_gap_below_min(&self) -> CssPixels {
        self.constant_or_zero(MathConstant::StretchStackGapBelowMin)
    }

    // fractionNumeratorShiftUp: MATH.MathConstants.fractionNumeratorShiftUp or Default fallback constant if the
    // constant is not available.
    fn fraction_numerator_shift_up(&self) -> CssPixels {
        self.constant_or_zero(MathConstant::FractionNumeratorShiftUp)
    }

    // fractionNumeratorDisplayStyleShiftUp: MATH.MathConstants.fractionNumeratorDisplayStyleShiftUp or Default
    // fallback constant if the constant is not available.
    fn fraction_numerator_display_style_shift_up(&self) -> CssPixels {
        self.constant_or_zero(MathConstant::FractionNumeratorDisplayStyleShiftUp)
    }

    // fractionDenominatorShiftDown: MATH.MathConstants.fractionDenominatorShiftDown or Default fallback constant if
    // the constant is not available.
    fn fraction_denominator_shift_down(&self) -> CssPixels {
        self.constant_or_zero(MathConstant::FractionDenominatorShiftDown)
    }

    // fractionDenominatorDisplayStyleShiftDown: MATH.MathConstants.fractionDenominatorDisplayStyleShiftDown or
    // Default fallback constant if the constant is not available.
    fn fraction_denominator_display_style_shift_down(&self) -> CssPixels {
        self.constant_or_zero(MathConstant::FractionDenominatorDisplayStyleShiftDown)
    }

    // fractionNumeratorGapMin: MATH.MathConstants.fractionNumeratorGapMin or default rule thickness if the constant is
    // not available.
    fn fraction_numerator_gap_min(&self) -> CssPixels {
        self.constant_or(MathConstant::FractionNumeratorGapMin, self.default_rule_thickness)
    }

    // fractionNumDisplayStyleGapMin: MATH.MathConstants.fractionNumDisplayStyleGapMin or 3 × default rule thickness if
    // the constant is not available.
    fn fraction_num_display_style_gap_min(&self) -> CssPixels {
        self.constant_or(
            MathConstant::FractionNumDisplayStyleGapMin,
            self.default_rule_thickness * 3usize,
        )
    }

    // fractionRuleThickness: MATH.MathConstants.fractionRuleThickness or default rule thickness if the constant is not
    // available.
    fn fraction_rule_thickness(&self) -> CssPixels {
        self.constant_or(MathConstant::FractionRuleThickness, self.default_rule_thickness)
    }

    // fractionDenominatorGapMin: MATH.MathConstants.fractionDenominatorGapMin or default rule thickness if the
    // constant is not available.
    fn fraction_denominator_gap_min(&self) -> CssPixels {
        self.constant_or(MathConstant::FractionDenominatorGapMin, self.default_rule_thickness)
    }

    // fractionDenomDisplayStyleGapMin: MATH.MathConstants.fractionDenomDisplayStyleGapMin or 3 × default rule
    // thickness if the constant is not available.
    fn fraction_denom_display_style_gap_min(&self) -> CssPixels {
        self.constant_or(
            MathConstant::FractionDenomDisplayStyleGapMin,
            self.default_rule_thickness * 3usize,
        )
    }

    // overbarVerticalGap: MATH.MathConstants.overbarVerticalGap or 3 × default rule thickness if the constant is not
    // available.
    fn overbar_vertical_gap(&self) -> CssPixels {
        self.constant_or(MathConstant::OverbarVerticalGap, self.default_rule_thickness * 3usize)
    }

    // overbarExtraAscender: MATH.MathConstants.overbarExtraAscender or default rule thickness if the constant is not
    // available.
    fn overbar_extra_ascender(&self) -> CssPixels {
        self.constant_or(MathConstant::OverbarExtraAscender, self.default_rule_thickness)
    }

    // underbarVerticalGap: MATH.MathConstants.underbarVerticalGap or 3 × default rule thickness if the constant is not
    // available.
    fn underbar_vertical_gap(&self) -> CssPixels {
        self.constant_or(MathConstant::UnderbarVerticalGap, self.default_rule_thickness * 3usize)
    }

    // underbarExtraDescender: MATH.MathConstants.underbarExtraDescender or default rule thickness if the constant is
    // not available.
    fn underbar_extra_descender(&self) -> CssPixels {
        self.constant_or(MathConstant::UnderbarExtraDescender, self.default_rule_thickness)
    }

    // radicalVerticalGap: MATH.MathConstants.radicalVerticalGap or 1¼ × default rule thickness if the constant is not
    // available.
    fn radical_vertical_gap(&self) -> CssPixels {
        self.constant_or(
            MathConstant::RadicalVerticalGap,
            self.default_rule_thickness.scaled(1.25),
        )
    }

    // radicalDisplayStyleVerticalGap: MATH.MathConstants.radicalDisplayStyleVerticalGap or default rule thickness + ¼
    // OS/2.sxHeight if the constant is not available.
    fn radical_display_style_vertical_gap(&self) -> CssPixels {
        self.constant_or(
            MathConstant::RadicalDisplayStyleVerticalGap,
            self.default_rule_thickness + self.x_height.scaled(0.25),
        )
    }

    // radicalRuleThickness: MATH.MathConstants.radicalRuleThickness or default rule thickness if the constant is not
    // available.
    fn radical_rule_thickness(&self) -> CssPixels {
        self.constant_or(MathConstant::RadicalRuleThickness, self.default_rule_thickness)
    }

    // radicalExtraAscender: MATH.MathConstants.radicalExtraAscender or default rule thickness if the constant is not
    // available.
    fn radical_extra_ascender(&self) -> CssPixels {
        self.constant_or(MathConstant::RadicalExtraAscender, self.default_rule_thickness)
    }

    // radicalKernBeforeDegree: MATH.MathConstants.radicalKernBeforeDegree or 5/18em if the constant is not available.
    fn radical_kern_before_degree(&self) -> CssPixels {
        self.constant_or(MathConstant::RadicalKernBeforeDegree, self.em.scaled(5.0 / 18.0))
    }

    // radicalKernAfterDegree: MATH.MathConstants.radicalKernAfterDegree or −10/18em if the constant is not available.
    fn radical_kern_after_degree(&self) -> CssPixels {
        self.constant_or(MathConstant::RadicalKernAfterDegree, self.em.scaled(-10.0 / 18.0))
    }

    // radicalDegreeBottomRaisePercent: MATH.MathConstants.radicalDegreeBottomRaisePercent / 100.0 or 0.6 if the
    // constant is not available.
    fn radical_degree_bottom_raise_percent(&self) -> f64 {
        self.data
            .constant(MathConstant::RadicalDegreeBottomRaisePercent)
            .map_or(0.6, |percent| f64::from(percent) / 100.0)
    }
}

pub(super) struct MathFormattingContext<'pass> {
    run: FormattingContextRun<'pass>,
    content: MathContent,
    derived_baselines_of_root_box: DerivedBaselines,
}

impl<'pass> MathFormattingContext<'pass> {
    pub(super) fn new(run: &FormattingContextRun<'pass>) -> Self {
        Self {
            run: FormattingContextRun {
                previous_line_data: None,
                ..run.clone()
            },
            content: MathContent::default(),
            derived_baselines_of_root_box: DerivedBaselines::default(),
        }
    }

    fn style(&self, node: Node) -> StyleValues<'pass> {
        StyleValues::for_node(&self.run.callbacks, node)
    }

    fn facts(&self, node: Node) -> NodeFacts<'_> {
        NodeFacts::new(&self.run.callbacks, node)
    }

    fn kind(&self, node: Node) -> MathLayoutKind {
        self.run.callbacks.arena().math_layout_kind(node)
    }

    fn in_flow_children(&self) -> Vec<Node> {
        self.in_flow_children_of(self.run.box_)
    }

    fn in_flow_children_of(&self, parent: Node) -> Vec<Node> {
        // https://w3c.github.io/mathml-core/#css-styling
        // The float property does not create floating of elements whose parent's computed display value is block
        // math or inline math, and does not take them out-of-flow.
        self.run
            .callbacks
            .children(parent)
            .filter(|&child| {
                let facts = self.facts(child);
                facts.is_box()
                    && !facts.is_absolutely_positioned()
                    && !self.run.callbacks.can_skip_is_anonymous_text_run(child)
            })
            .collect()
    }

    // https://w3c.github.io/mathml-core/#dfn-space-like
    fn is_space_like(&self, node: Node) -> bool {
        // A MathML Core element is a space-like element if it is:
        let kind = self.kind(node);
        // - an <mtext> or <mspace>;
        if matches!(kind, MathLayoutKind::Mtext | MathLayoutKind::Mspace) {
            return true;
        }
        // - or a grouping element or <mpadded> all of whose in-flow children are space-like.
        if kind.is_grouping_element() || kind == MathLayoutKind::Mpadded {
            return self
                .in_flow_children_of(node)
                .into_iter()
                .all(|child| self.is_space_like(child));
        }
        false
    }

    /// The core operator of the embellished operator `node`, or `None` if `node` is not an embellished operator.
    // https://w3c.github.io/mathml-core/#dfn-embellished-operator
    fn core_operator(&self, node: Node) -> Option<Node> {
        // A MathML Core element is an embellished operator if it is:
        let kind = self.kind(node);
        // - An <mo> element;
        if kind == MathLayoutKind::Mo {
            // The core operator of an <mo> element is the element itself.
            return Some(node);
        }
        // - a scripted element or an <mfrac>, whose first in-flow child exists and is an embellished operator;
        if kind.is_scripted_element() || kind == MathLayoutKind::Mfrac {
            // The core operator of an embellished scripted element or <mfrac> element is the core operator of its
            // first in-flow child.
            return self
                .in_flow_children_of(node)
                .first()
                .and_then(|&child| self.core_operator(child));
        }
        // - a grouping element or <mpadded>, whose in-flow children consist (in any order) of one embellished operator
        //   and zero or more space-like elements.
        if kind.is_grouping_element() || kind == MathLayoutKind::Mpadded {
            // The core operator of an embellished grouping element or <mpadded> is the core operator of its unique
            // embellished operator in-flow child.
            let mut core_operator = Option::None;
            for child in self.in_flow_children_of(node) {
                if let Some(child_core_operator) = self.core_operator(child) {
                    if core_operator.is_some() {
                        return Option::None;
                    }
                    core_operator = Some(child_core_operator);
                } else if !self.is_space_like(child) {
                    return Option::None;
                }
            }
            return core_operator;
        }
        Option::None
    }

    /// The text of an operator that contains only text content.
    fn operator_text_content(&self, operator: Node) -> Option<Vec<u16>> {
        let callbacks = &self.run.callbacks;
        let mut content = Vec::new();
        for child in callbacks.children(operator) {
            if !node_facts::kind_is_text(callbacks.node_data(child).kind.get()) {
                return Option::None;
            }
            content.extend_from_slice(&callbacks.text_content(child).text);
        }
        let is_whitespace = |unit: &u16| *unit <= 0x7f && (*unit as u8).is_ascii_whitespace();
        let start = content
            .iter()
            .position(|unit| !is_whitespace(unit))
            .unwrap_or(content.len());
        let end = content
            .iter()
            .rposition(|unit| !is_whitespace(unit))
            .map_or(start, |end| end + 1);
        Some(content[start..end].to_vec())
    }

    /// The character c of an operator whose content is made of a single character.
    fn operator_character(&self, operator: Node) -> Option<u32> {
        let content = self.operator_text_content(operator)?;
        let mut characters = char::decode_utf16(content.iter().copied());
        let character = characters.next()?.ok()?;
        characters.next().is_none().then_some(u32::from(character))
    }

    /// The stretch axis of an operator, which is the intrinsic stretch axis of its character.
    // https://w3c.github.io/mathml-core/#dfn-stretch-axis
    fn stretch_axis(&self, operator: Node) -> StretchAxis {
        self.operator_character(operator)
            .map_or(StretchAxis::Block, intrinsic_stretch_axis)
    }

    /// The indices of the first and last of `siblings` that are not space-like, if there are at least two such
    /// siblings and their parent is a grouping element, <mpadded> or <msqrt>.
    fn first_and_last_non_space_like(&self, siblings: &[Node], parent_kind: MathLayoutKind) -> Option<(usize, usize)> {
        if !parent_kind.is_grouping_element() && !matches!(parent_kind, MathLayoutKind::Mpadded | MathLayoutKind::Msqrt)
        {
            return Option::None;
        }
        let first = siblings.iter().position(|&sibling| !self.is_space_like(sibling))?;
        let last = siblings.iter().rposition(|&sibling| !self.is_space_like(sibling))?;
        (first != last).then_some((first, last))
    }

    // https://w3c.github.io/mathml-core/#dfn-algorithm-for-determining-the-form-of-an-embellished-operator
    // NB: `first_and_last_non_space_like` is what `first_and_last_non_space_like()` returns for the siblings.
    fn embellished_operator_form(
        &self,
        index: usize,
        parent_kind: MathLayoutKind,
        first_and_last_non_space_like: Option<(usize, usize)>,
    ) -> OperatorForm {
        // 1. If the form attribute is present and valid on the core operator, then its ASCII lowercased value is used.
        // NB: The caller handles this step.
        if let Some((first, last)) = first_and_last_non_space_like {
            // 2. If the embellished operator is the first in-flow child of a grouping element, <mpadded> or <msqrt>
            //    with more than one in-flow child (ignoring all space-like children) then it has form prefix.
            if index == first {
                return OperatorForm::Prefix;
            }
            // 3. Or, if the embellished operator is the last in-flow child of a grouping element, <mpadded> or
            //    <msqrt> with more than one in-flow child (ignoring all space-like children) then it has form postfix.
            if index == last {
                return OperatorForm::Postfix;
            }
        }
        // 4. Or, if the embellished operator is an in-flow child of a scripted element, other than the first in-flow
        //    child, then it has form postfix.
        if parent_kind.is_scripted_element() && index > 0 {
            return OperatorForm::Postfix;
        }
        // 5. Otherwise, the embellished operator has form infix.
        OperatorForm::Infix
    }

    /// The properties of the in-flow child at `index` of a box of kind `parent_kind`, if it is an embellished
    /// operator.
    // https://w3c.github.io/mathml-core/#dfn-algorithm-for-determining-the-properties-of-an-embellished-operator
    fn embellished_operator_properties(
        &self,
        siblings: &[Node],
        index: usize,
        parent_kind: MathLayoutKind,
        first_and_last_non_space_like: Option<(usize, usize)>,
    ) -> Option<OperatorProperties> {
        let core_operator = self.core_operator(siblings[index])?;
        let attributes = self.run.callbacks.arena().math_attribute_facts(core_operator);
        // 2. Otherwise, run the algorithm for determining the form of an embellished operator.
        let explicit_form = OperatorForm::from_attribute(attributes.form);
        let form = explicit_form
            .unwrap_or_else(|| self.embellished_operator_form(index, parent_kind, first_and_last_non_space_like));
        let mut category = OperatorCategory::Default;
        // 3. If the core operator contains only text content Content, then set Category to the result of the
        //    algorithm to determine the category of an operator (Content, Form) where Form is the form calculated at
        //    the previous step.
        if let Some(content) = self.operator_text_content(core_operator) {
            category = operator_category(&content, form);
            // 4. If Category is Default and the form of embellished operator was not explicitly specified as an
            //    attribute on its core operator:
            if category == OperatorCategory::Default && explicit_form.is_none() {
                // 1. Set Category to the result of the algorithm to determine the category of an operator (Content,
                //    Form) where Form is infix.
                // 2. If Category is Default, then run the algorithm again with Form set to postfix.
                // 3. If Category is Default, then run the algorithm again with Form set to prefix.
                for fallback_form in [OperatorForm::Infix, OperatorForm::Postfix, OperatorForm::Prefix] {
                    category = operator_category(&content, fallback_form);
                    if category != OperatorCategory::Default {
                        break;
                    }
                }
            }
        }
        // 5. Run the algorithm to set the properties of an operator from its category Category.
        // NB: Font-relative lengths for lspace and rspace rely on the font style of the core operator, not the one of
        //     the embellished operator.
        let core_style = self.style(core_operator);
        let (lspace, rspace) = category.spacing_in_eighteenths_of_em();
        let dictionary_lspace = core_style.font_size().scaled(f64::from(lspace) / 18.0);
        let dictionary_rspace = core_style.font_size().scaled(f64::from(rspace) / 18.0);
        // 1. If the corresponding stretchy, symmetric, largeop, movablelimits, lspace, rspace, maxsize or minsize
        //    attribute is present and valid on the core operator, then the ASCII lowercased value of this property is
        //    used.
        // NB: This step is applied last, as the dictionary gives the values for the attributes that are absent or
        //     invalid. Percentage values for lspace and rspace are interpreted relative to the value read from the
        //     dictionary.
        let lspace = attributes
            .lspace
            .resolve(&core_style, dictionary_lspace)
            .unwrap_or(dictionary_lspace);
        let rspace = attributes
            .rspace
            .resolve(&core_style, dictionary_rspace)
            .unwrap_or(dictionary_rspace);
        let dictionary_flags = category.flags();
        let flags = OperatorFlags {
            stretchy: math_boolean_or(attributes.stretchy, dictionary_flags.stretchy),
            symmetric: math_boolean_or(attributes.symmetric, dictionary_flags.symmetric),
            largeop: math_boolean_or(attributes.largeop, dictionary_flags.largeop),
            movablelimits: math_boolean_or(attributes.movablelimits, dictionary_flags.movablelimits),
        };
        Some(OperatorProperties {
            core_operator,
            lspace,
            rspace,
            flags,
        })
    }

    fn layout_child(&self, child: Node, constraints: ContainingBlockConstraints) -> Child {
        let run = &self.run;
        run.records.create_used_values(&run.callbacks, child, constraints);
        // NB: MathML layout does not break lines, so each child is laid out at its max-content inline size, as an
        //     atomic box whose block size follows from its content.
        let input = LayoutInput::new(
            AvailableSpace {
                inline_size: AvailableSize::MaxContent,
                block_size: AvailableSize::Indefinite,
            },
            constraints,
            ParticipationInParentFormattingContext::AtomicInline,
        );
        let used = run.records.used_values(child);
        let baselines =
            match formatting_context::layout_inside_child(run, None, None, child, run.layout_mode, input, true) {
                ChildLayoutOutcome::Created(result) => result.baselines,
                ChildLayoutOutcome::ReenterCurrent | ChildLayoutOutcome::Skipped => used.content_baselines_from_cells(),
            };
        let collapsed = used.uses_collapsing_borders_model.get();
        let block_size = used.margin_box_block_size(collapsed);
        // NB: A box without a baseline is aligned by its line-under margin edge.
        let ascent = baselines
            .first
            .map_or(block_size, |baseline| used.margin_box_top(collapsed) + baseline);
        Child {
            node: child,
            inline_size: used.margin_box_inline_size(collapsed),
            ascent,
            descent: block_size - ascent,
        }
    }

    pub(super) fn run(&mut self, run: &FormattingContextRun<'pass>, layout_input: LayoutInput) {
        let root = run.box_;
        let kind = self.kind(root);
        let constraints = run
            .sizing()
            .constraints_for_child_context(root, layout_input.containing_block_constraints);
        let children: Vec<Child> = self
            .in_flow_children()
            .into_iter()
            .map(|child| self.layout_child(child, constraints))
            .collect();

        let root_style = self.style(root);
        let constants = MathConstants::for_style(&root_style);
        let is_cramped = root_style.math_shift() == math_shift::COMPACT;
        let content = match (kind, children.len()) {
            (MathLayoutKind::Mfrac, 2) => self.layout_fraction(&children, &constants, &root_style),
            (MathLayoutKind::Msub, 2) => layout_scripts(&children[0], Some(&children[1]), None, &constants, is_cramped),
            (MathLayoutKind::Msup, 2) => layout_scripts(&children[0], None, Some(&children[1]), &constants, is_cramped),
            (MathLayoutKind::Msubsup, 3) => layout_scripts(
                &children[0],
                Some(&children[1]),
                Some(&children[2]),
                &constants,
                is_cramped,
            ),
            (MathLayoutKind::Munder, 2) => {
                self.layout_underover_element(&children[0], Some(&children[1]), None, &constants, &root_style)
            }
            (MathLayoutKind::Mover, 2) => {
                self.layout_underover_element(&children[0], None, Some(&children[1]), &constants, &root_style)
            }
            (MathLayoutKind::Munderover, 3) => self.layout_underover_element(
                &children[0],
                Some(&children[1]),
                Some(&children[2]),
                &constants,
                &root_style,
            ),
            (MathLayoutKind::Mspace, _) => self.layout_space(&root_style),
            (MathLayoutKind::Mpadded, _) => {
                // The <mpadded> element generates an anonymous <mrow> box called the mpadded inner box.
                let add_space = self.core_operator(root).is_none();
                let inner = self.layout_row_with_operator_spacing(&children, kind, add_space);
                self.layout_padded(inner, &root_style)
            }
            (MathLayoutKind::Mmultiscripts, _) if self.is_valid_multiscripts(&children) => layout_multiscripts(
                &children,
                self.prescripts_separator_index(&children),
                &constants,
                is_cramped,
            ),
            (MathLayoutKind::Mroot, 2) => {
                let radical = RadicalMetrics::new(&root_style, &constants);
                layout_root(&children[0], &children[1], &radical, &constants)
            }
            (MathLayoutKind::Msqrt, _) => {
                let radical = RadicalMetrics::new(&root_style, &constants);
                // NB: The children of <msqrt> are laid out in an anonymous <mrow> box.
                let base = self.layout_row_with_operator_spacing(&children, kind, true);
                layout_square_root(base, &radical)
            }
            _ => {
                // https://w3c.github.io/mathml-core/#layout-of-mrow
                // Set add-space to true if the box corresponds to a <math> element or is not an embellished operator;
                // and to false otherwise.
                let add_space = kind == MathLayoutKind::Math || self.core_operator(root).is_none();
                self.layout_row_with_operator_spacing(&children, kind, add_space)
            }
        };

        // https://w3c.github.io/mathml-core/#layout-algorithms
        // If a different size is specified for the content box, that size is used instead and the inline-start and
        // block-start edges of the math content box are aligned with the ones of the content box, unless otherwise
        // specified in a specific layout algorithm.
        let root_used = run.records.used_values(root);
        let content_box_inline_size = root_used.content_inline_size.get();
        let centers_math_content = match kind {
            // If the computed display property is equal to block math then that math content box is rendered
            // horizontally centered within the content box.
            MathLayoutKind::Math => self.facts(root).display().is_block_outside(),
            // The math content box is placed within the content box so that their block-start edges are aligned and
            // the middles of these edges are at the same position.
            MathLayoutKind::Mfrac | MathLayoutKind::Munder | MathLayoutKind::Mover | MathLayoutKind::Munderover => true,
            _ => false,
        };
        let content_box_is_known = run.layout_mode == LayoutMode::Normal && root_used.has_definite_inline_size();
        let is_rtl = root_style.direction() == direction::RTL;
        let mut math_content_line_left = CssPixels::default();
        if content_box_is_known {
            if centers_math_content {
                math_content_line_left = (content_box_inline_size - content.inline_size) / 2;
            } else if is_rtl {
                math_content_line_left = content_box_inline_size - content.inline_size;
            }
        }

        for (child, position) in children.iter().zip(content.positions.iter()) {
            let used = run.records.used_values(child.node);
            let collapsed = used.uses_collapsing_borders_model.get();
            let line_left_offset = if is_rtl {
                content.inline_size - position.line_left_offset - child.inline_size
            } else {
                position.line_left_offset
            };
            let margin_box_top = content.ascent + position.baseline_shift - child.ascent;
            let offset = FfiCssPixelPoint {
                x: math_content_line_left + line_left_offset + used.margin_left.get() + used.border_box_left(collapsed),
                y: margin_box_top + used.margin_box_top(collapsed),
            };
            formatting_context::place_child(run, child.node, offset, None);
        }

        self.derived_baselines_of_root_box = DerivedBaselines {
            first: Some(content.ascent),
            last: Some(content.ascent),
        };
        self.content = content;
    }

    fn prescripts_separator_index(&self, children: &[Child]) -> Option<usize> {
        children
            .iter()
            .position(|child| self.kind(child.node) == MathLayoutKind::Mprescripts)
    }

    // https://w3c.github.io/mathml-core/#prescripts-and-tensor-indices-mmultiscripts
    fn is_valid_multiscripts(&self, children: &[Child]) -> bool {
        // A valid <mmultiscripts> element contains the following in-flow children:
        // - A first in-flow child, called the mmultiscripts base, that is not an <mprescripts> element.
        // - Followed by an even number of in-flow children called mmultiscripts postscripts, none of them being a
        //   <mprescripts> element.
        // - Optionally followed by an <mprescripts> element and an even number of in-flow children called
        //   mmultiscripts prescripts, none of them being a <mprescripts> element.
        let separator_count = children
            .iter()
            .filter(|child| self.kind(child.node) == MathLayoutKind::Mprescripts)
            .count();
        match self.prescripts_separator_index(children) {
            Option::None => !children.is_empty() && children.len() % 2 == 1,
            Some(separator) => {
                separator_count == 1 && separator % 2 == 1 && (children.len() - separator - 1).is_multiple_of(2)
            }
        }
    }

    fn layout_row_with_operator_spacing(
        &self,
        children: &[Child],
        kind: MathLayoutKind,
        add_space: bool,
    ) -> MathContent {
        let nodes: Vec<Node> = children.iter().map(|child| child.node).collect();
        let first_and_last_non_space_like = if add_space {
            self.first_and_last_non_space_like(&nodes, kind)
        } else {
            Option::None
        };
        let spacing: Vec<Option<(CssPixels, CssPixels)>> = (0..nodes.len())
            .map(|index| {
                add_space
                    .then(|| self.embellished_operator_properties(&nodes, index, kind, first_and_last_non_space_like))
                    .flatten()
                    .map(|properties| (properties.lspace, properties.rspace))
            })
            .collect();
        layout_row(children, &spacing)
    }

    // https://w3c.github.io/mathml-core/#children-of-munder-mover-munderover
    fn layout_underover_element(
        &self,
        base: &Child,
        underscript: Option<&Child>,
        overscript: Option<&Child>,
        constants: &MathConstants,
        style: &StyleValues<'_>,
    ) -> MathContent {
        let base_properties = self.embellished_operator_properties(&[base.node], 0, self.kind(self.run.box_), None);
        // If the <munder>, <mover> or <munderover> elements have a computed math-style property equal to compact and
        // their base is an embellished operator with the movablelimits property, then their layout algorithms are
        // respectively the same as the ones described for <msub>, <msup> and <msubsup> in 3.4.1.2 Base with
        // subscript, 3.4.1.3 Base with superscript and 3.4.1.4 Base with subscript and superscript.
        if style.math_style() == math_style::COMPACT
            && base_properties.is_some_and(|properties| properties.flags.movablelimits)
        {
            let is_cramped = style.math_shift() == math_shift::COMPACT;
            return layout_scripts(base, underscript, overscript, constants, is_cramped);
        }
        // Otherwise, the <munder>, <mover> and <munderover> layout algorithms are respectively described in 3.4.2.3
        // Base with underscript, 3.4.2.4 Base with overscript and 3.4.2.5 Base with underscript and overscript.
        let base_kind = match base_properties {
            Some(properties) if properties.flags.largeop => UnderOverBase::LargeOperator,
            Some(properties)
                if properties.flags.stretchy && self.stretch_axis(properties.core_operator) == StretchAxis::Inline =>
            {
                UnderOverBase::InlineStretchyOperator
            }
            _ => UnderOverBase::Other,
        };
        layout_underover(
            base,
            underscript,
            overscript,
            constants,
            base_kind,
            self.accents(self.run.box_),
        )
    }

    /// Whether the accentunder and accent attributes of an <munder>, <mover> or <munderover> are a match to true.
    // https://w3c.github.io/mathml-core/#dfn-accent
    fn accents(&self, node: Node) -> (bool, bool) {
        let attributes = self.run.callbacks.arena().math_attribute_facts(node);
        (
            attributes.accentunder == math_boolean::TRUE,
            attributes.accent == math_boolean::TRUE,
        )
    }

    // https://w3c.github.io/mathml-core/#adjust-space-around-content-mpadded
    fn layout_padded(&self, inner: MathContent, style: &StyleValues<'_>) -> MathContent {
        let attributes = self.run.callbacks.arena().math_attribute_facts(self.run.box_);
        let resolve = |length: FfiMathLength| {
            (!length.is_percentage())
                .then(|| length.resolve(style, CssPixels::default()))
                .flatten()
        };
        // If the height attribute is absent, invalid or a percentage then the requested height is the inner
        // line-ascent. Otherwise the requested height is the resolved value of the height attribute, clamping negative
        // values to 0.
        let requested_height =
            resolve(attributes.height).map_or(inner.ascent, |height| height.max(CssPixels::default()));
        // If the depth attribute is absent, invalid or a percentage then the requested depth is the inner line-ascent.
        // Otherwise the requested depth is the resolved value of the depth attribute, clamping negative values to 0.
        // AD-HOC: The inner line-descent is used as the default requested depth, as the depth of the inner box is what
        //         it stands for. The inner line-ascent would give the content the depth of its height.
        let requested_depth = resolve(attributes.depth).map_or(inner.descent, |depth| depth.max(CssPixels::default()));
        // If the lspace attribute is absent, invalid or a percentage then the requested lspace is 0. Otherwise the
        // requested lspace is the resolved value of the lspace attribute, clamping negative values to 0.
        let requested_lspace =
            resolve(attributes.lspace).map_or(CssPixels::default(), |lspace| lspace.max(CssPixels::default()));
        // If the voffset attribute is absent, invalid or a percentage then the requested voffset is 0. Otherwise the
        // requested voffset is the resolved value of the voffset attribute.
        let requested_voffset = resolve(attributes.voffset).unwrap_or_default();
        // The inline size of the math content is the requested width.
        // NB: The width attribute is a presentational hint for the width property, which sizes the content box. The
        //     math content itself is as wide as the inner box.
        // The line-ascent of the math content is the requested height. The line-descent of the math content is the
        // requested depth. The mpadded inner box is placed so that its alphabetic baseline is shifted away from the
        // alphabetic baseline by the requested voffset towards the line-over.
        MathContent {
            inline_size: inner.inline_size,
            ascent: requested_height,
            descent: requested_depth,
            positions: inner
                .positions
                .into_iter()
                .map(|position| ChildPosition {
                    line_left_offset: position.line_left_offset + requested_lspace,
                    baseline_shift: position.baseline_shift - requested_voffset,
                })
                .collect(),
        }
    }

    // https://w3c.github.io/mathml-core/#space-mspace
    fn layout_space(&self, style: &StyleValues<'_>) -> MathContent {
        // If the height attribute is absent, invalid or a percentage then the requested line-ascent is 0. Otherwise
        // the requested line-ascent is the resolved value of the height attribute, clamping negative values to 0.
        let height = self.run.callbacks.arena().math_attribute_facts(self.run.box_).height;
        let requested_line_ascent = if height.is_percentage() {
            CssPixels::default()
        } else {
            height
                .resolve(style, CssPixels::default())
                .unwrap_or_default()
                .max(CssPixels::default())
        };
        // The line-ascent of the math content is equal to the requested line-ascent determined above.
        MathContent {
            ascent: requested_line_ascent,
            ..MathContent::default()
        }
    }

    // https://w3c.github.io/mathml-core/#fraction-with-nonzero-line-thickness
    fn layout_fraction(&self, children: &[Child], constants: &MathConstants, style: &StyleValues<'_>) -> MathContent {
        let numerator = &children[0];
        let denominator = &children[1];
        let is_display_style = style.math_style() == math_style::NORMAL;
        // https://w3c.github.io/mathml-core/#dfn-linethickness
        // The linethickness attribute indicates the fraction line thickness to use for the fraction bar. If present,
        // it must have a value that is a valid <length-percentage>. If the attribute is absent or has an invalid value,
        // FractionRuleThickness is used as the default value. A percentage is interpreted relative to that default
        // value. A negative value is interpreted as 0.
        let line_thickness = self
            .run
            .callbacks
            .arena()
            .math_attribute_facts(self.run.box_)
            .linethickness
            .resolve(style, constants.fraction_rule_thickness())
            .unwrap_or(constants.fraction_rule_thickness())
            .max(CssPixels::default());
        if line_thickness == CssPixels::default() {
            return layout_fraction_without_bar(numerator, denominator, constants, is_display_style);
        }
        let half_thickness = line_thickness / 2;
        let axis_height = constants.axis_height();

        // The inline size of the math content is the maximum between the inline size of the numerator's margin box
        // and the inline size of the denominator's margin box.
        let inline_size = numerator.inline_size.max(denominator.inline_size);

        // NumeratorShift is the maximum between:
        // - FractionNumeratorShiftUp (respectively FractionNumeratorDisplayStyleShiftUp) if the math-style is compact
        //   (respectively normal).
        // - The AxisHeight + half the fraction line thickness + FractionNumeratorGapMin (respectively
        //   FractionNumDisplayStyleGapMin) if the math-style is compact (respectively normal) + the ink line-descent of
        //   the numerator's margin box.
        let (numerator_shift_up, numerator_gap_min) = if is_display_style {
            (
                constants.fraction_numerator_display_style_shift_up(),
                constants.fraction_num_display_style_gap_min(),
            )
        } else {
            (
                constants.fraction_numerator_shift_up(),
                constants.fraction_numerator_gap_min(),
            )
        };
        let numerator_shift =
            numerator_shift_up.max(axis_height + half_thickness + numerator_gap_min + numerator.descent);
        // DenominatorShift is the maximum between:
        // - FractionDenominatorShiftDown (respectively FractionDenominatorDisplayStyleShiftDown) if the math-style is
        //   compact (respectively normal).
        // - Half the fraction line thickness + FractionDenominatorGapMin (respectively FractionDenomDisplayStyleGapMin)
        //   if the math-style is compact (respectively normal) + the ink line-ascent of the denominator's margin box −
        //   the AxisHeight.
        let (denominator_shift_down, denominator_gap_min) = if is_display_style {
            (
                constants.fraction_denominator_display_style_shift_down(),
                constants.fraction_denom_display_style_gap_min(),
            )
        } else {
            (
                constants.fraction_denominator_shift_down(),
                constants.fraction_denominator_gap_min(),
            )
        };
        let denominator_shift =
            denominator_shift_down.max(half_thickness + denominator_gap_min + denominator.ascent - axis_height);

        // The line-ascent of the math content is the maximum between:
        // - Numerator Shift + the line-ascent of the numerator's margin box.
        // - −Denominator Shift + the line-ascent of the denominator's margin box
        // - AxisHeight plus half the fraction line thickness.
        let ascent = (numerator_shift + numerator.ascent)
            .max(denominator.ascent - denominator_shift)
            .max(axis_height + half_thickness);
        // The line-descent of the math content is the maximum between:
        // - −Numerator Shift + the line-descent of the numerator's margin box.
        // - Denominator Shift + the line-descent of the denominator's margin box.
        // - −AxisHeight plus half the fraction line thickness.
        // - 0
        let descent = (numerator.descent - numerator_shift)
            .max(denominator_shift + denominator.descent)
            .max(half_thickness - axis_height)
            .max(CssPixels::default());

        MathContent {
            inline_size,
            ascent,
            descent,
            positions: vec![
                ChildPosition {
                    line_left_offset: (inline_size - numerator.inline_size) / 2,
                    baseline_shift: -numerator_shift,
                },
                ChildPosition {
                    line_left_offset: (inline_size - denominator.inline_size) / 2,
                    baseline_shift: denominator_shift,
                },
            ],
        }
    }

    pub(super) fn parent_did_dimension(&self) {
        if self.run.layout_mode != LayoutMode::Normal {
            return;
        }
        let root_used = self.run.records.used_values(self.run.box_);
        for child in self.run.callbacks.children(self.run.box_) {
            let facts = self.facts(child);
            if !facts.is_box() || !facts.is_absolutely_positioned() {
                continue;
            }
            let static_position_rect = abspos_inputs::StaticPositionRect {
                rect: geometry::LogicalRect {
                    offset: Default::default(),
                    size: geometry::LogicalSize {
                        inline_size: root_used.content_inline_size.get(),
                        block_size: root_used.content_block_size.get(),
                    },
                },
                inline_alignment: abspos_inputs::StaticPositionAlignment::Start,
                block_alignment: abspos_inputs::StaticPositionAlignment::Start,
                alignment_derives_from_own_computed_values: false,
                is_known: true,
            };
            formatting_context::register_contained_abspos_child(
                &self.run.callbacks,
                self.run.fragments.as_deref(),
                self.run.box_,
                child,
                static_position_rect,
                None,
            );
        }
    }

    pub(super) fn derived_baselines_of_root_box(&self) -> DerivedBaselines {
        self.derived_baselines_of_root_box
    }

    pub(super) fn automatic_content_inline_size(&self) -> CssPixels {
        self.content.inline_size
    }

    pub(super) fn automatic_content_block_size(&self) -> CssPixels {
        self.content.ascent + self.content.descent
    }
}

// https://w3c.github.io/mathml-core/#layout-of-mrow
// NB: `spacing` holds the lspace and rspace of each child that is an embellished operator, if add-space is true.
fn layout_row(children: &[Child], spacing: &[Option<(CssPixels, CssPixels)>]) -> MathContent {
    // FIXME: Handle italic correction.
    // Set inline-offset to 0.
    let mut inline_offset = CssPixels::default();
    let mut ascent = CssPixels::default();
    let mut descent = CssPixels::default();
    let mut positions = Vec::with_capacity(children.len());
    // For each in-flow child:
    for (child, spacing) in children.iter().zip(spacing) {
        // If the child is an embellished operator and add-space is true then increment inline-offset by its lspace
        // property.
        if let Some((lspace, _)) = spacing {
            inline_offset += *lspace;
        }
        // Set the inline offset of the child to inline-offset and its block offset such that the alphabetic baseline
        // of the child is aligned with the alphabetic baseline.
        positions.push(ChildPosition {
            line_left_offset: inline_offset,
            baseline_shift: CssPixels::default(),
        });
        // Increment inline-offset by the inline size of the child's margin box.
        inline_offset += child.inline_size;
        // If the child is an embellished operator and add-space is true then increment inline-offset by its rspace
        // property.
        if let Some((_, rspace)) = spacing {
            inline_offset += *rspace;
        }
        // The line-ascent (respectively line-descent) of the math content is the maximum of the line-ascents
        // (respectively line-descents) of all the in-flow children's margin boxes.
        ascent = ascent.max(child.ascent);
        descent = descent.max(child.descent);
    }
    MathContent {
        inline_size: inline_offset,
        ascent,
        descent,
        positions,
    }
}

// https://w3c.github.io/mathml-core/#fraction-with-zero-line-thickness
fn layout_fraction_without_bar(
    numerator: &Child,
    denominator: &Child,
    constants: &MathConstants,
    is_display_style: bool,
) -> MathContent {
    // The min-content inline size, max-content inline size and inline size of the math content are calculated the
    // same as in Fraction with nonzero line thickness.
    let inline_size = numerator.inline_size.max(denominator.inline_size);

    // If the math-style is compact then TopShift and BottomShift are respectively set to StackTopShiftUp and
    // StackBottomShiftDown. Otherwise math-style is normal and they are respectively set to
    // StackTopDisplayStyleShiftUp and StackBottomDisplayStyleShiftDown.
    let (mut top_shift, mut bottom_shift) = if is_display_style {
        (
            constants.stack_top_display_style_shift_up(),
            constants.stack_bottom_display_style_shift_down(),
        )
    } else {
        (constants.stack_top_shift_up(), constants.stack_bottom_shift_down())
    };

    // The Gap is defined to be (BottomShift − the ink line-ascent of the denominator's margin box) + (TopShift − the
    // ink line-descent of the numerator's margin box). If math-style is compact then GapMin is StackGapMin, otherwise
    // math-style is normal and it is StackDisplayStyleGapMin.
    let gap = (bottom_shift - denominator.ascent) + (top_shift - numerator.descent);
    let gap_min = if is_display_style {
        constants.stack_display_style_gap_min()
    } else {
        constants.stack_gap_min()
    };
    // If Δ = GapMin − Gap is positive then TopShift and BottomShift are respectively increased by Δ/2 and Δ − Δ/2.
    let delta = gap_min - gap;
    if delta > CssPixels::default() {
        top_shift += delta / 2;
        bottom_shift += delta - delta / 2;
    }

    // The line-ascent of the math content is the maximum between:
    // - TopShift + the line-ascent of the numerator's margin box.
    // - BottomShift + the line-ascent of the denominator's margin box.
    // The line-descent of the math content is the maximum between:
    // - TopShift + the line-descent of the numerator's margin box.
    // - BottomShift + the line-descent of the denominator's margin box.
    // NB: The shifts are applied in the directions the numerator and denominator are moved in.
    let ascent = (top_shift + numerator.ascent).max(denominator.ascent - bottom_shift);
    let descent = (numerator.descent - top_shift).max(bottom_shift + denominator.descent);

    // The inline offsets of the numerator and denominator are calculated the same as in Fraction with nonzero line
    // thickness.
    // The alphabetic baseline of the numerator (respectively denominator) is shifted away from the alphabetic baseline
    // by a distance of TopShift (respectively − BottomShift) towards the line-over (respectively line-under).
    MathContent {
        inline_size,
        ascent,
        descent,
        positions: vec![
            ChildPosition {
                line_left_offset: (inline_size - numerator.inline_size) / 2,
                baseline_shift: -top_shift,
            },
            ChildPosition {
                line_left_offset: (inline_size - denominator.inline_size) / 2,
                baseline_shift: bottom_shift,
            },
        ],
    }
}

// https://w3c.github.io/mathml-core/#subscripts-and-superscripts-msub-msup-msubsup
/// The SubShift and SuperShift of a base with a subscript, a superscript or both.
// https://w3c.github.io/mathml-core/#base-with-subscript-and-superscript
fn script_shifts(
    base: &Child,
    subscript: Option<&Child>,
    superscript: Option<&Child>,
    constants: &MathConstants,
    is_cramped: bool,
) -> (Option<CssPixels>, Option<CssPixels>) {
    // FIXME: Use the italic correction of the base.
    // SubShift is the maximum between:
    // - SubscriptShiftDown
    // - the ink line-ascent of the subscript's margin box − SubscriptTopMax
    // - SubscriptBaselineDropMin + the ink line-descent of the msub base's margin box
    let mut sub_shift = subscript.map(|subscript| {
        constants
            .subscript_shift_down()
            .max(subscript.ascent - constants.subscript_top_max())
            .max(constants.subscript_baseline_drop_min() + base.descent)
    });
    // SuperShift is the maximum between:
    // - The value SuperscriptShiftUpCramped if the element has a computed math-shift property equal to compact, or
    //   SuperscriptShiftUp otherwise.
    // - SuperscriptBottomMin + the ink line-descent of the superscript's margin box
    // - The ink line-ascent of the msup base's margin box − SuperscriptBaselineDropMax
    let superscript_shift_up = if is_cramped {
        constants.superscript_shift_up_cramped()
    } else {
        constants.superscript_shift_up()
    };
    let mut super_shift = superscript.map(|superscript| {
        superscript_shift_up
            .max(constants.superscript_bottom_min() + superscript.descent)
            .max(base.ascent - constants.superscript_baseline_drop_max())
    });

    if let (Some(subscript), Some(superscript), Some(sub), Some(sup)) =
        (subscript, superscript, sub_shift.as_mut(), super_shift.as_mut())
    {
        let sub_superscript_gap_min = constants.sub_superscript_gap_min();
        // SubSuperGap is the gap between the two scripts along the block axis and is defined by (SubShift − the ink
        // line-ascent of the msubsup subscript's margin box) + (SuperShift − the ink line-descent of the msubsup
        // superscript's margin box).
        let mut sub_super_gap = (*sub - subscript.ascent) + (*sup - superscript.descent);
        // If SubSuperGap is not at least SubSuperscriptGapMin then the following steps are performed to ensure that
        // the condition holds:
        if sub_super_gap < sub_superscript_gap_min {
            // 1. Let Δ be SuperscriptBottomMaxWithSubscript − (SuperShift − the ink line-descent of the msubsup
            //    superscript's margin box). If Δ > 0 then set Δ to the minimum between Δ set SubSuperscriptGapMin −
            //    SubSuperGap and increase SuperShift (and so SubSuperGap too) by Δ.
            let mut delta = constants.superscript_bottom_max_with_subscript() - (*sup - superscript.descent);
            if delta > CssPixels::default() {
                delta = delta.min(sub_superscript_gap_min - sub_super_gap);
                *sup += delta;
                sub_super_gap += delta;
            }
            // 2. Let Δ be SubSuperscriptGapMin − SubSuperGap. If Δ > 0 then increase SubscriptShift (and so
            //    SubSuperGap too) by Δ.
            let delta = sub_superscript_gap_min - sub_super_gap;
            if delta > CssPixels::default() {
                *sub += delta;
            }
        }
    }
    (sub_shift, super_shift)
}

fn layout_scripts(
    base: &Child,
    subscript: Option<&Child>,
    superscript: Option<&Child>,
    constants: &MathConstants,
    is_cramped: bool,
) -> MathContent {
    let space_after_script = constants.space_after_script();
    let (sub_shift, super_shift) = script_shifts(base, subscript, superscript, constants, is_cramped);

    let mut ascent = base.ascent;
    let mut descent = base.descent;
    let mut positions = vec![ChildPosition::default()];
    let mut inline_size = base.inline_size;
    if let (Some(subscript), Some(sub)) = (subscript, sub_shift) {
        ascent = ascent.max(subscript.ascent - sub);
        descent = descent.max(subscript.descent + sub);
        positions.push(ChildPosition {
            line_left_offset: base.inline_size,
            baseline_shift: sub,
        });
        inline_size = inline_size.max(base.inline_size + subscript.inline_size + space_after_script);
    }
    if let (Some(superscript), Some(sup)) = (superscript, super_shift) {
        ascent = ascent.max(superscript.ascent + sup);
        descent = descent.max(superscript.descent - sup);
        positions.push(ChildPosition {
            line_left_offset: base.inline_size,
            baseline_shift: -sup,
        });
        inline_size = inline_size.max(base.inline_size + superscript.inline_size + space_after_script);
    }
    MathContent {
        inline_size,
        ascent,
        descent,
        positions,
    }
}

// https://w3c.github.io/mathml-core/#base-with-prescripts-and-postscripts
fn layout_multiscripts(
    children: &[Child],
    separator: Option<usize>,
    constants: &MathConstants,
    is_cramped: bool,
) -> MathContent {
    // FIXME: Use the italic correction of the postscripts' base.
    let space_after_script = constants.space_after_script();
    let base = &children[0];
    let postscripts = &children[1..separator.unwrap_or(children.len())];
    let prescripts = separator.map_or(&children[0..0], |separator| &children[separator + 1..]);

    // SubShift (respectively SuperShift) is calculated by taking the maximum of all subshifts (respectively
    // supershifts) of each subscript/superscript pair as described in <msubsup>.
    let mut sub_shift = CssPixels::default();
    let mut super_shift = CssPixels::default();
    for pair in postscripts.chunks(2).chain(prescripts.chunks(2)) {
        let (pair_sub_shift, pair_super_shift) =
            script_shifts(base, Some(&pair[0]), Some(&pair[1]), constants, is_cramped);
        sub_shift = sub_shift.max(pair_sub_shift.unwrap_or_default());
        super_shift = super_shift.max(pair_super_shift.unwrap_or_default());
    }

    // The line-ascent (respectively line-descent) of the math content is calculated by taking the maximum of all the
    // line-ascent (respectively line-descent) of each subscript/superscript pair as described in <msubsup> but using
    // the SubShift and SuperShift values calculated above.
    let mut ascent = base.ascent;
    let mut descent = base.descent;
    for pair in postscripts.chunks(2).chain(prescripts.chunks(2)) {
        ascent = ascent.max(pair[0].ascent - sub_shift).max(pair[1].ascent + super_shift);
        descent = descent
            .max(pair[0].descent + sub_shift)
            .max(pair[1].descent - super_shift);
    }

    // Finally, the placement of the in-flow children is performed using the following algorithm:
    let mut positions = vec![ChildPosition::default(); children.len()];
    // Set inline-offset to 0.
    let mut inline_offset = CssPixels::default();
    // For each subscript/superscript pair of mmultiscripts prescripts:
    if let Some(separator) = separator {
        for (pair_index, pair) in prescripts.chunks(2).enumerate() {
            // Increment inline-offset by SpaceAfterScript.
            inline_offset += space_after_script;
            // Set pair-inline-size to the maximum of the inline size of the subscript's margin box and the inline size
            // of the superscript's margin box.
            let pair_inline_size = pair[0].inline_size.max(pair[1].inline_size);
            // Place the subscript (respectively superscript) at inline-start position inline-offset +
            // pair-inline-size − the inline size of the subscript's (respectively superscript's) margin box, so its
            // alphabetic baseline is shifted away from the alphabetic baseline by SubShift (respectively SuperShift)
            // towards the line-under (respectively line-over).
            let index = separator + 1 + pair_index * 2;
            positions[index] = ChildPosition {
                line_left_offset: inline_offset + pair_inline_size - pair[0].inline_size,
                baseline_shift: sub_shift,
            };
            positions[index + 1] = ChildPosition {
                line_left_offset: inline_offset + pair_inline_size - pair[1].inline_size,
                baseline_shift: -super_shift,
            };
            // Increment inline-offset by pair-inline-size.
            inline_offset += pair_inline_size;
        }
        // Place the mmultiscripts base and <mprescripts> boxes at inline offsets inline-offset and with their
        // alphabetic baselines aligned with the alphabetic baseline.
        positions[separator].line_left_offset = inline_offset;
    }
    positions[0].line_left_offset = inline_offset;
    // Increment inline-offset by the inline size of the mmultiscripts base's margin box.
    inline_offset += base.inline_size;
    // For each subscript/superscript pair of mmultiscripts postscripts:
    for (pair_index, pair) in postscripts.chunks(2).enumerate() {
        // Set pair-inline-size to the maximum of the inline size of the subscript's margin box and the inline size of
        // the superscript's margin box.
        let pair_inline_size = pair[0].inline_size.max(pair[1].inline_size);
        // Place the subscript at inline-start position inline-offset − LargeOpItalicCorrection, and the superscript
        // at inline-start position inline-offset + ItalicCorrection.
        let index = 1 + pair_index * 2;
        positions[index] = ChildPosition {
            line_left_offset: inline_offset,
            baseline_shift: sub_shift,
        };
        positions[index + 1] = ChildPosition {
            line_left_offset: inline_offset,
            baseline_shift: -super_shift,
        };
        // Increment inline-offset by pair-inline-size and SpaceAfterScript.
        inline_offset += pair_inline_size + space_after_script;
    }
    MathContent {
        inline_size: inline_offset,
        ascent,
        descent,
        positions,
    }
}

/// What the base of an <munder>, <mover> or <munderover> is, which selects the parameters the scripts are placed with.
#[derive(Clone, Copy, PartialEq, Eq)]
enum UnderOverBase {
    /// An embellished operator with the largeop property.
    LargeOperator,
    /// An embellished operator with the stretchy property and stretch axis inline.
    InlineStretchyOperator,
    Other,
}

// https://w3c.github.io/mathml-core/#underscripts-and-overscripts-munder-mover-munderover
fn layout_underover(
    base: &Child,
    underscript: Option<&Child>,
    overscript: Option<&Child>,
    constants: &MathConstants,
    base_kind: UnderOverBase,
    (accentunder, accent): (bool, bool),
) -> MathContent {
    // FIXME: Use the LargeOpItalicCorrection of the base and the TopAccentAttachment of the overscript.
    // Parameters UnderShift and UnderExtraDescender are determined by considering three cases in the following order:
    // NB: For the first two cases, UnderShift accounts for the line-ascent of the underscript, so it is the distance
    //     from the line-under edge of the base to the alphabetic baseline of the underscript.
    let under_parameters = |underscript: &Child| match base_kind {
        // 1. The munder base is an embellished operator with the largeop property. UnderShift is the maximum of
        //    - LowerLimitBaselineDropMin
        //    - LowerLimitGapMin + the ascent of the munder underscript.
        //    UnderExtraDescender is 0.
        UnderOverBase::LargeOperator => (
            constants
                .lower_limit_baseline_drop_min()
                .max(constants.lower_limit_gap_min() + underscript.ascent),
            CssPixels::default(),
        ),
        // 2. The munder base is an embellished operator with the stretchy property and stretch axis inline. UnderShift
        //    is the maximum of:
        //    - StretchStackBottomShiftDown
        //    - StretchStackGapAboveMin + the ascent of the munder underscript.
        //    UnderExtraDescender is 0.
        UnderOverBase::InlineStretchyOperator => (
            constants
                .stretch_stack_bottom_shift_down()
                .max(constants.stretch_stack_gap_above_min() + underscript.ascent),
            CssPixels::default(),
        ),
        // 3. Otherwise, UnderShift is equal to UnderbarVerticalGap if the accentunder attribute is not an ASCII
        //    case-insensitive match to true and to zero otherwise. UnderExtraAscender is UnderbarExtraDescender.
        // AD-HOC: The underscript's margin box is placed UnderShift below the base's margin box, so that UnderShift is
        //         the gap between them, rather than placing the alphabetic baseline of the underscript UnderShift
        //         below the line-under edge of the base, which would make it overlap the base.
        UnderOverBase::Other => (
            if accentunder {
                CssPixels::default()
            } else {
                constants.underbar_vertical_gap()
            } + underscript.ascent,
            constants.underbar_extra_descender(),
        ),
    };
    // Parameters OverShift and OverExtraDescender are determined by considering three cases in the following order:
    // NB: For the first two cases, OverShift accounts for the line-descent of the overscript, so it is the distance
    //     from the line-over edge of the base to the alphabetic baseline of the overscript.
    let over_parameters = |overscript: &Child| match base_kind {
        // 1. The mover base is an embellished operator with the largeop property. OverShift is the maximum of
        //    - UpperLimitBaselineRiseMin
        //    - UpperLimitGapMin + the descent of the mover overscript.
        //    OverExtraAscender is 0.
        UnderOverBase::LargeOperator => (
            constants
                .upper_limit_baseline_rise_min()
                .max(constants.upper_limit_gap_min() + overscript.descent),
            CssPixels::default(),
        ),
        // 2. The mover base is an embellished operator with the stretchy property and stretch axis inline. OverShift
        //    is the maximum of:
        //    - StretchStackTopShiftUp
        //    - StretchStackGapBelowMin + the descent of the mover overscript.
        //    OverExtraDescender is 0.
        UnderOverBase::InlineStretchyOperator => (
            constants
                .stretch_stack_top_shift_up()
                .max(constants.stretch_stack_gap_below_min() + overscript.descent),
            CssPixels::default(),
        ),
        // 3. Otherwise, OverShift is equal to OverbarVerticalGap if the accent attribute is not an ASCII
        //    case-insensitive match to true. Or AccentBaseHeight minus the line-ascent of the mover base's margin box
        //    if this difference is nonnegative. Or 0 otherwise. OverExtraAscender is OverbarExtraAscender.
        // AD-HOC: The overscript's margin box is placed OverShift above the base's margin box, so that OverShift is
        //         the gap between them, rather than placing the alphabetic baseline of the overscript OverShift above
        //         the line-over edge of the base, which would make it overlap the base.
        UnderOverBase::Other => (
            if accent {
                (constants.accent_base_height() - base.ascent).max(CssPixels::default())
            } else {
                constants.overbar_vertical_gap()
            } + overscript.descent,
            constants.overbar_extra_ascender(),
        ),
    };

    // NB: The children are centered on the same vertical axis.
    let inline_size = [Some(base), underscript, overscript]
        .into_iter()
        .flatten()
        .map(|child| child.inline_size)
        .max()
        .unwrap_or_default();
    let centered = |child: &Child| (inline_size - child.inline_size) / 2;

    let mut ascent = base.ascent;
    let mut descent = base.descent;
    let mut positions = vec![ChildPosition {
        line_left_offset: centered(base),
        baseline_shift: CssPixels::default(),
    }];
    if let Some(underscript) = underscript {
        // The alphabetic baseline of the munder underscript is shifted away from the alphabetic baseline and towards
        // the line-under by a distance equal to the ink line-descent of the munder base's margin box + UnderShift.
        let (under_shift, under_extra_descender) = under_parameters(underscript);
        let shift = base.descent + under_shift;
        descent = descent.max(shift + underscript.descent + under_extra_descender);
        positions.push(ChildPosition {
            line_left_offset: centered(underscript),
            baseline_shift: shift,
        });
    }
    if let Some(overscript) = overscript {
        // The alphabetic baseline of the mover overscript is shifted away from the alphabetic baseline and towards the
        // line-over by a distance equal to the ink line-ascent of the base + OverShift.
        let (over_shift, over_extra_ascender) = over_parameters(overscript);
        let shift = base.ascent + over_shift;
        ascent = ascent.max(shift + overscript.ascent + over_extra_ascender);
        positions.push(ChildPosition {
            line_left_offset: centered(overscript),
            baseline_shift: -shift,
        });
    }
    MathContent {
        inline_size,
        ascent,
        descent,
        positions,
    }
}

/// The radical glyph of an <msqrt> or <mroot> and the radical parameters of its font.
// https://w3c.github.io/mathml-core/#radical-symbol
struct RadicalMetrics {
    glyph_advance: CssPixels,
    glyph_ascent: CssPixels,
    glyph_descent: CssPixels,
    gap: CssPixels,
    rule_thickness: CssPixels,
    extra_ascender: CssPixels,
}

impl RadicalMetrics {
    fn new(style: &StyleValues<'_>, constants: &MathConstants) -> Self {
        // The radical glyph is the glyph obtained as a result of running get a glyph corresponding to the U+221A
        // SQUARE ROOT character given dir.
        // FIXME: Stretch the radical glyph to the radical target size, and mirror it for dir rtl.
        const SQUARE_ROOT: u32 = 0x221A;
        let font = style.frozen_font_list().font_for_code_point(
            SQUARE_ROOT,
            libgfx_rust::font::emoji_presentation_for_code_point(SQUARE_ROOT, None),
        );
        let facts = font.facts();
        // The radical gap is given by RadicalVerticalGap if the math-style is compact and
        // RadicalDisplayStyleVerticalGap if the math-style is normal.
        let gap = if style.math_style() == math_style::NORMAL {
            constants.radical_display_style_vertical_gap()
        } else {
            constants.radical_vertical_gap()
        };
        Self {
            glyph_advance: CssPixels::nearest_value_for_f32(font.glyph_width(SQUARE_ROOT)),
            glyph_ascent: CssPixels::nearest_value_for_f32(facts.ascent),
            glyph_descent: CssPixels::nearest_value_for_f32(facts.descent),
            gap,
            rule_thickness: constants.radical_rule_thickness(),
            extra_ascender: constants.radical_extra_ascender(),
        }
    }
}

// https://w3c.github.io/mathml-core/#square-root
fn layout_square_root(base: MathContent, radical: &RadicalMetrics) -> MathContent {
    // The inline size of the math content is the sum of the advance width of the box metrics of the radical glyph
    // and of the inline size of the msqrt base's margin's box.
    let inline_size = radical.glyph_advance + base.inline_size;
    // The line-ascent of the math content is the maximum between:
    // - The line-ascent of the msqrt base's margin box.
    // - The sum of the ink line-ascent of the msqrt base, the radical gap, the RadicalRuleThickness and
    //   RadicalExtraAscender.
    let ascent = base
        .ascent
        .max(base.ascent + radical.gap + radical.rule_thickness + radical.extra_ascender);
    // The line-descent of the math content is the maximum between:
    // - The line-descent of the msqrt base's margin box.
    // - The sum of the line-ascent and line-descent for the box metrics of the radical glyph and of the
    //   RadicalExtraAscender, minus the line-ascent of the math content.
    let descent = base
        .descent
        .max(radical.glyph_ascent + radical.glyph_descent + radical.extra_ascender - ascent);
    // The inline offsets of the msqrt base and overbar are also the same and equal to the width of the box metrics of
    // the radical glyph.
    let positions = base
        .positions
        .into_iter()
        .map(|position| ChildPosition {
            line_left_offset: position.line_left_offset + radical.glyph_advance,
            ..position
        })
        .collect();
    MathContent {
        inline_size,
        ascent,
        descent,
        positions,
    }
}

// https://w3c.github.io/mathml-core/#root-with-index
fn layout_root(base: &Child, index: &Child, radical: &RadicalMetrics, constants: &MathConstants) -> MathContent {
    // The mroot index is first ignored and the mroot base and radical glyph are laid out as shown on figure using the
    // same algorithm as in <msqrt> in order to produce a margin box B.
    let b = layout_square_root(
        MathContent {
            inline_size: base.inline_size,
            ascent: base.ascent,
            descent: base.descent,
            positions: vec![ChildPosition::default()],
        },
        radical,
    );
    let b_block_size = b.ascent + b.descent;
    let kern_before_degree = constants.radical_kern_before_degree();
    let kern_after_degree = constants.radical_kern_after_degree();
    let degree_bottom_raise = b_block_size.scaled(constants.radical_degree_bottom_raise_percent());
    // AdjustedRadicalKernBeforeDegree and AdjustedRadicalKernAfterDegree are respectively defined as
    // max(0, RadicalKernBeforeDegree) and max(−inline size of the index's margin box, RadicalKernAfterDegree).
    let adjusted_kern_before_degree = kern_before_degree.max(CssPixels::default());
    let adjusted_kern_after_degree = kern_after_degree.max(-index.inline_size);
    // The inline size of the math content is the sum of AdjustedRadicalKernBeforeDegree, the inline size of the
    // index's margin box, AdjustedRadicalKernAfterDegree and of the inline size of B.
    let inline_size = adjusted_kern_before_degree + index.inline_size + adjusted_kern_after_degree + b.inline_size;
    // The alphabetic baseline of the index is shifted away from the line-under edge by a distance of
    // RadicalDegreeBottomRaisePercent × the block size of B + the line-descent of the index's margin box.
    let index_baseline_shift = b.descent - degree_bottom_raise - index.descent;
    // NB: The line-ascent and line-descent of the math content are those of the union of B and the index.
    let ascent = b.ascent.max(index.ascent - index_baseline_shift);
    let descent = b.descent.max(index.descent + index_baseline_shift);
    // The inline offset of the index is AdjustedRadicalKernBeforeDegree. The inline-offset of the mroot base is the
    // same + the inline size of the index's margin box.
    // NB: The base also follows the radical glyph and AdjustedRadicalKernAfterDegree.
    let b_offset = adjusted_kern_before_degree + index.inline_size + adjusted_kern_after_degree;
    MathContent {
        inline_size,
        ascent,
        descent,
        positions: vec![
            ChildPosition {
                line_left_offset: b_offset + b.positions[0].line_left_offset,
                baseline_shift: CssPixels::default(),
            },
            ChildPosition {
                line_left_offset: adjusted_kern_before_degree,
                baseline_shift: index_baseline_shift,
            },
        ],
    }
}
