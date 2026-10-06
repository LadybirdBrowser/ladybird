/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use ak::Utf16FlyString;
use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::native_function::raw_native;
use crate::runtime::object::{MayInterfereWithIndexedPropertyAccess, ORDINARY_OBJECT_METHODS, define_object_class};
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::realm::Realm;

#[cfg(unix)]
use {
    crate::contrib::test262::agents::{self, AgentFailure, AgentRole, BroadcastNumber},
    crate::runtime::abstract_operations::call,
    crate::runtime::array_buffer::{ArrayBuffer, DataBlockStorage},
    crate::runtime::big_int::{BigInt, SignedBigInteger},
    crate::runtime::error::ErrorKind,
    crate::runtime::error_types::ErrorType,
    crate::runtime::primitive_string::PrimitiveString,
    crate::utf16::{Utf16View, utf16_from_wtf8},
    ak::Utf16String,
    std::os::fd::AsFd,
};

#[repr(C)]
#[derive(Trace)]
pub struct AgentObject {
    base: Object,
}

define_object_class!(AgentObject, extends: [Object], methods: {
    initialize: AgentObject::initialize,
    ..ORDINARY_OBJECT_METHODS
});

impl AgentObject {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<AgentObject> {
        realm.create_object(
            vm,
            AgentObject {
                base: Object::new_without_prototype(vm, Self::CLASS, realm, MayInterfereWithIndexedPropertyAccess::No),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        let attr = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        let define = |name: &str, function, length| {
            object.define_native_function(
                vm,
                realm,
                &PropertyKey::from(Utf16FlyString::from_utf8(name)),
                function,
                length,
                attr,
                None,
            );
        };
        define("monotonicNow", raw_native!(AgentObject::monotonic_now), 0);
        define("sleep", raw_native!(AgentObject::sleep), 1);

        // Agents run in processes of their own, which only Unix hosts set up.
        #[cfg(unix)]
        match agents::role() {
            AgentRole::Test => {
                define("start", raw_native!(AgentObject::start), 1);
                define("broadcast", raw_native!(AgentObject::broadcast), 2);
                define("getReport", raw_native!(AgentObject::get_report), 0);
            }
            AgentRole::Agent => {
                define("receiveBroadcast", raw_native!(AgentObject::receive_broadcast), 1);
                define("report", raw_native!(AgentObject::report), 1);
                define("leaving", raw_native!(AgentObject::leaving), 0);
            }
            AgentRole::Unavailable => {}
        }
    }

    #[allow(clippy::unnecessary_wraps, reason = "native functions return a completion")]
    fn monotonic_now(_vm: &Vm) -> ThrowCompletionOr<Value> {
        let milliseconds = monotonic_time_now_in_milliseconds();
        #[allow(
            clippy::cast_precision_loss,
            reason = "the milliseconds are returned as a JavaScript number, which is a double"
        )]
        Ok(Value::from_f64(milliseconds as f64))
    }

    fn sleep(vm: &Vm) -> ThrowCompletionOr<Value> {
        let milliseconds = vm.argument(0).to_i32(vm)?;
        // NB: Core::System::sleep_ms() takes a u32, so a negative count of milliseconds wraps around.
        sleep_milliseconds(milliseconds.cast_unsigned());
        Ok(Value::UNDEFINED)
    }
}

#[cfg(unix)]
impl AgentObject {
    fn start(vm: &Vm) -> ThrowCompletionOr<Value> {
        let source = vm.argument(0).to_utf16_string(vm)?;
        if let Err(message) = agents::start_agent(&Utf16View::of_string(&source).to_wtf8()) {
            return vm.throw_completion_with_message(ErrorKind::Error, message);
        }
        Ok(Value::UNDEFINED)
    }

    fn broadcast(vm: &Vm) -> ThrowCompletionOr<Value> {
        let buffer = vm.argument(0);
        let shared_array_buffer = buffer
            .is_object()
            .then(|| buffer.as_object().downcast::<ArrayBuffer>())
            .flatten()
            .filter(|array_buffer| array_buffer.is_shared_array_buffer());
        let Some(shared_array_buffer) = shared_array_buffer else {
            return vm.throw_completion_with_message(
                ErrorKind::TypeError,
                "$262.agent.broadcast() takes a SharedArrayBuffer".to_string(),
            );
        };

        let number = vm.argument(1);
        let number = if number.is_undefined() {
            BroadcastNumber::Undefined
        } else if number.is_bigint() {
            BroadcastNumber::BigInt(number.as_bigint().big_integer().to_signed_bytes_le())
        } else {
            BroadcastNumber::Number(number.to_number(vm)?.as_f64())
        };

        let shared_memory = shared_array_buffer.with_data_block(|block| match &block.byte_buffer {
            DataBlockStorage::Shared(store) => store
                .shared_memory()
                .try_clone_to_owned()
                .ok()
                .map(|shared_memory| (shared_memory, store.object_id())),
            _ => None,
        });
        let Some((shared_memory, object_id)) = shared_memory else {
            return vm.throw_completion_with_message(
                ErrorKind::TypeError,
                "Agents cannot map a SharedArrayBuffer that is not in shared memory".to_string(),
            );
        };
        agents::broadcast(
            shared_memory.as_fd(),
            shared_array_buffer.byte_length(),
            object_id,
            &number,
        );
        Ok(Value::UNDEFINED)
    }

