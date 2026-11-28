/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Qualified names, which Rust allocates, interns and frees, and C++ reads in place.
//! https://dom.spec.whatwg.org/#concept-element-qualified-name
//!
//! Names that are equal in every part share one [`QualifiedNameImpl`], so a name is told apart from another by its
//! address. The parts count the references to them, which C++'s DOM::QualifiedName takes and drops inline, and the last
//! one to go frees them through [`rust_dom_qualified_name_free`], whichever side drops it. Only the main thread makes,
//! counts and frees names.

use core::cell::UnsafeCell;
use core::mem::offset_of;
use core::ptr::NonNull;

use hashbrown::HashTable;

use super::strings::FlyStringField;
use super::strings::OptionalFlyStringField;
use super::strings::units_contain_ascii_uppercase;
use crate::stage::MainThread;

super::trace::fields_without_cells!(QualifiedNameField);

crate::stage::main_thread_ffi_entries!(without_host);

/// The interned parts of a qualified name, which C++ reads as DOM::QualifiedName::Impl. Rust makes, interns and frees
/// them, and C++ only counts the references to them, and fills in the uppercased name as it first asks for it.
///
/// NB: QualifiedName.cpp asserts the same layout.
#[repr(C)]
pub struct QualifiedNameImpl {
    reference_count: UnsafeCell<u32>,
    local_name: FlyStringField,
    lowercased_local_name: FlyStringField,
    prefix: OptionalFlyStringField,
    namespace: OptionalFlyStringField,
    as_string: FlyStringField,
    ascii_uppercased_as_string: OptionalFlyStringField,
}

const _: () = assert!(size_of::<QualifiedNameImpl>() == 56 && align_of::<QualifiedNameImpl>() == 8);
const _: () = assert!(offset_of!(QualifiedNameImpl, reference_count) == 0);
const _: () = assert!(offset_of!(QualifiedNameImpl, local_name) == 8);
const _: () = assert!(offset_of!(QualifiedNameImpl, lowercased_local_name) == 16);
const _: () = assert!(offset_of!(QualifiedNameImpl, prefix) == 24);
const _: () = assert!(offset_of!(QualifiedNameImpl, namespace) == 32);
const _: () = assert!(offset_of!(QualifiedNameImpl, as_string) == 40);
const _: () = assert!(offset_of!(QualifiedNameImpl, ascii_uppercased_as_string) == 48);

impl QualifiedNameImpl {
    /// https://dom.spec.whatwg.org/#concept-attribute-local-name
    #[inline(always)]
    pub fn local_name(&self) -> &FlyStringField {
        &self.local_name
    }

    /// The namespace, or none for the null namespace.
    #[inline(always)]
    pub fn namespace(&self) -> Option<&FlyStringField> {
        self.namespace.get()
    }

    /// The namespace prefix, or none for the null prefix.
    #[inline(always)]
    pub fn prefix(&self) -> Option<&FlyStringField> {
        self.prefix.get()
    }

    /// The qualified name as a string.
    #[inline(always)]
    pub fn as_string(&self) -> &FlyStringField {
        &self.as_string
    }

    /// The raw words of the local name, the prefix and the namespace, which name the parts in the table.
    #[inline(always)]
    fn key(&self) -> Key {
        Key {
            local_name: self.local_name.raw_identity(),
            prefix: self.prefix.get().map_or(0, FlyStringField::raw_identity),
            namespace: self.namespace.get().map_or(0, FlyStringField::raw_identity),
        }
    }

    /// Takes a reference to the parts.
    ///
    /// # Safety
    ///
    /// The call must be made on the main thread, which alone counts the references to qualified names.
    #[inline(always)]
    pub(super) unsafe fn add_reference(&self) {
        // SAFETY: Guaranteed by the caller.
        unsafe { *self.reference_count.get() += 1 };
    }
}

impl Drop for QualifiedNameImpl {
    fn drop(&mut self) {
        // SAFETY: The parts own a reference to each string they hold, which nothing borrows as they are freed.
        unsafe {
            drop(self.local_name.replace(ak::Utf16FlyString::default()));
            drop(self.lowercased_local_name.replace(ak::Utf16FlyString::default()));
            drop(self.prefix.replace(None));
            drop(self.namespace.replace(None));
            drop(self.as_string.replace(ak::Utf16FlyString::default()));
            drop(self.ascii_uppercased_as_string.replace(None));
        }
    }
}

