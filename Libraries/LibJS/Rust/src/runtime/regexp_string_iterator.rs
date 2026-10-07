/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;

use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::runtime::object::{MayInterfereWithIndexedPropertyAccess, ORDINARY_OBJECT_METHODS, define_object_class};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::realm::Realm;

#[repr(C)]
#[derive(Trace)]
pub struct RegExpStringIterator {
    base: Object,
    regexp_object: Gc<Object>,
    string: Gc<PrimitiveString>,
    #[gc(untraced)]
    global: bool,
    #[gc(untraced)]
    unicode: bool,
    #[gc(untraced)]
    done: Cell<bool>,
}

define_object_class!(RegExpStringIterator, extends: [Object], methods: {
    ..ORDINARY_OBJECT_METHODS
});

impl RegExpStringIterator {
    // 22.2.9.1 CreateRegExpStringIterator ( R, S, global, fullUnicode ), https://tc39.es/ecma262/#sec-createregexpstringiterator
    pub fn create(
        vm: &Vm,
        realm: Gc<Realm>,
        regexp_object: Gc<Object>,
        string: Gc<PrimitiveString>,
        global: bool,
        unicode: bool,
    ) -> Gc<RegExpStringIterator> {
        // 1. Let iterator be OrdinaryObjectCreate(%RegExpStringIteratorPrototype%, « [[IteratingRegExp]], [[IteratedString]], [[Global]], [[Unicode]], [[Done]] »).
        // 2. Set iterator.[[IteratingRegExp]] to R.
        // 3. Set iterator.[[IteratedString]] to S.
        // 4. Set iterator.[[Global]] to global.
        // 5. Set iterator.[[Unicode]] to fullUnicode.
        // 6. Set iterator.[[Done]] to false.
        // 7. Return iterator.
        realm.create_object(
            vm,
            RegExpStringIterator {
                base: Object::new_with_prototype(
                    vm,
                    Self::CLASS,
                    realm.intrinsics().regexp_string_iterator_prototype(),
                    MayInterfereWithIndexedPropertyAccess::No,
                ),
                regexp_object,
                string,
                global,
                unicode,
                done: Cell::new(false),
            },
        )
    }

    pub fn regexp_object(&self) -> Gc<Object> {
        self.regexp_object
    }

    pub fn string(&self) -> Gc<PrimitiveString> {
        self.string
    }

    pub fn global(&self) -> bool {
        self.global
    }

    pub fn unicode(&self) -> bool {
        self.unicode
    }

    pub fn done(&self) -> bool {
        self.done.get()
    }

    pub fn set_done(&self) {
        self.done.set(true);
    }
}
