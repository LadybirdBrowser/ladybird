/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The CoreAnimation and AVFoundation objects that a remote layer is made of, each message typed as its method is
//! declared. `CAContext`, `CABackdropLayer` and `CAFilter` are not public API, so they are looked up and checked before
//! they are used.

use super::frameworks::{CFStringRef, CFTypeRef, CGAffineTransform, CGColorRef, CGFloat, CGPathRef, CGPoint, CGRect};
use super::objc::{Class, Id, ObjcBool, address_of_symbol, sel, send};
use std::ffi::{CStr, c_char, c_void};
use std::sync::OnceLock;

#[link(name = "QuartzCore", kind = "framework")]
unsafe extern "C" {
    static kCAFilterNearest: CFStringRef;
    static kCAFilterLinear: CFStringRef;
    static kCAFilterTrilinear: CFStringRef;
    static kCAFillRuleNonZero: CFStringRef;
    static kCAFillRuleEvenOdd: CFStringRef;
}

#[link(name = "AVFoundation", kind = "framework")]
unsafe extern "C" {
    static AVLayerVideoGravityResize: CFStringRef;
}

// The corners of CACornerMask, named in a layer's own coordinates.
pub const MIN_X_MIN_Y_CORNER: usize = 1;
pub const MAX_X_MIN_Y_CORNER: usize = 2;
pub const MIN_X_MAX_Y_CORNER: usize = 4;
pub const MAX_X_MAX_Y_CORNER: usize = 8;

fn yes_or_no(value: bool) -> ObjcBool {
    ObjcBool::from(value)
}

/// A class of the system, which is there on every system this runs on.
fn class_of_system(cache: &'static OnceLock<SystemClass>, name: &CStr) -> Class {
    cache
        .get_or_init(|| SystemClass(Class::named(name).expect("a class of the system")))
        .0
}

// Classes live for as long as the process, and are only messaged on the main thread.
struct SystemClass(Class);
// SAFETY: A class is never freed, and is only used on the main thread.
unsafe impl Send for SystemClass {}
// SAFETY: As above.
unsafe impl Sync for SystemClass {}

fn layer_class() -> Class {
    static CLASS: OnceLock<SystemClass> = OnceLock::new();
    class_of_system(&CLASS, c"CALayer")
}

fn shape_layer_class() -> Class {
    static CLASS: OnceLock<SystemClass> = OnceLock::new();
    class_of_system(&CLASS, c"CAShapeLayer")
}

fn transaction_class() -> Class {
    static CLASS: OnceLock<SystemClass> = OnceLock::new();
    class_of_system(&CLASS, c"CATransaction")
}

fn sample_buffer_display_layer_class() -> Class {
    static CLASS: OnceLock<SystemClass> = OnceLock::new();
    class_of_system(&CLASS, c"AVSampleBufferDisplayLayer")
}

fn value_class() -> Class {
    static CLASS: OnceLock<SystemClass> = OnceLock::new();
    class_of_system(&CLASS, c"NSValue")
}

/// A class that is not public API, where this system has it and it answers what it is used for.
fn private_class(cache: &'static OnceLock<Option<SystemClass>>, find: impl FnOnce() -> Option<Class>) -> Option<Class> {
    cache
        .get_or_init(|| find().map(SystemClass))
        .as_ref()
        .map(|class| class.0)
}

fn context_class() -> Option<Class> {
    static CLASS: OnceLock<Option<SystemClass>> = OnceLock::new();
    private_class(&CLASS, || {
        let class = Class::named(c"CAContext")?;
        if !class.responds_to_class_message(sel!("contextWithCGSConnection:options:")) {
            return None;
        }
        // The properties are generated at run time, so instances do not answer to their selectors before one exists.
        if !class.has_property(c"contextId") || !class.has_property(c"layer") {
            return None;
        }
        Some(class)
    })
}

fn backdrop_layer_class() -> Option<Class> {
    static CLASS: OnceLock<Option<SystemClass>> = OnceLock::new();
    private_class(&CLASS, || {
        Class::named(c"CABackdropLayer").filter(|class| class.is_subclass_of(layer_class()))
    })
}

fn filter_class() -> Option<Class> {
    static CLASS: OnceLock<Option<SystemClass>> = OnceLock::new();
    private_class(&CLASS, || {
        Class::named(c"CAFilter").filter(|class| class.responds_to_class_message(sel!("filterWithType:")))
    })
}

