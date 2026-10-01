/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use ak::Utf16FlyString;

use crate::bytecode::property_access::Strict;
use crate::gc::visitor::{Trace, Visitor};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::value::Value;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::environment::{Environment, InitializeBindingHint};
use crate::runtime::environment_coordinate::EnvironmentCoordinate;
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::object::{Object, PropertyLookupPhase, ShouldThrowExceptions};
use crate::runtime::private_environment::PrivateName;
use crate::runtime::property_key::PropertyKey;
use crate::utf16::Utf16View;

/// Mirrors Reference::BaseType.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum BaseType {
    Unresolvable,
    Value,
    Environment,
}

/// The [[ReferencedName]] of a Reference: a property key, or the private name of a private reference.
#[derive(Clone)]
enum ReferencedName {
    PropertyKey(PropertyKey),
    PrivateName(PrivateName),
}

// 6.2.4 The Reference Record Specification Type, https://tc39.es/ecma262/#sec-reference-record-specification-type
#[derive(Clone)]
pub struct Reference {
    name: ReferencedName,
    /// The [[Base]] of a reference whose base type is Value. C++ keeps it in a union with the base environment.
    base_value: Value,
    base_environment: Option<Gc<Environment>>,
    this_value: Option<Value>,
    environment_coordinate: Option<EnvironmentCoordinate>,
    base_type: BaseType,
    strict: Strict,
}

// SAFETY: Visits the base, the property key and the this value, as Reference::visit_edges does.
unsafe impl Trace for Reference {
    fn trace(&self, visitor: &mut Visitor) {
        match self.base_type {
            BaseType::Value => self.base_value.trace(visitor),
            BaseType::Environment => self.base_environment.trace(visitor),
            BaseType::Unresolvable => {}
        }
        match &self.name {
            ReferencedName::PropertyKey(key) => key.trace(visitor),
            // No GC pointers.
            ReferencedName::PrivateName(_) => {}
        }
        self.this_value.trace(visitor);
    }
}

impl Reference {
    pub fn with_base_type(base_type: BaseType, name: PropertyKey, strict: Strict) -> Self {
        Self {
            name: ReferencedName::PropertyKey(name),
            base_value: Value::UNDEFINED,
            base_environment: None,
            this_value: None,
            environment_coordinate: None,
            base_type,
            strict,
        }
    }

    pub fn with_base_value(base: Value, name: PropertyKey, this_value: Option<Value>, strict: Strict) -> Self {
        Self {
            name: ReferencedName::PropertyKey(name),
            base_value: base,
            base_environment: None,
            this_value,
            environment_coordinate: None,
            base_type: BaseType::Value,
            strict,
        }
    }

    pub fn with_base_environment(
        base: Gc<Environment>,
        referenced_name: Utf16FlyString,
        strict: Strict,
        environment_coordinate: Option<EnvironmentCoordinate>,
    ) -> Self {
        Self {
            name: ReferencedName::PropertyKey(PropertyKey::from(referenced_name)),
            base_value: Value::UNDEFINED,
            base_environment: Some(base),
            this_value: None,
            environment_coordinate,
            base_type: BaseType::Environment,
            strict,
        }
    }

    pub fn with_private_name(base: Value, name: PrivateName) -> Self {
        Self {
            name: ReferencedName::PrivateName(name),
            base_value: base,
            base_environment: None,
            this_value: None,
            environment_coordinate: None,
            base_type: BaseType::Value,
            strict: Strict::Yes,
        }
    }

    pub fn base(&self) -> Value {
        assert!(self.base_type == BaseType::Value);
        self.base_value
    }

    pub fn base_environment(&self) -> Gc<Environment> {
        assert!(self.base_type == BaseType::Environment);
        self.base_environment
            .expect("an environment reference has its environment")
    }

    pub fn name(&self) -> &PropertyKey {
        match &self.name {
            ReferencedName::PropertyKey(key) => key,
            ReferencedName::PrivateName(_) => panic!("the reference is a private reference"),
        }
    }

    pub fn private_name(&self) -> &PrivateName {
        match &self.name {
            ReferencedName::PrivateName(name) => name,
            ReferencedName::PropertyKey(_) => panic!("the reference is not a private reference"),
        }
    }

