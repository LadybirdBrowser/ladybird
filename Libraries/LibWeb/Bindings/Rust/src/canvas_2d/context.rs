/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The state that CanvasRenderingContext2D and OffscreenCanvasRenderingContext2D share: the drawing state, the current
//! default path, and the backing storage, which is a context in the Compositor that the context records commands for.

use core::cell::{Cell, Ref as CellRef, RefCell, RefMut};
use core::marker::PhantomData;
use core::ops::Deref;
use core::ptr::NonNull;

use ak::Utf16String;
use libcompositing_rust::display_list::canvas_2d::{Compositing, Paint, Stroke};
use libcompositing_rust::display_list::canvas_2d_recorder::{AlphaType, Recorder};
use libcompositing_rust::display_list::commands::CanvasId;
use libgfx_rust::path::Path;
use libgfx_rust::{AffineTransform, Color, CompositingAndBlendingOperator, FloatRect, IntRect, IntSize, WindingRule};
use libjs_rust::api::{ForeignCellSlot, Gc, Realm, Trace, Visitor, Vm};

use super::canvas::{Canvas, FontMetricsSource, ResolvedFont, StyleValue, parse_color};
use super::drawing_state::{DrawingState, FillOrStrokeStyle};
use super::host::{Bitmap, CppTransport, LentImageFrame};
use crate::bindings::interface::Wrappable;
use crate::html::canvas_rendering_context2d_settings::CanvasRenderingContext2DSettings;

pub(crate) type CanvasRecorder = Recorder<CppTransport>;

/// What tells the two kinds of 2D context apart: the canvas that each draws for, an HTMLCanvasElement or an
/// OffscreenCanvas. Everything else they share.
pub(crate) trait CanvasKind: 'static {}

/// The kind of a CanvasRenderingContext2D.
pub(crate) struct OnScreen;
/// The kind of an OffscreenCanvasRenderingContext2D.
pub(crate) struct Offscreen;

impl CanvasKind for OnScreen {}
impl CanvasKind for Offscreen {}

/// The attributes and operations that CanvasRenderingContext2D and OffscreenCanvasRenderingContext2D share are
/// implemented once, for every `K`.
pub(crate) struct RenderingContext2D<K: CanvasKind> {
    state: ContextState,
    kind: PhantomData<K>,
}

impl<K: CanvasKind> RenderingContext2D<K> {
    pub(crate) fn new(state: ContextState) -> Self {
        Self {
            state,
            kind: PhantomData,
        }
    }
}

impl<K: CanvasKind> Deref for RenderingContext2D<K> {
    type Target = ContextState;

    fn deref(&self) -> &ContextState {
        &self.state
    }
}

// SAFETY: The state holds every cell that a context can reach.
unsafe impl<K: CanvasKind> Trace for RenderingContext2D<K> {
    fn trace(&self, visitor: &mut Visitor) {
        self.state.trace(visitor);
    }
}

impl<K: CanvasKind> Wrappable for RenderingContext2D<K> {
    fn relevant_realm(&self, _vm: &Vm) -> Option<Gc<Realm>> {
        self.state.canvas.relevant_realm()
    }

    const OWNS_EXTERNAL_MEMORY: bool = true;

    fn external_memory_size(&self) -> usize {
        self.state.external_memory_size()
    }
}

/// The backing storage of a context: the recorder of the commands of its context in the Compositor. Dropping it
/// destroys that context.
struct BackingStorage {
    recorder: CanvasRecorder,
}

impl Drop for BackingStorage {
    fn drop(&mut self) {
        // Flush the shared stream before destroying the context: it may still hold commands targeting this canvas, and
        // DrawCanvas commands from other canvases referencing it, which must replay while it is alive.
        self.recorder.flush();
        self.recorder.destroy_context();
    }
}

