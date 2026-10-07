/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::ops::Deref;

use ak::Utf16String;
use libjs_runtime_macros::Trace;

use super::completion::{Throw, ThrowCompletionOr};
use super::error_types::ErrorType;
use crate::gc::class::{Class, GcCell, define_cell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::aggregate_error::AggregateError;
use crate::runtime::error_data::{CompactTraceback, ErrorData};
use crate::runtime::object::MayInterfereWithIndexedPropertyAccess;
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::realm::Realm;
use crate::runtime::suppressed_error::SuppressedError;
use crate::utf16::Utf16Display;

/// The constructors of the errors the runtime and its embedder throw: %Error%, the NativeError constructors,
/// %AggregateError% and %SuppressedError%.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ErrorKind {
    Error,
    EvalError,
    InternalError,
    RangeError,
    ReferenceError,
    SyntaxError,
    TypeError,
    URIError,
    AggregateError,
    SuppressedError,
}

impl ErrorKind {
    /// T::create(realm) for the error class T this kind names: a new error of this kind in `realm`, without a
    /// "message".
    pub fn create_without_message(self, vm: &Vm, realm: Gc<Realm>) -> Gc<Error> {
        match self {
            Self::Error => Error::create(vm, realm),
            Self::EvalError => EvalError::create(vm, realm).upcast(),
            Self::InternalError => InternalError::create(vm, realm).upcast(),
            Self::RangeError => RangeError::create(vm, realm).upcast(),
            Self::ReferenceError => ReferenceError::create(vm, realm).upcast(),
            Self::SyntaxError => SyntaxError::create(vm, realm).upcast(),
            Self::TypeError => TypeError::create(vm, realm).upcast(),
            Self::URIError => URIError::create(vm, realm).upcast(),
            Self::AggregateError => AggregateError::create(vm, realm).upcast(),
            Self::SuppressedError => SuppressedError::create(vm, realm).upcast(),
        }
    }

    /// T::create(realm, message) for the error class T this kind names: a new error of this kind in `realm`, whose
    /// "message" is `message`.
    pub fn create(self, vm: &Vm, realm: Gc<Realm>, message: Utf16String) -> Gc<Error> {
        let error = self.create_without_message(vm, realm);
        error.set_message(vm, message);
        error
    }
}

impl Vm {
    /// 5.2.3.2 Throw an Exception, https://tc39.es/ecma262/#sec-throw-an-exception
    #[cold]
    pub fn throw_completion<T>(
        &self,
        kind: ErrorKind,
        error_type: ErrorType,
        arguments: &[&dyn Utf16Display],
    ) -> ThrowCompletionOr<T> {
        self.throw_completion_with_utf16_message(kind, error_type.message(arguments))
    }

    /// Throws a new error of `kind` with `message`.
    #[cold]
    pub fn throw_completion_with_message<T>(&self, kind: ErrorKind, message: String) -> ThrowCompletionOr<T> {
        self.throw_completion_with_utf16_message(kind, Utf16String::from_utf8(&message))
    }

    /// Throws a new error of `kind` with `message`, for the messages that can hold any code unit of a string.
    #[cold]
    pub fn throw_completion_with_utf16_message<T>(
        &self,
        kind: ErrorKind,
        message: Utf16String,
    ) -> ThrowCompletionOr<T> {
        let realm = if kind == ErrorKind::TypeError {
            self.type_error_realm()
        } else {
            self.current_realm()
        };
        let realm = realm.expect("an error is thrown in an execution context with a realm");
        let completion = Value::from_object(kind.create(self, realm, message));
        crate::embedding::completion::log_exception_if_enabled(self, completion);
        Err(Throw::new(completion))
    }
}

/// The Error objects, which have an [[ErrorData]] internal slot. The NativeError objects extend it.
#[repr(C)]
#[derive(Trace)]
pub struct Error {
    base: Object,
    error_data: ErrorData,
}

