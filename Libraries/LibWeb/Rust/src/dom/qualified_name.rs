/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Qualified names, as C++ interns them and Rust reads them in place.
//! https://dom.spec.whatwg.org/#concept-element-qualified-name

use core::cell::UnsafeCell;
use core::mem::offset_of;
use core::ptr::NonNull;

use super::strings::FlyStringField;
use super::strings::OptionalFlyStringField;

/// The interned parts of a qualified name, laid out as C++'s DOM::QualifiedName::Impl, which C++ allocates, interns,
/// counts the references to and frees. Names that are equal in every part share one.
///
/// NB: This mirrors the C++ layout, which QualifiedName.cpp asserts.
#[repr(C)]
pub struct QualifiedNameImpl {
    _reference_count: UnsafeCell<u32>,
    local_name: FlyStringField,
    _lowercased_local_name: FlyStringField,
    _prefix: OptionalFlyStringField,
    namespace: OptionalFlyStringField,
    _as_string: FlyStringField,
    _ascii_uppercased_as_string: OptionalFlyStringField,
}

const _: () = assert!(size_of::<QualifiedNameImpl>() == 56 && align_of::<QualifiedNameImpl>() == 8);
const _: () = assert!(offset_of!(QualifiedNameImpl, local_name) == 8);
const _: () = assert!(offset_of!(QualifiedNameImpl, _lowercased_local_name) == 16);
const _: () = assert!(offset_of!(QualifiedNameImpl, _prefix) == 24);
const _: () = assert!(offset_of!(QualifiedNameImpl, namespace) == 32);
const _: () = assert!(offset_of!(QualifiedNameImpl, _as_string) == 40);
const _: () = assert!(offset_of!(QualifiedNameImpl, _ascii_uppercased_as_string) == 48);

/// A field that holds a qualified name, laid out as C++'s DOM::QualifiedName, which is what C++ sees it as: a reference
/// to the name's interned parts, which C++ takes and drops as it constructs, assigns and destroys the field.
#[repr(transparent)]
pub struct QualifiedNameField(UnsafeCell<NonNull<QualifiedNameImpl>>);

impl QualifiedNameField {
    #[inline(always)]
    fn parts(&self) -> &QualifiedNameImpl {
        // SAFETY: C++ does not write the field while Rust borrows it, and the parts it names live as long as it does.
        unsafe { (*self.0.get()).as_ref() }
    }

    /// https://dom.spec.whatwg.org/#concept-element-local-name
    #[inline(always)]
    pub fn local_name(&self) -> &FlyStringField {
        &self.parts().local_name
    }

    /// The namespace, or none for the null namespace.
    /// https://dom.spec.whatwg.org/#concept-element-namespace
    #[inline(always)]
    pub fn namespace(&self) -> Option<&FlyStringField> {
        self.parts().namespace.get()
    }
}

#[cfg(test)]
impl QualifiedNameField {
    /// The name `local_name` in `namespace`, whose parts the test leaks.
    pub(crate) fn for_test(local_name: &str, namespace: Option<&str>) -> Self {
        let parts = Box::new(QualifiedNameImpl {
            _reference_count: UnsafeCell::new(1),
            local_name: FlyStringField::for_test(local_name),
            _lowercased_local_name: FlyStringField::for_test(&local_name.to_ascii_lowercase()),
            _prefix: OptionalFlyStringField::for_test(None),
            namespace: OptionalFlyStringField::for_test(namespace),
            _as_string: FlyStringField::for_test(local_name),
            _ascii_uppercased_as_string: OptionalFlyStringField::for_test(None),
        });
        Self(UnsafeCell::new(NonNull::from(Box::leak(parts))))
    }
}

const _: () = assert!(size_of::<QualifiedNameField>() == 8 && align_of::<QualifiedNameField>() == 8);

#[cfg(test)]
mod tests {
    use super::*;

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
}
