/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The parts of the Objective-C runtime that the window server is driven through: classes and selectors by name,
//! messages sent through `objc_msgSend`, and the reference counts of objects.
//!
//! `objc_msgSend` is called through a function pointer of the exact type of each message, as the compiler would call
//! it. Two rules keep that sound on every Mac:
//! - No message returns a struct. On Intel, a struct too large for registers comes back through
//!   `objc_msgSend_stret`, which this does not reach, so getters of geometry are never sent.
//! - Nothing newer than the deployment target is linked by name. A method is asked about with `respondsToSelector:`
//!   and a constant is looked up with `dlsym` first, so that the process still starts on an older system.

use std::ffi::{CStr, c_char, c_void};
use std::ptr::NonNull;
use std::sync::atomic::{AtomicPtr, Ordering};

/// A selector, which the runtime keeps for as long as the process lives.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(transparent)]
pub struct Sel(*const c_void);

/// A class, which the runtime keeps for as long as the process lives.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(transparent)]
pub struct Class(NonNull<c_void>);

/// The `BOOL` of a message, which is a `signed char` on Intel and a `bool` on Apple silicon. Either is passed and
/// returned as one byte that is 0 or 1.
pub type ObjcBool = i8;

#[link(name = "objc", kind = "dylib")]
unsafe extern "C" {
    fn objc_getClass(name: *const c_char) -> *mut c_void;
    fn sel_registerName(name: *const c_char) -> Sel;
    fn class_getProperty(class: *mut c_void, name: *const c_char) -> *mut c_void;
    fn class_getClassMethod(class: *mut c_void, selector: Sel) -> *mut c_void;
    fn objc_msgSend();
    fn objc_retain(object: *mut c_void) -> *mut c_void;
    fn objc_release(object: *mut c_void);
    fn objc_autoreleasePoolPush() -> *mut c_void;
    fn objc_autoreleasePoolPop(pool: *mut c_void);
}

unsafe extern "C" {
    fn dlsym(handle: *mut c_void, symbol: *const c_char) -> *mut c_void;
}

// `RTLD_DEFAULT` of <dlfcn.h> on macOS.
const RTLD_DEFAULT: *mut c_void = -2isize as *mut c_void;

/// The address of a symbol of any image loaded into the process, or null where none has it.
pub fn address_of_symbol(name: &CStr) -> *mut c_void {
    // SAFETY: The name is a C string.
    unsafe { dlsym(RTLD_DEFAULT, name.as_ptr()) }
}

/// The selector named `name`, which ends in a nul, cached in `cache`.
pub fn cached_selector(cache: &AtomicPtr<c_void>, name: &'static str) -> Sel {
    let cached = cache.load(Ordering::Relaxed);
    if !cached.is_null() {
        return Sel(cached);
    }
    let name = CStr::from_bytes_with_nul(name.as_bytes()).expect("a selector name ends in a nul");
    // SAFETY: The name is a C string.
    let selector = unsafe { sel_registerName(name.as_ptr()) };
    cache.store(selector.0.cast_mut(), Ordering::Relaxed);
    selector
}

/// The selector of a literal name, looked up once.
macro_rules! sel {
    ($name:literal) => {{
        static CACHE: std::sync::atomic::AtomicPtr<std::ffi::c_void> =
            std::sync::atomic::AtomicPtr::new(std::ptr::null_mut());
        $crate::window_server::core_animation::objc::cached_selector(&CACHE, concat!($name, "\0"))
    }};
}
pub(crate) use sel;

impl Class {
    /// The class of the name, where the runtime has one.
    pub fn named(name: &CStr) -> Option<Class> {
        // SAFETY: The name is a C string.
        NonNull::new(unsafe { objc_getClass(name.as_ptr()) }).map(Class)
    }

    pub fn as_ptr(self) -> *mut c_void {
        self.0.as_ptr()
    }

    pub fn has_property(self, name: &CStr) -> bool {
        // SAFETY: The class is live, and the name is a C string.
        !unsafe { class_getProperty(self.as_ptr(), name.as_ptr()) }.is_null()
    }

    pub fn responds_to_class_message(self, selector: Sel) -> bool {
        // SAFETY: The class is live.
        !unsafe { class_getClassMethod(self.as_ptr(), selector) }.is_null()
    }

    pub fn is_subclass_of(self, other: Class) -> bool {
        // SAFETY: Both classes are live, and the message takes a class and answers a BOOL.
        unsafe { send::<_, ObjcBool>(self.as_ptr(), sel!("isSubclassOfClass:"), (other.as_ptr(),)) != 0 }
    }