type MainConnectionIdFunction = unsafe extern "C" fn() -> u32;

fn main_window_server_connection_id_function() -> Option<MainConnectionIdFunction> {
    static FUNCTION: OnceLock<usize> = OnceLock::new();
    let address = *FUNCTION.get_or_init(|| address_of_symbol(c"CGSMainConnectionID") as usize);
    // SAFETY: CGSMainConnectionID takes nothing and answers the connection of the process to the window server.
    (address != 0).then(|| unsafe { std::mem::transmute::<usize, MainConnectionIdFunction>(address) })
}

/// Whether the layer trees that another process shows by the ID of their context can be made.
pub fn contexts_are_available() -> bool {
    main_window_server_connection_id_function().is_some() && context_class().is_some()
}

/// Whether a layer can run what the window server shows below it through filters.
pub fn backdrop_filters_are_available() -> bool {
    backdrop_layer_class().is_some() && filter_class().is_some()
}

/// Changes made outside an explicit transaction open an implicit one, which only a run loop commits, and while it is
/// open it holds back the explicit ones nested in it as well. This process has no run loop, so every change is made in
/// an explicit transaction, and any implicit one that something opened anyway is flushed after it.
pub mod transaction {
    use super::*;

    pub fn begin() {
        // SAFETY: `+begin` takes nothing.
        unsafe { send::<_, ()>(transaction_class().as_ptr(), sel!("begin"), ()) };
    }

    pub fn commit() {
        // SAFETY: `+commit` takes nothing.
        unsafe { send::<_, ()>(transaction_class().as_ptr(), sel!("commit"), ()) };
    }

    pub fn flush() {
        // SAFETY: `+flush` takes nothing.
        unsafe { send::<_, ()>(transaction_class().as_ptr(), sel!("flush"), ()) };
    }

    pub fn set_disable_actions(disable: bool) {
        // SAFETY: `+setDisableActions:` takes a BOOL.
        unsafe {
            send::<_, ()>(
                transaction_class().as_ptr(),
                sel!("setDisableActions:"),
                (yes_or_no(disable),),
            );
        }
    }
}

/// A `CALayer`, or a layer of a subclass of it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Layer(Id);

/// Sends a setter to an object.
macro_rules! set {
    ($object:expr, $selector:literal, $value:expr) => {{
        let value = $value;
        // SAFETY: The object answers the setter, which takes the value as its property's type.
        unsafe { send::<_, ()>($object.as_ptr(), sel!($selector), (value,)) }
    }};
}

impl Layer {
    /// A new layer with its anchor point at its origin, so that its position places its origin.
    pub fn new() -> Self {
        Self::of_class(layer_class())
    }

    fn of_class(class: Class) -> Self {
        let layer = Self(class.new_object());
        layer.set_anchor_point(CGPoint { x: 0.0, y: 0.0 });
        layer
    }

    /// A new `CABackdropLayer`, where the system has one.
    pub fn new_backdrop() -> Option<Self> {
        backdrop_layer_class().map(Self::of_class)
    }

    pub fn as_ptr(&self) -> *mut c_void {
        self.0.as_ptr()
    }

    pub fn set_anchor_point(&self, point: CGPoint) {
        set!(self, "setAnchorPoint:", point);
    }

    pub fn set_geometry_flipped(&self, flipped: bool) {
        set!(self, "setGeometryFlipped:", yes_or_no(flipped));
    }

    pub fn set_frame(&self, frame: CGRect) {
        set!(self, "setFrame:", frame);
    }

    pub fn set_bounds(&self, bounds: CGRect) {
        set!(self, "setBounds:", bounds);
    }

    pub fn set_position(&self, position: CGPoint) {
        set!(self, "setPosition:", position);
    }

    pub fn set_affine_transform(&self, transform: CGAffineTransform) {
        set!(self, "setAffineTransform:", transform);
    }

    /// Shows the object, such as an IOSurface, as the layer's contents, or nothing.
    pub fn set_contents(&self, contents: CFTypeRef) {
        set!(self, "setContents:", contents);
    }

    pub fn has_contents(&self) -> bool {
        // SAFETY: `-contents` answers an object or nil.
        !unsafe { send::<_, *mut c_void>(self.as_ptr(), sel!("contents"), ()) }.is_null()
    }

