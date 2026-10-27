/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// https://w3c.github.io/mathml-core/#layout-algorithms

use super::*;
use crate::layout::formatting_context::ChildLayoutResult;
use crate::layout::geometry::MathStretchSizeConstraint;
use libgfx_rust::font::FontHandle;
use libgfx_rust::math_table::{FfiGlyphMetrics, FfiMathData, FfiMathGlyphPart, MathConstant, StretchAxis};

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
    /// The fraction bars and radical overbars, positioned like the children.
    rules: Vec<MathRule>,
    /// The radical symbols and stretched operator glyphs, positioned like the children.
    glyph_runs: Vec<PlacedGlyphRun>,
}

impl MathContent {
    /// The rules and glyphs of the math content, positioned in the content box.
    fn paint_data(
        &self,
        math_content_line_left: CssPixels,
        content_box_inline_size: CssPixels,
        is_rtl: bool,
    ) -> MathPaintData {
        let content = self;
        let line_left_in_content_box = |line_left_offset: CssPixels, inline_size: CssPixels| {
            math_content_line_left
                + if is_rtl {
                    content.inline_size - line_left_offset - inline_size
                } else {
                    line_left_offset
                }
        };
        let rules = content
            .rules
            .iter()
            .map(|rule| {
                let (x, width) = if rule.spans_content_box {
                    (CssPixels::default(), content_box_inline_size)
                } else {
                    (
                        line_left_in_content_box(rule.line_left_offset, rule.inline_size),
                        rule.inline_size,
                    )
                };
                CssPixelRect::new(x, content.ascent + rule.baseline_shift, width, rule.block_size)
            })
            .collect();
        let glyph_runs = content
            .glyph_runs
            .iter()
            .map(|placed| {
                let run = &placed.glyphs;
                let origin_x = line_left_in_content_box(placed.line_left_offset, run.advance);
                let baseline = content.ascent + placed.baseline_shift;
                MathGlyphRun {
                    font: run.font.clone(),
                    glyphs: run
                        .glyphs
                        .iter()
                        .map(|glyph| MathGlyph {
                            x: origin_x.to_float() + glyph.x,
                            y: baseline.to_float() + glyph.y,
                            glyph_id: glyph.glyph_id,
                        })
                        .collect(),
                    ink_bounds: CssPixelRect::new(
                        origin_x,
                        baseline - run.ascent,
                        run.advance,
                        run.ascent + run.descent,
                    ),
                }
            })
            .collect();
        MathPaintData {
            rules,
            glyph_runs,
            replaces_text: false,
        }
    }

    /// The content with its children, rules and glyphs moved by the given line-left offset and baseline shift.
    fn translated(self, line_left_offset: CssPixels, baseline_shift: CssPixels) -> Self {
        Self {
            positions: self
                .positions
                .into_iter()
                .map(|position| ChildPosition {
                    line_left_offset: position.line_left_offset + line_left_offset,
                    baseline_shift: position.baseline_shift + baseline_shift,
                })
                .collect(),
            rules: self
                .rules
                .into_iter()
                .map(|rule| MathRule {
                    line_left_offset: rule.line_left_offset + line_left_offset,
                    baseline_shift: rule.baseline_shift + baseline_shift,
                    ..rule
                })
                .collect(),
            glyph_runs: self
                .glyph_runs
                .into_iter()
                .map(|run| PlacedGlyphRun {
                    line_left_offset: run.line_left_offset + line_left_offset,
                    baseline_shift: run.baseline_shift + baseline_shift,
                    ..run
                })
                .collect(),
            ..self
        }
    }
}

/// A rule that the box paints: its line-left offset and the offset of its line-over edge from the alphabetic
/// baseline of the math content towards the line-under.
#[derive(Clone, Copy)]
struct MathRule {
    line_left_offset: CssPixels,
    baseline_shift: CssPixels,
    inline_size: CssPixels,
    block_size: CssPixels,
    /// Whether the rule spans the content box along the inline axis instead, like a fraction bar.
    spans_content_box: bool,
}

/// Glyphs that the box paints, with their origin at a line-left offset, on a baseline shifted away from the
/// alphabetic baseline of the math content towards the line-under.
struct PlacedGlyphRun {
    line_left_offset: CssPixels,
    baseline_shift: CssPixels,
    glyphs: GlyphRun,
}

/// Glyphs of one font, positioned relative to their origin, with the box metrics they cover.
#[derive(Clone)]
struct GlyphRun {
    font: FontHandle,
    /// The glyph ids, with the offsets of their origins from the origin of the run.
    glyphs: Vec<MathGlyph>,
    advance: CssPixels,
    ascent: CssPixels,
    descent: CssPixels,
}

impl GlyphRun {
    /// A single glyph, with the box metrics of its ink.
    fn for_glyph(font: FontHandle, glyph_id: u32) -> Self {
        let metrics = font.glyph_metrics(glyph_id);
        Self {
            font,
            glyphs: vec![MathGlyph {
                x: 0.0,
                y: 0.0,
                glyph_id,
            }],
            advance: CssPixels::nearest_value_for_f32(metrics.advance),
            ascent: CssPixels::nearest_value_for_f32(metrics.ascent),
            descent: CssPixels::nearest_value_for_f32(metrics.descent),
        }
    }
}

/// The size of a glyph along a stretch axis: its advance width along the inline axis, and its height along the block
/// axis.
fn glyph_size_along(metrics: &FfiGlyphMetrics, axis: StretchAxis) -> f32 {
    match axis {
        StretchAxis::Inline => metrics.advance,
        StretchAxis::Block => metrics.ascent + metrics.descent,
    }
}

/// The maximum number of glyphs in a shaped glyph assembly.
const MAXIMUM_ASSEMBLY_GLYPH_COUNT: usize = u16::MAX as usize;

/// A GlyphAssembly table, with the quantities that building an assembly from it relies on.
// https://w3c.github.io/mathml-core/#the-glyphassembly-table
struct GlyphAssembly {
    assembly: libgfx_rust::math_table::MathGlyphAssembly,
    axis: StretchAxis,
    /// omin, MATH.MathVariant.minConnectorOverlap.
    minimum_overlap: f32,
    extender_count: usize,
    non_extender_count: usize,
    extender_advance_sum: f32,
    non_extender_advance_sum: f32,
}