/// What a name is interned by: the raw words of its local name, its prefix and its namespace, 0 for a null one.
#[derive(Clone, Copy, PartialEq, Eq)]
struct Key {
    local_name: usize,
    prefix: usize,
    namespace: usize,
}

impl Key {
    #[inline(always)]
    fn hash(self) -> u64 {
        let mixed =
            (self.local_name as u64) ^ (self.prefix as u64).rotate_left(21) ^ (self.namespace as u64).rotate_left(42);
        let mixed = mixed.wrapping_mul(0x9E37_79B9_7F4A_7C15);
        mixed ^ (mixed >> 29)
    }
}

/// The interned names, by their parts.
struct Names(HashTable<NonNull<QualifiedNameImpl>>);

impl Names {
    const fn new() -> Self {
        Self(HashTable::new())
    }

    #[inline(always)]
    fn find(&self, key: Key) -> Option<NonNull<QualifiedNameImpl>> {
        // SAFETY: The table holds live parts only, see remove().
        self.0
            .find(key.hash(), |parts| unsafe { parts.as_ref() }.key() == key)
            .copied()
    }

    fn insert(&mut self, parts: NonNull<QualifiedNameImpl>) {
        // SAFETY: The table holds live parts only, and `parts` is live.
        let hash = unsafe { parts.as_ref() }.key().hash();
        self.0.insert_unique(hash, parts, |parts| {
            // SAFETY: The table holds live parts only.
            unsafe { parts.as_ref() }.key().hash()
        });
    }

    fn remove(&mut self, parts: NonNull<QualifiedNameImpl>) {
        // SAFETY: `parts` is live until the caller frees it, after this.
        let hash = unsafe { parts.as_ref() }.key().hash();
        if let Ok(entry) = self.0.find_entry(hash, |entry| *entry == parts) {
            entry.remove();
        }
    }
}

#[cfg(not(test))]
mod names {
    use core::cell::UnsafeCell;

    use super::Names;
    use crate::stage::MainThread;

    #[repr(transparent)]
    struct InternedNames(UnsafeCell<Names>);

    // SAFETY: Only the main thread reads or writes the table, which every access proves with its token.
    unsafe impl Sync for InternedNames {}

    static NAMES: InternedNames = InternedNames(UnsafeCell::new(Names::new()));

    #[inline(always)]
    pub(super) fn with<R>(_main_thread: &mut MainThread<'_>, f: impl FnOnce(&mut Names) -> R) -> R {
        // SAFETY: Only the main thread reaches the table, see InternedNames, and nothing else borrows it across `f`,
        //         which calls into no one who could.
        f(unsafe { &mut *NAMES.0.get() })
    }
}

// NB: Unit tests run on many threads, each of which stands in for the main thread of its own DOM.
#[cfg(test)]
mod names {
    use core::cell::RefCell;

    use super::Names;
    use crate::stage::MainThread;

    std::thread_local! {
        static NAMES: RefCell<Names> = const { RefCell::new(Names::new()) };
    }

    pub(super) fn with<R>(_main_thread: &mut MainThread<'_>, f: impl FnOnce(&mut Names) -> R) -> R {
        NAMES.with_borrow_mut(f)
    }
}

/// The parts of the name whose local name, prefix and namespace are `local_name`, `prefix` and `namespace` (raw words of
/// interned strings, 0 for a null prefix or namespace), with a reference to them that passes to the caller.
///
/// # Safety
///
/// The strings must be live for the call.
pub(super) unsafe fn ensure(
    main_thread: &mut MainThread<'_>,
    local_name: usize,
    prefix: usize,
    namespace: usize,
) -> NonNull<QualifiedNameImpl> {
    let key = Key {
        local_name,
        prefix,
        namespace,
    };
    if let Some(parts) = names::with(main_thread, |names| names.find(key)) {
        // SAFETY: The table holds live parts, on the main thread.
        unsafe { parts.as_ref().add_reference() };
        return parts;
    }
    // SAFETY: Guaranteed by the caller.
    unsafe { make(main_thread, key) }
}