    pub fn is_strict(&self) -> bool {
        self.strict == Strict::Yes
    }

    // 6.2.4.2 IsUnresolvableReference ( V ), https://tc39.es/ecma262/#sec-isunresolvablereference
    pub fn is_unresolvable(&self) -> bool {
        self.base_type == BaseType::Unresolvable
    }

    // 6.2.4.1 IsPropertyReference ( V ), https://tc39.es/ecma262/#sec-ispropertyreference
    pub fn is_property_reference(&self) -> bool {
        if self.is_unresolvable() {
            return false;
        }
        if self.base_type == BaseType::Environment {
            return false;
        }
        true
    }

    // 6.2.4.7 GetThisValue ( V ), https://tc39.es/ecma262/#sec-getthisvalue
    pub fn get_this_value(&self) -> Value {
        assert!(self.is_property_reference());
        if let Some(this_value) = self.this_value {
            return this_value;
        }
        self.base_value
    }

    // 6.2.4.3 IsSuperReference ( V ), https://tc39.es/ecma262/#sec-issuperreference
    pub fn is_super_reference(&self) -> bool {
        self.this_value.is_some()
    }

    // 6.2.4.4 IsPrivateReference ( V ), https://tc39.es/ecma262/#sec-isprivatereference
    pub fn is_private_reference(&self) -> bool {
        matches!(self.name, ReferencedName::PrivateName(_))
    }

    // Note: Non-standard helper.
    pub fn is_environment_reference(&self) -> bool {
        self.base_type == BaseType::Environment
    }

    pub fn is_valid_reference(&self) -> bool {
        true
    }

    pub fn environment_coordinate(&self) -> Option<EnvironmentCoordinate> {
        self.environment_coordinate
    }

    /// The base environment as the declarative environment the reference's coordinate indexes into.
    fn coordinate_environment(&self) -> &crate::runtime::declarative_environment::DeclarativeEnvironment {
        let environment = self
            .base_environment
            .as_ref()
            .expect("an environment reference has its environment");
        environment
            .as_declarative_environment()
            .expect("only declarative environments report binding coordinates")
    }

    // 6.2.4.6 PutValue ( V, W ), https://tc39.es/ecma262/#sec-putvalue
    pub fn put_value(&self, vm: &Vm, value: Value) -> ThrowCompletionOr<()> {
        // 1. ReturnIfAbrupt(V).
        // 2. ReturnIfAbrupt(W).

        // 3. If V is not a Reference Record, throw a ReferenceError exception.
        if !self.is_valid_reference() {
            return vm.throw_completion(ErrorKind::ReferenceError, ErrorType::InvalidLeftHandAssignment, &[]);
        }

        // 4. If IsUnresolvableReference(V) is true, then
        if self.is_unresolvable() {
            // a. If V.[[Strict]] is true, throw a ReferenceError exception.
            if self.strict == Strict::Yes {
                return self.throw_reference_error(vm);
            }

            // b. Let globalObj be GetGlobalObject().
            let global_object = vm.get_global_object();

            // c. Perform ? Set(globalObj, V.[[ReferencedName]], W, false).
            global_object.set(vm, self.name(), value, ShouldThrowExceptions::No)?;

            // Return unused.
            return Ok(());
        }

        // 5. If IsPropertyReference(V) is true, then
        if self.is_property_reference() {
            // a. Let baseObj be ? ToObject(V.[[Base]]).
            let base_obj = self.base_value.to_object(vm)?;

            // b. If IsPrivateReference(V) is true, then
            if self.is_private_reference() {
                // i. Return ? PrivateSet(baseObj, V.[[ReferencedName]], W).
                return base_obj.private_set(vm, self.private_name(), value);
            }

            // c. Let succeeded be ? baseObj.[[Set]](V.[[ReferencedName]], W, GetThisValue(V)).
            let succeeded = base_obj.internal_set(
                vm,
                self.name(),
                value,
                self.get_this_value(),
                None,
                PropertyLookupPhase::OwnProperty,
            )?;

            // d. If succeeded is false and V.[[Strict]] is true, throw a TypeError exception.
            if !succeeded && self.strict == Strict::Yes {
                return vm.throw_completion(
                    ErrorKind::TypeError,
                    ErrorType::ReferenceNullishSetProperty,
                    &[self.name(), &self.base_value],
                );
            }

            // e. Return unused.
            return Ok(());
        }

        // 6. Else,
        // a. Let base be V.[[Base]].

        // b. Assert: base is an Environment Record.
        assert!(self.base_type == BaseType::Environment);
        let base_environment = self.base_environment();

        // c. Return ? base.SetMutableBinding(V.[[ReferencedName]], W, V.[[Strict]]) (see 9.1).
        if let Some(environment_coordinate) = self.environment_coordinate {
            return self.coordinate_environment().set_mutable_binding_direct(
                vm,
                environment_coordinate.index as usize,
                value,
                self.strict == Strict::Yes,
            );
        }
        base_environment.set_mutable_binding(vm, self.name().as_string(), value, self.strict == Strict::Yes)
    }