    /// A new object of the class, made with `alloc` and `init`.
    pub fn new_object(self) -> Id {
        // SAFETY: `alloc` and `init` answer a new object that the caller owns.
        unsafe {
            let allocated: *mut c_void = send(self.as_ptr(), sel!("alloc"), ());
            let initialized: *mut c_void = send(allocated, sel!("init"), ());
            Id::adopt(initialized).expect("a new object")
        }
    }
}

/// Arguments of a message, which are passed to `objc_msgSend` as the C function of the message's type takes them.
///
/// # Safety
///
/// Each argument must be of a type that the C calling convention passes as the Objective-C method declares it.
pub unsafe trait MessageArguments {
    /// # Safety
    ///
    /// The receiver must answer the selector with a method that takes these arguments and returns `R`, which must not
    /// be a struct.
    unsafe fn send<R>(receiver: *mut c_void, selector: Sel, arguments: Self) -> R;
}

macro_rules! message_arguments {
    ($($argument:ident: $type:ident),*) => {
        // SAFETY: The arguments are passed as the C function type of the message passes them.
        unsafe impl<$($type),*> MessageArguments for ($($type,)*) {
            unsafe fn send<R>(receiver: *mut c_void, selector: Sel, ($($argument,)*): Self) -> R {
                // SAFETY: `objc_msgSend` jumps to the method, which the caller vouches takes these arguments.
                let function = unsafe {
                    std::mem::transmute::<unsafe extern "C" fn(), unsafe extern "C" fn(*mut c_void, Sel $(, $type)*) -> R>(
                        objc_msgSend,
                    )
                };
                // SAFETY: As above.
                unsafe { function(receiver, selector $(, $argument)*) }
            }
        }
    };
}

message_arguments!();
message_arguments!(a: A);
message_arguments!(a: A, b: B);
message_arguments!(a: A, b: B, c: C);

/// Sends the message to the receiver.
///
/// # Safety
///
/// The receiver must be a live object or class that answers the selector with a method that takes these arguments and
/// returns `R`, which must not be a struct.
pub unsafe fn send<A: MessageArguments, R>(receiver: *mut c_void, selector: Sel, arguments: A) -> R {
    // SAFETY: The caller vouches for the message.
    unsafe { A::send(receiver, selector, arguments) }
}

/// An object of which this holds one reference.
#[derive(Debug, PartialEq, Eq)]
pub struct Id(NonNull<c_void>);

impl Id {
    /// Takes over a reference the caller owns, as one that `alloc`, `new`, `copy` or a `Create` function answers.
    ///
    /// # Safety
    ///
    /// A non-null object must be live, with a reference that the caller hands over.
    pub unsafe fn adopt(object: *mut c_void) -> Option<Id> {
        NonNull::new(object).map(Id)
    }

    /// Takes a reference of its own to an object, as one that an autoreleasing message answers.
    ///
    /// # Safety
    ///
    /// A non-null object must be live.
    pub unsafe fn retain(object: *mut c_void) -> Option<Id> {
        // SAFETY: The caller passes a live object.
        NonNull::new(object).map(|object| Id(NonNull::new(unsafe { objc_retain(object.as_ptr()) }).expect("an object")))
    }

    pub fn as_ptr(&self) -> *mut c_void {
        self.0.as_ptr()
    }

    pub fn responds_to(&self, selector: Sel) -> bool {
        // SAFETY: Every object answers `respondsToSelector:`.
        unsafe { send::<_, ObjcBool>(self.as_ptr(), sel!("respondsToSelector:"), (selector,)) != 0 }
    }
}

impl Clone for Id {
    fn clone(&self) -> Self {
        // SAFETY: This holds a reference to a live object.
        unsafe { Id::retain(self.as_ptr()) }.expect("an object")
    }
}

impl Drop for Id {
    fn drop(&mut self) {
        // SAFETY: This gives up the reference it holds.
        unsafe { objc_release(self.as_ptr()) };
    }
}

/// Drains the objects that are autoreleased while it is held, as `@autoreleasepool` does.
pub struct AutoreleasePool(*mut c_void);

impl AutoreleasePool {
    pub fn push() -> Self {
        // SAFETY: A pool is popped in the order it was pushed, as this is dropped.
        Self(unsafe { objc_autoreleasePoolPush() })
    }
}

impl Drop for AutoreleasePool {
    fn drop(&mut self) {
        // SAFETY: The pool was pushed by this, and pools nested in it were popped before.
        unsafe { objc_autoreleasePoolPop(self.0) };
    }
}