/// Makes and interns the parts of the name `key` names, with one reference, which passes to the caller.
///
/// # Safety
///
/// The strings `key` names must be live for the call.
#[inline(never)]
unsafe fn make(main_thread: &mut MainThread<'_>, key: Key) -> NonNull<QualifiedNameImpl> {
    // SAFETY: Guaranteed by the caller: each nonzero word is that of a live interned string.
    let borrowed = |raw: usize| unsafe { ak::Utf16FlyString::from_raw_borrowed(raw) };
    let local_name = borrowed(key.local_name);
    let lowercased_local_name = match units_contain_ascii_uppercase(local_name.as_units()) {
        false => local_name.clone(),
        true => {
            let mut units = local_name.to_utf16().into_owned();
            for unit in &mut units {
                if (u16::from(b'A')..=u16::from(b'Z')).contains(unit) {
                    *unit += u16::from(b'a' - b'A');
                }
            }
            ak::Utf16FlyString::from_utf16(&units)
        }
    };
    let prefix = (key.prefix != 0).then(|| borrowed(key.prefix));
    let namespace = (key.namespace != 0).then(|| borrowed(key.namespace));

    // https://dom.spec.whatwg.org/#concept-attribute-qualified-name
    // https://dom.spec.whatwg.org/#concept-element-qualified-name
    // NB: The spec allows this: "User agents could have this as an internal slot as an optimization."
    let as_string = match &prefix {
        None => local_name.clone(),
        Some(prefix) => {
            let mut units = prefix.to_utf16().into_owned();
            units.push(u16::from(b':'));
            units.extend_from_slice(&local_name.to_utf16());
            ak::Utf16FlyString::from_utf16(&units)
        }
    };

    let parts = Box::new(QualifiedNameImpl {
        reference_count: UnsafeCell::new(1),
        local_name: FlyStringField::from_owned(local_name),
        lowercased_local_name: FlyStringField::from_owned(lowercased_local_name),
        prefix: OptionalFlyStringField::from_owned(prefix),
        namespace: OptionalFlyStringField::from_owned(namespace),
        as_string: FlyStringField::from_owned(as_string),
        ascii_uppercased_as_string: OptionalFlyStringField::from_owned(None),
    });
    let parts = NonNull::from(Box::leak(parts));
    names::with(main_thread, |names| names.insert(parts));
    parts
}

/// Drops a reference to the parts of a name, which frees them where it is the last.
///
/// # Safety
///
/// The caller must hold the reference.
#[inline(always)]
pub(super) unsafe fn release(main_thread: &mut MainThread<'_>, parts: NonNull<QualifiedNameImpl>) {
    // SAFETY: Guaranteed by the caller, on the main thread.
    let count = unsafe { &mut *parts.as_ref().reference_count.get() };
    *count -= 1;
    if *count == 0 {
        // SAFETY: Nothing holds a reference to the parts any longer.
        unsafe { free(main_thread, parts) };
    }
}

/// Takes the parts of a name out of the table, and frees them.
///
/// # Safety
///
/// Nothing may hold a reference to the parts.
#[inline(never)]
unsafe fn free(main_thread: &mut MainThread<'_>, parts: NonNull<QualifiedNameImpl>) {
    names::with(main_thread, |names| names.remove(parts));
    // SAFETY: Guaranteed by the caller: the parts are a box that make() leaked, which no one refers to any more.
    drop(unsafe { Box::from_raw(parts.as_ptr()) });
}

/// The parts of the name whose local name, prefix and namespace are the strings whose raw words are `local_name`,
/// `prefix` and `namespace` (0 for null), with a reference of the caller's own to them, for DOM::QualifiedName.
///
/// # Safety
///
/// The strings must be live interned strings, and the call must be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_qualified_name_ensure(
    local_name: usize,
    prefix: usize,
    namespace: usize,
) -> NonNull<QualifiedNameImpl> {
    // SAFETY: Guaranteed by the caller.
    let mut main_thread = unsafe { main_thread() };
    // SAFETY: Guaranteed by the caller.
    unsafe { ensure(&mut main_thread, local_name, prefix, namespace) }
}

/// Frees the parts of a name, for DOM::QualifiedName as it drops the last reference to them.
///
/// # Safety
///
/// No one may hold a reference to the parts, and the call must be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_qualified_name_free(parts: NonNull<QualifiedNameImpl>) {
    // SAFETY: Guaranteed by the caller.
    let mut main_thread = unsafe { main_thread() };
    // SAFETY: Guaranteed by the caller.
    unsafe { free(&mut main_thread, parts) }
}

/// A field that holds a qualified name, laid out as C++'s DOM::QualifiedName, which is what C++ sees it as: a reference
/// to the name's interned parts, which C++ takes and drops as it constructs, assigns and destroys the field.
#[repr(transparent)]
pub struct QualifiedNameField(UnsafeCell<NonNull<QualifiedNameImpl>>);