impl GlyphAssembly {
    /// The GlyphAssembly table of a glyph along an axis, if it is valid.
    fn new(font: &FontHandle, assembly: libgfx_rust::math_table::MathGlyphAssembly, axis: StretchAxis) -> Option<Self> {
        let minimum_overlap = font.math_data().min_connector_overlap(axis);
        let (extenders, non_extenders): (Vec<&FfiMathGlyphPart>, Vec<&FfiMathGlyphPart>) =
            assembly.parts.iter().partition(|part| part.is_extender);
        let glyph_assembly = Self {
            minimum_overlap,
            extender_count: extenders.len(),
            non_extender_count: non_extenders.len(),
            extender_advance_sum: extenders.iter().map(|part| part.full_advance).sum(),
            non_extender_advance_sum: non_extenders.iter().map(|part| part.full_advance).sum(),
            assembly,
            axis,
        };
        // User agents must treat the GlyphAssembly as invalid if the following conditions are not satisfied:
        // - NExt > 0. Otherwise, the assembly cannot be grown by repeating extenders.
        // - SExt,NonOverlapping > 0. Otherwise, the assembly does not grow when joining extenders.
        // - For each GlyphPartRecord in GlyphAssembly.partRecords, the values of GlyphPartRecord.startConnectorLength
        //   and GlyphPartRecord.endConnectorLength must be at least omin. Otherwise, it is not possible to satisfy
        //   the condition of MathVariant.minConnectorOverlap.
        // INTEROP: The start connector of the first part and the end connector of the last part are not checked
        //         unless the part is an extender, as they connect to no other part. Other browsers accept such
        //         assemblies, which fonts commonly have.
        let parts = &glyph_assembly.assembly.parts;
        let is_valid = glyph_assembly.extender_count > 0
            && glyph_assembly.non_overlapping_extender_advance_sum() > 0.0
            && parts.iter().enumerate().all(|(index, part)| {
                let start_connects = index > 0 || part.is_extender;
                let end_connects = index + 1 < parts.len() || part.is_extender;
                (!start_connects || part.start_connector_length >= minimum_overlap)
                    && (!end_connects || part.end_connector_length >= minimum_overlap)
            });
        is_valid.then_some(glyph_assembly)
    }

    /// SExt,NonOverlapping = SExt − omin NExt is the sum of maximum non overlapping parts of extenders.
    fn non_overlapping_extender_advance_sum(&self) -> f32 {
        self.extender_advance_sum - self.minimum_overlap * self.extender_count as f32
    }

    /// AssemblyGlyphCount(r) = NNonExt + r NExt
    fn glyph_count(&self, repetitions: usize) -> usize {
        self.non_extender_count + repetitions * self.extender_count
    }

    /// AssembySize(o, r) = SNonExt + r SExt − o (AssemblyGlyphCount(r) − 1)
    fn size(&self, overlap: f32, repetitions: usize) -> f32 {
        self.non_extender_advance_sum + repetitions as f32 * self.extender_advance_sum
            - overlap * (self.glyph_count(repetitions) as f32 - 1.0)
    }

    /// rmin and omax for a target size, or `None` if the assembly would have more than
    /// `MAXIMUM_ASSEMBLY_GLYPH_COUNT` glyphs.
    fn repetitions_and_overlap(&self, target: f32) -> Option<(usize, f32)> {
        // rmin is the minimal number of repetitions needed to obtain an assembly of size at least T, i.e. the minimal
        // r such that AssembySize(omin, r) ≥ T. It is defined as the maximum between 0 and the ceiling of
        // ((T − SNonExt + omin (NNonExt − 1)) / SExt,NonOverlapping).
        let repetitions = ((target - self.non_extender_advance_sum
            + self.minimum_overlap * (self.non_extender_count as f32 - 1.0))
            / self.non_overlapping_extender_advance_sum())
        .ceil()
        .max(0.0);
        // AD-HOC: The target size and the font data are untrusted, so an assembly that would need too many glyphs is
        //         not built.
        let maximum_repetitions =
            (MAXIMUM_ASSEMBLY_GLYPH_COUNT.checked_sub(self.non_extender_count)? / self.extender_count) as f32;
        if repetitions > maximum_repetitions {
            return Option::None;
        }
        let repetitions = repetitions as usize;
        // omax is the maximum overlap possible to build an assembly of size at least T by repeating each extender
        // rmin times. If AssemblyGlyphCount(rmin) ≤ 1, then the actual overlap value is irrelevant. Otherwise, omax is
        // defined to be the minimum of:
        let glyph_count = self.glyph_count(repetitions);
        if glyph_count <= 1 {
            return Some((repetitions, 0.0));
        }
        // - omax,theorical = (AssembySize(0, rmin) − T) / (AssemblyGlyphCount(rmin) − 1)
        let mut overlap = (self.size(0.0, repetitions) - target) / (glyph_count as f32 - 1.0);
        let parts = &self.assembly.parts;
        for (index, part) in parts.iter().enumerate() {
            // - GlyphPartRecord.startConnectorLength for all the entries in GlyphAssembly.partRecords, excluding the
            //   last one if it is not an extender.
            if index > 0 || part.is_extender {
                overlap = overlap.min(part.start_connector_length);
            }
            // - GlyphPartRecord.endConnectorLength for all the entries in GlyphAssembly.partRecords, excluding the
            //   first one if it is not an extender.
            if index + 1 < parts.len() || part.is_extender {
                overlap = overlap.min(part.end_connector_length);
            }
        }
        // INTEROP: A part connects to the next part with its end connector and to the previous part with its start
        //          connector, so the connectors that are left out above are swapped: the first part's start connector
        //          and the last part's end connector connect to no part. Other browsers leave out those two connectors
        //          too.
        Some((repetitions, overlap))
    }