/// A readback of the pixels of a rectangle of the canvas.
#[derive(Clone)]
struct CachedReadback {
    pixels: Bitmap,
    rect: IntRect,
    alpha_type: AlphaType,
}

/// A color that a context was given as a string. Context-independent simple colors store a resolved color. Other
/// colors store their parsed style value so they can be resolved against the current canvas context on every use. An
/// entry with neither is a parse failure.
struct ColorCacheEntry {
    input: Utf16String,
    parsed_value: Option<StyleValue>,
    resolved_color: Option<Color>,
}

const MAX_COLOR_CACHE_SIZE: usize = 8;
const MAX_COLOR_CACHE_INPUT_LENGTH: usize = 128;

/// What a context is created with.
pub(crate) struct ContextParameters {
    pub(crate) canvas: Canvas,
    pub(crate) context_attributes: CanvasRenderingContext2DSettings,
    pub(crate) size: IntSize,
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct ContextState {
    canvas_cell: ForeignCellSlot,
    #[gc(untraced)]
    canvas: Canvas,
    #[gc(untraced)]
    pub(crate) context_attributes: CanvasRenderingContext2DSettings,
    #[gc(untraced)]
    size: Cell<IntSize>,
    /// https://html.spec.whatwg.org/multipage/canvas.html#concept-canvas-origin-clean
    #[gc(untraced)]
    origin_clean: Cell<bool>,
    /// https://html.spec.whatwg.org/multipage/canvas.html#concept-canvas-context-lost
    #[gc(untraced)]
    context_lost: Cell<bool>,
    /// The drawing state stack, with the current drawing state last.
    states: RefCell<Vec<DrawingState>>,
    /// The current default path, in the coordinates of the current transformation matrix.
    #[gc(untraced)]
    path: RefCell<Path>,
    /// The matrix in whose coordinates the current default path is.
    ///
    /// NB: This is the current transformation matrix while that matrix is invertible. A path cannot be kept in the
    ///     coordinates of a matrix that is not invertible, so it stays in those of the last matrix that was.
    #[gc(untraced)]
    path_transform: Cell<AffineTransform>,
    #[gc(untraced)]
    backing_storage: RefCell<Option<BackingStorage>>,
    /// OPTIMIZATION: A readback requires a synchronous IPC to the Compositor, so its pixels are reused while no
    /// drawing command has changed them.
    #[gc(untraced)]
    cached_readback: RefCell<Option<CachedReadback>>,
    #[gc(untraced)]
    color_cache: RefCell<Vec<ColorCacheEntry>>,
}

fn empty_path() -> Path {
    Path::new()
}

/// The paint of a draw with the style, with the image of a pattern added to the stream of the recorder.
pub(crate) fn paint_of<'a>(recorder: &CanvasRecorder, style: &'a FillOrStrokeStyle) -> Paint<'a> {
    match style {
        FillOrStrokeStyle::Color(color) => Paint::Color(*color),
        // SAFETY: Nothing adds a stop to the gradient while the recorder records the paint.
        FillOrStrokeStyle::Gradient(gradient) => unsafe { gradient.paint_unguarded() },
        FillOrStrokeStyle::Pattern(pattern) => Paint::Pattern {
            image: pattern.image().map(|frame| {
                let frame = NonNull::new(frame.as_raw().cast_mut()).expect("an image frame has a frame");
                recorder.add_image_frame(&LentImageFrame(frame))
            }),
            repetition: pattern.repetition(),
            transform: pattern.transform(),
        },
    }
}

impl ContextState {
    pub(crate) fn new(parameters: ContextParameters) -> Self {
        let canvas_cell = ForeignCellSlot::empty();
        // SAFETY: The canvas is a live cell of the heap that the runtime allocates from.
        unsafe { canvas_cell.set(Some(parameters.canvas.cell)) };
        Self {
            canvas_cell,
            canvas: parameters.canvas,
            context_attributes: parameters.context_attributes,
            size: Cell::new(parameters.size),
            origin_clean: Cell::new(true),
            context_lost: Cell::new(false),
            states: RefCell::new(vec![DrawingState::default()]),
            path: RefCell::new(empty_path()),
            path_transform: Cell::new(AffineTransform::identity()),
            backing_storage: RefCell::new(None),
            cached_readback: RefCell::new(None),
            color_cache: RefCell::new(Vec::new()),
        }
    }