impl QualifiedNameField {
    #[inline(always)]
    pub fn parts(&self) -> &QualifiedNameImpl {
        // SAFETY: C++ does not write the field while Rust borrows it, and the parts it names live as long as it does.
        unsafe { (*self.0.get()).as_ref() }
    }

    /// https://dom.spec.whatwg.org/#concept-element-local-name
    #[inline(always)]
    pub fn local_name(&self) -> &FlyStringField {
        &self.parts().local_name
    }

    /// The local name in ASCII lowercase.
    #[inline(always)]
    pub fn lowercased_local_name(&self) -> &FlyStringField {
        &self.parts().lowercased_local_name
    }

    /// The namespace, or none for the null namespace.
    /// https://dom.spec.whatwg.org/#concept-element-namespace
    #[inline(always)]
    pub fn namespace(&self) -> Option<&FlyStringField> {
        self.parts().namespace.get()
    }

    /// The qualified name as a string: the prefix, a colon and the local name, or the local name where there is no
    /// prefix.
    /// https://dom.spec.whatwg.org/#concept-element-qualified-name
    #[inline(always)]
    pub fn as_string(&self) -> &FlyStringField {
        &self.parts().as_string
    }

    /// The word that names the name, which is the same for every field holding a name equal in every part.
    #[inline(always)]
    pub fn identity(&self) -> usize {
        // SAFETY: C++ does not write the field while Rust borrows it.
        unsafe { (*self.0.get()).as_ptr() as usize }
    }
}

#[cfg(test)]
impl QualifiedNameField {
    /// The name `local_name` in `namespace`, which the test leaks.
    pub(crate) fn for_test(local_name: &str, namespace: Option<&str>) -> Self {
        let mut main_thread = MainThread::for_test();
        let local_name = ak::Utf16FlyString::from_utf8(local_name);
        let namespace = namespace.map(ak::Utf16FlyString::from_utf8);
        // SAFETY: The strings are live for the call.
        let parts = unsafe {
            ensure(
                &mut main_thread,
                local_name.raw_identity(),
                0,
                namespace.as_ref().map_or(0, ak::Utf16FlyString::raw_identity),
            )
        };
        Self(UnsafeCell::new(parts))
    }
}

const _: () = assert!(size_of::<QualifiedNameField>() == 8 && align_of::<QualifiedNameField>() == 8);

#[cfg(test)]
mod tests {
    use super::*;
    use crate::dom::strings::units_equal_ascii;

    #[test]
    fn a_name_reads_its_parts_in_place() {
        let name = QualifiedNameField::for_test("svg", Some("http://www.w3.org/2000/svg"));
        assert_eq!(
            name.local_name().raw_identity(),
            FlyStringField::for_test("svg").raw_identity()
        );
        assert_eq!(
            name.namespace().map(FlyStringField::raw_identity),
            Some(FlyStringField::for_test("http://www.w3.org/2000/svg").raw_identity())
        );
        assert!(QualifiedNameField::for_test("svg", None).namespace().is_none());
    }

    #[test]
    fn names_equal_in_every_part_share_their_parts_until_the_last_reference_goes() {
        let mut main_thread = MainThread::for_test();
        let local_name = ak::Utf16FlyString::from_utf8("viewBox");
        let prefix = ak::Utf16FlyString::from_utf8("svg");
        let namespace = ak::Utf16FlyString::from_utf8("http://www.w3.org/2000/svg");
        // SAFETY: The strings are live for the calls, and each reference taken is dropped once.
        unsafe {
            let a = ensure(&mut main_thread, local_name.raw_identity(), 0, 0);
            let b = ensure(&mut main_thread, local_name.raw_identity(), 0, 0);
            assert_eq!(a, b);
            assert!(units_equal_ascii(a.as_ref().lowercased_local_name.units(), "viewbox"));
            let prefixed = ensure(
                &mut main_thread,
                local_name.raw_identity(),
                prefix.raw_identity(),
                namespace.raw_identity(),
            );
            assert_ne!(a, prefixed);
            assert!(units_equal_ascii(prefixed.as_ref().as_string().units(), "svg:viewBox"));
            release(&mut main_thread, a);
            release(&mut main_thread, b);
            release(&mut main_thread, prefixed);
            assert!(names::with(&mut main_thread, |names| names.0.is_empty()));
        }
    }
}