    fn get_report(vm: &Vm) -> ThrowCompletionOr<Value> {
        match agents::take_report() {
            Ok(Some(report)) => {
                let report = utf16_from_wtf8(&report).expect("agents report WTF-8");
                Ok(Value::from_string(PrimitiveString::create(
                    vm,
                    Utf16String::from_utf16(&report),
                )))
            }
            Ok(None) => Ok(Value::NULL),
            // Keeping an unimplemented feature an InternalError lets the runner count the test as a TODO.
            Err(AgentFailure::UncaughtException { name, message }) if name == "InternalError" => {
                vm.throw_completion_with_message(ErrorKind::InternalError, message)
            }
            Err(AgentFailure::UncaughtException { name, message }) => {
                vm.throw_completion_with_message(ErrorKind::Error, format!("An agent threw {name}: {message}"))
            }
            Err(AgentFailure::Panic(message) | AgentFailure::Exited(message)) => panic!("{message}"),
        }
    }

    fn receive_broadcast(vm: &Vm) -> ThrowCompletionOr<Value> {
        let callback = vm.argument(0);
        if !callback.is_function() {
            return vm.throw_completion_with_message(
                ErrorKind::TypeError,
                "$262.agent.receiveBroadcast() takes a function".to_string(),
            );
        }

        let broadcast = agents::receive_broadcast();
        let realm = vm
            .current_realm()
            .expect("$262.agent.receiveBroadcast() runs in a realm");
        let Ok(shared_array_buffer) = ArrayBuffer::create_from_shared_memory(
            vm,
            realm,
            broadcast.shared_memory.as_fd(),
            broadcast.byte_length,
            broadcast.object_id,
        ) else {
            return vm.throw_completion(
                ErrorKind::RangeError,
                ErrorType::NotEnoughMemoryToAllocate,
                &[&broadcast.byte_length],
            );
        };
        let number = match broadcast.number {
            BroadcastNumber::Undefined => Value::UNDEFINED,
            BroadcastNumber::Number(number) => Value::from_f64(number),
            BroadcastNumber::BigInt(bytes) => {
                Value::from_bigint(BigInt::create(vm, SignedBigInteger::from_signed_bytes_le(&bytes)))
            }
        };
        call(
            vm,
            callback,
            Value::UNDEFINED,
            &[Value::from_object(shared_array_buffer), number],
        )?;
        Ok(Value::UNDEFINED)
    }

    fn report(vm: &Vm) -> ThrowCompletionOr<Value> {
        let message = vm.argument(0).to_utf16_string(vm)?;
        agents::report(&Utf16View::of_string(&message).to_wtf8());
        Ok(Value::UNDEFINED)
    }

    #[allow(clippy::unnecessary_wraps, reason = "native functions return a completion")]
    fn leaving(_vm: &Vm) -> ThrowCompletionOr<Value> {
        Ok(Value::UNDEFINED)
    }
}

/// Core::System::sleep_ms().
#[cfg(unix)]
fn sleep_milliseconds(milliseconds: u32) {
    // SAFETY: usleep has no preconditions; a failure is ignored.
    unsafe { libc::usleep(milliseconds.wrapping_mul(1000)) };
}

/// Core::System::sleep_ms() on Windows: Sleep(), which takes the milliseconds as they are.
#[cfg(windows)]
fn sleep_milliseconds(milliseconds: u32) {
    std::thread::sleep(core::time::Duration::from_millis(u64::from(milliseconds)));
}

/// MonotonicTime::now().milliseconds(): CLOCK_MONOTONIC, with the milliseconds rounded up as AK's
/// Duration::to_milliseconds() rounds them.
#[cfg(unix)]
fn monotonic_time_now_in_milliseconds() -> i64 {
    let mut time = libc::timespec { tv_sec: 0, tv_nsec: 0 };
    // SAFETY: The timespec is valid for writing.
    unsafe { libc::clock_gettime(libc::CLOCK_MONOTONIC, &raw mut time) };
    let seconds: i64 = time.tv_sec;
    let nanoseconds: i64 = time.tv_nsec;
    let mut milliseconds = seconds * 1000 + nanoseconds / 1_000_000;
    if nanoseconds % 1_000_000 != 0 {
        milliseconds += 1;
    }
    milliseconds
}

/// MonotonicTime::now().milliseconds() on Windows, rounded up the same way, but counted from the first call rather than
/// from boot, which makes no difference to scripts, as they can only compare the times they read.
#[cfg(windows)]
fn monotonic_time_now_in_milliseconds() -> i64 {
    static FIRST_CALL: std::sync::OnceLock<std::time::Instant> = std::sync::OnceLock::new();
    let elapsed = FIRST_CALL.get_or_init(std::time::Instant::now).elapsed();
    i64::try_from(elapsed.as_nanos().div_ceil(1_000_000)).unwrap_or(i64::MAX)
}
