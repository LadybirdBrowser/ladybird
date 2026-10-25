/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! https://html.spec.whatwg.org/multipage/canvas.html#canvastextdrawingstyles

use ak::Utf16String;

use super::canvas_rendering_context2d::{CanvasDirection, CanvasTextAlign, CanvasTextBaseline};
use crate::bindings::{CallContext, WebIdlResult};
use crate::canvas_2d::canvas::parse_length;
use crate::canvas_2d::context::{CanvasKind, RenderingContext2D};
use crate::canvas_2d::drawing_state::Spacing;

/// https://html.spec.whatwg.org/multipage/canvas.html#canvasfontkerning
#[repr(u8)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum CanvasFontKerning {
    Auto,
    Normal,
    None,
}

/// https://html.spec.whatwg.org/multipage/canvas.html#canvasfontstretch
#[repr(u8)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum CanvasFontStretch {
    UltraCondensed,
    ExtraCondensed,
    Condensed,
    SemiCondensed,
    Normal,
    SemiExpanded,
    Expanded,
    ExtraExpanded,
    UltraExpanded,
}

/// https://html.spec.whatwg.org/multipage/canvas.html#canvasfontvariantcaps
#[repr(u8)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum CanvasFontVariantCaps {
    Normal,
    SmallCaps,
    AllSmallCaps,
    PetiteCaps,
    AllPetiteCaps,
    Unicase,
    TitlingCaps,
}

/// https://html.spec.whatwg.org/multipage/canvas.html#canvastextrendering
#[repr(u8)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum CanvasTextRendering {
    Auto,
    OptimizeSpeed,
    OptimizeLegibility,
    GeometricPrecision,
}

