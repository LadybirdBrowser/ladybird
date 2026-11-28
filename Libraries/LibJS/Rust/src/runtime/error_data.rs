/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;
use core::ops::ControlFlow;

use ak::Utf16String;
use libgc_api::Trace;

use crate::gc::class::{ExternalMemorySize, GcCell, define_cell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::{CellHeader, Gc};
use crate::runtime::function_object::FunctionObject;
use crate::runtime::primitive_string::PrimitiveString;
use crate::source_range::SourceRange;
use crate::utf16::{Utf16View, utf16_string_external_memory_size};

pub struct TracebackFrame {
    pub function_name: Utf16String,
    pub cached_source_range: Option<SourceRange>,
}

impl TracebackFrame {
    /// The filename, line and column of the frame's source range, which are empty for a frame without one.
    pub fn source_position(&self) -> (Utf16View<'_>, u32, u32) {
        match &self.cached_source_range {
            Some(source_range) => (
                Utf16View::of_string(source_range.filename()),
                source_range.start.line,
                source_range.start.column,
            ),
            None => (Utf16View::Ascii(b""), 0, 0),
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CompactTraceback {
    No,
    Yes,
}

/// How a stack string writes each frame: indented, as "    at f (url:line:col)", or with an at sign, as
/// "f@url:line:col". The embedder picks the syntax, since scripts read it to tell engines apart.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(u8)]
pub enum StackFrameSyntax {
    #[default]
    Indented = 0,
    AtSign = 1,
}

impl StackFrameSyntax {
    pub fn from_abi(syntax: u8) -> Self {
        match syntax {
            1 => Self::AtSign,
            _ => Self::Indented,
        }
    }
}

/// The initial value of Error.stackTraceLimit.
pub const DEFAULT_STACK_TRACE_LIMIT: i32 = 10;

/// How many frames an error keeps at least while the VM keeps long stack traces, as it does for a debugger that lists
/// them.
pub const LONG_STACK_TRACE_FRAME_COUNT: usize = 200;

/// How many frames a new error keeps: the value of Error.stackTraceLimit on the %Error% of the current realm, cut to an
/// integer, with negative numbers and NaN as 0 and Infinity as every frame. None, for no stack at all, when the property
/// is not a number, which includes an accessor (whose getter is never called) and a deleted property. The other engines
/// limit their stacks too, and pages read and set this property to see the frames they expect.
pub fn stack_trace_limit(vm: &Vm) -> Option<usize> {
    let Some(realm) = vm.current_realm() else {
        return Some(DEFAULT_STACK_TRACE_LIMIT as usize);
    };
    let error_constructor = realm.intrinsics().error_constructor(vm);
    let value = error_constructor.storage_get(vm, &vm.names.stackTraceLimit)?.value;
    if value.is_int32() {
        return Some(usize::try_from(value.as_i32()).unwrap_or(0));
    }
    if !value.is_number() {
        return None;
    }
    // NB: The cast saturates, and takes NaN to 0.
    Some(value.as_f64() as usize)
}

/// Which of the innermost frames a stack leaves out.
#[derive(Clone, Copy)]
pub enum SkippedFrames {
    /// That many frames.
    Count(usize),
    /// Every frame up to the innermost frame of the function, and that frame too: all of them if the function has no
    /// frame on the stack.
    UpToFrameOf(Gc<FunctionObject>),
}

/// [[ErrorData]]: the call stack an error was created on.
#[derive(Trace)]
pub struct ErrorData {
    #[gc(untraced)]
    traceback: Vec<TracebackFrame>,
    /// How many of the frames, from the innermost, the stack string shows, or None for an error without a stack, whose
    /// stack is undefined. Such an error has frames only while the VM keeps long stack traces.
    shown_frame_count: Option<usize>,
    /// How many of the frames, from the innermost, the error would keep if the VM kept no long stack traces. What script
    /// can observe, such as the location of an error event, reads only these, so it does not change while a debugger is
    /// attached. Unlike shown_frame_count, it counts the outermost execution context when the walk got to it inside the
    /// limit.
    script_visible_frame_count: usize,
    cached_string: Cell<Option<Gc<PrimitiveString>>>,
}

impl ErrorData {
    pub fn new(vm: &Vm) -> Self {
        Self::capture(vm, SkippedFrames::Count(0), stack_trace_limit(vm))
    }

    /// Error data with the frames below those that `skipped_frames` leaves out, as Error.captureStackTrace() captures
    /// it.
    pub fn new_skipping(vm: &Vm, skipped_frames: SkippedFrames) -> Self {
        Self::capture(vm, skipped_frames, stack_trace_limit(vm))
    }

    /// Error data captured by an Error constructor, whose own frame is the innermost one. A stack string shows the
    /// code that made the error, never the constructors that made it: the frames up to and including the innermost
    /// frame of `new_target`, which are those of the constructors of the subclasses an error class extends, are left
    /// out. When `new_target` has no frame on the stack, as with Reflect.construct(), only the frame of the
    /// constructor itself is.
    pub fn new_in_constructor(vm: &Vm, new_target: Gc<FunctionObject>) -> Self {
        let mut frames_up_to_new_target = 0;
        let mut found_new_target = false;
        vm.for_each_stack_frame_top_to_bottom(|frame| {
            frames_up_to_new_target += 1;
            if frame.function == Some(new_target) {
                found_new_target = true;
                return ControlFlow::Break(());
            }
            ControlFlow::Continue(())
        });
        let skipped_frames = if found_new_target { frames_up_to_new_target } else { 1 };
        Self::capture(vm, SkippedFrames::Count(skipped_frames), stack_trace_limit(vm))
    }

    /// Error data with every frame of the call stack, whatever Error.stackTraceLimit is, for a trace of the running code
    /// that is not the stack of an error.
    pub fn with_whole_stack(vm: &Vm) -> Self {
        Self::capture(vm, SkippedFrames::Count(0), Some(usize::MAX))
    }

    /// Error data for an exception whose source location came from another agent.
    pub fn from_source_range(source_range: SourceRange) -> Self {
        Self {
            traceback: vec![TracebackFrame {
                function_name: Utf16String::default(),
                cached_source_range: Some(source_range),
            }],
            shown_frame_count: Some(0),
            script_visible_frame_count: 1,
            cached_string: Cell::new(None),
        }
    }

    /// Error data with at most `limit` frames below those that `skipped_frames` leaves out, or with no stack for a
    /// `limit` of None. While the VM keeps long stack traces, it keeps at least LONG_STACK_TRACE_FRAME_COUNT frames for
    /// the debugger, even with no stack, but the stack string and the script-visible frames stay as without them.
    fn capture(vm: &Vm, skipped_frames: SkippedFrames, limit: Option<usize>) -> Self {
        let captured_limit = match limit {
            Some(limit) if vm.keeps_long_stack_traces() => limit.max(LONG_STACK_TRACE_FRAME_COUNT),
            Some(limit) => limit,
            None if vm.keeps_long_stack_traces() => LONG_STACK_TRACE_FRAME_COUNT,
            None => {
                return Self {
                    traceback: Vec::new(),
                    shown_frame_count: None,
                    script_visible_frame_count: 0,
                    cached_string: Cell::new(None),
                };
            }
        };
        let (traceback, reached_limit) = Self::populate_stack(vm, skipped_frames, captured_limit);
        // NB: A stack string leaves out the outermost execution context, the last frame when the walk got to it.
        let frame_count = if reached_limit {
            traceback.len()
        } else {
            traceback.len().saturating_sub(1)
        };
        let shown_frame_count = limit.map(|limit| frame_count.min(limit));
        let script_visible_frame_count = limit.map_or(0, |limit| traceback.len().min(limit));
        Self {
            traceback,
            shown_frame_count,
            script_visible_frame_count,
            cached_string: Cell::new(None),
        }
    }

    pub fn traceback_storage_size(&self) -> usize {
        self.traceback.capacity() * size_of::<TracebackFrame>()
    }

    pub fn external_memory_size(&self) -> usize {
        let function_names_size: usize = self
            .traceback
            .iter()
            .map(|frame| utf16_string_external_memory_size(&frame.function_name))
            .sum();
        self.traceback_storage_size() + function_names_size
    }

    /// The frames from the running one down, after those `skipped_frames` leaves out, until there are `limit` of
    /// them, and whether the walk stopped there with frames left below. The walk never goes further, so capture costs
    /// the same at any depth, except for the walk to the frame of a function that has none.
    fn populate_stack(vm: &Vm, mut skipped_frames: SkippedFrames, limit: usize) -> (Vec<TracebackFrame>, bool) {
        let mut traceback = Vec::new();
        let mut reached_limit = false;
        vm.for_each_stack_frame_top_to_bottom(|frame| {
            match skipped_frames {
                SkippedFrames::Count(0) => {}
                SkippedFrames::Count(count) => {
                    skipped_frames = SkippedFrames::Count(count - 1);
                    return ControlFlow::Continue(());
                }
                SkippedFrames::UpToFrameOf(function) => {
                    if frame.function == Some(function) {
                        skipped_frames = SkippedFrames::Count(0);
                    }
                    return ControlFlow::Continue(());
                }
            }
            if traceback.len() == limit {
                reached_limit = true;
                return ControlFlow::Break(());
            }
            traceback.push(TracebackFrame {
                function_name: frame
                    .function
                    .map(|function| function.name_for_call_stack())
                    .unwrap_or_default(),
                cached_source_range: frame.source_range(),
            });
            ControlFlow::Continue(())
        });
        (traceback, reached_limit)
    }

    /// Every frame the error kept, for a debugger that lists them.
    pub fn traceback(&self) -> &[TracebackFrame] {
        &self.traceback
    }

    /// The frames the error would keep if the VM kept no long stack traces, for what script can observe.
    pub fn script_visible_traceback(&self) -> &[TracebackFrame] {
        &self.traceback[..self.script_visible_frame_count]
    }

    /// Whether the error has a stack string; its stack is undefined if not, even with frames kept for a debugger.
    pub fn has_stack(&self) -> bool {
        self.shown_frame_count.is_some()
    }

    pub fn set_cached_string(&self, string: Gc<PrimitiveString>) {
        self.cached_string.set(Some(string));
    }

    pub fn cached_string(&self) -> Option<Gc<PrimitiveString>> {
        self.cached_string.get()
    }

    pub fn stack_string(&self, compact: CompactTraceback) -> Utf16String {
        self.stack_string_with_syntax(compact, StackFrameSyntax::Indented)
    }

    pub fn stack_string_with_syntax(&self, compact: CompactTraceback, syntax: StackFrameSyntax) -> Utf16String {
        let used_frames = self.shown_frame_count.unwrap_or(0);
        if used_frames == 0 {
            return Utf16String::default();
        }

        let mut stack_string_builder: Vec<u16> = Vec::new();

        let append_frame = |builder: &mut Vec<u16>, frame: &TracebackFrame| {
            let function_name = Utf16View::of_string(&frame.function_name);
            let (filename, line, column) = frame.source_position();
            if syntax == StackFrameSyntax::AtSign {
                // "f@url:line:col", "@url:line:col" for a frame without a name, and "[native code]" in place of a
                // location for a frame without one.
                function_name.append_to(builder);
                builder.push(u16::from(b'@'));
                if !filename.is_empty() || line != 0 || column != 0 {
                    filename.append_to(builder);
                    builder.extend(format!(":{line}:{column}\n").encode_utf16());
                } else {
                    builder.extend("[native code]\n".encode_utf16());
                }
                return;
            }
            builder.extend("    at ".encode_utf16());
            // Note: Since we don't know whether we have a valid SourceRange here we just check for some default values.
            if !filename.is_empty() || line != 0 || column != 0 {
                if function_name.is_empty() {
                    filename.append_to(builder);
                    builder.extend(format!(":{line}:{column}\n").encode_utf16());
                } else {
                    function_name.append_to(builder);
                    builder.extend(" (".encode_utf16());
                    filename.append_to(builder);
                    builder.extend(format!(":{line}:{column})\n").encode_utf16());
                }
            } else {
                if function_name.is_empty() {
                    builder.extend("<unknown>".encode_utf16());
                } else {
                    function_name.append_to(builder);
                }
                builder.push(u16::from(b'\n'));
            }
        };

        let is_same_frame = |a: &TracebackFrame, b: &TracebackFrame| {
            if a.function_name.is_empty() && b.function_name.is_empty() {
                let (filename_a, line_a, _) = a.source_position();
                let (filename_b, line_b, _) = b.source_position();
                return filename_a == filename_b && line_a == line_b;
            }
            a.function_name == b.function_name
        };

        // Note: The error's name and message get prepended by Error.prototype.stack
        let mut repetitions: usize = 0;
        for i in 0..used_frames {
            let frame = &self.traceback[i];
            if compact == CompactTraceback::Yes && i + 1 < used_frames {
                let next_traceback_frame = &self.traceback[i + 1];
                if is_same_frame(frame, next_traceback_frame) {
                    repetitions += 1;
                    continue;
                }
            }
            if repetitions > 4 {
                // If more than 5 (1 + >4) consecutive function calls with the same name, print
                // the name only once and show the number of repetitions instead. This prevents
                // printing ridiculously large call stacks of recursive functions.
                append_frame(&mut stack_string_builder, frame);
                stack_string_builder.extend(format!("    {repetitions} more calls\n").encode_utf16());
            } else {
                for _ in 0..repetitions + 1 {
                    append_frame(&mut stack_string_builder, frame);
                }
            }
            repetitions = 0;
        }
        for _ in 0..repetitions {
            append_frame(&mut stack_string_builder, &self.traceback[used_frames - 1]);
        }

        Utf16String::from_utf16(&stack_string_builder)
    }
}

/// The [[ErrorData]] of a host object that is not an Error, such as a DOMException, in a cell of its own, which the
/// embedder keeps alive with the object.
#[repr(C)]
#[derive(Trace)]
pub struct ErrorDataCell {
    header: CellHeader,
    error_data: ErrorData,
}

define_cell!(ErrorDataCell, Other, external_memory_size: external_memory_size);

/// Where an error data cell keeps its error data.
pub const ERROR_DATA_CELL_ERROR_DATA_OFFSET: usize = core::mem::offset_of!(ErrorDataCell, error_data);

impl ExternalMemorySize for ErrorDataCell {
    fn external_memory_size(&self) -> usize {
        self.error_data.external_memory_size()
    }
}

impl ErrorDataCell {
    /// ErrorDataCell::capture(vm): error data with the call stack of the running execution context.
    pub fn capture(vm: &Vm) -> Gc<ErrorDataCell> {
        Self::create(vm, ErrorData::new(vm))
    }

    /// Error data with every frame of the call stack, as ErrorData::with_whole_stack() has.
    pub fn capture_whole_stack(vm: &Vm) -> Gc<ErrorDataCell> {
        Self::create(vm, ErrorData::with_whole_stack(vm))
    }

    /// Error data for an exception whose source location came from another agent.
    pub fn from_source_range(vm: &Vm, source_range: SourceRange) -> Gc<ErrorDataCell> {
        Self::create(vm, ErrorData::from_source_range(source_range))
    }

    fn create(vm: &Vm, error_data: ErrorData) -> Gc<ErrorDataCell> {
        let cell = vm.heap().allocate(|_| Self {
            header: CellHeader::for_class(Self::CLASS),
            error_data,
        });
        vm.heap()
            .did_allocate_external_memory(cell.error_data.traceback_storage_size());
        cell
    }

    pub fn error_data(&self) -> &ErrorData {
        &self.error_data
    }
}