    fn throw_reference_error<T>(&self, vm: &Vm) -> ThrowCompletionOr<T> {
        if self.is_private_reference() {
            return vm.throw_completion(ErrorKind::ReferenceError, ErrorType::ReferenceUnresolvable, &[]);
        }
        vm.throw_completion(
            ErrorKind::ReferenceError,
            ErrorType::UnknownIdentifier,
            &[&Utf16View::of_string(&self.name().to_utf16_string()).to_utf8()],
        )
    }

    // 6.2.4.5 GetValue ( V ), https://tc39.es/ecma262/#sec-getvalue
    pub fn get_value(&self, vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. ReturnIfAbrupt(V).
        // 2. If V is not a Reference Record, return V.

        // 3. If IsUnresolvableReference(V) is true, throw a ReferenceError exception.
        if !self.is_valid_reference() || self.is_unresolvable() {
            return self.throw_reference_error(vm);
        }

        // 4. If IsPropertyReference(V) is true, then
        if self.is_property_reference() {
            // a. Let baseObj be ? ToObject(V.[[Base]]).
            // NOTE: Deferred as an optimization; we might not actually need to create an object.

            // b. If IsPrivateReference(V) is true, then
            if self.is_private_reference() {
                // FIXME: We need to be able to specify the receiver for this
                // if we want to use it in error messages in future
                // as things currently stand this does the "wrong thing" but
                // the error is unobservable

                let base_obj = self.base_value.to_object(vm)?;

                // i. Return ? PrivateGet(baseObj, V.[[ReferencedName]]).
                return base_obj.private_get(vm, self.private_name());
            }

            // OPTIMIZATION: For various primitives we can avoid actually creating a new object for them.
            let realm = || {
                vm.current_realm()
                    .expect("GetValue runs in an execution context with a realm")
            };
            let base_obj: Gc<Object> = if self.base_value.is_string() {
                let string_value = self.base_value.as_string().get(vm, self.name())?;
                if let Some(string_value) = string_value {
                    return Ok(string_value);
                }
                realm().string_prototype()
            } else if self.base_value.is_number() {
                realm().number_prototype()
            } else if self.base_value.is_boolean() {
                realm().boolean_prototype()
            } else if self.base_value.is_bigint() {
                realm().bigint_prototype()
            } else if self.base_value.is_symbol() {
                realm().symbol_prototype()
            } else {
                self.base_value.to_object(vm)?
            };

            // c. Return ? baseObj.[[Get]](V.[[ReferencedName]], GetThisValue(V)).
            return base_obj.internal_get(
                vm,
                self.name(),
                self.get_this_value(),
                None,
                PropertyLookupPhase::OwnProperty,
            );
        }

        // 5. Else,
        // a. Let base be V.[[Base]].

        // b. Assert: base is an Environment Record.
        assert!(self.base_type == BaseType::Environment);
        let base_environment = self.base_environment();

        // c. Return ? base.GetBindingValue(V.[[ReferencedName]], V.[[Strict]]) (see 9.1).
        if let Some(environment_coordinate) = self.environment_coordinate {
            return self
                .coordinate_environment()
                .get_binding_value_direct(vm, environment_coordinate.index as usize);
        }
        base_environment.get_binding_value(vm, self.name().as_string(), self.strict == Strict::Yes)
    }

