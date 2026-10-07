/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;
use core::ops::Deref;

use ak::Utf16String;
use libjs_runtime_macros::Trace;

use crate::gc::class::{Finalize, GcCell, define_cell};
use crate::gc::gc_ref_cell::GcRefCell;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::runtime::abstract_operations::OptionType;
use crate::runtime::intl::collator_compare_function::CollatorCompareFunction;
use crate::runtime::intl::intl_object::{IntlObject, ResolutionOptionDescriptor};
use crate::runtime::object::MayInterfereWithIndexedPropertyAccess;
use crate::unicode::intl::{self as unicode, CaseFirst, Sensitivity, Usage};
use crate::utf16::Utf16View;

/// 10 Collator Objects, https://tc39.es/ecma402/#collator-objects
#[repr(C)]
#[derive(Trace)]
pub struct Collator {
    base: Object,
    #[gc(untraced)]
    locale: GcRefCell<Utf16String>, // [[Locale]]
    #[gc(untraced)]
    usage: Cell<Usage>, // [[Usage]]
    #[gc(untraced)]
    sensitivity: Cell<Sensitivity>, // [[Sensitivity]]
    #[gc(untraced)]
    case_first: Cell<CaseFirst>, // [[CaseFirst]]
    #[gc(untraced)]
    collation: GcRefCell<Utf16String>, // [[Collation]]
    #[gc(untraced)]
    ignore_punctuation: Cell<bool>, // [[IgnorePunctuation]]
    #[gc(untraced)]
    numeric: Cell<bool>, // [[Numeric]]
    bound_compare: Cell<Option<Gc<CollatorCompareFunction>>>, // [[BoundCompare]]

    // Non-standard. Stores the ICU collator for the Intl object's collation options.
    #[gc(untraced)]
    collator: GcRefCell<Option<unicode::Collator>>,
}

define_cell!(Collator, Object, extends: [Object], finalize: finalize);

impl Deref for Collator {
    type Target = Object;

    fn deref(&self) -> &Object {
        &self.base
    }
}

impl Finalize for Collator {
    fn finalize(&self) {
        drop(self.collator.replace(None));
    }
}

impl Collator {
    pub fn new(vm: &Vm, prototype: Gc<Object>) -> Collator {
        Collator {
            base: Object::new_with_prototype(vm, Self::CLASS, prototype, MayInterfereWithIndexedPropertyAccess::No),
            locale: GcRefCell::default(),
            usage: Cell::new(Usage::Sort),
            sensitivity: Cell::new(Sensitivity::Variant),
            case_first: Cell::new(CaseFirst::False),
            collation: GcRefCell::default(),
            ignore_punctuation: Cell::new(false),
            numeric: Cell::new(false),
            bound_compare: Cell::new(None),
            collator: GcRefCell::new(None),
        }
    }

    pub fn locale(&self) -> Utf16String {
        self.locale.borrow().clone()
    }

    pub fn set_locale(&self, locale: Utf16String) {
        self.locale.replace(locale);
    }

    pub fn usage(&self) -> Usage {
        self.usage.get()
    }

    pub fn set_usage(&self, usage: Utf16View<'_>) {
        self.usage.set(unicode::usage_from_string(usage));
    }

    pub fn usage_string(&self) -> &'static str {
        unicode::usage_to_string(self.usage.get())
    }

    pub fn sensitivity(&self) -> Sensitivity {
        self.sensitivity.get()
    }

    pub fn set_sensitivity(&self, sensitivity: Sensitivity) {
        self.sensitivity.set(sensitivity);
    }

    pub fn sensitivity_string(&self) -> &'static str {
        unicode::sensitivity_to_string(self.sensitivity.get())
    }

    pub fn case_first(&self) -> CaseFirst {
        self.case_first.get()
    }

    pub fn set_case_first(&self, case_first: Utf16View<'_>) {
        self.case_first.set(unicode::case_first_from_string(case_first));
    }

    pub fn case_first_string(&self) -> &'static str {
        unicode::case_first_to_string(self.case_first.get())
    }

    pub fn collation(&self) -> Utf16String {
        self.collation.borrow().clone()
    }

    pub fn set_collation(&self, collation: Utf16String) {
        self.collation.replace(collation);
    }

    pub fn ignore_punctuation(&self) -> bool {
        self.ignore_punctuation.get()
    }

    pub fn set_ignore_punctuation(&self, ignore_punctuation: bool) {
        self.ignore_punctuation.set(ignore_punctuation);
    }

    pub fn numeric(&self) -> bool {
        self.numeric.get()
    }

    pub fn set_numeric(&self, numeric: bool) {
        self.numeric.set(numeric);
    }

    pub fn bound_compare(&self) -> Option<Gc<CollatorCompareFunction>> {
        self.bound_compare.get()
    }

    pub fn set_bound_compare(&self, bound_compare: Option<Gc<CollatorCompareFunction>>) {
        self.bound_compare.set(bound_compare);
    }

    /// Calls `callback` with the ICU collator, which must not allocate or call into the VM.
    pub fn with_collator<R>(&self, callback: impl FnOnce(&unicode::Collator) -> R) -> R {
        let collator = self.collator.borrow();
        callback(
            collator
                .as_ref()
                .expect("the Intl.Collator constructor creates the ICU collator"),
        )
    }

    pub fn set_collator(&self, collator: unicode::Collator) {
        self.collator.replace(Some(collator));
    }
}

impl IntlObject for Collator {
    // 10.2.3 Internal slots, https://tc39.es/ecma402/#sec-intl-collator-internal-slots
    fn relevant_extension_keys(&self) -> &'static [&'static str] {
        // The value of the [[RelevantExtensionKeys]] internal slot is a List that must include the element "co", may include any or all of the elements "kf" and "kn", and must not include any other elements.
        &["co", "kf", "kn"]
    }

    // 10.2.3 Internal slots, https://tc39.es/ecma402/#sec-intl-collator-internal-slots
    fn resolution_option_descriptors<'vm>(&self, vm: &'vm Vm) -> Vec<ResolutionOptionDescriptor<'vm>> {
        // The value of the [[ResolutionOptionDescriptors]] internal slot is « { [[Key]]: "co", [[Property]]: "collation" }, { [[Key]]: "kn", [[Property]]: "numeric", [[Type]]: boolean }, { [[Key]]: "kf", [[Property]]: "caseFirst", [[Values]]: « "upper", "lower", "false" » } ».
        vec![
            ResolutionOptionDescriptor::string("co", &vm.names.collation),
            ResolutionOptionDescriptor {
                key: "kn",
                property: &vm.names.numeric,
                type_: OptionType::Boolean,
                values: &[],
            },
            ResolutionOptionDescriptor {
                key: "kf",
                property: &vm.names.caseFirst,
                type_: OptionType::String,
                values: &["upper", "lower", "false"],
            },
        ]
    }
}