    /// The canvas, which the canvas attribute returns.
    pub(crate) fn canvas_handle(&self) -> Canvas {
        self.canvas
    }

    pub(crate) fn size(&self) -> IntSize {
        self.size.get()
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#concept-canvas-origin-clean
    pub(crate) fn origin_clean(&self) -> bool {
        self.origin_clean.get()
    }

    pub(crate) fn set_origin_clean(&self, origin_clean: bool) {
        self.origin_clean.set(origin_clean);
    }

    pub(crate) fn context_is_lost(&self) -> bool {
        self.context_lost.get()
    }

    pub(crate) fn set_context_lost(&self, context_lost: bool) {
        self.context_lost.set(context_lost);
    }

    /// The current drawing state, which nothing may change while the borrow lasts.
    pub(crate) fn drawing_state(&self) -> CellRef<'_, DrawingState> {
        CellRef::map(self.states.borrow(), |states| {
            states.last().expect("a context has a current drawing state")
        })
    }

    /// The current drawing state, to change it. Nothing may run script or collect garbage while the borrow lasts.
    pub(crate) fn drawing_state_mut(&self) -> RefMut<'_, DrawingState> {
        RefMut::map(self.states.borrow_mut(), |states| {
            states.last_mut().expect("a context has a current drawing state")
        })
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-save
    pub(crate) fn push_drawing_state(&self) {
        let mut states = self.states.borrow_mut();
        let current = states.last().expect("a context has a current drawing state").clone();
        states.push(current);
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-restore
    pub(crate) fn pop_drawing_state(&self) -> bool {
        let mut states = self.states.borrow_mut();
        if states.len() == 1 {
            return false;
        }
        states.pop();
        true
    }

    /// The current default path, which nothing may change while the borrow lasts.
    pub(crate) fn path(&self) -> CellRef<'_, Path> {
        self.path.borrow()
    }