    /// The glyph assembly shaped for a target size, or `None` if it would have too many glyphs.
    // https://w3c.github.io/mathml-core/#dfn-shaping-of-the-glyph-assembly
    fn shape(&self, font: &FontHandle, target: f32) -> Option<GlyphRun> {
        // 1. Calculate rmin and omax.
        let (repetitions, overlap) = self.repetitions_and_overlap(target)?;
        // The glyph assembly stretch size for a target size T is AssembySize(omax, rmin).
        let stretch_size = self.size(overlap, repetitions);
        // 2. Set (x, y) to (0, 0), RepetitionCounter to 0 and PartIndex to -1.
        // 3. Repeat the following steps:
        // NB: The y coordinate grows towards the line-under here, so the assembly grows upwards by decreasing it.
        let mut x = 0.0;
        let mut y = 0.0;
        let mut glyphs = Vec::with_capacity(self.glyph_count(repetitions));
        let mut max_advance: f32 = 0.0;
        let mut max_ascent: f32 = 0.0;
        let mut max_descent: f32 = 0.0;
        for part in &self.assembly.parts {
            // 1. If RepetitionCounter is 0: Increment PartIndex. If PartIndex is GlyphAssembly.partCount then stop.
            //    Otherwise, set Part to GlyphAssembly.partRecords[PartIndex]. Set RepetitionCounter to rmin if Part is
            //    an extender and to 1 otherwise.
            let repetition_count = if part.is_extender { repetitions } else { 1 };
            let metrics = font.glyph_metrics(part.glyph_id);
            max_advance = max_advance.max(metrics.advance);
            max_ascent = max_ascent.max(metrics.ascent);
            max_descent = max_descent.max(metrics.descent);
            for _ in 0..repetition_count {
                match self.axis {
                    // 2. If the glyph assembly is horizontal then draw the glyph of ID Part.glyphID so that its
                    //    (left, baseline) coordinates are at position (x, y). Set x to x + Part.fullAdvance − omax.
                    StretchAxis::Inline => {
                        glyphs.push(MathGlyph {
                            x,
                            y,
                            glyph_id: part.glyph_id,
                        });
                        x += part.full_advance - overlap;
                    }
                    // 3. Otherwise (if the glyph assembly is vertical), then draw the glyph of id Part.glyphID so that
                    //    its (left, bottom) coordinates are at position (x, y). Set y to y − Part.fullAdvance + omax.
                    StretchAxis::Block => {
                        glyphs.push(MathGlyph {
                            x,
                            y: y - metrics.descent,
                            glyph_id: part.glyph_id,
                        });
                        y -= part.full_advance - overlap;
                    }
                }
                // 4. Decrement RepetitionCounter.
            }
        }
        // If GlyphAssembly is vertical, the width is the maximum advance width of the glyphs of ID
        // GlyphPartRecord.glyphID for all the GlyphPartRecord in GlyphAssembly.partRecords, the ascent is the glyph
        // assembly stretch size for a given target size T and the descent is 0. Otherwise, the GlyphAssembly is
        // horizontal, the width is glyph assembly stretch size for a given target size T while the ascent
        // (respectively descent) is the maximum ascent (respectively descent) of the glyphs of ID
        // GlyphPartRecord.glyphID for all the GlyphPartRecord in GlyphAssembly.partRecords.
        let (advance, ascent, descent) = match self.axis {
            StretchAxis::Block => (max_advance, stretch_size, 0.0),
            StretchAxis::Inline => (stretch_size, max_ascent, max_descent),
        };
        Some(GlyphRun {
            font: font.clone(),
            glyphs,
            advance: CssPixels::nearest_value_for_f32(advance),
            ascent: CssPixels::nearest_value_for_f32(ascent),
            descent: CssPixels::nearest_value_for_f32(descent),
        })
    }
}

// https://w3c.github.io/mathml-core/#dfn-shape-a-stretchy-glyph
fn shape_stretchy_glyph(font: &FontHandle, glyph_id: u32, axis: StretchAxis, target: CssPixels) -> Option<GlyphRun> {
    let target = target.to_float();
    // FIXME: Keep the italic correction that comes with the glyph, for the scripts attached to the operator.
    let normal_shaping = |glyph_id: u32| GlyphRun::for_glyph(font.clone(), glyph_id);
    // 1. If there is not any MathGlyphConstruction table in the MathVariants.horizGlyphConstructionOffsets table
    //    (respectively MathVariants.vertGlyphConstructionOffsets table) for the given glyph then exit with failure.
    // NB: HarfBuzz reads an absent MathGlyphConstruction table like one without variants or assembly.
    if !font.math_data().has_math_table {
        return None;
    }
    let variants = font.math_glyph_variants(glyph_id, axis);
    let assembly = font.math_glyph_assembly(glyph_id, axis);
    if variants.is_empty() && assembly.is_none() {
        return None;
    }
    // 2. If the glyph's advance width (respectively height) is at least T then use normal shaping and bounding box
    //    for that glyph, the MathItalicsCorrectionInfo for that glyph as italic correction and exit with success.
    if glyph_size_along(&font.glyph_metrics(glyph_id), axis) >= target {
        return Some(normal_shaping(glyph_id));
    }
    // 3. Browse the list of MathGlyphVariantRecord in MathGlyphConstruction.mathGlyphVariantRecord. If one
    //    MathGlyphVariantRecord.advanceMeasurement is at least T then use normal shaping and bounding box for
    //    MathGlyphVariantRecord.variantGlyph, the MathItalicsCorrectionInfo for that glyph as italic correction and
    //    exit with success.
    if let Some(variant) = variants.iter().find(|variant| variant.advance_measurement >= target) {
        return Some(normal_shaping(variant.glyph_id));
    }
    // 4. If there is valid GlyphAssembly subtable then use the bounding box given by glyph assembly width, glyph
    //    assembly height, glyph assembly ascent, glyph assembly descent, the value GlyphAssembly.italicsCorrection as
    //    italic correction, perform shaping of the glyph assembly and exit with success.
    // NB: An assembly that would have too many glyphs is not used.
    if let Some(glyph_run) = assembly
        .and_then(|assembly| GlyphAssembly::new(font, assembly, axis))
        .and_then(|assembly| assembly.shape(font, target))
    {
        return Some(glyph_run);
    }
    // 5. If none of the stretch options above allowed to cover the target size T, then choose last one that was tried
    //    and exit with success.
    Some(normal_shaping(
        variants.last().map_or(glyph_id, |variant| variant.glyph_id),
    ))
}

/// What MathML layout produced for a box that painting draws: fraction bars, radical symbols and the glyphs of
/// stretched operators. Positions are relative to the top-left corner of the content box.
#[derive(Debug, PartialEq)]
pub(crate) struct MathPaintData {
    pub(crate) rules: Vec<CssPixelRect>,
    pub(crate) glyph_runs: Vec<MathGlyphRun>,
    /// Whether the glyphs are those of a stretched or large operator, which are painted instead of its text.
    pub(crate) replaces_text: bool,
}