    pub fn set_contents_rect(&self, rect: CGRect) {
        set!(self, "setContentsRect:", rect);
    }

    pub fn set_contents_scale(&self, scale: CGFloat) {
        set!(self, "setContentsScale:", scale);
    }

    pub fn contents_are_flipped(&self) -> bool {
        // SAFETY: `-contentsAreFlipped` answers a BOOL.
        unsafe { send::<_, ObjcBool>(self.as_ptr(), sel!("contentsAreFlipped"), ()) != 0 }
    }

    pub fn set_masks_to_bounds(&self, masks: bool) {
        set!(self, "setMasksToBounds:", yes_or_no(masks));
    }

    pub fn set_allows_group_opacity(&self, allows: bool) {
        set!(self, "setAllowsGroupOpacity:", yes_or_no(allows));
    }

    pub fn set_allows_edge_antialiasing(&self, allows: bool) {
        set!(self, "setAllowsEdgeAntialiasing:", yes_or_no(allows));
    }

    /// Has the window server cut every edge of the layer at whole pixels. An edge of a new layer that falls between
    /// pixels covers them only in part, so two layers that meet there let what is below them show through.
    pub fn stop_antialiasing_edges(&self) {
        const NO_EDGES_OF_CA_EDGE_ANTIALIASING_MASK: u32 = 0;
        set!(self, "setEdgeAntialiasingMask:", NO_EDGES_OF_CA_EDGE_ANTIALIASING_MASK);
    }

    pub fn set_opacity(&self, opacity: f32) {
        set!(self, "setOpacity:", opacity);
    }

    pub fn set_opaque(&self, opaque: bool) {
        set!(self, "setOpaque:", yes_or_no(opaque));
    }

    pub fn set_corner_radius(&self, radius: CGFloat) {
        set!(self, "setCornerRadius:", radius);
    }

    /// The corners that the radius rounds, as a mask of the `*_CORNER` constants.
    pub fn set_masked_corners(&self, corners: usize) {
        set!(self, "setMaskedCorners:", corners);
    }

    pub fn set_background_color(&self, color: CGColorRef) {
        set!(self, "setBackgroundColor:", color);
    }

    pub fn set_mask(&self, mask: Option<&Layer>) {
        set!(self, "setMask:", mask.map_or(std::ptr::null_mut(), Layer::as_ptr));
    }

    /// The sublayers, bottom to top, replacing the ones it had.
    pub fn set_sublayers(&self, sublayers: &[&Layer]) {
        let pointers: Vec<CFTypeRef> = sublayers.iter().map(|layer| layer.as_ptr().cast_const()).collect();
        let array = super::frameworks::CfRetained::array(&pointers);
        set!(self, "setSublayers:", array.as_ptr());
    }

    pub fn add_sublayer(&self, sublayer: &Layer) {
        set!(self, "addSublayer:", sublayer.as_ptr());
    }

    pub fn replace_sublayer(&self, sublayer: &Layer, replacement: &Layer) {
        // SAFETY: `-replaceSublayer:with:` takes two layers.
        unsafe {
            send::<_, ()>(
                self.as_ptr(),
                sel!("replaceSublayer:with:"),
                (sublayer.as_ptr(), replacement.as_ptr()),
            );
        }
    }

    pub fn remove_from_superlayer(&self) {
        // SAFETY: `-removeFromSuperlayer` takes nothing.
        unsafe { send::<_, ()>(self.as_ptr(), sel!("removeFromSuperlayer"), ()) };
    }

    /// The layer this is a sublayer of, if it is one.
    pub fn superlayer(&self) -> Option<Layer> {
        // SAFETY: `-superlayer` answers a layer or nil, which this retains.
        unsafe { Id::retain(send::<_, *mut c_void>(self.as_ptr(), sel!("superlayer"), ())) }.map(Layer)
    }

    pub fn set_magnification_filter(&self, filter: ScalingFilter) {
        let name = filter.name();
        set!(self, "setMagnificationFilter:", name);
    }

    pub fn set_minification_filter(&self, filter: ScalingFilter) {
        let name = filter.name();
        set!(self, "setMinificationFilter:", name);
    }

    pub fn set_filters(&self, filters: &[Id]) {
        let pointers: Vec<CFTypeRef> = filters.iter().map(|filter| filter.as_ptr().cast_const()).collect();
        let array = super::frameworks::CfRetained::array(&pointers);
        set!(self, "setFilters:", array.as_ptr());
    }

