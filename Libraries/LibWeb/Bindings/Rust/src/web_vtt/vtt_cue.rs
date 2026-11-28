/*
 * Copyright (c) 2024-2025, Jamie Mansfield <jmansfield@cadixdev.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! VTTCue, https://w3c.github.io/webvtt/#vttcue

use core::cell::{Cell, RefCell};
use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::{Trace, Vm};

use crate::bindings::conversions::CppInterface;
use crate::bindings::event_target::{CppEventTarget, GetTheParent};
use crate::bindings::interface::{Inherits, Ref, Wrappable, allocate};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::html::text_track_cue::TextTrackCue;
use crate::web_vtt::vtt_region::VTTRegion;

/// The enumeration AutoKeyword of WebVTT/VTTCue.idl.
#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum AutoKeyword {
    Auto,
}

/// The union LineAndPositionSetting of WebVTT/VTTCue.idl, whose members the bindings convert.
#[derive(Clone, Copy, PartialEq)]
pub(crate) enum LineAndPositionSetting {
    Double(f64),
    AutoKeyword(AutoKeyword),
}

/// The enumeration DirectionSetting of WebVTT/VTTCue.idl.
#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum DirectionSetting {
    Empty,
    Rl,
    Lr,
}

/// The enumeration LineAlignSetting of WebVTT/VTTCue.idl.
#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum LineAlignSetting {
    Start,
    Center,
    End,
}

/// The enumeration PositionAlignSetting of WebVTT/VTTCue.idl.
#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum PositionAlignSetting {
    LineLeft,
    Center,
    LineRight,
    Auto,
}

/// The enumeration AlignSetting of WebVTT/VTTCue.idl.
#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum AlignSetting {
    Start,
    Center,
    End,
    Left,
    Right,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum WritingDirection {
    // https://w3c.github.io/webvtt/#webvtt-cue-horizontal-writing-direction
    Horizontal,

    // https://w3c.github.io/webvtt/#webvtt-cue-vertical-growing-left-writing-direction
    VerticalGrowingLeft,

    // https://w3c.github.io/webvtt/#webvtt-cue-vertical-growing-right-writing-direction
    VerticalGrowingRight,
}

// https://w3c.github.io/webvtt/#vttcue
#[derive(Trace)]
pub(crate) struct VTTCue {
    cue: TextTrackCue,

    // https://w3c.github.io/webvtt/#cue-text
    #[gc(untraced)]
    text: RefCell<Utf16String>,

    // https://w3c.github.io/webvtt/#webvtt-cue-writing-direction
    #[gc(untraced)]
    writing_direction: Cell<WritingDirection>,

    // https://w3c.github.io/webvtt/#webvtt-cue-snap-to-lines-flag
    snap_to_lines: Cell<bool>,

    // https://w3c.github.io/webvtt/#webvtt-cue-line
    #[gc(untraced)]
    line: Cell<LineAndPositionSetting>,

    // https://w3c.github.io/webvtt/#webvtt-cue-line-alignment
    #[gc(untraced)]
    line_alignment: Cell<LineAlignSetting>,

    // https://w3c.github.io/webvtt/#webvtt-cue-position
    #[gc(untraced)]
    position: Cell<LineAndPositionSetting>,

    // https://w3c.github.io/webvtt/#webvtt-cue-position-alignment
    #[gc(untraced)]
    position_alignment: Cell<PositionAlignSetting>,

    // https://w3c.github.io/webvtt/#webvtt-cue-size
    size: Cell<f64>,

    // https://w3c.github.io/webvtt/#webvtt-cue-text-alignment
    #[gc(untraced)]
    text_alignment: Cell<AlignSetting>,

    // https://w3c.github.io/webvtt/#webvtt-cue-region
    region: Cell<Option<Ref<VTTRegion>>>,
}

impl Inherits<TextTrackCue> for VTTCue {
    fn parent(&self) -> &TextTrackCue {
        &self.cue
    }
}

impl Wrappable for VTTCue {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.cue.cpp_parent()
    }
}

impl GetTheParent for VTTCue {
    fn get_the_parent(&self) -> Option<CppEventTarget> {
        self.cue.get_the_parent()
    }
}

impl VTTCue {
    // https://w3c.github.io/webvtt/#dom-vttcue-vttcue
    pub(crate) fn create(vm: &Vm, start_time: f64, end_time: f64, text: Utf16String) -> WebIdlResult<Ref<VTTCue>> {
        // 1. Create a new WebVTT cue. Let cue be that WebVTT cue.
        // 2. Let cue’s text track cue start time be the value of the startTime argument.
        // 3. If the value of the endTime argument is negative Infinity or a Not-a-Number (NaN) value, then throw a
        //    TypeError exception. Otherwise, let cue’s text track cue end time be the value of the endTime argument.
        // NB: The cue is allocated below, once nothing can throw.
        if end_time == f64::NEG_INFINITY || end_time.is_nan() {
            return Err(Exception::type_error("End time is negative infinity or NaN"));
        }

        let cue = allocate(vm, |_| VTTCue {
            // 4. Let cue’s cue text be the value of the text argument, and let the rules for extracting the chapter
            //    title be the WebVTT rules for extracting the chapter title.
            // FIXME: let the rules for extracting the chapter title be the WebVTT rules for extracting the chapter
            //        title.
            text: RefCell::new(text),

            // 5. Let cue’s text track cue identifier be the empty string.
            // 6. Let cue’s text track cue pause-on-exit flag be false.
            cue: TextTrackCue::new(start_time, end_time),

            // 7. Let cue’s WebVTT cue region be null.
            region: Cell::new(None),

            // 8. Let cue’s WebVTT cue writing direction be horizontal.
            writing_direction: Cell::new(WritingDirection::Horizontal),

            // 9. Let cue’s WebVTT cue snap-to-lines flag be true.
            snap_to_lines: Cell::new(true),

            // 10. Let cue’s WebVTT cue line be auto.
            line: Cell::new(LineAndPositionSetting::AutoKeyword(AutoKeyword::Auto)),

            // 11. Let cue’s WebVTT cue line alignment be start alignment.
            line_alignment: Cell::new(LineAlignSetting::Start),

            // 12. Let cue’s WebVTT cue position be auto.
            position: Cell::new(LineAndPositionSetting::AutoKeyword(AutoKeyword::Auto)),

            // 13. Let cue’s WebVTT cue position alignment be auto.
            position_alignment: Cell::new(PositionAlignSetting::Auto),

            // 14. Let cue’s WebVTT cue size be 100.
            size: Cell::new(100.0),

            // 15. Let cue’s WebVTT cue text alignment be center alignment.
            text_alignment: Cell::new(AlignSetting::Center),
        });
        cue.cue.event_target().set_owner(cue);

        // 16. Return the VTTCue object representing cue.
        Ok(cue)
    }

    // https://w3c.github.io/webvtt/#dom-vttcue-vttcue
    pub(crate) fn construct(
        cx: &mut CallContext,
        start_time: f64,
        end_time: f64,
        text: Utf16String,
    ) -> WebIdlResult<Ref<VTTCue>> {
        Self::create(cx.vm(), start_time, end_time, text)
    }

    // https://w3c.github.io/webvtt/#dom-vttcue-region
    pub(crate) fn region(&self, _: &mut CallContext) -> WebIdlResult<Option<Ref<VTTRegion>>> {
        Ok(self.region.get())
    }

    // https://w3c.github.io/webvtt/#dom-vttcue-region
    pub(crate) fn set_region(&self, _: &mut CallContext, value: Option<Ref<VTTRegion>>) -> WebIdlResult<()> {
        self.region.set(value);
        Ok(())
    }

    // https://w3c.github.io/webvtt/#dom-vttcue-vertical
    pub(crate) fn vertical(&self, _: &mut CallContext) -> WebIdlResult<DirectionSetting> {
        Ok(match self.writing_direction.get() {
            WritingDirection::Horizontal => DirectionSetting::Empty,
            WritingDirection::VerticalGrowingLeft => DirectionSetting::Rl,
            WritingDirection::VerticalGrowingRight => DirectionSetting::Lr,
        })
    }

    // https://w3c.github.io/webvtt/#dom-vttcue-vertical
    pub(crate) fn set_vertical(&self, _: &mut CallContext, value: DirectionSetting) -> WebIdlResult<()> {
        self.writing_direction.set(match value {
            DirectionSetting::Empty => WritingDirection::Horizontal,
            DirectionSetting::Rl => WritingDirection::VerticalGrowingLeft,
            DirectionSetting::Lr => WritingDirection::VerticalGrowingRight,
        });
        Ok(())
    }

    // https://w3c.github.io/webvtt/#dom-vttcue-snaptolines
    pub(crate) fn snap_to_lines(&self, _: &mut CallContext) -> WebIdlResult<bool> {
        Ok(self.snap_to_lines.get())
    }

    // https://w3c.github.io/webvtt/#dom-vttcue-snaptolines
    pub(crate) fn set_snap_to_lines(&self, _: &mut CallContext, value: bool) -> WebIdlResult<()> {
        self.snap_to_lines.set(value);
        Ok(())
    }

    // https://w3c.github.io/webvtt/#dom-vttcue-line
    pub(crate) fn line(&self, _: &mut CallContext) -> WebIdlResult<LineAndPositionSetting> {
        Ok(self.line.get())
    }

    // https://w3c.github.io/webvtt/#dom-vttcue-line
    pub(crate) fn set_line(&self, _: &mut CallContext, value: LineAndPositionSetting) -> WebIdlResult<()> {
        self.line.set(value);
        Ok(())
    }

    // https://w3c.github.io/webvtt/#dom-vttcue-linealign
    pub(crate) fn line_align(&self, _: &mut CallContext) -> WebIdlResult<LineAlignSetting> {
        Ok(self.line_alignment.get())
    }

    // https://w3c.github.io/webvtt/#dom-vttcue-linealign
    pub(crate) fn set_line_align(&self, _: &mut CallContext, value: LineAlignSetting) -> WebIdlResult<()> {
        self.line_alignment.set(value);
        Ok(())
    }

    // https://w3c.github.io/webvtt/#dom-vttcue-position
    pub(crate) fn position(&self, _: &mut CallContext) -> WebIdlResult<LineAndPositionSetting> {
        Ok(self.position.get())
    }

    // https://w3c.github.io/webvtt/#dom-vttcue-position
    pub(crate) fn set_position(&self, _: &mut CallContext, value: LineAndPositionSetting) -> WebIdlResult<()> {
        // On setting, if the new value is negative or greater than 100, then an IndexSizeError exception must be thrown.
        if let LineAndPositionSetting::Double(position) = value
            && !(0.0..=100.0).contains(&position)
        {
            return Err(Exception::index_size_error(Utf16String::from_utf8(
                "Value is negative or greater than 100",
            )));
        }

        // Otherwise, the WebVTT cue position must be set to the new value; if the new value is the string "auto", then
        // it must be interpreted as the special value auto.
        self.position.set(value);
        Ok(())
    }

    // https://w3c.github.io/webvtt/#dom-vttcue-positionalign
    pub(crate) fn position_align(&self, _: &mut CallContext) -> WebIdlResult<PositionAlignSetting> {
        Ok(self.position_alignment.get())
    }

    // https://w3c.github.io/webvtt/#dom-vttcue-positionalign
    pub(crate) fn set_position_align(&self, _: &mut CallContext, value: PositionAlignSetting) -> WebIdlResult<()> {
        self.position_alignment.set(value);
        Ok(())
    }

    // https://w3c.github.io/webvtt/#dom-vttcue-size
    pub(crate) fn size(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.size.get())
    }

    // https://w3c.github.io/webvtt/#dom-vttcue-size
    pub(crate) fn set_size(&self, _: &mut CallContext, value: f64) -> WebIdlResult<()> {
        // On setting, if the new value is negative or greater than 100, then an IndexSizeError exception must be thrown.
        if !(0.0..=100.0).contains(&value) {
            return Err(Exception::index_size_error(Utf16String::from_utf8(
                "Value is negative or greater than 100",
            )));
        }

        // Otherwise, the WebVTT cue size must be set to the new value.
        self.size.set(value);
        Ok(())
    }

    // https://w3c.github.io/webvtt/#dom-vttcue-align
    pub(crate) fn align(&self, _: &mut CallContext) -> WebIdlResult<AlignSetting> {
        Ok(self.text_alignment.get())
    }

    // https://w3c.github.io/webvtt/#dom-vttcue-align
    pub(crate) fn set_align(&self, _: &mut CallContext, value: AlignSetting) -> WebIdlResult<()> {
        self.text_alignment.set(value);
        Ok(())
    }

    // https://w3c.github.io/webvtt/#dom-vttcue-getcueashtml
    pub(crate) fn get_cue_as_html(&self, cx: &mut CallContext) -> WebIdlResult<CppInterface> {
        let text = self.text.borrow().clone();
        Ok(super::cue_text::create_fragment(cx, &text))
    }

    // https://w3c.github.io/webvtt/#dom-vttcue-text
    pub(crate) fn text(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(self.text.borrow().clone())
    }

    // https://w3c.github.io/webvtt/#dom-vttcue-text
    pub(crate) fn set_text(&self, _: &mut CallContext, value: Utf16String) -> WebIdlResult<()> {
        *self.text.borrow_mut() = value;
        Ok(())
    }
}