    pub(crate) fn path_mut(&self) -> RefMut<'_, Path> {
        self.path.borrow_mut()
    }

    /// Empties the list of subpaths of the current default path.
    pub(crate) fn clear_path(&self) {
        self.path.replace(empty_path());
    }

    /// The matrix in whose coordinates the current default path is.
    pub(crate) fn path_transform(&self) -> AffineTransform {
        self.path_transform.get()
    }

    pub(crate) fn set_path_transform(&self, transform: AffineTransform) {
        self.path_transform.set(transform);
    }

    /// Transforms every point of the current default path.
    pub(crate) fn transform_path(&self, transform: AffineTransform) {
        let transformed = self.path.borrow().copy_transformed(transform.values);
        self.path.replace(transformed);
    }

    fn external_memory_size(&self) -> usize {
        // NB: LibGC asks while it sweeps, when nothing borrows these mutably.
        let mut size = self
            .cached_readback
            .try_borrow()
            .ok()
            .and_then(|readback| readback.as_ref().map(|readback| readback.pixels.size_in_bytes()))
            .unwrap_or(0);
        let has_backing_storage = self
            .backing_storage
            .try_borrow()
            .is_ok_and(|backing_storage| backing_storage.is_some());
        if !has_backing_storage {
            return size;
        }
        let surface_size = self.size.get();
        if surface_size.width <= 0 || surface_size.height <= 0 {
            return size;
        }
        let pixel_size = usize::try_from(surface_size.width)
            .ok()
            .zip(usize::try_from(surface_size.height).ok())
            .and_then(|(width, height)| width.checked_mul(height))
            .and_then(|pixels| pixels.checked_mul(4));
        let Some(pixel_size) = pixel_size else {
            return usize::MAX;
        };
        size = size.saturating_add(pixel_size);
        size
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#concept-canvas-alpha
    pub(crate) fn clear_color(&self) -> Color {
        if self.context_attributes.alpha {
            Color::TRANSPARENT
        } else {
            Color::from_rgb(0, 0, 0)
        }
    }

    pub(crate) fn has_backing_storage(&self) -> bool {
        self.backing_storage.borrow().is_some()
    }

    fn ensure_remote_canvas_context(&self) -> bool {
        if self.has_backing_storage() {
            return true;
        }
        let Some(transport) = self.canvas.create_transport() else {
            return false;
        };
        // FIXME: implement context attribute .color_space
        // FIXME: implement context attribute .color_type
        // FIXME: implement context attribute .desynchronized
        // FIXME: implement context attribute .will_read_frequently
        let recorder = Recorder::new(transport);
        if !recorder.create_context(self.size.get(), self.context_attributes.alpha) {
            return false;
        }
        self.backing_storage.replace(Some(BackingStorage { recorder }));
        true
    }

    pub(crate) fn ensure_backing_storage(&self) {
        let size = self.size.get();
        if self.has_backing_storage() || size.width <= 0 || size.height <= 0 {
            return;
        }
        if !self.ensure_remote_canvas_context() {
            return;
        }
        self.canvas.did_create_backing_storage();
    }

    pub(crate) fn discard_backing_storage(&self) {
        self.cached_readback.replace(None);
        let backing_storage = self.backing_storage.take();
        drop(backing_storage);
    }

    /// Readies the recorder of the canvas to record, which it is unless the context is lost or the canvas has no
    /// backing storage. As the steps of a draw call it before each command they record.
    pub(crate) fn recorder_is_ready(&self) -> bool {
        if self.context_lost.get() {
            return false;
        }
        self.ensure_backing_storage();
        let backing_storage = self.backing_storage.borrow();
        let Some(backing_storage) = backing_storage.as_ref() else {
            return false;
        };
        backing_storage.recorder.prepare_to_record();
        true
    }

    /// Records with the recorder, which the caller has readied.
    pub(crate) fn record<R>(&self, record: impl FnOnce(&mut CanvasRecorder) -> R) -> R {
        let mut backing_storage = self.backing_storage.borrow_mut();
        let backing_storage = backing_storage
            .as_mut()
            .expect("the canvas has backing storage while the recorder is ready");
        record(&mut backing_storage.recorder)
    }

    /// Readies the recorder and records with it, unless the canvas cannot draw.
    pub(crate) fn record_if_ready(&self, record: impl FnOnce(&mut CanvasRecorder)) -> bool {
        if !self.recorder_is_ready() {
            return false;
        }
        self.record(record);
        true
    }

    pub(crate) fn did_draw(&self) {
        self.cached_readback.replace(None);
        // FIXME: Make use of the rectangle that changed to reduce the invalidated area when possible.
        self.canvas.did_change_content();
    }

    /// The pixels of the rectangle of the canvas, read back from the Compositor. Unpremultiplied pixels come back as
    /// RGBA8888, and premultiplied pixels as BGRA8888.
    pub(crate) fn read_pixels(&self, rect: IntRect, alpha_type: AlphaType) -> Option<Bitmap> {
        if !self.has_backing_storage() {
            return None;
        }
        if let Some(readback) = self.cached_readback.borrow().as_ref()
            && readback.rect == rect
            && readback.alpha_type == alpha_type
        {
            return Some(readback.pixels.clone());
        }
        let pixels = {
            let backing_storage = self.backing_storage.borrow();
            let recorder = &backing_storage
                .as_ref()
                .expect("the canvas has backing storage")
                .recorder;
            recorder.flush();
            recorder.read_back_pixels(rect, alpha_type)
        };
        if let Some(pixels) = &pixels {
            self.cached_readback.replace(Some(CachedReadback {
                pixels: pixels.clone(),
                rect,
                alpha_type,
            }));
        }
        pixels
    }

    pub(crate) fn drop_cached_readback(&self) {
        self.cached_readback.replace(None);
    }

    pub(crate) fn set_size(&self, size: IntSize) {
        if self.size.get() == size {
            return;
        }
        self.size.set(size);
        self.discard_backing_storage();
    }

    pub(crate) fn prepare_for_compositing(&self) {
        if let Some(backing_storage) = self.backing_storage.borrow().as_ref() {
            backing_storage.recorder.present();
        }
    }

    pub(crate) fn canvas_id(&self) -> Option<CanvasId> {
        self.backing_storage
            .borrow()
            .as_ref()
            .and_then(|backing_storage| backing_storage.recorder.canvas_id())
    }

    fn full_rect(&self) -> FloatRect {
        let size = self.size.get();
        #[allow(
            clippy::cast_precision_loss,
            reason = "the rectangle is in floats, as the C++ had it"
        )]
        FloatRect::new(0.0, 0.0, size.width as f32, size.height as f32)
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#reset-the-rendering-context-to-its-default-state
    pub(crate) fn reset_to_default_state(&self) {
        let has_recorder = self.has_backing_storage() && self.recorder_is_ready();

        // 1. Clear canvas's bitmap to transparent black.
        // NB: The clear is recorded after the reset of the recorder, which happens in step 4. The recorded clear is
        //     then not under the transformation matrix and the clipping region of the drawing state that is reset,
        //     so it clears the whole bitmap.
        if has_recorder {
            let rect = self.full_rect();
            let color = self.clear_color();
            self.record(|recorder| {
                recorder.reset();
                recorder.clear_rect(rect, color);
            });
        }

        // 2. Empty the list of subpaths in the context's current default path.
        self.clear_path();

        // 3. Clear the context's drawing state stack.
        // 4. Reset everything that drawing state consists of to their initial values.
        self.states.replace(vec![DrawingState::default()]);
        self.path_transform.set(AffineTransform::identity());

        if has_recorder {
            self.did_draw();
        }
    }

    /// Clears the bitmap of an OffscreenCanvas when transferToImageBitmap() takes it.
    pub(crate) fn replace_bitmap_with_cleared_bitmap(&self) {
        self.cached_readback.replace(None);
        // NB: The new bitmap is origin-clean, but the fill and stroke styles only taint the context when they are set.
        //     A pattern that is not origin-clean and is still a style of the context, or of a state on the stack,
        //     could then draw onto a clean bitmap, so the flag stays unset while one is.
        let holds_a_tainted_style = self
            .states
            .borrow()
            .iter()
            .any(|state| state.fill_style.is_not_origin_clean() || state.stroke_style.is_not_origin_clean());
        self.origin_clean.set(!holds_a_tainted_style);
        if !self.has_backing_storage() {
            return;
        }
        let color = self.clear_color();
        if self.record_if_ready(|recorder| recorder.clear_canvas(color)) {
            self.did_draw();
        }
    }

    /// Sets the transformation matrix of the recorder to the current one.
    pub(crate) fn flush_transform(&self) {
        let transform = self.drawing_state().transform;
        self.record_if_ready(|recorder| recorder.set_transform(transform));
    }

    /// What the font of the current drawing state is resolved with, for the CSS values that are resolved against the
    /// canvas.
    pub(crate) fn font_metrics_source(&self) -> FontMetricsSource {
        FontMetricsSource::of(self.drawing_state().font.as_ref())
    }

    /// https://drafts.csswg.org/css-color-4/#parse-a-css-color-value
    pub(crate) fn parse_a_css_color_value(&self, value: &Utf16String) -> Option<Color> {
        // To parse a CSS <color> value, given a string input, and an optional context element element:

        // 1. Parse input as a <color>. If the result is failure, return failure; otherwise, let color be the result.
        let input_is_cacheable = value.length_in_code_units() <= MAX_COLOR_CACHE_INPUT_LENGTH;
        let mut cached = None;
        if input_is_cacheable {
            let mut color_cache = self.color_cache.borrow_mut();
            if let Some(index) = color_cache.iter().position(|entry| entry.input == *value) {
                let entry = color_cache.remove(index);
                cached = Some((entry.parsed_value.clone(), entry.resolved_color));
                color_cache.push(entry);
            }
        }

        let (color, resolved_color) = match cached {
            Some(cached) => cached,
            None => {
                let resolved_color = match value.as_units() {
                    ak::Utf16StringUnits::Ascii(units) => libweb_rust::parse_simple_color_ascii(units),
                    ak::Utf16StringUnits::Utf16(units) => libweb_rust::parse_simple_color_utf16(units),
                }
                .map(|[red, green, blue, alpha]| Color::from_rgba(red, green, blue, alpha));
                let color = if resolved_color.is_none() {
                    parse_color(value)
                } else {
                    None
                };
                if input_is_cacheable {
                    let mut color_cache = self.color_cache.borrow_mut();
                    if color_cache.len() == MAX_COLOR_CACHE_SIZE {
                        color_cache.remove(0);
                    }
                    color_cache.push(ColorCacheEntry {
                        input: value.clone(),
                        parsed_value: color.clone(),
                        resolved_color,
                    });
                }
                (color, resolved_color)
            }
        };

        if resolved_color.is_some() {
            return resolved_color;
        }
        let color = color?;

        // 2. Let used color be the result of resolving color to a used color. If the value of other properties on the
        //    element a <color> is on is required to do the resolution (such as resolving a currentcolor or system
        //    color), use element if it was passed, or the initial values of the properties if not.
        // AD-HOC: The spec tells us to use the element's properties as context but doesn't tell us how to resolve other
        //         context dependent values (e.g. viewport relative lengths). This also differs from how font-relative
        //         lengths are absolutized (i.e. using the canvas' font rather than the element's) in other canvas values
        //         (e.g. letterSpacing) so we instead use a computation context based on the drawing state. See
        //         https://github.com/whatwg/html/issues/12505.
        let used_color = self.canvas.resolve_color(&color, self.font_metrics_source());

        // 3. Return used color.
        Some(used_color)
    }

    /// Sets the font of the current drawing state, unless the value is not a CSS font.
    pub(crate) fn assign_font(&self, font: &Utf16String) {
        if let Some(font) = self.canvas.resolve_font(font) {
            self.drawing_state_mut().font = Some(font);
        }
    }

    /// The font of the current drawing state, which is resolved the first time the context needs one, and again when
    /// the fonts that the canvas can use change.
    pub(crate) fn current_font(&self) -> ResolvedFont {
        // When font style value is empty load default font
        if self.drawing_state().font.is_none() {
            self.assign_font(&Utf16String::from_ascii(b"10px sans-serif"));
        }

        // NB: Drawing states, including saved states, retain their cascades across font loads and font-display
        //     transitions. Refresh them lazily so cached invisible fallback glyphs and metrics cannot outlive the font
        //     environment that selected them.
        let font = self
            .drawing_state()
            .font
            .clone()
            .expect("a context has a font once it is set");
        if font.environment_generation != self.canvas.font_environment_generation() {
            self.assign_font(&font.serialized);
            return self
                .drawing_state()
                .font
                .clone()
                .expect("a context has a font once it is set");
        }
        font
    }

    /// The letter spacing of the current drawing state in CSS pixels.
    pub(crate) fn resolved_letter_spacing(&self) -> f32 {
        let letter_spacing = self.drawing_state().letter_spacing.clone();
        match letter_spacing {
            Some(letter_spacing) => self
                .canvas
                .resolve_length(&letter_spacing.length, self.font_metrics_source()),
            None => 0.0,
        }
    }

    /// How a draw composites under the current drawing state.
    pub(crate) fn compositing(state: &DrawingState) -> Compositing<'_> {
        Compositing {
            filter: state.filter.as_deref(),
            global_alpha: state.global_alpha,
            compositing_and_blending_operator: state.current_compositing_and_blending_operator,
        }
    }

    /// A shadow is drawn through the filter with its operator, but without the global alpha, which its color holds.
    fn shadow_compositing(state: &DrawingState) -> Compositing<'_> {
        Compositing {
            filter: state.filter.as_deref(),
            global_alpha: 1.0,
            compositing_and_blending_operator: state.current_compositing_and_blending_operator,
        }
    }

    /// The color and the transformation matrix of the shadow of a draw, unless the draw casts none.
    fn shadow_of(state: &DrawingState) -> Option<(Color, AffineTransform)> {
        if state.current_compositing_and_blending_operator == CompositingAndBlendingOperator::Copy {
            return None;
        }
        if state.shadow_blur == 0.0 && state.shadow_offset_x == 0.0 && state.shadow_offset_y == 0.0 {
            return None;
        }
        let mut alpha = state.global_alpha * (f32::from(state.shadow_color.alpha()) / 255.0);
        if let Some(fill_style_color) = state.fill_style.as_color()
            && fill_style_color.alpha() > 0
        {
            alpha = (f32::from(fill_style_color.alpha()) / 255.0) * state.global_alpha;
        }
        if alpha == 0.0 {
            return None;
        }
        let transform = multiply_transforms(
            translated(
                AffineTransform::identity(),
                state.shadow_offset_x,
                state.shadow_offset_y,
            ),
            state.transform,
        );
        Some((state.shadow_color.with_opacity(alpha), transform))
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#drawing-model
    /// 7. Composite B within the clipping region over the current output bitmap using the current compositing and
    ///    blending operator.
    /// NB: A transparent source paint changes nothing only under source-over.
    fn transparent_source_paint_can_be_ignored(state: &DrawingState) -> bool {
        state.current_compositing_and_blending_operator == CompositingAndBlendingOperator::SourceOver
    }

    pub(crate) fn fill_internal(&self, path: &Path, winding_rule: WindingRule) {
        if !self.recorder_is_ready() {
            return;
        }
        {
            let state = self.drawing_state();
            if !state.fill_style.is_visible() && Self::transparent_source_paint_can_be_ignored(&state) {
                return;
            }
        }

        let path_bytes = path.serialize_to_bytes();
        self.paint_shadow_for_fill_internal(&path_bytes, winding_rule);

        {
            let state = self.drawing_state();
            self.record(|recorder| {
                let paint = paint_of(recorder, &state.fill_style);
                recorder.fill_path(&path_bytes, &paint, winding_rule, 0.0, &Self::compositing(&state));
            });
        }
        self.did_draw();
    }

    /// `path_bytes` is a serialized Gfx::Path. The caller has readied the recorder.
    fn paint_shadow_for_fill_internal(&self, path_bytes: &[u8], winding_rule: WindingRule) {
        {
            let state = self.drawing_state();
            let Some((shadow_color, transform)) = Self::shadow_of(&state) else {
                return;
            };
            self.record(|recorder| {
                recorder.save();
                recorder.set_transform(transform);
                recorder.fill_path(
                    path_bytes,
                    &Paint::Color(shadow_color),
                    winding_rule,
                    state.shadow_blur,
                    &Self::shadow_compositing(&state),
                );
                recorder.restore();
            });
        }
        self.did_draw();
    }

    pub(crate) fn stroke_internal(&self, path: &Path) {
        if !self.recorder_is_ready() {
            return;
        }
        {
            let state = self.drawing_state();
            if !state.stroke_style.is_visible() && Self::transparent_source_paint_can_be_ignored(&state) {
                return;
            }
        }

        let dash_array = self.drawing_state().dash_array();
        let path_bytes = path.serialize_to_bytes();
        self.paint_shadow_for_stroke_internal(&path_bytes, &dash_array);

        {
            let state = self.drawing_state();
            let stroke = Stroke {
                thickness: state.line_width,
                cap_style: state.cap_style(),
                join_style: state.join_style(),
                miter_limit: state.miter_limit,
                dash_array: &dash_array,
                dash_offset: state.line_dash_offset,
            };
            self.record(|recorder| {
                let paint = paint_of(recorder, &state.stroke_style);
                recorder.stroke_path(&path_bytes, &paint, &stroke, 0.0, &Self::compositing(&state));
            });
        }
        self.did_draw();
    }

    /// `path_bytes` is a serialized Gfx::Path. The caller has readied the recorder.
    fn paint_shadow_for_stroke_internal(&self, path_bytes: &[u8], dash_array: &[f32]) {
        {
            let state = self.drawing_state();
            let Some((shadow_color, transform)) = Self::shadow_of(&state) else {
                return;
            };
            let stroke = Stroke {
                thickness: state.line_width,
                cap_style: state.cap_style(),
                join_style: state.join_style(),
                miter_limit: state.miter_limit,
                dash_array,
                dash_offset: state.line_dash_offset,
            };
            self.record(|recorder| {
                recorder.save();
                recorder.set_transform(transform);
                recorder.stroke_path(
                    path_bytes,
                    &Paint::Color(shadow_color),
                    &stroke,
                    state.shadow_blur,
                    &Self::shadow_compositing(&state),
                );
                recorder.restore();
            });
        }
        self.did_draw();
    }

    pub(crate) fn clip_internal(&self, path: &Path, winding_rule: WindingRule) {
        if !self.recorder_is_ready() {
            return;
        }
        let path_bytes = path.serialize_to_bytes();
        self.record(|recorder| recorder.clip_path(&path_bytes, winding_rule));
    }
}