    // 13.5.1.2 Runtime Semantics: Evaluation, https://tc39.es/ecma262/#sec-delete-operator-runtime-semantics-evaluation
    pub fn delete_(&self, vm: &Vm) -> ThrowCompletionOr<bool> {
        // 13.5.1.2 Runtime Semantics: Evaluation, https://tc39.es/ecma262/#sec-delete-operator-runtime-semantics-evaluation
        // UnaryExpression : delete UnaryExpression

        // NOTE: The following steps have already been evaluated by the time we get here:
        // 1. Let ref be the result of evaluating UnaryExpression.
        // 2. ReturnIfAbrupt(ref).
        // 3. If ref is not a Reference Record, return true.

        // 4. If IsUnresolvableReference(ref) is true, then
        if self.is_unresolvable() {
            // a. Assert: ref.[[Strict]] is false.
            assert!(self.strict == Strict::No);
            // b. Return true.
            return Ok(true);
        }

        // 5. If IsPropertyReference(ref) is true, then
        if self.is_property_reference() {
            // a. Assert: IsPrivateReference(ref) is false.
            assert!(!self.is_private_reference());

            // b. If IsSuperReference(ref) is true, throw a ReferenceError exception.
            if self.is_super_reference() {
                return vm.throw_completion(
                    ErrorKind::ReferenceError,
                    ErrorType::UnsupportedDeleteSuperProperty,
                    &[],
                );
            }

            // c. Let baseObj be ? ToObject(ref.[[Base]]).
            let base_obj = self.base_value.to_object(vm)?;

            // d. Let deleteStatus be ? baseObj.[[Delete]](ref.[[ReferencedName]]).
            let delete_status = base_obj.internal_delete(vm, self.name())?;

            // e. If deleteStatus is false and ref.[[Strict]] is true, throw a TypeError exception.
            if !delete_status && self.strict == Strict::Yes {
                return vm.throw_completion(
                    ErrorKind::TypeError,
                    ErrorType::ReferenceNullishDeleteProperty,
                    &[self.name(), &self.base_value],
                );
            }

            // f. Return deleteStatus.
            return Ok(delete_status);
        }

        // 6. Else,
        //    a. Let base be ref.[[Base]].
        //    b. Assert: base is an Environment Record.

        assert!(self.base_type == BaseType::Environment);

        //    c. Return ? base.DeleteBinding(ref.[[ReferencedName]]).
        self.base_environment().delete_binding(vm, self.name().as_string())
    }

    // 6.2.4.8 InitializeReferencedBinding ( V, W ), https://tc39.es/ecma262/#sec-object.prototype.hasownproperty
    // 1.2.1.1 InitializeReferencedBinding ( V, W, hint ), https://tc39.es/proposal-explicit-resource-management/#sec-initializereferencedbinding
    pub fn initialize_referenced_binding(
        &self,
        vm: &Vm,
        value: Value,
        hint: InitializeBindingHint,
    ) -> ThrowCompletionOr<()> {
        assert!(!self.is_unresolvable());
        assert!(self.base_type == BaseType::Environment);
        self.base_environment()
            .initialize_binding(vm, self.name().as_string(), value, hint)
    }
}

// 6.2.4.9 MakePrivateReference ( baseValue, privateIdentifier ), https://tc39.es/ecma262/#sec-makeprivatereference
pub fn make_private_reference(vm: &Vm, base_value: Value, private_identifier: &Utf16FlyString) -> Reference {
    let context = vm
        .running_execution_context()
        .expect("MakePrivateReference runs in an execution context");

    // 1. Let privEnv be the running execution context's PrivateEnvironment.
    // SAFETY: The running execution context is live.
    let private_environment = unsafe { context.as_ref() }.private_environment.get();

    // 2. Assert: privEnv is not null.
    let private_environment = private_environment.expect("there is a private environment");

    // 3. Let privateName be ResolvePrivateIdentifier(privEnv, privateIdentifier).
    let private_name = private_environment.resolve_private_identifier(private_identifier);

    // 4. Return the Reference Record { [[Base]]: baseValue, [[ReferencedName]]: privateName, [[Strict]]: true, [[ThisValue]]: empty }.
    Reference::with_private_name(base_value, private_name)
}