define_cell!(Error, Object, extends: [Object]);

impl Deref for Error {
    type Target = Object;

    fn deref(&self) -> &Object {
        &self.base
    }
}

/// The error_data method of ordinary objects, which finds the slot of the Error objects among them.
pub fn error_data_of_error(object: &Object) -> Option<&ErrorData> {
    if !object.is::<Error>() {
        return None;
    }
    // SAFETY: The object is an Error, which starts with its Object.
    let error = unsafe { &*core::ptr::from_ref(object).cast::<Error>() };
    Some(&error.error_data)
}

impl Error {
    /// An error with `prototype`, for `class`, which is Error or a class that extends it.
    pub fn new(vm: &Vm, class: &'static Class, prototype: Gc<Object>) -> Error {
        Error {
            base: Object::new_with_prototype(vm, class, prototype, MayInterfereWithIndexedPropertyAccess::No),
            error_data: ErrorData::new(vm),
        }
    }

    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<Error> {
        realm.create_object(vm, Error::new(vm, Error::CLASS, realm.intrinsics().error_prototype(vm)))
    }

    pub fn create_with_message(vm: &Vm, realm: Gc<Realm>, message: Utf16String) -> Gc<Error> {
        let error = Error::create(vm, realm);
        error.set_message(vm, message);
        error
    }

    pub fn stack_string(&self, compact: CompactTraceback) -> Utf16String {
        self.error_data.stack_string(compact)
    }

    // 20.5.8.1 InstallErrorCause ( O, options ), https://tc39.es/ecma262/#sec-installerrorcause
    pub fn install_error_cause(&self, vm: &Vm, options: Value) -> ThrowCompletionOr<()> {
        // 1. If Type(options) is Object and ? HasProperty(options, "cause") is true, then
        if options.is_object() && options.as_object().has_property(vm, &vm.names.cause)? {
            // a. Let cause be ? Get(options, "cause").
            let cause = options.as_object().get(vm, &vm.names.cause)?;

            // b. Perform CreateNonEnumerableDataPropertyOrThrow(O, "cause", cause).
            self.create_non_enumerable_data_property_or_throw(vm, &vm.names.cause, cause);
        }

        // 2. Return unused.
        Ok(())
    }

    pub fn set_message(&self, vm: &Vm, message: Utf16String) {
        let attributes = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        self.define_direct_property(
            vm,
            &vm.names.message,
            Value::from_string(PrimitiveString::create(vm, message)),
            attributes,
        );
    }
}

// NOTE: Making these inherit from Error is not required by the spec but
//       our way of implementing the [[ErrorData]] internal slot, which is
//       used in Object.prototype.toString().
macro_rules! define_native_errors {
    ($($class:ident: $prototype:ident;)*) => {
        $(
            #[repr(C)]
            #[derive(Trace)]
            pub struct $class {
                base: Error,
            }

            define_cell!($class, Object, extends: [Error, Object]);

            impl Deref for $class {
                type Target = Error;

                fn deref(&self) -> &Error {
                    &self.base
                }
            }

            impl $class {
                pub fn new(vm: &Vm, prototype: Gc<Object>) -> $class {
                    $class {
                        base: Error::new(vm, Self::CLASS, prototype),
                    }
                }

                pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<$class> {
                    realm.create_object(vm, $class::new(vm, realm.intrinsics().$prototype(vm)))
                }

                pub fn create_with_message(vm: &Vm, realm: Gc<Realm>, message: Utf16String) -> Gc<$class> {
                    let error = $class::create(vm, realm);
                    error.set_message(vm, message);
                    error
                }
            }
        )*
    };
}

define_native_errors! {
    EvalError: eval_error_prototype;
    InternalError: internal_error_prototype;
    RangeError: range_error_prototype;
    ReferenceError: reference_error_prototype;
    SyntaxError: syntax_error_prototype;
    TypeError: type_error_prototype;
    URIError: uri_error_prototype;
}