/// The transform translated by (x, y) in its own coordinates, as Gfx::AffineTransform::translate().
pub(crate) fn translated(transform: AffineTransform, x: f32, y: f32) -> AffineTransform {
    let [a, b, c, d, e, f] = transform.values;
    if a == 1.0 && b == 0.0 && c == 0.0 && d == 1.0 {
        return AffineTransform::new(a, b, c, d, e + x, f + y);
    }
    AffineTransform::new(a, b, c, d, e + (x * a + y * c), f + (x * b + y * d))
}

/// The transform scaled by (x, y) in its own coordinates, as Gfx::AffineTransform::scale().
pub(crate) fn scaled(transform: AffineTransform, x: f32, y: f32) -> AffineTransform {
    let [a, b, c, d, e, f] = transform.values;
    AffineTransform::new(a * x, b * x, c * y, d * y, e, f)
}

/// The transform multiplied by another, which applies first, as Gfx::AffineTransform::multiply().
pub(crate) fn multiply_transforms(transform: AffineTransform, other: AffineTransform) -> AffineTransform {
    if other.is_identity() {
        return transform;
    }
    let [a, b, c, d, e, f] = transform.values;
    let [oa, ob, oc, od, oe, of] = other.values;
    AffineTransform::new(
        oa * a + ob * c,
        oa * b + ob * d,
        oc * a + od * c,
        oc * b + od * d,
        oe * a + of * c + e,
        oe * b + of * d + f,
    )
}

/// The transform rotated by the angle in radians, as Gfx::AffineTransform::rotate_radians().
pub(crate) fn rotated(transform: AffineTransform, radians: f32) -> AffineTransform {
    let (sin, cos) = radians.sin_cos();
    multiply_transforms(transform, AffineTransform::new(cos, sin, -sin, cos, 0.0, 0.0))
}
