/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StdLibExtras.h>
#include <AK/Types.h>

namespace Web::CSS {

// Which principal box an element asks for before its computed style has a say. The element's own type and state
// decide this; the layout tree build resolves it against the element's computed display and appearance.
// Mirrors Rust `ElementBoxKind`; it crosses the boundary as its raw byte.
enum class ElementBoxKind : u8 {
    // The computed display decides the box on its own.
    FromDisplay,
    // The element generates no box, whatever its display says.
    NoBox,
    Break,
    FieldSet,
    Legend,
    Audio,
    Video,
    Canvas,
    NavigableContainerViewport,
    TextArea,
    Image,
    SvgGraphics,
    SvgSvg,
    SvgText,
    SvgTextPath,
    SvgForeignObject,
    SvgImage,
    SvgGeometry,
    // An input's native widget. `appearance: none` suppresses it, and then the computed display decides the box like
    // it does for any other element.
    InputButton,
    InputCheckBox,
    InputRadioButton,
    InputRange,
    InputText,
};
static_assert(to_underlying(ElementBoxKind::InputText) == 22, "Rust's ElementBoxKind lists the same kinds in the same order");

}