    /// Extended dynamic range costs power, so a layer asks for it only while it shows content brighter than SDR white.
    /// The window server then tone maps the content to the headroom of the display it is on.
    pub fn let_show_high_dynamic_range(&self, shows_high_dynamic_range: bool) {
        if self.0.responds_to(sel!("setPreferredDynamicRange:")) {
            let name = if shows_high_dynamic_range {
                c"CADynamicRangeHigh"
            } else {
                c"CADynamicRangeStandard"
            };
            let dynamic_range = address_of_symbol(name).cast::<CFStringRef>();
            if !dynamic_range.is_null() {
                // SAFETY: The symbol is the NSString constant of the name.
                let dynamic_range = unsafe { *dynamic_range };
                set!(self, "setPreferredDynamicRange:", dynamic_range);
                return;
            }
        }
        if self.0.responds_to(sel!("setWantsExtendedDynamicRangeContent:")) {
            set!(
                self,
                "setWantsExtendedDynamicRangeContent:",
                yes_or_no(shows_high_dynamic_range)
            );
        }
    }

    /// How many times brighter than SDR white the brightest content of the layer is, where the system takes it.
    pub fn set_contents_headroom(&self, headroom: CGFloat) {
        if self.0.responds_to(sel!("setContentsHeadroom:")) {
            set!(self, "setContentsHeadroom:", headroom);
        }
    }
}

/// How a layer filters its contents when it scales them.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ScalingFilter {
    Nearest,
    Linear,
    Trilinear,
}

impl ScalingFilter {
    fn name(self) -> CFStringRef {
        // SAFETY: The constants are NSStrings of QuartzCore.
        unsafe {
            match self {
                ScalingFilter::Nearest => kCAFilterNearest,
                ScalingFilter::Linear => kCAFilterLinear,
                ScalingFilter::Trilinear => kCAFilterTrilinear,
            }
        }
    }
}

/// A `CAShapeLayer`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ShapeLayer(Layer);

impl ShapeLayer {
    pub fn new() -> Self {
        Self(Layer::of_class(shape_layer_class()))
    }

    pub fn layer(&self) -> &Layer {
        &self.0
    }

    pub fn set_path(&self, path: CGPathRef) {
        set!(self.0, "setPath:", path);
    }

    pub fn set_fill_rule_even_odd(&self, even_odd: bool) {
        // SAFETY: The constants are NSStrings of QuartzCore.
        let rule = unsafe {
            if even_odd {
                kCAFillRuleEvenOdd
            } else {
                kCAFillRuleNonZero
            }
        };
        set!(self.0, "setFillRule:", rule);
    }
}

/// An `AVSampleBufferDisplayLayer`, which shows the video frames enqueued in its renderer as AVFoundation shows them.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct SampleBufferDisplayLayer(Layer);

// AVQueuedSampleBufferRenderingStatusFailed of AVQueuedSampleBufferRendering.h.
const RENDERING_STATUS_FAILED: isize = 2;

impl SampleBufferDisplayLayer {
    pub fn new() -> Self {
        let layer = Self(Layer::of_class(sample_buffer_display_layer_class()));
        // SAFETY: AVLayerVideoGravityResize is an NSString of AVFoundation.
        let gravity = unsafe { AVLayerVideoGravityResize };
        set!(layer.0, "setVideoGravity:", gravity);
        layer
    }

    pub fn layer(&self) -> &Layer {
        &self.0
    }

    fn renderer(&self) -> *mut c_void {
        // SAFETY: `-sampleBufferRenderer` answers the layer's renderer.
        unsafe { send::<_, *mut c_void>(self.0.as_ptr(), sel!("sampleBufferRenderer"), ()) }
    }

    /// Shows the sample buffer as soon as it is enqueued. The renderer stops after failing, or after the system took
    /// away its decoding resources, until it is flushed.
    pub fn enqueue(&self, sample_buffer: CFTypeRef) {
        let renderer = self.renderer();
        // SAFETY: The renderer answers these as AVQueuedSampleBufferRendering declares them.
        unsafe {
            let status: isize = send(renderer, sel!("status"), ());
            let requires_flush: ObjcBool = send(renderer, sel!("requiresFlushToResumeDecoding"), ());
            if status == RENDERING_STATUS_FAILED || requires_flush != 0 {
                send::<_, ()>(renderer, sel!("flush"), ());
            }
            send::<_, ()>(renderer, sel!("enqueueSampleBuffer:"), (sample_buffer,));
            let status: isize = send(renderer, sel!("status"), ());
            if status == RENDERING_STATUS_FAILED {
                let error: *mut c_void = send(renderer, sel!("error"), ());
                eprintln!(
                    "Compositor: AVFoundation failed to show a video surface: {}",
                    description_of(error)
                );
            }
        }
    }