#[derive(Debug, PartialEq)]
pub(crate) struct MathGlyphRun {
    pub(crate) font: FontHandle,
    /// The glyphs, each positioned by its origin on its baseline.
    pub(crate) glyphs: Vec<MathGlyph>,
    pub(crate) ink_bounds: CssPixelRect,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub(crate) struct MathGlyph {
    pub(crate) x: f32,
    pub(crate) y: f32,
    pub(crate) glyph_id: u32,
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

    // displayOperatorMinHeight: MATH.MathConstants.displayOperatorMinHeight or Default fallback constant if the
    // constant is not available.
    fn display_operator_min_height(&self) -> CssPixels {
        self.constant_or_zero(MathConstant::DisplayOperatorMinHeight)
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

/// The glyph that an <mo> is drawn with when it is stretched or a large operator.
struct OperatorGlyph {
    glyph: GlyphRun,
    /// Δ, how far the glyph is shifted away from the alphabetic baseline towards the line-under.
    baseline_shift: CssPixels,
}

/// Lays out an <mo> whose text was laid out in a block formatting context with the glyph it is drawn with instead,
/// if it is stretched or a large operator.
// https://w3c.github.io/mathml-core/#layout-of-operators
pub(super) fn layout_operator(run: &FormattingContextRun<'_>, input: &LayoutInput, result: &mut ChildLayoutResult) {
    let context = MathFormattingContext::new(run);
    let operator = run.box_;
    if context.kind(operator) != MathLayoutKind::Mo {
        return;
    }
    let Some(OperatorGlyph { glyph, baseline_shift }) =
        context.operator_glyph(operator, input.sizing.math_stretch_size_constraint)
    else {
        return;
    };
    let glyphs = glyph;
    let ascent = glyphs.ascent - baseline_shift;
    let descent = glyphs.descent + baseline_shift;
    let inline_size = glyphs.advance;
    let used = run.records.used_values(operator);
    // NB: The math content box sizes the content box, unless the width property gives it a size.
    if sizing_context::is_plain_auto(context.style(operator).width()) {
        used.content_inline_size.set(inline_size);
    }
    result.automatic_content_block_size = ascent + descent;
    result.baselines = DerivedBaselines {
        inline_alignment: None,
        first: Some(ascent),
        last: Some(ascent),
    };
    if run.fragments.is_none() {
        return;
    }
    let content = MathContent {
        inline_size,
        ascent,
        descent,
        glyph_runs: vec![PlacedGlyphRun {
            line_left_offset: CssPixels::default(),
            baseline_shift,
            glyphs,
        }],
        ..MathContent::default()
    };
    let is_rtl = context.style(operator).direction() == direction::RTL;
    let paint_data = MathPaintData {
        replaces_text: true,
        ..content.paint_data(CssPixels::default(), inline_size, is_rtl)
    };
    used.rare_data_mut().math_paint_data = Some(std::sync::Arc::new(paint_data));
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

    /// The properties of an operator, as the core operator of the outermost embellished operator it is the core
    /// operator of.
    fn core_operator_properties(&self, operator: Node) -> Option<OperatorProperties> {
        let callbacks = &self.run.callbacks;
        let lays_out_math = |node: Node| !node.is_invalid() && self.kind(node).uses_math_layout();
        let mut embellished_operator = operator;
        loop {
            let parent = callbacks.parent(embellished_operator);
            if !lays_out_math(parent) || self.core_operator(parent) != Some(operator) {
                break;
            }
            embellished_operator = parent;
        }
        let parent = callbacks.parent(embellished_operator);
        if lays_out_math(parent) {
            let siblings = self.in_flow_children_of(parent);
            if let Some(index) = siblings.iter().position(|&sibling| sibling == embellished_operator) {
                let parent_kind = self.kind(parent);
                let first_and_last_non_space_like = self.first_and_last_non_space_like(&siblings, parent_kind);
                return self.embellished_operator_properties(
                    &siblings,
                    index,
                    parent_kind,
                    first_and_last_non_space_like,
                );
            }
        }
        self.embellished_operator_properties(&[embellished_operator], 0, MathLayoutKind::NotMathML, Option::None)
    }

    /// The glyph an <mo> is drawn with, if it is stretched or a large operator.
    // https://w3c.github.io/mathml-core/#layout-of-operators
    fn operator_glyph(
        &self,
        operator: Node,
        stretch_size_constraint: Option<MathStretchSizeConstraint>,
    ) -> Option<OperatorGlyph> {
        // 1. If the content of the <mo> element is not made of a single character c then fall back to the layout
        //    algorithm of 3.2.1.1 Layout of <mtext>. If it is not possible to get a glyph corresponding to c given
        //    directionality dir, then fall back to the layout algorithm of 3.2.1.1 Layout of <mtext>. Otherwise, let g
        //    be the result of running get a glyph corresponding to c given directionality dir.
        let character = self.operator_character(operator)?;
        // FIXME: Mirror the glyph for dir rtl.
        let style = self.style(operator);
        let font = style.first_available_font();
        let glyph_id = font.glyph_id_for_code_point(character);
        if glyph_id == 0 {
            return Option::None;
        }
        // OPTIMIZATION: A stretchy glyph can only be shaped with a font that has a MATH table, so the properties of
        //               the operator are not looked up without one.
        if !font.math_data().has_math_table {
            return Option::None;
        }
        let properties = self.core_operator_properties(operator)?;
        let constants = MathConstants::for_style(&style);
        let axis_height = constants.axis_height();
        // 2. If the operator has the stretchy property:
        if properties.flags.stretchy {
            // - If the stretch axis of the operator is inline:
            if self.stretch_axis(operator) == StretchAxis::Inline {
                // 1. If it is not possible to shape a stretchy glyph g in the inline direction with the first available
                //    font then fall back to the layout algorithm of 3.2.1.1 Layout of <mtext>.
                // 2. The min-content inline size and max-content inline size of the math content are set to the one
                //    obtained by the layout algorithm of 3.2.1.1 Layout of <mtext>.
                // 3. If there is not any inline stretch size constraint Tinline then fall back to the layout algorithm
                //    of 3.2.1.1 Layout of <mtext>.
                let Some(MathStretchSizeConstraint::Inline(target)) = stretch_size_constraint else {
                    return Option::None;
                };
                // 4. The inline size and (ink) block metrics of the math content are given by algorithm to shape a
                //    stretchy glyph g to inline dimension Tinline.
                // 5. The painting of the operator is performed by the algorithm to shape a stretchy glyph g stretched
                //    to inline dimension Tinline and at position determined by the previous box metrics.
                let glyph = shape_stretchy_glyph(&font, glyph_id, StretchAxis::Inline, target)?;
                return Some(OperatorGlyph {
                    glyph,
                    baseline_shift: CssPixels::default(),
                });
            }
            // - Otherwise, the stretch axis of the operator is block. The following steps are performed:
            //    1. If it is not possible to shape a stretchy glyph g in the block direction with the first available
            //       font then fall back to the layout algorithm of 3.2.1.1 Layout of <mtext>.
            //    2. The min-content inline size and max-content inline size of the math content are set to the
            //       preferred inline size of a glyph stretched along the block axis.
            //    3. If there is not any block stretch size constraint (Uascent, Udescent) then fall back to the layout
            //       algorithm of 3.2.1.1 Layout of <mtext>.
            // FIXME: Use the preferred inline size of a glyph stretched along the block axis for the min-content and
            //        max-content inline sizes, rather than the inline size of the stretched glyph.
            let Some(
                MathStretchSizeConstraint::Block {
                    ascent: unconstrained_ascent,
                    descent: unconstrained_descent,
                }
                | MathStretchSizeConstraint::BlockCoveringContent {
                    ascent: unconstrained_ascent,
                    descent: unconstrained_descent,
                },
            ) = stretch_size_constraint
            else {
                return Option::None;
            };
            // 4. If the operator has the symmetric property then set the target sizes Tascent and Tdescent to Sascent
            //    and Sdescent respectively:
            //    Sascent = max(Uascent − AxisHeight, Udescent + AxisHeight) + AxisHeight
            //    Sdescent = max(Uascent − AxisHeight, Udescent + AxisHeight) − AxisHeight
            //    Otherwise set them to Uascent and Udescent respectively.
            let (mut target_ascent, mut target_descent) = if properties.flags.symmetric {
                let half_size = (unconstrained_ascent - axis_height).max(unconstrained_descent + axis_height);
                (half_size + axis_height, half_size - axis_height)
            } else {
                (unconstrained_ascent, unconstrained_descent)
            };
            // 5. Let minsize and maxsize be the minsize and maxsize properties on the operator. Percentage values are
            //    interpreted relative to the height of g. Let T = Tascent + Tdescent be the target size. If minsize < 0
            //    then set minsize to 0. If maxsize < minsize then set maxsize to minsize. With 0 ≤ minsize ≤
            //    maxsize:
            // NB: Font-relative lengths for minsize and maxsize rely on the font style of the core operator. The
            //     dictionary sets minsize to 100% and maxsize to ∞, which is represented by `None`.
            let attributes = self.run.callbacks.arena().math_attribute_facts(operator);
            let glyph_metrics = font.glyph_metrics(glyph_id);
            let glyph_height = CssPixels::nearest_value_for_f32(glyph_metrics.ascent + glyph_metrics.descent);
            let mut minsize = attributes.minsize.resolve(&style, glyph_height).unwrap_or(glyph_height);
            let mut maxsize = attributes.maxsize.resolve(&style, glyph_height);
            let target = target_ascent + target_descent;
            minsize = minsize.max(CssPixels::default());
            if let Some(size) = maxsize
                && size < minsize
            {
                maxsize = Some(minsize);
            }
            let scaled_target_ascent = |size: CssPixels| {
                CssPixels::nearest_value_for(
                    (f64::from((target_ascent - axis_height).to_float()) * f64::from(size.to_float())
                        / f64::from(target.to_float()))
                        + f64::from(axis_height.to_float()),
                )
                .max(CssPixels::default())
            };
            //    - If T ≤ 0 then set Tascent to minsize / 2 + AxisHeight and then set Tdescent to minsize − Tascent.
            if target <= CssPixels::default() {
                target_ascent = minsize / 2 + axis_height;
                target_descent = minsize - target_ascent;
            }
            //    - Otherwise, if 0 < T < minsize then set Tascent to max(0, (Tascent − AxisHeight) × minsize / T +
            //      AxisHeight) and Tdescent to minsize − Tascent.
            else if target < minsize {
                target_ascent = scaled_target_ascent(minsize);
                target_descent = minsize - target_ascent;
            }
            //    - Otherwise, if maxsize < T then set Tascent to max(0, (Tascent − AxisHeight) × maxsize / T +
            //      AxisHeight) and Tdescent to maxsize − Tascent.
            else if let Some(maxsize) = maxsize
                && maxsize < target
            {
                target_ascent = scaled_target_ascent(maxsize);
                target_descent = maxsize - target_ascent;
            }
            // 6. The inline size, ink line-ascent, ink line-descent, line-ascent and line-descent of the math content
            //    are obtained by the algorithm to shape a stretchy glyph g to block dimension Tascent + Tdescent.
            let glyph = shape_stretchy_glyph(&font, glyph_id, StretchAxis::Block, target_ascent + target_descent)?;
            //    The inline size of the math content is the width of the stretchy glyph. The stretchy glyph is shifted
            //    towards the line-under by a value Δ so that its center aligns with the center of the target: the ink
            //    ascent of the math content is the ascent of the stretchy glyph − Δ and the ink descent of the math
            //    content is the descent of the stretchy glyph + Δ. These centers have coordinates "½(ascent − descent)"
            //    so Δ = [(ascent of stretchy glyph − descent of stretchy glyph) − (Tascent − Tdescent)] / 2.
            let delta = ((glyph.ascent - glyph.descent) - (target_ascent - target_descent)) / 2;
            // 7. The painting of the operator is performed by the algorithm to shape a stretchy glyph g stretched to
            //    block dimension Tascent + Tdescent and at position determined by the previous box metrics shifted by Δ
            //    towards the line-over.
            return Some(OperatorGlyph {
                glyph,
                baseline_shift: delta,
            });
        }
        // 3. If the operator has the largeop property and if math-style on the <mo> element is normal, then:
        if properties.flags.largeop && style.math_style() == math_style::NORMAL {
            // 1. If it is not possible to shape a stretchy glyph g in the block direction with the first available
            //    font then fall back to the layout algorithm of 3.2.1.1 Layout of <mtext>.
            // 2. The min-content inline size and max-content inline size of the math content are set to the preferred
            //    inline size of a glyph stretched along the block axis.
            // 3. The inline size, ink line-ascent, ink line-descent, line-ascent and line-descent of the math content
            //    are obtained by the algorithm to shape a stretchy glyph g to block dimension DisplayOperatorMinHeight.
            //    The inline size of the math content is the width of the stretchy glyph. The stretchy glyph is shifted
            //    towards the line-under by a value Δ so that its center aligns with the center of the target when
            //    symmetric: the ink ascent of the math content is the ascent of the stretchy glyph − Δ and the ink
            //    descent of the math content is the descent of the stretchy glyph + Δ.
            //    - If the operator has the symmetric property, then Δ = [(ascent of stretchy glyph − descent of
            //      stretchy glyph) − 2 * AxisHeight] / 2.
            //    - Otherwise, Δ = 0.
            // NB: The min-content and max-content inline sizes are the inline size of the glyph, as it is shaped the
            //     same whatever the stretch size constraint.
            let glyph = shape_stretchy_glyph(
                &font,
                glyph_id,
                StretchAxis::Block,
                constants.display_operator_min_height(),
            )?;
            let delta = if properties.flags.symmetric {
                ((glyph.ascent - glyph.descent) - axis_height * 2usize) / 2
            } else {
                CssPixels::default()
            };
            // 4. The painting of the operator is performed by the algorithm to shape a stretchy glyph g stretched to
            //    block dimension DisplayOperatorMinHeight and at position determined by the previous box metrics
            //    shifted by Δ towards the line-over.
            return Some(OperatorGlyph {
                glyph,
                baseline_shift: delta,
            });
        }
        // 4. Otherwise fall back to the layout algorithm of 3.2.1.1 Layout of <mtext>.
        Option::None
    }

    /// Whether the box lays out its in-flow children like an <mrow>, as the layout algorithm its tag name selects
    /// falls back to that of <mrow> or generates an anonymous <mrow> box.
    fn lays_out_children_like_mrow(&self, kind: MathLayoutKind, children: &[Node]) -> bool {
        match (kind, children.len()) {
            (
                MathLayoutKind::Mfrac
                | MathLayoutKind::Msub
                | MathLayoutKind::Msup
                | MathLayoutKind::Munder
                | MathLayoutKind::Mover
                | MathLayoutKind::Mroot,
                2,
            )
            | (MathLayoutKind::Msubsup | MathLayoutKind::Munderover, 3)
            | (MathLayoutKind::Mspace, _) => false,
            (MathLayoutKind::Mmultiscripts, _) => !self.is_valid_multiscripts(children),
            _ => true,
        }
    }

    /// The index of the in-flow child that the core operator of the box belongs to, if the box is an embellished
    /// operator.
    fn embellished_operator_child(&self, children: &[Node]) -> Option<usize> {
        self.core_operator(self.run.box_)?;
        let kind = self.kind(self.run.box_);
        if kind.is_grouping_element() || kind == MathLayoutKind::Mpadded {
            return children.iter().position(|&child| self.core_operator(child).is_some());
        }
        Some(0)
    }

    /// Whether the in-flow child at `index` is an embellished operator with the stretchy property and the given
    /// stretch axis.
    fn is_stretchy_operator_along(
        &self,
        children: &[Node],
        index: usize,
        first_and_last_non_space_like: Option<(usize, usize)>,
        axis: StretchAxis,
    ) -> bool {
        self.embellished_operator_properties(children, index, self.kind(self.run.box_), first_and_last_non_space_like)
            .is_some_and(|properties| properties.flags.stretchy && self.stretch_axis(properties.core_operator) == axis)
    }

    /// Lays out the in-flow children, stretching the operators among them as the layout algorithm of the box asks.
    fn layout_children(
        &self,
        children: &[Node],
        kind: MathLayoutKind,
        constraints: ContainingBlockConstraints,
        stretch_size_constraint: Option<MathStretchSizeConstraint>,
    ) -> Vec<Child> {
        // https://w3c.github.io/mathml-core/#algorithm-for-stretching-operators-along-the-block-axis
        // 1. If there is a block stretch size constraint or an inline stretch size constraint then the element being
        //    laid out is an embellished operator. Lay out the one in-flow child that is an embellished operator with
        //    the same stretch size constraint and all the other in-flow children without any stretch size constraint
        //    and stop.
        // NB: The same goes for the other layout algorithms, which lay out their first in-flow child with the same
        //     stretch size constraint.
        if let Some(stretch_size_constraint) = stretch_size_constraint {
            let embellished_operator_child = self.embellished_operator_child(children);
            let mut laid_out: Vec<Option<Child>> = children
                .iter()
                .enumerate()
                .map(|(index, &child)| {
                    (Some(index) != embellished_operator_child)
                        .then(|| self.layout_child(child, constraints, Option::None))
                })
                .collect();
            if let Some(index) = embellished_operator_child {
                let constraint = match stretch_size_constraint {
                    MathStretchSizeConstraint::BlockCoveringContent { ascent, descent } => {
                        MathStretchSizeConstraint::BlockCoveringContent {
                            ascent: laid_out
                                .iter()
                                .flatten()
                                .map(|child| child.ascent)
                                .fold(ascent, CssPixels::max),
                            descent: laid_out
                                .iter()
                                .flatten()
                                .map(|child| child.descent)
                                .fold(descent, CssPixels::max),
                        }
                    }
                    constraint => constraint,
                };
                laid_out[index] = Some(self.layout_child(children[index], constraints, Some(constraint)));
            }
            return laid_out.into_iter().flatten().collect();
        }
        let is_underover = matches!(
            (kind, children.len()),
            (MathLayoutKind::Munder | MathLayoutKind::Mover, 2) | (MathLayoutKind::Munderover, 3)
        );
        let (axis, first_and_last_non_space_like) = if is_underover {
            (StretchAxis::Inline, Option::None)
        } else if self.lays_out_children_like_mrow(kind, children) {
            (StretchAxis::Block, self.first_and_last_non_space_like(children, kind))
        } else {
            return children
                .iter()
                .map(|&child| self.layout_child(child, constraints, Option::None))
                .collect();
        };
        // 2. Otherwise, split the list of in-flow children into a first list LToStretch containing embellished
        //    operators with a stretchy property and block stretch axis; and a second list LNotToStretch.
        // NB: The algorithm for stretching operators along the inline axis, which <munder>, <mover> and <munderover>
        //     use, has the same steps for operators with inline stretch axis, an inline stretch size constraint 0, and
        //     a target size T that is the maximum inline size of the margin boxes of the child boxes laid out.
        // https://w3c.github.io/mathml-core/#algorithm-for-stretching-operators-along-the-inline-axis
        let mut to_stretch: Vec<bool> = (0..children.len())
            .map(|index| self.is_stretchy_operator_along(children, index, first_and_last_non_space_like, axis))
            .collect();
        // 3. Perform layout without any stretch size constraint on all the items of LNotToStretch.
        let mut laid_out: Vec<Option<Child>> = children
            .iter()
            .zip(&to_stretch)
            .map(|(&child, &to_stretch)| (!to_stretch).then(|| self.layout_child(child, constraints, Option::None)))
            .collect();
        // 4. If LToStretch is empty then stop.
        if !to_stretch.contains(&true) {
            return laid_out.into_iter().flatten().collect();
        }
        // 5. If LNotToStretch is empty, perform layout with block stretch size constraint (0, 0) for all the items of
        //    LToStretch.
        // AD-HOC: A box is laid out once in a run, so the metrics the core operators of the items of LToStretch have
        //         with that stretch size constraint stand in for those of the items, which are then laid out with a
        //         stretch size constraint that also covers their other in-flow children. An operator without a
        //         stretched glyph keeps its unstretched layout.
        let mut metrics: Vec<(CssPixels, CssPixels, CssPixels)> = laid_out
            .iter()
            .flatten()
            .map(|child| (child.inline_size, child.ascent, child.descent))
            .collect();
        let not_to_stretch_is_empty = metrics.is_empty();
        if not_to_stretch_is_empty {
            let constraint = match axis {
                StretchAxis::Block => MathStretchSizeConstraint::Block {
                    ascent: CssPixels::default(),
                    descent: CssPixels::default(),
                },
                StretchAxis::Inline => MathStretchSizeConstraint::Inline(CssPixels::default()),
            };
            for (index, &child) in children.iter().enumerate() {
                let operator_glyph = self
                    .core_operator(child)
                    .and_then(|core_operator| self.operator_glyph(core_operator, Some(constraint)));
                match operator_glyph {
                    Some(OperatorGlyph { glyph, baseline_shift }) => metrics.push((
                        glyph.advance,
                        glyph.ascent - baseline_shift,
                        glyph.descent + baseline_shift,
                    )),
                    Option::None => {
                        let child = self.layout_child(child, constraints, Option::None);
                        metrics.push((child.inline_size, child.ascent, child.descent));
                        laid_out[index] = Some(child);
                        to_stretch[index] = false;
                    }
                }
            }
        }
        // 6. Calculate the unconstrained target sizes Uascent and Udescent as respectively the maximum ink ascent and
        //    maximum ink descent of the margin boxes of in-flow children that have been laid out in the previous step.
        let unconstrained_ascent = metrics.iter().map(|metrics| metrics.1).max().unwrap_or_default();
        let unconstrained_descent = metrics.iter().map(|metrics| metrics.2).max().unwrap_or_default();
        let constraint = match axis {
            StretchAxis::Block if not_to_stretch_is_empty => MathStretchSizeConstraint::BlockCoveringContent {
                ascent: unconstrained_ascent,
                descent: unconstrained_descent,
            },
            StretchAxis::Block => MathStretchSizeConstraint::Block {
                ascent: unconstrained_ascent,
                descent: unconstrained_descent,
            },
            StretchAxis::Inline => {
                MathStretchSizeConstraint::Inline(metrics.iter().map(|metrics| metrics.0).max().unwrap_or_default())
            }
        };
        // 7. Lay out or relayout all the elements of LToStretch with block stretch size constraint (Uascent,
        //    Udescent).
        for (index, &child) in children.iter().enumerate() {
            if to_stretch[index] {
                laid_out[index] = Some(self.layout_child(child, constraints, Some(constraint)));
            }
        }
        laid_out.into_iter().flatten().collect()
    }

    fn layout_child(
        &self,
        child: Node,
        constraints: ContainingBlockConstraints,
        stretch_size_constraint: Option<MathStretchSizeConstraint>,
    ) -> Child {
        let run = &self.run;
        run.records.create_used_values(&run.callbacks, child, constraints);
        // NB: MathML layout does not break lines, so each child is laid out at its max-content inline size, as an
        //     atomic box whose block size follows from its content.
        let mut input = LayoutInput::new(
            AvailableSpace {
                inline_size: AvailableSize::MaxContent,
                block_size: AvailableSize::Indefinite,
            },
            constraints,
            ParticipationInParentFormattingContext::AtomicInline,
        );
        input.sizing.math_stretch_size_constraint = stretch_size_constraint;
        let used = run.records.used_values(child);
        let baselines = match formatting_context::layout_inside_child(run, None, None, child, input, true) {
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
        let nodes = self.in_flow_children();
        let children = self.layout_children(
            &nodes,
            kind,
            constraints,
            layout_input.sizing.math_stretch_size_constraint,
        );

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
            (MathLayoutKind::Mmultiscripts, _) if self.is_valid_multiscripts(&nodes) => layout_multiscripts(
                &children,
                self.prescripts_separator_index(&nodes),
                &constants,
                is_cramped,
            ),
            (MathLayoutKind::Mroot, 2) => {
                let base = &children[0];
                let radical = RadicalMetrics::new(&root_style, &constants, base.ascent + base.descent);
                layout_root(base, &children[1], &radical, &constants)
            }
            (MathLayoutKind::Msqrt, _) => {
                // NB: The children of <msqrt> are laid out in an anonymous <mrow> box.
                let base = self.layout_row_with_operator_spacing(&children, kind, true);
                let radical = RadicalMetrics::new(&root_style, &constants, base.ascent + base.descent);
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
        let content_box_is_known = root_used.has_definite_inline_size();
        // NB: A MathML box measured for its content inline sizes takes the inline size of its math content.
        if !content_box_is_known {
            root_used.set_content_inline_size(content.inline_size);
        }
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

        if run.fragments.is_some() && (!content.rules.is_empty() || !content.glyph_runs.is_empty()) {
            let bar_inline_size = if content_box_is_known {
                content_box_inline_size
            } else {
                content.inline_size
            };
            let paint_data = content.paint_data(math_content_line_left, bar_inline_size, is_rtl);
            root_used.rare_data_mut().math_paint_data = Some(std::sync::Arc::new(paint_data));
        }

        self.derived_baselines_of_root_box = DerivedBaselines {
            inline_alignment: None,
            first: Some(content.ascent),
            last: Some(content.ascent),
        };
        self.content = content;
    }

    fn prescripts_separator_index(&self, children: &[Node]) -> Option<usize> {
        children
            .iter()
            .position(|&child| self.kind(child) == MathLayoutKind::Mprescripts)
    }

    // https://w3c.github.io/mathml-core/#prescripts-and-tensor-indices-mmultiscripts
    fn is_valid_multiscripts(&self, children: &[Node]) -> bool {
        // A valid <mmultiscripts> element contains the following in-flow children:
        // - A first in-flow child, called the mmultiscripts base, that is not an <mprescripts> element.
        // - Followed by an even number of in-flow children called mmultiscripts postscripts, none of them being a
        //   <mprescripts> element.
        // - Optionally followed by an <mprescripts> element and an even number of in-flow children called
        //   mmultiscripts prescripts, none of them being a <mprescripts> element.
        let separator_count = children
            .iter()
            .filter(|&&child| self.kind(child) == MathLayoutKind::Mprescripts)
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
            ..MathContent::default()
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

        // The inline size of the fraction bar is the inline size of the content box and its inline-start edge is the
        // aligned with the one the content box. The center of the fraction bar is shifted away from the alphabetic
        // baseline of the math content box by a distance of AxisHeight towards the line-over. Its block size is the
        // fraction line thickness.
        let fraction_bar = MathRule {
            line_left_offset: CssPixels::default(),
            baseline_shift: -(axis_height + half_thickness),
            inline_size,
            block_size: line_thickness,
            spans_content_box: true,
        };
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
            rules: vec![fraction_bar],
            ..MathContent::default()
        }
    }

    pub(super) fn parent_did_dimension(&self) {
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
        ..MathContent::default()
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
        ..MathContent::default()
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
        ..MathContent::default()
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
        ..MathContent::default()
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
        ..MathContent::default()
    }
}

/// The radical glyph of an <msqrt> or <mroot> and the radical parameters of its font.
// https://w3c.github.io/mathml-core/#radical-symbol
struct RadicalMetrics {
    glyph: GlyphRun,
    gap: CssPixels,
    rule_thickness: CssPixels,
    extra_ascender: CssPixels,
}

impl RadicalMetrics {
    fn new(style: &StyleValues<'_>, constants: &MathConstants, base_ink_height: CssPixels) -> Self {
        // The radical glyph is the glyph obtained as a result of running get a glyph corresponding to the U+221A
        // SQUARE ROOT character given dir.
        // FIXME: Mirror the radical glyph for dir rtl.
        const SQUARE_ROOT: u32 = 0x221A;
        let font = style.frozen_font_list().font_for_code_point(
            SQUARE_ROOT,
            libgfx_rust::font::emoji_presentation_for_code_point(SQUARE_ROOT, None),
        );
        let glyph_id = font.glyph_id_for_code_point(SQUARE_ROOT);
        // The radical gap is given by RadicalVerticalGap if the math-style is compact and
        // RadicalDisplayStyleVerticalGap if the math-style is normal.
        let gap = if style.math_style() == math_style::NORMAL {
            constants.radical_display_style_vertical_gap()
        } else {
            constants.radical_vertical_gap()
        };
        let rule_thickness = constants.radical_rule_thickness();
        // The radical target size for the stretchy radical glyph is the sum of RadicalRuleThickness, radical gap and
        // the ink height of the base.
        let target_size = rule_thickness + gap + base_ink_height;
        // The box metrics of the radical glyph and painting of the surd are given by the algorithm to shape a stretchy
        // glyph to the target size for the radical glyph in the block dimension.
        // NB: A glyph that cannot be stretched is used as it is.
        let glyph = shape_stretchy_glyph(&font, glyph_id, StretchAxis::Block, target_size)
            .unwrap_or_else(|| GlyphRun::for_glyph(font, glyph_id));
        Self {
            glyph,
            gap,
            rule_thickness,
            extra_ascender: constants.radical_extra_ascender(),
        }
    }
}

// https://w3c.github.io/mathml-core/#square-root
fn layout_square_root(base: MathContent, radical: &RadicalMetrics) -> MathContent {
    let glyph = &radical.glyph;
    // The inline size of the math content is the sum of the advance width of the box metrics of the radical glyph
    // and of the inline size of the msqrt base's margin's box.
    let inline_size = glyph.advance + base.inline_size;
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
        .max(glyph.ascent + glyph.descent + radical.extra_ascender - ascent);
    // The inline size of the overbar is the inline size of the msqrt base's margin's box. The inline offsets of the
    // msqrt base and overbar are also the same and equal to the width of the box metrics of the radical glyph.
    // The block size of the overbar is RadicalRuleThickness. Its vertical center is shifted away from the alphabetic
    // baseline by a distance towards the line-over equal to the line-ascent of the math content, minus the
    // RadicalExtraAscender, minus half the RadicalRuleThickness.
    let overbar_baseline_shift = radical.extra_ascender - ascent;
    let base_inline_size = base.inline_size;
    let mut content = base.translated(glyph.advance, CssPixels::default());
    content.rules.push(MathRule {
        line_left_offset: glyph.advance,
        baseline_shift: overbar_baseline_shift,
        inline_size: base_inline_size,
        block_size: radical.rule_thickness,
        spans_content_box: false,
    });
    // Finally, the painting of the surd is performed:
    // - At inline offset 0.
    // - At block offset shifted away from the line-over edge of the overbar towards the line-under by a distance
    //   given by the ascent of the box metrics of the radical glyph.
    content.glyph_runs.push(PlacedGlyphRun {
        line_left_offset: CssPixels::default(),
        baseline_shift: overbar_baseline_shift + glyph.ascent,
        glyphs: glyph.clone(),
    });
    MathContent {
        inline_size,
        ascent,
        descent,
        ..content
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
            ..MathContent::default()
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
    let mut content = b.translated(b_offset, CssPixels::default());
    content.positions.push(ChildPosition {
        line_left_offset: adjusted_kern_before_degree,
        baseline_shift: index_baseline_shift,
    });
    MathContent {
        inline_size,
        ascent,
        descent,
        ..content
    }
}