impl<K: CanvasKind> RenderingContext2D<K> {
    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-font
    pub(crate) fn font(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        // On getting, the font attribute must return the serialized form of the current font of the context (with no
        // 'line-height' component).
        // NB: When the context has no font yet, that is the font it starts with.
        Ok(self.drawing_state().font.as_ref().map_or_else(
            || Utf16String::from_ascii(b"10px sans-serif"),
            |font| font.serialized.clone(),
        ))
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-font
    pub(crate) fn set_font(&self, cx: &mut CallContext, font: Utf16String) -> WebIdlResult<()> {
        // The font IDL attribute, on setting, must be parsed as a CSS <'font'> value (but without supporting
        // property-independent style sheet syntax like 'inherit'), and the resulting font must be assigned to the
        // context, with the 'line-height' component forced to 'normal', with the 'font-size' component converted to
        // CSS pixels, and with system fonts being computed to explicit values.
        // If the new value is syntactically incorrect (including using property-independent style sheet syntax like
        // 'inherit' or 'initial'), then it must be ignored, without assigning a new font value.
        // NB: LibWeb's C++ parses the value and resolves it against the canvas.
        self.assign_font(cx.vm(), &font);
        Ok(())
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-textalign
    pub(crate) fn text_align(&self, _: &mut CallContext) -> WebIdlResult<CanvasTextAlign> {
        Ok(self.drawing_state().text_align)
    }

    pub(crate) fn set_text_align(&self, cx: &mut CallContext, text_align: CanvasTextAlign) -> WebIdlResult<()> {
        self.drawing_state_mut(cx.vm()).text_align = text_align;
        Ok(())
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-textbaseline
    pub(crate) fn text_baseline(&self, _: &mut CallContext) -> WebIdlResult<CanvasTextBaseline> {
        Ok(self.drawing_state().text_baseline)
    }

    pub(crate) fn set_text_baseline(
        &self,
        cx: &mut CallContext,
        text_baseline: CanvasTextBaseline,
    ) -> WebIdlResult<()> {
        self.drawing_state_mut(cx.vm()).text_baseline = text_baseline;
        Ok(())
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-direction
    pub(crate) fn direction(&self, _: &mut CallContext) -> WebIdlResult<CanvasDirection> {
        Ok(self.drawing_state().direction)
    }

    pub(crate) fn set_direction(&self, cx: &mut CallContext, direction: CanvasDirection) -> WebIdlResult<()> {
        self.drawing_state_mut(cx.vm()).direction = direction;
        Ok(())
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-letterspacing
    pub(crate) fn letter_spacing(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        // The letterSpacing getter steps are to return the serialized form of this's letter spacing.
        Ok(self.drawing_state().letter_spacing.as_ref().map_or_else(
            || Utf16String::from_ascii(b"0px"),
            |letter_spacing| letter_spacing.serialized.clone(),
        ))
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-letterspacing
    pub(crate) fn set_letter_spacing(&self, cx: &mut CallContext, letter_spacing: Utf16String) -> WebIdlResult<()> {
        // 1. Let parsed be the result of parsing the given value as a CSS <length>.
        // 2. If parsed is failure, then return.
        let Some((length, serialized)) = parse_length(&letter_spacing) else {
            return Ok(());
        };

        // 3. Set this's letter spacing to parsed.
        self.drawing_state_mut(cx.vm()).letter_spacing = Some(Spacing { length, serialized });
        Ok(())
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-lang
    pub(crate) fn lang(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        // The lang getter steps are to return this's language.
        Ok(self.drawing_state().language.clone())
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-lang
    pub(crate) fn set_lang(&self, cx: &mut CallContext, lang: Utf16String) -> WebIdlResult<()> {
        // The lang setter steps are to set this's language to the given value.
        self.drawing_state_mut(cx.vm()).language = lang;
        Ok(())
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-wordspacing
    pub(crate) fn word_spacing(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        // The wordSpacing getter steps are to return the serialized form of this's word spacing.
        Ok(self.drawing_state().word_spacing.as_ref().map_or_else(
            || Utf16String::from_ascii(b"0px"),
            |word_spacing| word_spacing.serialized.clone(),
        ))
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-wordspacing
    pub(crate) fn set_word_spacing(&self, cx: &mut CallContext, word_spacing: Utf16String) -> WebIdlResult<()> {
        // 1. Let parsed be the result of parsing the given value as a CSS <length>.
        // 2. If parsed is failure, then return.
        let Some((length, serialized)) = parse_length(&word_spacing) else {
            return Ok(());
        };

        // 3. Set this's word spacing to parsed.
        self.drawing_state_mut(cx.vm()).word_spacing = Some(Spacing { length, serialized });
        Ok(())
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-fontkerning
    pub(crate) fn font_kerning(&self, _: &mut CallContext) -> WebIdlResult<CanvasFontKerning> {
        // The fontKerning IDL attribute, on getting, must return the current value.
        Ok(self.drawing_state().font_kerning)
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-fontkerning
    pub(crate) fn set_font_kerning(&self, cx: &mut CallContext, font_kerning: CanvasFontKerning) -> WebIdlResult<()> {
        // On setting, the current value must be changed to the new value.
        if self.drawing_state().font_kerning != font_kerning {
            self.drawing_state_mut(cx.vm()).font_kerning = font_kerning;
            self.font_selection_did_change(cx.vm());
        }
        Ok(())
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-fontstretch
    pub(crate) fn font_stretch(&self, _: &mut CallContext) -> WebIdlResult<CanvasFontStretch> {
        // The fontStretch IDL attribute, on getting, must return the current value.
        Ok(self.drawing_state().font_stretch)
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-fontstretch
    pub(crate) fn set_font_stretch(&self, cx: &mut CallContext, font_stretch: CanvasFontStretch) -> WebIdlResult<()> {
        // On setting, the current value must be changed to the new value.
        if self.drawing_state().font_stretch != font_stretch {
            self.drawing_state_mut(cx.vm()).font_stretch = font_stretch;
            self.font_selection_did_change(cx.vm());
        }
        Ok(())
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-fontvariantcaps
    pub(crate) fn font_variant_caps(&self, _: &mut CallContext) -> WebIdlResult<CanvasFontVariantCaps> {
        // The fontVariantCaps IDL attribute, on getting, must return the current value.
        Ok(self.drawing_state().font_variant_caps)
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-fontvariantcaps
    pub(crate) fn set_font_variant_caps(
        &self,
        cx: &mut CallContext,
        font_variant_caps: CanvasFontVariantCaps,
    ) -> WebIdlResult<()> {
        // On setting, the current value must be changed to the new value.
        if self.drawing_state().font_variant_caps != font_variant_caps {
            self.drawing_state_mut(cx.vm()).font_variant_caps = font_variant_caps;
            self.font_selection_did_change(cx.vm());
        }
        Ok(())
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-textrendering
    pub(crate) fn text_rendering(&self, _: &mut CallContext) -> WebIdlResult<CanvasTextRendering> {
        // The textRendering IDL attribute, on getting, must return the current value.
        Ok(self.drawing_state().text_rendering)
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-textrendering
    pub(crate) fn set_text_rendering(
        &self,
        cx: &mut CallContext,
        text_rendering: CanvasTextRendering,
    ) -> WebIdlResult<()> {
        // On setting, the current value must be changed to the new value.
        if self.drawing_state().text_rendering != text_rendering {
            self.drawing_state_mut(cx.vm()).text_rendering = text_rendering;
            self.font_selection_did_change(cx.vm());
        }
        Ok(())
    }
}