    pub fn remove_displayed_image(&self) {
        let renderer = self.renderer();
        // SAFETY: `-flushWithRemovalOfDisplayedImage:completionHandler:` takes a BOOL and a block, which can be nil.
        unsafe {
            send::<_, ()>(
                renderer,
                sel!("flushWithRemovalOfDisplayedImage:completionHandler:"),
                (yes_or_no(true), std::ptr::null_mut::<c_void>()),
            );
        }
    }
}

/// What `-description` says of an object, or nothing for nil.
fn description_of(object: *mut c_void) -> String {
    if object.is_null() {
        return String::new();
    }
    // SAFETY: Every object answers `-description` with an NSString, which answers `-UTF8String`.
    unsafe {
        let description: *mut c_void = send(object, sel!("description"), ());
        if description.is_null() {
            return String::new();
        }
        let string: *const c_char = send(description, sel!("UTF8String"), ());
        if string.is_null() {
            return String::new();
        }
        CStr::from_ptr(string).to_string_lossy().into_owned()
    }
}

/// A `CAContext`, which carries a layer tree to the window server, where a `CALayerHost` of another process shows it by
/// the context's ID.
pub struct Context(Id);

impl Context {
    /// A context of this process's connection to the window server, where the system has contexts.
    pub fn create() -> Option<Self> {
        let connection_id = main_window_server_connection_id_function()?;
        let class = context_class()?;
        let options = super::frameworks::CfRetained::dictionary(&[]);
        // SAFETY: `+contextWithCGSConnection:options:` takes the connection and a dictionary, and answers an
        // autoreleased context, which this retains.
        let context = unsafe {
            let connection = connection_id();
            Id::retain(send::<_, *mut c_void>(
                class.as_ptr(),
                sel!("contextWithCGSConnection:options:"),
                (connection, options.as_ptr()),
            ))
        }?;
        Some(Self(context))
    }

    pub fn id(&self) -> u32 {
        // SAFETY: `-contextId` answers a uint32_t.
        unsafe { send::<_, u32>(self.0.as_ptr(), sel!("contextId"), ()) }
    }

    pub fn set_layer(&self, layer: Option<&Layer>) {
        set!(self.0, "setLayer:", layer.map_or(std::ptr::null_mut(), Layer::as_ptr));
    }
}

/// A `CAFilter` of a type, such as `gaussianBlur`, where the system has filters.
pub fn filter_of_type(type_name: &CStr) -> Option<Id> {
    let class = filter_class()?;
    let type_name = super::frameworks::CfRetained::string(type_name);
    // SAFETY: `+filterWithType:` takes an NSString and answers an autoreleased filter, which this retains.
    unsafe {
        Id::retain(send::<_, *mut c_void>(
            class.as_ptr(),
            sel!("filterWithType:"),
            (type_name.as_ptr(),),
        ))
    }
}

/// Sets a value of an object by key, as `-setValue:forKey:` does.
pub fn set_value_for_key(object: &Id, value: CFTypeRef, key: &CStr) {
    let key = super::frameworks::CfRetained::string(key);
    // SAFETY: Every object answers `-setValue:forKey:`, which takes an object and an NSString.
    unsafe { send::<_, ()>(object.as_ptr(), sel!("setValue:forKey:"), (value, key.as_ptr())) };
}

/// An `NSValue` of the bytes of a value of the Objective-C type encoded as `type_encoding`.
pub fn value_with_bytes(bytes: *const c_void, type_encoding: &CStr) -> Option<Id> {
    // SAFETY: `+valueWithBytes:objCType:` copies the bytes of a value of the type, and answers an autoreleased value,
    // which this retains.
    unsafe {
        Id::retain(send::<_, *mut c_void>(
            value_class().as_ptr(),
            sel!("valueWithBytes:objCType:"),
            (bytes, type_encoding.as_ptr()),
        ))
    }
}
