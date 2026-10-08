/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::cell::RefCell;
use std::collections::{HashMap, HashSet};
use std::hash::{Hash, Hasher};
use std::rc::Rc;

use ak::{Utf16FlyString, Utf16String};
use libjs_abi::Builtin;
use libjs_runtime_macros::Trace;

use crate::bytecode::executable::StaticPropertyLookupCacheSite as CacheSite;
use crate::bytecode::property_access::get_own_property_without_side_effects;
use crate::gc::class::{GcCell, define_cell};
use crate::gc::root::MarkedVec;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::property_lookup_cache::PropertyLookupCache;
use crate::layout::value::Value;
use crate::layout_forward::RawNativeFunctionPointer;
use crate::runtime::abstract_operations::{
    call_function_object, checked_js_string_length_sum, construct, get_substitution, length_of_array_like,
    species_constructor,
};
use crate::runtime::array::Array;
use crate::runtime::completion::{Must, ThrowCompletionOr};
use crate::runtime::ecmascript_regex::{EcmaScriptRegex, MatchResult};
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::native_function::raw_native;
use crate::runtime::object::{MayInterfereWithIndexedPropertyAccess, ORDINARY_OBJECT_METHODS, define_object_class};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_attributes::{Attribute, DEFAULT_ATTRIBUTES, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::prototype_object::{this_object, typed_this_object};
use crate::runtime::realm::Realm;
use crate::runtime::regexp_constructor::is_raw_native_function_running;
use crate::runtime::regexp_legacy_static_properties::{
    invalidate_legacy_regexp_static_properties, update_legacy_regexp_static_properties_lazy,
};
use crate::runtime::regexp_object::{
    REGEXP_FLAGS_WITH_CHARACTERS, RegExpFlags, RegExpObject, compile_flags_for, parse_regex_pattern,
};
use crate::runtime::regexp_string_iterator::RegExpStringIterator;
use crate::runtime::string_prototype::code_point_at;
use crate::runtime::value::same_value;
use crate::utf16::{Utf16StringBuilder, Utf16View, concatenate};

/// The most legacy static properties ($1-$9) a match updates.
const MAX_LEGACY_CAPTURES: u32 = 9;

fn contains_code_unit(string: &Utf16String, code_unit: u8) -> bool {
    Utf16View::of_string(string)
        .code_units()
        .any(|unit| unit == u16::from(code_unit))
}

fn cache(vm: &Vm, site: CacheSite) -> &PropertyLookupCache {
    vm.static_property_lookup_cache(site)
}

/// The getter of RegExp.prototype.flags, which reading_flags_is_unobservable() looks for.
static FLAGS_GETTER: RawNativeFunctionPointer = raw_native!(RegExpPrototype::flags);

macro_rules! define_regexp_flag_getters {
    ($($flag:ident, $flag_name:ident, $property_name:ident, $getter_static:ident, $flags_cache:ident, $unobservable_cache:ident;)*) => {
        $(
            /// The getter of this flag, which reading_flags_is_unobservable() looks for.
            static $getter_static: RawNativeFunctionPointer = raw_native!(RegExpPrototype::$flag_name);
        )*

        impl RegExpPrototype {
            fn define_flag_accessors(object: &Object, vm: &Vm, realm: Gc<Realm>) {
                $(
                    object.define_native_accessor(
                        vm,
                        realm,
                        &vm.names.$property_name,
                        $getter_static,
                        None,
                        PropertyAttributes::new(Attribute::CONFIGURABLE),
                    );
                )*
            }

            $(
                fn $flag_name(vm: &Vm) -> ThrowCompletionOr<Value> {
                    let realm = vm.current_realm().expect("a builtin runs in a realm");
                    // 1. If Type(R) is not Object, throw a TypeError exception.
                    let regexp_object = this_object(vm)?;
                    // 2. If R does not have an [[OriginalFlags]] internal slot, then
                    let Some(typed_regexp_object) = regexp_object.downcast::<RegExpObject>() else {
                        // a. If SameValue(R, %RegExp.prototype%) is true, return undefined.
                        if same_value(
                            Value::from_object(regexp_object),
                            Value::from_object(realm.intrinsics().regexp_prototype(vm)),
                        ) {
                            return Ok(Value::UNDEFINED);
                        }
                        // b. Otherwise, throw a TypeError exception.
                        return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAnObjectOfType, &[&"RegExp"]);
                    };
                    // 3. Let flags be R.[[OriginalFlags]].
                    let flags = typed_regexp_object.flag_bits();
                    // 4. If flags contains codeUnit, return true.
                    // 5. Return false.
                    Ok(Value::from_bool(flags.has(RegExpFlags::$flag)))
                }
            )*

            /// Appends the character of each flag whose property is truthy on `regexp_object`, as steps 4 to 19 of
            /// get RegExp.prototype.flags do.
            fn append_truthy_flags(
                vm: &Vm,
                regexp_object: Gc<Object>,
                builder: &mut Utf16StringBuilder,
            ) -> ThrowCompletionOr<()> {
                $(
                    {
                        let flag_value = regexp_object.get_with_cache(
                            vm,
                            &vm.names.$property_name,
                            cache(vm, CacheSite::$flags_cache),
                        )?;
                        if flag_value.to_boolean() {
                            builder.append_code_unit(u16::from(flag_character(RegExpFlags::$flag)));
                        }
                    }
                )*
                Ok(())
            }

            fn every_flag_has_its_intrinsic_getter(vm: &Vm, regexp_prototype: &Object) -> bool {
                $(
                    if !has_intrinsic_getter(
                        vm,
                        regexp_prototype,
                        &vm.names.$property_name,
                        cache(vm, CacheSite::$unobservable_cache),
                        $getter_static,
                    ) {
                        return false;
                    }
                )*
                true
            }
        }
    };
}

fn flag_character(flag: RegExpFlags) -> u8 {
    REGEXP_FLAGS_WITH_CHARACTERS
        .iter()
        .find(|(candidate, _)| *candidate == flag)
        .map(|(_, flag_character)| *flag_character)
        .expect("every flag has a character")
}

// 22.2.6.3 get RegExp.prototype.dotAll, https://tc39.es/ecma262/#sec-get-regexp.prototype.dotAll
// 22.2.6.5 get RegExp.prototype.global, https://tc39.es/ecma262/#sec-get-regexp.prototype.global
// 22.2.6.6 get RegExp.prototype.hasIndices, https://tc39.es/ecma262/#sec-get-regexp.prototype.hasIndices
// 22.2.6.7 get RegExp.prototype.ignoreCase, https://tc39.es/ecma262/#sec-get-regexp.prototype.ignorecase
// 22.2.6.10 get RegExp.prototype.multiline, https://tc39.es/ecma262/#sec-get-regexp.prototype.multiline
// 22.2.6.15 get RegExp.prototype.sticky, https://tc39.es/ecma262/#sec-get-regexp.prototype.sticky
// 22.2.6.18 get RegExp.prototype.unicode, https://tc39.es/ecma262/#sec-get-regexp.prototype.unicode
// 22.2.6.19 get RegExp.prototype.unicodeSets, https://tc39.es/ecma262/#sec-get-regexp.prototype.unicodesets
define_regexp_flag_getters! {
    HAS_INDICES, has_indices, hasIndices, HAS_INDICES_GETTER, RegExpPrototypeFlagsHasIndices, RegExpFlagsUnobservableHasIndices;
    GLOBAL, global, global, GLOBAL_GETTER, RegExpPrototypeFlagsGlobal, RegExpFlagsUnobservableGlobal;
    IGNORE_CASE, ignore_case, ignoreCase, IGNORE_CASE_GETTER, RegExpPrototypeFlagsIgnoreCase, RegExpFlagsUnobservableIgnoreCase;
    MULTILINE, multiline, multiline, MULTILINE_GETTER, RegExpPrototypeFlagsMultiline, RegExpFlagsUnobservableMultiline;
    DOT_ALL, dot_all, dotAll, DOT_ALL_GETTER, RegExpPrototypeFlagsDotAll, RegExpFlagsUnobservableDotAll;
    UNICODE, unicode, unicode, UNICODE_GETTER, RegExpPrototypeFlagsUnicode, RegExpFlagsUnobservableUnicode;
    UNICODE_SETS, unicode_sets, unicodeSets, UNICODE_SETS_GETTER, RegExpPrototypeFlagsUnicodeSets, RegExpFlagsUnobservableUnicodeSets;
    STICKY, sticky, sticky, STICKY_GETTER, RegExpPrototypeFlagsSticky, RegExpFlagsUnobservableSticky;
}

/// %RegExp.prototype%.
#[repr(C)]
#[derive(Trace)]
pub struct RegExpPrototype {
    base: Object,
}

define_object_class!(RegExpPrototype, extends: [Object], methods: {
    initialize: RegExpPrototype::initialize,
    ..ORDINARY_OBJECT_METHODS
});

// Non-standard abstraction around steps used by multiple prototypes.
fn increment_last_index(
    vm: &Vm,
    regexp_object: Gc<Object>,
    string: Utf16View<'_>,
    unicode: bool,
) -> ThrowCompletionOr<()> {
    // Let thisIndex be ℝ(? ToLength(? Get(rx, "lastIndex"))).
    let last_index_value = regexp_object.get_with_cache(
        vm,
        &vm.names.lastIndex,
        cache(vm, CacheSite::RegExpIncrementLastIndexGetLastIndex),
    )?;
    let mut last_index = last_index_value.to_length(vm)?;

    // Let nextIndex be AdvanceStringIndex(S, thisIndex, fullUnicode).
    last_index = advance_string_index(string, last_index, unicode);

    // Perform ? Set(rx, "lastIndex", 𝔽(nextIndex), true).
    regexp_object.set_with_cache(
        vm,
        &vm.names.lastIndex,
        Value::from_f64(last_index as f64),
        cache(vm, CacheSite::RegExpIncrementLastIndexSetLastIndex),
    )
}

/// get_or_compile_regex(): the compiled regex of a RegExp object, which is compiled the first time it is asked for.
/// Returns None for a pattern the regex engine cannot compile.
// FIXME: Add an eviction policy to bound the size of this cache.
struct RegexCacheKey {
    pattern: Utf16String,
    flags: RegExpFlags,
}

impl PartialEq for RegexCacheKey {
    fn eq(&self, other: &Self) -> bool {
        self.flags == other.flags && self.pattern == other.pattern
    }
}

impl Eq for RegexCacheKey {}

impl Hash for RegexCacheKey {
    fn hash<H: Hasher>(&self, state: &mut H) {
        for code_unit in Utf16View::of_string(&self.pattern).code_units() {
            state.write_u16(code_unit);
        }
        state.write_u8(self.flags.bits());
    }
}

thread_local! {
    /// The compiled regexes of every pattern and flags. Compiled regexes hold no cells, and each thread runs its own
    /// VM, so each thread keeps its own.
    static REGEX_CACHE: RefCell<HashMap<RegexCacheKey, Rc<EcmaScriptRegex>, foldhash::fast::RandomState>> =
        RefCell::new(HashMap::default());
}

fn get_or_compile_regex(regexp_object: &RegExpObject) -> Option<Rc<EcmaScriptRegex>> {
    // Fast path: check the inline cache on the RegExpObject.
    if let Some(cached) = regexp_object.cached_regex() {
        return Some(cached);
    }

    let pattern = regexp_object.pattern();
    let flag_bits = regexp_object.flag_bits();

    let cache_key = RegexCacheKey {
        pattern,
        flags: flag_bits,
    };

    if let Some(cached) = REGEX_CACHE.with_borrow(|cache| cache.get(&cache_key).cloned()) {
        regexp_object.set_cached_regex(Some(Rc::clone(&cached)));
        return Some(cached);
    }
    let pattern = &cache_key.pattern;

    let unicode = flag_bits.has(RegExpFlags::UNICODE);
    let unicode_sets = flag_bits.has(RegExpFlags::UNICODE_SETS);

    // Normalize non-ASCII code units to ASCII escapes before compiling the pattern.
    let mut pattern_code_units = Vec::new();
    Utf16View::of_string(pattern).append_to(&mut pattern_code_units);
    let normalized_pattern = parse_regex_pattern(&pattern_code_units, unicode, unicode_sets).ok()?;

    let compiled =
        EcmaScriptRegex::compile(Utf16View::Utf16(&normalized_pattern), compile_flags_for(flag_bits)).ok()?;

    let compiled = Rc::new(compiled);
    REGEX_CACHE.with_borrow_mut(|cache| cache.insert(cache_key, Rc::clone(&compiled)));
    regexp_object.set_cached_regex(Some(Rc::clone(&compiled)));
    Some(compiled)
}

struct ExecWithLastIndexResult {
    result: MatchResult,
    effective_last_index: usize,
}

fn exec_with_unicode_last_index_retry(
    compiled_regex: &EcmaScriptRegex,
    utf16_view: Utf16View<'_>,
    last_index: usize,
    unicode_mode: bool,
    sticky: bool,
) -> ExecWithLastIndexResult {
    let exec_at = |index: usize| ExecWithLastIndexResult {
        result: compiled_regex.exec(utf16_view, index),
        effective_last_index: index,
    };

    if !unicode_mode || last_index == 0 || last_index >= utf16_view.length_in_code_units() {
        return exec_at(last_index);
    }

    let current = utf16_view.code_unit_at(last_index);
    let previous = utf16_view.code_unit_at(last_index - 1);
    if !((0xDC00..=0xDFFF).contains(&current) && (0xD800..=0xDBFF).contains(&previous)) {
        return exec_at(last_index);
    }

    if !sticky && compiled_regex.is_single_non_bmp_literal() {
        return exec_at(last_index);
    }

    // NB: V8/SpiderMonkey first try the code point that starts at the
    // surrogate pair boundary, but zero-width patterns can still match at the
    // original low-surrogate index when that earlier retry fails. Consuming
    // retries must still be rejected so /u and /v regexes never split the
    // surrogate pair.
    let snapped_result = exec_at(last_index - 1);
    if snapped_result.result != MatchResult::NoMatch {
        return snapped_result;
    }

    let retried_result = exec_at(last_index);
    if retried_result.result != MatchResult::Match {
        return retried_result;
    }

    let match_start = compiled_regex.capture_slot(0);
    let match_end = compiled_regex.capture_slot(1);
    if match_start >= 0 && match_end >= 0 && match_start as usize == last_index && match_end as usize == last_index {
        return retried_result;
    }

    ExecWithLastIndexResult {
        result: MatchResult::NoMatch,
        effective_last_index: last_index,
    }
}

/// The start and end of each of the first nine captures of the last exec of `compiled_regex`, which the legacy
/// static properties are updated with.
/// The capture slots that the legacy static properties record for $1 through $9, kept inline.
struct LegacyCaptureSlots {
    starts: [i32; MAX_LEGACY_CAPTURES as usize],
    ends: [i32; MAX_LEGACY_CAPTURES as usize],
    count: usize,
}

impl LegacyCaptureSlots {
    fn starts(&self) -> &[i32] {
        &self.starts[..self.count]
    }

    fn ends(&self) -> &[i32] {
        &self.ends[..self.count]
    }
}

fn legacy_capture_slots(compiled_regex: &EcmaScriptRegex, n_capture_groups: u32) -> LegacyCaptureSlots {
    let total_groups = compiled_regex.total_groups();
    let capture_count = MAX_LEGACY_CAPTURES.min(n_capture_groups);
    let mut slots = LegacyCaptureSlots {
        starts: [-1; MAX_LEGACY_CAPTURES as usize],
        ends: [-1; MAX_LEGACY_CAPTURES as usize],
        count: capture_count as usize,
    };
    for group in 0..capture_count {
        let group_index = group + 1;
        if group_index < total_groups {
            slots.starts[group as usize] = compiled_regex.capture_slot(group_index * 2);
            slots.ends[group as usize] = compiled_regex.capture_slot(group_index * 2 + 1);
        }
    }
    slots
}

fn throw_backtrack_limit_exceeded<T>(vm: &Vm) -> ThrowCompletionOr<T> {
    vm.throw_completion(ErrorKind::InternalError, ErrorType::RegExpBacktrackLimitExceeded, &[])
}

fn index_value(index: usize) -> Value {
    Value::from_f64(index as f64)
}

fn substring(vm: &Vm, string: Gc<PrimitiveString>, start: usize, length: usize) -> Value {
    Value::from_string(PrimitiveString::create_from_substring(vm, string, start, length))
}

fn indices_pair(vm: &Vm, realm: Gc<Realm>, start: usize, end: usize) -> Value {
    let pair = Array::create(vm, realm, 2, None).must();
    pair.indexed_put(0, index_value(start), DEFAULT_ATTRIBUTES);
    pair.indexed_put(1, index_value(end), DEFAULT_ATTRIBUTES);
    Value::from_object(pair)
}

// 22.2.7.2 RegExpBuiltinExec ( R, S ), https://tc39.es/ecma262/#sec-regexpbuiltinexec
// 22.2.7.2 RegExpBuiltInExec ( R, S ), https://github.com/tc39/proposal-regexp-legacy-features#regexpbuiltinexec--r-s-
fn regexp_builtin_exec(
    vm: &Vm,
    regexp_object: Gc<RegExpObject>,
    string: Gc<PrimitiveString>,
) -> ThrowCompletionOr<Value> {
    let realm = vm.current_realm().expect("RegExpBuiltinExec runs in a realm");
    let regexp_as_object: Gc<Object> = regexp_object.upcast();

    let last_index_value = regexp_as_object.get_with_cache(
        vm,
        &vm.names.lastIndex,
        cache(vm, CacheSite::RegExpBuiltinExecGetLastIndex),
    )?;
    let mut last_index = last_index_value.to_length(vm)?;

    let flag_bits = regexp_object.flag_bits();

    let global = flag_bits.has(RegExpFlags::GLOBAL);
    let sticky = flag_bits.has(RegExpFlags::STICKY);
    let has_indices = flag_bits.has(RegExpFlags::HAS_INDICES);
    if !global && !sticky {
        last_index = 0;
    }

    if last_index > string.length_in_utf16_code_units() as u64 {
        if sticky || global {
            regexp_as_object.set_with_cache(
                vm,
                &vm.names.lastIndex,
                Value::from_i32(0),
                cache(vm, CacheSite::RegExpBuiltinExecResetLastIndexPastEnd),
            )?;
        }
        return Ok(Value::NULL);
    }
    let last_index = last_index as usize;

    let Some(compiled_regex) = get_or_compile_regex(&regexp_object) else {
        return Ok(Value::NULL);
    };

    let unicode_mode = flag_bits.has(RegExpFlags::UNICODE) || flag_bits.has(RegExpFlags::UNICODE_SETS);
    let utf16_view = string.utf16_string_view();
    let exec_result = exec_with_unicode_last_index_retry(&compiled_regex, utf16_view, last_index, unicode_mode, sticky);
    if exec_result.result == MatchResult::LimitExceeded {
        return throw_backtrack_limit_exceeded(vm);
    }
    let mut matched = exec_result.result == MatchResult::Match;

    // For sticky mode, the match must start at exactly lastIndex.
    if matched && sticky {
        let match_start = compiled_regex.capture_slot(0);
        if match_start < 0 || match_start as usize != exec_result.effective_last_index {
            matched = false;
        }
    }

    if !matched {
        if sticky || global {
            regexp_as_object.set_with_cache(
                vm,
                &vm.names.lastIndex,
                Value::from_i32(0),
                cache(vm, CacheSite::RegExpBuiltinExecResetLastIndexNoMatch),
            )?;
        }
        return Ok(Value::NULL);
    }

    // Group 0 is the full match -- read directly from internal capture buffer.
    let match_index = compiled_regex.capture_slot(0) as usize;
    let end_index = compiled_regex.capture_slot(1) as usize;

    // In Unicode mode, match_index and end_index are already in code unit indices from the VM.
    // Update lastIndex.
    if global || sticky {
        regexp_as_object.set_with_cache(
            vm,
            &vm.names.lastIndex,
            index_value(end_index),
            cache(vm, CacheSite::RegExpBuiltinExecSetLastIndex),
        )?;
    }

    let n_capture_groups = compiled_regex.capture_count();
    let named_groups = compiled_regex.named_groups();

    let array = Array::create(vm, realm, 0, None).must();
    array.unsafe_set_shape(realm.intrinsics().regexp_builtin_exec_array_shape());
    // OPTIMIZATION: The match and the captures are consecutive default data properties, so they are written into
    //               packed indexed storage of the final length directly.
    array.set_indexed_property_elements_to_undefined(n_capture_groups + 1);

    // "index" property.
    array.put_direct(
        realm.intrinsics().regexp_builtin_exec_array_index_offset(),
        index_value(match_index),
    );

    // "input" property.
    array.put_direct(
        realm.intrinsics().regexp_builtin_exec_array_input_offset(),
        Value::from_string(string),
    );

    // NB: The groups slot holds undefined until it is set below, so that the array never exposes an unset slot.
    array.put_direct(
        realm.intrinsics().regexp_builtin_exec_array_groups_offset(),
        Value::UNDEFINED,
    );

    // Element 0: the full match substring.
    array.set_packed_indexed_element(0, substring(vm, string, match_index, end_index - match_index));

    let has_groups = !named_groups.is_empty();
    let mut groups = if has_groups {
        Value::from_object(Object::create(vm, realm, None))
    } else {
        Value::UNDEFINED
    };

    // "groups" property.
    array.put_direct(realm.intrinsics().regexp_builtin_exec_array_groups_offset(), groups);

    // Track which group names have been matched (non-undefined) to handle duplicate names.
    let mut matched_group_names: HashSet<Utf16FlyString> = HashSet::new();

    let total_groups = compiled_regex.total_groups();

    for i in 1..=n_capture_groups {
        let capture_start = if i < total_groups {
            compiled_regex.capture_slot(i * 2)
        } else {
            -1
        };
        let capture_end = if i < total_groups {
            compiled_regex.capture_slot(i * 2 + 1)
        } else {
            -1
        };

        let captured_value = if capture_start >= 0 && capture_end >= 0 {
            substring(
                vm,
                string,
                capture_start as usize,
                (capture_end - capture_start) as usize,
            )
        } else {
            Value::UNDEFINED
        };

        array.set_packed_indexed_element(i, captured_value);

        // Named groups: find by linear scan (typically very few named groups).
        for named_group in named_groups {
            if named_group.index == i {
                let group_name = &named_group.name;
                if matched_group_names.contains(group_name) {
                    // Name already matched with a non-undefined value; skip.
                    break;
                }
                if !captured_value.is_undefined() {
                    matched_group_names.insert(group_name.clone());
                }
                groups
                    .as_object()
                    .create_data_property_or_throw(vm, &PropertyKey::from(group_name.clone()), captured_value)
                    .must();
                break;
            }
        }
    }

    // Ensure named groups are enumerated in source order.
    if has_groups {
        let original_groups = groups;
        groups = Value::from_object(Object::create(vm, realm, None));

        for named_group in named_groups {
            let group_name = PropertyKey::from(named_group.name.clone());
            let value = original_groups.as_object().get_without_side_effects(vm, &group_name);
            groups
                .as_object()
                .create_data_property_or_throw(vm, &group_name, value)
                .must();
        }

        array
            .set_with_cache(
                vm,
                &vm.names.groups,
                groups,
                cache(vm, CacheSite::RegExpBuiltinExecSetGroups),
            )
            .must();
    }

    // Legacy RegExp static properties (lazy -- defer $1-$9 string creation).
    let needs_legacy = regexp_object.legacy_features_enabled() && realm == regexp_object.realm();
    if needs_legacy {
        let legacy_captures = legacy_capture_slots(&compiled_regex, n_capture_groups);
        update_legacy_regexp_static_properties_lazy(
            vm,
            realm.intrinsics().regexp_constructor(vm),
            string,
            match_index,
            end_index,
            legacy_captures.starts(),
            legacy_captures.ends(),
        );
    } else if realm == regexp_object.realm() {
        invalidate_legacy_regexp_static_properties(realm.intrinsics().regexp_constructor(vm));
    }

    // hasIndices ("d" flag).
    if has_indices {
        let indices_array = Array::create(vm, realm, 0, None).must();
        // Index 0: full match
        indices_array.indexed_put(0, indices_pair(vm, realm, match_index, end_index), DEFAULT_ATTRIBUTES);
        for i in 1..=n_capture_groups {
            let index_start = if i < total_groups {
                compiled_regex.capture_slot(i * 2)
            } else {
                -1
            };
            let index_end = if i < total_groups {
                compiled_regex.capture_slot(i * 2 + 1)
            } else {
                -1
            };
            if index_start >= 0 && index_end >= 0 {
                indices_array.indexed_put(
                    i,
                    indices_pair(vm, realm, index_start as usize, index_end as usize),
                    DEFAULT_ATTRIBUTES,
                );
            } else {
                indices_array.indexed_put(i, Value::UNDEFINED, DEFAULT_ATTRIBUTES);
            }
        }

        let indices_groups = if has_groups {
            Value::from_object(Object::create(vm, realm, None))
        } else {
            Value::UNDEFINED
        };
        if has_groups {
            let mut matched_index_group_names: HashSet<Utf16FlyString> = HashSet::new();
            for named_group in named_groups {
                let group_name = &named_group.name;
                if matched_index_group_names.contains(group_name) {
                    continue;
                }
                let group_index = named_group.index;
                let group_start = if group_index < total_groups {
                    compiled_regex.capture_slot(group_index * 2)
                } else {
                    -1
                };
                let group_end = if group_index < total_groups {
                    compiled_regex.capture_slot(group_index * 2 + 1)
                } else {
                    -1
                };
                if group_start >= 0 && group_end >= 0 {
                    matched_index_group_names.insert(group_name.clone());
                    let pair = indices_pair(vm, realm, group_start as usize, group_end as usize);
                    indices_groups
                        .as_object()
                        .create_data_property_or_throw(vm, &PropertyKey::from(group_name.clone()), pair)
                        .must();
                } else {
                    indices_groups
                        .as_object()
                        .create_data_property_or_throw(vm, &PropertyKey::from(group_name.clone()), Value::UNDEFINED)
                        .must();
                }
            }
        }

        indices_array
            .create_data_property_or_throw(vm, &vm.names.groups, indices_groups)
            .must();
        array
            .create_data_property_or_throw(vm, &vm.names.indices, Value::from_object(indices_array))
            .must();
    }

    Ok(Value::from_object(array))
}

// 22.2.7.1 RegExpExec ( R, S ), https://tc39.es/ecma262/#sec-regexpexec
pub fn regexp_exec(vm: &Vm, regexp_object: &Object, string: Gc<PrimitiveString>) -> ThrowCompletionOr<Value> {
    // 1. Let exec be ? Get(R, "exec").
    let exec = regexp_object.get_with_cache(vm, &vm.names.exec, cache(vm, CacheSite::RegExpExecExec))?;

    let typed_regexp_object = regexp_object.as_gc().downcast::<RegExpObject>();

    // 2. If IsCallable(exec) is true, then
    if exec.is_function() {
        let exec_function = exec.as_function();
        if let Some(typed_regexp_object) = typed_regexp_object
            && exec_function.realm() == vm.current_realm()
            && exec_function.builtin() == Some(Builtin::RegExpPrototypeExec)
        {
            return regexp_builtin_exec(vm, typed_regexp_object, string);
        }

        // a. Let result be ? Call(exec, R, « S »).
        let result = call_function_object(
            vm,
            exec_function,
            Value::from_object(regexp_object.as_gc()),
            &[Value::from_string(string)],
        )?;

        // b. If Type(result) is neither Object nor Null, throw a TypeError exception.
        if !result.is_object() && !result.is_null() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAnObjectOrNull, &[&result]);
        }

        // c. Return result.
        return Ok(result);
    }

    // 3. Perform ? RequireInternalSlot(R, [[RegExpMatcher]]).
    let Some(typed_regexp_object) = typed_regexp_object else {
        return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAnObjectOfType, &[&"RegExp"]);
    };

    // 4. Return ? RegExpBuiltinExec(R, S).
    regexp_builtin_exec(vm, typed_regexp_object, string)
}

// 22.2.7.3 AdvanceStringIndex ( S, index, unicode ), https://tc39.es/ecma262/#sec-advancestringindex
pub fn advance_string_index(string: Utf16View<'_>, index: u64, unicode: bool) -> u64 {
    // 1. Assert: index ≤ 2^53 - 1.

    // 2. If unicode is false, return index + 1.
    if !unicode {
        return index + 1;
    }

    // 3. Let length be the length of S.
    // 4. If index + 1 ≥ length, return index + 1.
    if index + 1 >= string.length_in_code_units() as u64 {
        return index + 1;
    }

    // 5. Let cp be CodePointAt(S, index).
    let code_point = code_point_at(string, index as usize);

    // 6. Return index + cp.[[CodeUnitCount]].
    index + code_point.code_unit_count as u64
}

fn has_intrinsic_getter(
    vm: &Vm,
    regexp_prototype: &Object,
    name: &PropertyKey,
    cache: &PropertyLookupCache,
    intrinsic_getter: RawNativeFunctionPointer,
) -> bool {
    let accessor = get_own_property_without_side_effects(regexp_prototype, name, cache);
    if !accessor.is_accessor() {
        return false;
    }

    let Some(getter) = accessor.as_accessor().getter() else {
        return false;
    };
    is_raw_native_function_running(vm, getter, intrinsic_getter)
}

impl RegExpPrototype {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<RegExpPrototype> {
        realm.create_object(
            vm,
            RegExpPrototype {
                base: Object::new_with_prototype(
                    vm,
                    Self::CLASS,
                    realm.object_prototype(),
                    MayInterfereWithIndexedPropertyAccess::No,
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        let names = &vm.names;
        let symbols = vm.well_known_symbols();

        let attributes = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        object.define_native_function(
            vm,
            realm,
            &names.toString,
            raw_native!(RegExpPrototype::to_string),
            0,
            attributes,
            None,
        );
        object.define_native_function(
            vm,
            realm,
            &names.test,
            raw_native!(RegExpPrototype::test),
            1,
            attributes,
            None,
        );
        object.define_native_function(
            vm,
            realm,
            &names.exec,
            raw_native!(RegExpPrototype::exec),
            1,
            attributes,
            Some(Builtin::RegExpPrototypeExec),
        );
        object.define_native_function(
            vm,
            realm,
            &names.compile,
            raw_native!(RegExpPrototype::compile),
            2,
            attributes,
            None,
        );

        object.define_native_function(
            vm,
            realm,
            &PropertyKey::from(symbols.match_),
            raw_native!(RegExpPrototype::symbol_match),
            1,
            attributes,
            None,
        );
        object.define_native_function(
            vm,
            realm,
            &PropertyKey::from(symbols.match_all),
            raw_native!(RegExpPrototype::symbol_match_all),
            1,
            attributes,
            None,
        );
        object.define_native_function(
            vm,
            realm,
            &PropertyKey::from(symbols.replace),
            raw_native!(RegExpPrototype::symbol_replace),
            2,
            attributes,
            Some(Builtin::RegExpPrototypeReplace),
        );
        object.define_native_function(
            vm,
            realm,
            &PropertyKey::from(symbols.search),
            raw_native!(RegExpPrototype::symbol_search),
            1,
            attributes,
            None,
        );
        object.define_native_function(
            vm,
            realm,
            &PropertyKey::from(symbols.split),
            raw_native!(RegExpPrototype::symbol_split),
            2,
            attributes,
            Some(Builtin::RegExpPrototypeSplit),
        );

        object.define_native_accessor(
            vm,
            realm,
            &names.flags,
            FLAGS_GETTER,
            None,
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
        object.define_native_accessor(
            vm,
            realm,
            &names.source,
            raw_native!(RegExpPrototype::source),
            None,
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );

        Self::define_flag_accessors(object, vm, realm);
    }

    // The below predicates guard the fast paths the way other engines do: V8 BranchIfFastRegExp (builtins-regexp-gen.cc),
    // JSC RegExpObject::isSymbolReplaceFastAndNonObservable and its per-operation siblings (RegExpObjectInlines.h), and
    // SpiderMonkey IsOptimizableRegExpObject (RegExp.cpp) all compare the object's structure and consult a watchpoint,
    // protector cell, or fuse — never a property read. We have no such invalidation means, so we read the slots directly.
    //
    // RegExp.prototype carries enough properties that finding one by name is a binary search, and these run on every call.
    // So, they reach its slots through a cache keyed on its shape — just like the bytecode interpreter's property reads do.
    // And that cache can't go stale. Caching the lookup is safe because what's cached is the slot number, never the value:
    // Assigning RegExp.prototype.exec leaves the shape alone, so the cached slot is still right and the read sees the new
    // function — while adding, deleting, or redefining a property changes the shape, so the cache misses and looks again.

    // Whether this is a plain RegExp carrying this realm's intrinsic prototype, with nothing of its own to shadow what the
    // algorithm reads, and the intrinsic exec still on that prototype.
    fn is_unmodified_regexp_instance(vm: &Vm, realm: Gc<Realm>, regexp_object: &Object) -> bool {
        if !regexp_object.is::<RegExpObject>() {
            return false;
        }

        let regexp_prototype = realm.intrinsics().regexp_prototype(vm);
        if regexp_object.prototype() != Some(regexp_prototype) {
            return false;
        }

        // A RegExp is created with lastIndex as its one own property, and lastIndex can't be deleted — so a lone own
        // property is sufficient to confirm that nothing of the object's own shadows exec, a flag, constructor, or @@match.
        // One count confirms it for all of them at once — whereas asking by name would cost a slot lookup for each.
        // NB: An own exec shadows the prototype's, and it may belong to another realm — which the algorithm Calls, so that
        // it runs there and updates that realm's legacy statics, rather than running it here.
        if regexp_object.shape().property_count() != 1 {
            return false;
        }

        let exec_function = get_own_property_without_side_effects(
            &regexp_prototype,
            &vm.names.exec,
            cache(vm, CacheSite::RegExpUnmodifiedInstanceExec),
        );
        if !exec_function.is_function() || exec_function.as_function().builtin() != Some(Builtin::RegExpPrototypeExec) {
            return false;
        }

        // The intrinsic exec of another realm is still the builtin, and assigning it here would have the algorithm Call it.
        // So, it would run in that realm — and our legacy statics would never see the match.
        exec_function.as_function().realm() == Some(realm)
    }

    // get RegExp.prototype.flags reads every flag off the receiver, so an own property — data or accessor — changes the
    // string the algorithm derives global and fullUnicode from, and a replaced getter runs code of its own. Each of those
    // reads resolves to that flag's own accessor on the prototype, and redefining one of those leaves both flags and the
    // shape alone — so every one of them is asked for, too.
    // NB: Those are exactly the properties JSC installs its regExpPrimordialProperties watchpoints on (JSGlobalObject.cpp).
    // V8 reaches the same properties thru the prototype's map (PrototypeCheckAssembler in builtins-regexp-gen.cc) and
    // SpiderMonkey thru a realm fuse — either of which a redefinition invalidates on its own.
    fn reading_flags_is_unobservable(vm: &Vm, realm: Gc<Realm>) -> bool {
        let regexp_prototype = realm.intrinsics().regexp_prototype(vm);

        if !has_intrinsic_getter(
            vm,
            &regexp_prototype,
            &vm.names.flags,
            cache(vm, CacheSite::RegExpFlagsUnobservableFlags),
            FLAGS_GETTER,
        ) {
            return false;
        }

        Self::every_flag_has_its_intrinsic_getter(vm, &regexp_prototype)
    }

    // ToLength(Get(R, "lastIndex")) runs whatever valueOf the property holds, so the fast path needs a plain number there.
    // V8's BranchIfFastRegExp states its smi check "is required to omit ToLength(lastIndex) calls with possible user-code
    // execution on the fast path". JSC's getLastIndex().isNumber() is the same check.
    fn reading_last_index_is_unobservable(vm: &Vm, regexp_object: &Object) -> bool {
        get_own_property_without_side_effects(
            regexp_object,
            &vm.names.lastIndex,
            cache(vm, CacheSite::RegExpLastIndexUnobservableLastIndex),
        )
        .is_number()
    }

    // RegExpBuiltinExec writes lastIndex back only if the pattern's global|sticky, and a non-writable lastIndex turns that
    // Set into a throw. So only an op reaching the write needs the property writable. This one asks the receiver by name,
    // where the read above goes thru a cache: It runs on the ops that walk a match, whose cost the lookup disappears into.
    fn writing_last_index_is_unobservable(vm: &Vm, regexp_object: &Object) -> bool {
        regexp_object
            .storage_get(vm, &vm.names.lastIndex)
            .is_some_and(|last_index| last_index.attributes.is_writable())
    }

    // 22.2.6.11 RegExp.prototype [ @@replace ] reads flags, writes lastIndex back when the pattern is global, and reads
    // it again to advance past an empty match.
    fn replace_is_fast_and_non_observable(vm: &Vm, realm: Gc<Realm>, regexp_object: &Object) -> bool {
        Self::is_unmodified_regexp_instance(vm, realm, regexp_object)
            && Self::reading_flags_is_unobservable(vm, realm)
            && Self::reading_last_index_is_unobservable(vm, regexp_object)
            && Self::writing_last_index_is_unobservable(vm, regexp_object)
    }

    // 22.2.6.14 RegExp.prototype [ @@split ] reads flags, resolves the species constructor, and constructs a splitter
    // from it — and that construction runs IsRegExp(rx), which reads rx[@@match].
    fn split_is_fast_and_non_observable(vm: &Vm, realm: Gc<Realm>, regexp_object: &Object) -> bool {
        if !Self::is_unmodified_regexp_instance(vm, realm, regexp_object) {
            return false;
        }
        if !Self::reading_flags_is_unobservable(vm, realm) {
            return false;
        }

        let regexp_prototype = realm.intrinsics().regexp_prototype(vm);
        let inherited_match = regexp_prototype.storage_get(vm, &PropertyKey::from(vm.well_known_symbols().match_));
        if inherited_match.is_none_or(|inherited_match| inherited_match.value.is_accessor()) {
            return false;
        }

        // SpeciesConstructor(rx, %RegExp%) reads rx.constructor and then that constructor's @@species, so both have to
        // still be the intrinsics for skipping the whole resolution to be unobservable.
        let regexp_constructor = realm.intrinsics().regexp_constructor(vm);
        let Some(inherited_constructor) = regexp_prototype.storage_get(vm, &vm.names.constructor) else {
            return false;
        };
        if !inherited_constructor.value.is_object() {
            return false;
        }
        if inherited_constructor.value.as_object() != regexp_constructor.upcast::<Object>() {
            return false;
        }

        regexp_constructor.has_intrinsic_symbol_species_getter(vm)
    }

    // 22.2.6.16 RegExp.prototype.test goes thru RegExpExec, which reads exec, and on into RegExpBuiltinExec, which coerces
    // lastIndex. Flags reach that algorithm from [[OriginalFlags]]. So an own flag property never touches it, and the fast
    // path runs only on a pattern that's neither global nor sticky — which is exactly when lastIndex is never written back.
    fn test_is_fast_and_non_observable(vm: &Vm, realm: Gc<Realm>, regexp_object: &Object) -> bool {
        Self::is_unmodified_regexp_instance(vm, realm, regexp_object)
            && Self::reading_last_index_is_unobservable(vm, regexp_object)
    }

    // 22.2.6.2 RegExp.prototype.exec ( string ), https://tc39.es/ecma262/#sec-regexp.prototype.exec
    fn exec(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let R be the this value.
        // 2. Perform ? RequireInternalSlot(R, [[RegExpMatcher]]).
        let regexp_object = typed_this_object::<RegExpObject>(vm, "RegExp")?;

        // 3. Let S be ? ToString(string).
        let string = vm.argument(0).to_primitive_string(vm)?;

        // 4. Return ? RegExpBuiltinExec(R, S).
        regexp_builtin_exec(vm, regexp_object, string)
    }

    // 22.2.6.4 get RegExp.prototype.flags, https://tc39.es/ecma262/#sec-get-regexp.prototype.flags
    fn flags(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let R be the this value.
        // 2. If Type(R) is not Object, throw a TypeError exception.
        let regexp_object = this_object(vm)?;

        // 3. Let result be the empty String.
        let mut builder = Utf16StringBuilder::with_capacity(8);

        // 4. Let hasIndices be ToBoolean(? Get(R, "hasIndices")).
        // 5. If hasIndices is true, append the code unit 0x0064 (LATIN SMALL LETTER D) as the last code unit of result.
        // 6. Let global be ToBoolean(? Get(R, "global")).
        // 7. If global is true, append the code unit 0x0067 (LATIN SMALL LETTER G) as the last code unit of result.
        // 8. Let ignoreCase be ToBoolean(? Get(R, "ignoreCase")).
        // 9. If ignoreCase is true, append the code unit 0x0069 (LATIN SMALL LETTER I) as the last code unit of result.
        // 10. Let multiline be ToBoolean(? Get(R, "multiline")).
        // 11. If multiline is true, append the code unit 0x006D (LATIN SMALL LETTER M) as the last code unit of result.
        // 12. Let dotAll be ToBoolean(? Get(R, "dotAll")).
        // 13. If dotAll is true, append the code unit 0x0073 (LATIN SMALL LETTER S) as the last code unit of result.
        // 14. Let unicode be ToBoolean(? Get(R, "unicode")).
        // 15. If unicode is true, append the code unit 0x0075 (LATIN SMALL LETTER U) as the last code unit of result.
        // 16. Let unicodeSets be ! ToBoolean(? Get(R, "unicodeSets")).
        // 17. If unicodeSets is true, append the code unit 0x0076 (LATIN SMALL LETTER V) as the last code unit of result.
        // 18. Let sticky be ToBoolean(? Get(R, "sticky")).
        // 19. If sticky is true, append the code unit 0x0079 (LATIN SMALL LETTER Y) as the last code unit of result.
        Self::append_truthy_flags(vm, regexp_object, &mut builder)?;

        // 20. Return result.
        Ok(Value::from_string(PrimitiveString::create(
            vm,
            builder.to_utf16_string(),
        )))
    }

    // 22.2.6.8 RegExp.prototype [ @@match ] ( string ), https://tc39.es/ecma262/#sec-regexp.prototype-@@match
    fn symbol_match(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("a builtin runs in a realm");

        // 1. Let rx be the this value.
        // 2. If Type(rx) is not Object, throw a TypeError exception.
        let regexp_object = this_object(vm)?;

        // 3. Let S be ? ToString(string).
        let string = vm.argument(0).to_primitive_string(vm)?;

        // 4. Let flags be ? ToString(? Get(rx, "flags")).
        let flags_value = regexp_object.get_with_cache(
            vm,
            &vm.names.flags,
            cache(vm, CacheSite::RegExpPrototypeSymbolMatchFlags),
        )?;
        let flags = flags_value.to_utf16_string(vm)?;

        // 5. If flags does not contain "g", then
        if !contains_code_unit(&flags, b'g') {
            // a. Return ? RegExpExec(rx, S).
            return regexp_exec(vm, &regexp_object, string);
        }

        // 6. Else,
        // a. If flags contains "u" or flags contains "v", let fullUnicode be true. Otherwise, let fullUnicode be false.
        let full_unicode = contains_code_unit(&flags, b'u') || contains_code_unit(&flags, b'v');

        // b. Perform ? Set(rx, "lastIndex", +0𝔽, true).
        regexp_object.set_with_cache(
            vm,
            &vm.names.lastIndex,
            Value::from_i32(0),
            cache(vm, CacheSite::RegExpPrototypeSymbolMatchLastIndex),
        )?;

        // c. Let A be ! ArrayCreate(0).
        let array = Array::create(vm, realm, 0, None).must();

        // d. Let n be 0.
        let mut n: u32 = 0;

        // e. Repeat,
        loop {
            // i. Let result be ? RegExpExec(rx, S).
            let result_value = regexp_exec(vm, &regexp_object, string)?;

            // ii. If result is null, then
            if result_value.is_null() {
                // 1. If n = 0, return null.
                if n == 0 {
                    return Ok(Value::NULL);
                }

                // 2. Return A.
                return Ok(Value::from_object(array));
            }

            assert!(result_value.is_object());
            let result = result_value.as_object();

            // iii. Else,

            // 1. Let matchStr be ? ToString(? Get(result, "0")).
            let match_value = result.get(vm, &PropertyKey::from(0u32))?;
            let match_string = match_value.to_utf16_string(vm)?;
            let match_string_is_empty = Utf16View::of_string(&match_string).is_empty();

            // 2. Perform ! CreateDataPropertyOrThrow(A, ! ToString(𝔽(n)), matchStr).
            array.indexed_put(
                n,
                Value::from_string(PrimitiveString::create(vm, match_string)),
                DEFAULT_ATTRIBUTES,
            );

            // 3. If matchStr is the empty String, then
            if match_string_is_empty {
                // Steps 3a-3c are implemented by increment_last_index.
                let string_data = string.utf16_string();
                increment_last_index(vm, regexp_object, Utf16View::of_string(&string_data), full_unicode)?;
            }

            // 4. Set n to n + 1.
            n = n.wrapping_add(1);
        }
    }

    // 22.2.6.9 RegExp.prototype [ @@matchAll ] ( string ), https://tc39.es/ecma262/#sec-regexp-prototype-matchall
    fn symbol_match_all(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("a builtin runs in a realm");

        // 1. Let R be the this value.
        // 2. If Type(R) is not Object, throw a TypeError exception.
        let regexp_object = this_object(vm)?;

        // 3. Let S be ? ToString(string).
        let string = vm.argument(0).to_primitive_string(vm)?;

        // 4. Let C be ? SpeciesConstructor(R, %RegExp%).
        let constructor = species_constructor(vm, &regexp_object, realm.intrinsics().regexp_constructor(vm).upcast())?;

        // 5. Let flags be ? ToString(? Get(R, "flags")).
        let flags_value = regexp_object.get_with_cache(
            vm,
            &vm.names.flags,
            cache(vm, CacheSite::RegExpPrototypeSymbolMatchAllFlags),
        )?;
        let flags = flags_value.to_utf16_string(vm)?;

        // Steps 9-12 are performed early so that flags can be moved.

        // 9. If flags contains "g", let global be true.
        // 10. Else, let global be false.
        let global = contains_code_unit(&flags, b'g');

        // 11. If flags contains "u" or flags contains "v", let fullUnicode be true.
        // 12. Else, let fullUnicode be false.
        let full_unicode = contains_code_unit(&flags, b'u') || contains_code_unit(&flags, b'v');

        // 6. Let matcher be ? Construct(C, « R, flags »).
        let matcher = construct(
            vm,
            constructor,
            &[
                Value::from_object(regexp_object),
                Value::from_string(PrimitiveString::create(vm, flags)),
            ],
            None,
        )?;

        // 7. Let lastIndex be ? ToLength(? Get(R, "lastIndex")).
        let last_index_value = regexp_object.get_with_cache(
            vm,
            &vm.names.lastIndex,
            cache(vm, CacheSite::RegExpPrototypeSymbolMatchAllGetLastIndex),
        )?;
        let last_index = last_index_value.to_length(vm)?;

        // 8. Perform ? Set(matcher, "lastIndex", lastIndex, true).
        matcher.set_with_cache(
            vm,
            &vm.names.lastIndex,
            Value::from_f64(last_index as f64),
            cache(vm, CacheSite::RegExpPrototypeSymbolMatchAllSetLastIndex),
        )?;

        // 13. Return CreateRegExpStringIterator(matcher, S, global, fullUnicode).
        Ok(Value::from_object(RegExpStringIterator::create(
            vm,
            realm,
            matcher,
            string,
            global,
            full_unicode,
        )))
    }

    // 22.2.6.11 RegExp.prototype [ @@replace ] ( string, replaceValue ), https://tc39.es/ecma262/#sec-regexp.prototype-@@replace
    fn symbol_replace(vm: &Vm) -> ThrowCompletionOr<Value> {
        let string_value = vm.argument(0);
        let replace_value = vm.argument(1);

        // 1. Let rx be the this value.
        // 2. If Type(rx) is not Object, throw a TypeError exception.
        let regexp_object = this_object(vm)?;

        // 3. Let S be ? ToString(string).
        let string = string_value.to_primitive_string(vm)?;

        Self::symbol_replace_impl(vm, regexp_object, string, replace_value)
    }

    /// The fast path of symbol_replace_impl(): str.replace(regexp, simple_string), on an unmodified RegExp whose flags,
    /// lastIndex and exec are all the intrinsic ones. Returns None when the regex cannot be compiled, in which case the
    /// generic path runs.
    fn symbol_replace_fast_path(
        vm: &Vm,
        realm: Gc<Realm>,
        typed_regexp: Gc<RegExpObject>,
        string: Gc<PrimitiveString>,
        replace_string: Utf16View<'_>,
    ) -> ThrowCompletionOr<Option<Value>> {
        let typed_regexp_object: Gc<Object> = typed_regexp.upcast();
        let flag_bits = typed_regexp.flag_bits();
        let is_global = flag_bits.has(RegExpFlags::GLOBAL);
        let is_sticky = flag_bits.has(RegExpFlags::STICKY);
        let is_unicode = flag_bits.has(RegExpFlags::UNICODE);
        let is_unicode_sets = flag_bits.has(RegExpFlags::UNICODE_SETS);
        // The flags string the algorithm derives global and fullUnicode from comes from the intrinsic
        // getter reading these very bits, which the predicate has just confirmed is what would happen.
        let full_unicode = is_unicode || is_unicode_sets;

        let Some(compiled_regex) = get_or_compile_regex(&typed_regexp) else {
            return Ok(None);
        };

        let string_data = string.utf16_string();
        let utf16_view = Utf16View::of_string(&string_data);
        let length_s = utf16_view.length_in_code_units();

        let mut last_index: u64 = 0;
        if is_global || is_sticky {
            let last_index_value = typed_regexp_object.get_with_cache(
                vm,
                &vm.names.lastIndex,
                cache(vm, CacheSite::RegExpPrototypeSymbolReplaceFastGetLastIndex),
            )?;
            last_index = last_index_value.to_length(vm)?;
        }
        if is_global {
            typed_regexp_object.set_with_cache(
                vm,
                &vm.names.lastIndex,
                Value::from_i32(0),
                cache(vm, CacheSite::RegExpPrototypeSymbolReplaceFastResetLastIndex),
            )?;
            last_index = 0;
        }

        let n_capture_groups = compiled_regex.capture_count();

        let need_legacy = typed_regexp.legacy_features_enabled() && realm == typed_regexp.realm();

        // NB: The result is at least as long as the string when the replacement is not shorter than what it replaces.
        let mut accumulated_result =
            Utf16StringBuilder::with_capacity(length_s + replace_string.length_in_code_units());
        let mut accumulated_result_length: usize = 0;
        let mut next_source_position: usize = 0;
        let mut had_match = false;
        let mut last_match_start: usize = 0;
        let mut last_match_end: usize = 0;

        // OPTIMIZATION: For global, non-sticky patterns, use batch find_all
        // to find all matches in a single Rust call.
        if is_global && !is_sticky && !full_unicode {
            let num_matches = compiled_regex.find_all(utf16_view, last_index as usize);
            if num_matches < 0 {
                return throw_backtrack_limit_exceeded(vm);
            }
            if num_matches > 0 {
                had_match = true;
                let last_match = compiled_regex.find_all_match(num_matches - 1);
                last_match_start = last_match.start as usize;
                last_match_end = last_match.end as usize;

                for i in 0..num_matches {
                    let found_match = compiled_regex.find_all_match(i);
                    let (match_start, match_end) = (found_match.start as usize, found_match.end as usize);
                    if match_start >= next_source_position {
                        let substring =
                            utf16_view.substring_view(next_source_position, match_start - next_source_position);
                        accumulated_result_length = checked_js_string_length_sum(
                            vm,
                            accumulated_result_length,
                            substring.length_in_code_units(),
                            ErrorType::StringSizeMustNotOverflow,
                        )?;
                        accumulated_result.append(substring);
                        accumulated_result_length = checked_js_string_length_sum(
                            vm,
                            accumulated_result_length,
                            replace_string.length_in_code_units(),
                            ErrorType::StringSizeMustNotOverflow,
                        )?;
                        accumulated_result.append(replace_string);
                        next_source_position = match_end;
                    }
                }
            }
        } else {
            // Loop finding matches and building the result string.
            loop {
                if last_index > length_s as u64 {
                    if is_sticky || is_global {
                        typed_regexp_object.set_with_cache(
                            vm,
                            &vm.names.lastIndex,
                            Value::from_i32(0),
                            cache(vm, CacheSite::RegExpPrototypeSymbolReplaceFastResetLastIndex),
                        )?;
                    }
                    break;
                }

                let exec_result = exec_with_unicode_last_index_retry(
                    &compiled_regex,
                    utf16_view,
                    last_index as usize,
                    full_unicode,
                    is_sticky,
                );
                if exec_result.result == MatchResult::LimitExceeded {
                    return throw_backtrack_limit_exceeded(vm);
                }
                let mut matched = exec_result.result == MatchResult::Match;

                // For sticky, match must start at exactly lastIndex.
                if matched && is_sticky && compiled_regex.capture_slot(0) as usize != exec_result.effective_last_index {
                    matched = false;
                }

                if !matched {
                    if is_sticky || is_global {
                        typed_regexp_object.set_with_cache(
                            vm,
                            &vm.names.lastIndex,
                            Value::from_i32(0),
                            cache(vm, CacheSite::RegExpPrototypeSymbolReplaceFastResetLastIndex),
                        )?;
                    }
                    break;
                }

                let match_start = compiled_regex.capture_slot(0) as usize;
                let match_end = compiled_regex.capture_slot(1) as usize;
                let match_length = match_end - match_start;
                had_match = true;
                last_match_start = match_start;
                last_match_end = match_end;

                // For sticky (non-global), update lastIndex on each match.
                // For global, lastIndex is always reset to 0 after the loop,
                // so skip intermediate updates.
                if is_sticky && !is_global {
                    typed_regexp_object.set_with_cache(
                        vm,
                        &vm.names.lastIndex,
                        index_value(match_end),
                        cache(vm, CacheSite::RegExpPrototypeSymbolReplaceFastSetLastIndex),
                    )?;
                }

                // Append the part of the string before this match + the replacement.
                if match_start >= next_source_position {
                    let substring = utf16_view.substring_view(next_source_position, match_start - next_source_position);
                    accumulated_result_length = checked_js_string_length_sum(
                        vm,
                        accumulated_result_length,
                        substring.length_in_code_units(),
                        ErrorType::StringSizeMustNotOverflow,
                    )?;
                    accumulated_result.append(substring);
                    accumulated_result_length = checked_js_string_length_sum(
                        vm,
                        accumulated_result_length,
                        replace_string.length_in_code_units(),
                        ErrorType::StringSizeMustNotOverflow,
                    )?;
                    accumulated_result.append(replace_string);
                    next_source_position = match_start + match_length;
                }

                if !is_global {
                    break;
                }

                // Handle empty match advancement.
                if match_length == 0 {
                    if full_unicode {
                        last_index = advance_string_index(utf16_view, match_end as u64, true);
                    } else {
                        last_index = match_end as u64 + 1;
                    }
                } else {
                    last_index = match_end as u64;
                }
            }
        } // end else (non-batch path)

        // Update legacy RegExp static properties once, with the last match.
        // For string replacements (no function callback), only the final
        // state matters since JS can't observe intermediate updates.
        if need_legacy && had_match {
            // For global replace, the internal buffer was overwritten by the
            // final failed search. Re-exec at the last match position to
            // populate captures. For non-global, the buffer is still valid.
            // A pattern without capture groups has no captures to populate.
            if is_global && n_capture_groups > 0 {
                let re_exec_result = compiled_regex.exec(utf16_view, last_match_start);
                if re_exec_result == MatchResult::LimitExceeded {
                    return throw_backtrack_limit_exceeded(vm);
                }
            }
            let legacy_captures = legacy_capture_slots(&compiled_regex, n_capture_groups);
            update_legacy_regexp_static_properties_lazy(
                vm,
                realm.intrinsics().regexp_constructor(vm),
                string,
                last_match_start,
                last_match_end,
                legacy_captures.starts(),
                legacy_captures.ends(),
            );
        } else if had_match && realm == typed_regexp.realm() {
            invalidate_legacy_regexp_static_properties(realm.intrinsics().regexp_constructor(vm));
        }

        // Fast path: if no matches were found, return the original string.
        if !had_match {
            return Ok(Some(Value::from_string(string)));
        }

        // Append the trailing portion of the string.
        if next_source_position < length_s {
            let substring = utf16_view.substring_view(next_source_position, length_s - next_source_position);
            checked_js_string_length_sum(
                vm,
                accumulated_result_length,
                substring.length_in_code_units(),
                ErrorType::StringSizeMustNotOverflow,
            )?;
            accumulated_result.append(substring);
        }

        Ok(Some(Value::from_string(PrimitiveString::create(
            vm,
            accumulated_result.to_utf16_string(),
        ))))
    }

    pub fn symbol_replace_impl(
        vm: &Vm,
        regexp_object: Gc<Object>,
        string: Gc<PrimitiveString>,
        replace_value: Value,
    ) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("a builtin runs in a realm");

        // 6. If functionalReplace is false, then
        //     a. Set replaceValue to ? ToString(replaceValue).
        // The fast path needs the coerced replacement to decide whether it applies, so it takes this step itself and
        // hands the result down to the generic path; ToString runs once per call either way.
        let mut replace_string: Option<Utf16String> = None;

        // OPTIMIZATION: Fast path for str.replace(regexp, simple_string).
        // When the replacement is a string without $ substitution patterns,
        // we can do the entire replace natively without creating any JS objects.
        if !replace_value.is_function() {
            let typed_regexp = regexp_object.downcast::<RegExpObject>();
            if Self::replace_is_fast_and_non_observable(vm, realm, &regexp_object) {
                let coerced_replace_string = replace_value.to_utf16_string(vm)?;

                // NB: Coercing the replacement runs user code — and that code can redefine exec, install a flag accessor,
                // or swap out the prototype. So, ask again before choosing the fast path: V8 re-casts to FastJSRegExp
                // after its own ToString (regexp-replace.tq), and JSC calls isSymbolReplaceFastAndNonObservable a 2nd time
                // (StringPrototype.cpp) — while SpiderMonkey puts its single check after the coercion (RegExp.js).
                // OPTIMIZATION: Coercing a primitive runs no user code, so nothing can have changed then.
                if (!replace_value.is_object() || Self::replace_is_fast_and_non_observable(vm, realm, &regexp_object))
                    && !contains_code_unit(&coerced_replace_string, b'$')
                {
                    let typed_regexp = typed_regexp.expect("the fast path only runs on RegExp objects");
                    if let Some(result) = Self::symbol_replace_fast_path(
                        vm,
                        realm,
                        typed_regexp,
                        string,
                        Utf16View::of_string(&coerced_replace_string),
                    )? {
                        return Ok(result);
                    }
                }
                replace_string = Some(coerced_replace_string);
            }
        }

        // 4. Let lengthS be the number of code unit elements in S.
        // 5. Let functionalReplace be IsCallable(replaceValue).

        // 6. If functionalReplace is false, then
        //     a. Set replaceValue to ? ToString(replaceValue).
        if !replace_value.is_function() && replace_string.is_none() {
            replace_string = Some(replace_value.to_utf16_string(vm)?);
        }

        // 7. Let flags be ? ToString(? Get(rx, "flags")).
        let flags_value = regexp_object.get_with_cache(
            vm,
            &vm.names.flags,
            cache(vm, CacheSite::RegExpPrototypeSymbolReplaceFlags),
        )?;
        let flags = flags_value.to_utf16_string(vm)?;

        // 8. If flags contains "g", let global be true. Otherwise, let global be false.
        let global = contains_code_unit(&flags, b'g');

        // 9. If global is true, then
        if global {
            // a. Perform ? Set(rx, "lastIndex", +0𝔽, true).
            regexp_object.set_with_cache(
                vm,
                &vm.names.lastIndex,
                Value::from_i32(0),
                cache(vm, CacheSite::RegExpPrototypeSymbolReplaceLastIndex),
            )?;
        }

        // 10. Let results be a new empty List.
        let results: MarkedVec<'_, Gc<Object>> = MarkedVec::new(vm);

        // 11. Let done be false.
        // 12. Repeat, while done is false,
        loop {
            // a. Let result be ? RegExpExec(rx, S).
            let result = regexp_exec(vm, &regexp_object, string)?;

            // b. If result is null, set done to true.
            if result.is_null() {
                break;
            }

            // c. Else,

            // i. Append result to the end of results.
            results.push(result.as_object());

            // ii. If global is false, set done to true.
            if !global {
                break;
            }

            // iii. Else,

            // 1. Let matchStr be ? ToString(? Get(result, "0")).
            let match_value = result.get(vm, &PropertyKey::from(0u32))?;
            let match_string = match_value.to_utf16_string(vm)?;

            // 2. If matchStr is the empty String, then
            if Utf16View::of_string(&match_string).is_empty() {
                // b. If flags contains "u" or flags contains "v", let fullUnicode be true. Otherwise, let fullUnicode be false.
                let full_unicode = contains_code_unit(&flags, b'u') || contains_code_unit(&flags, b'v');

                // Steps 2a, 2c-2d are implemented by increment_last_index.
                let string_data = string.utf16_string();
                increment_last_index(vm, regexp_object, Utf16View::of_string(&string_data), full_unicode)?;
            }
        }

        // 13. Let accumulatedResult be the empty String.
        let mut accumulated_result = Utf16StringBuilder::new();
        let mut accumulated_result_length: usize = 0;

        // 14. Let nextSourcePosition be 0.
        let mut next_source_position: usize = 0;

        let string_data = string.utf16_string();
        let string_view = Utf16View::of_string(&string_data);

        // 15. For each element result of results, do
        for result_index in 0..results.len() {
            let result = results.get(result_index).expect("the index is in bounds");

            // a. Let resultLength be ? LengthOfArrayLike(result).
            let result_length = length_of_array_like(vm, &result)?;

            // b. Let nCaptures be max(resultLength - 1, 0).
            let n_captures = result_length.saturating_sub(1);

            // c. Let matched be ? ToString(? Get(result, "0")).
            let matched_value = result.get(vm, &PropertyKey::from(0u32))?;
            let matched = matched_value.to_primitive_string(vm)?;

            // d. Let matchLength be the length of matched.
            let matched_length = matched.length_in_utf16_code_units();

            // e. Let position be ? ToIntegerOrInfinity(? Get(result, "index")).
            let position_value = result.get_with_cache(
                vm,
                &vm.names.index,
                cache(vm, CacheSite::RegExpPrototypeSymbolReplaceIndex),
            )?;
            let mut position = position_value.to_integer_or_infinity(vm)?;

            // f. Set position to the result of clamping position between 0 and lengthS.
            position = position.clamp(0.0, string.length_in_utf16_code_units() as f64);

            // g. Let captures be a new empty List.
            let captures: MarkedVec<'_, Value> = MarkedVec::new(vm);

            // h. Let n be 1.
            // i. Repeat, while n ≤ nCaptures,
            for n in 1..=n_captures {
                // i. Let capN be ? Get(result, ! ToString(𝔽(n))).
                let mut capture = result.get(vm, &PropertyKey::from_number(n))?;

                // ii. If capN is not undefined, then
                if !capture.is_undefined() {
                    // 1. Set capN to ? ToString(capN).
                    capture = Value::from_string(PrimitiveString::create(vm, capture.to_utf16_string(vm)?));
                }

                // iii. Append capN as the last element of captures.
                captures.push(capture);

                // iv. NOTE: When n = 1, the preceding step puts the first element into captures (at index 0). More generally, the nth capture (the characters captured by the nth set of capturing parentheses) is at captures[n - 1].
                // v. Set n to n + 1.
            }

            // j. Let namedCaptures be ? Get(result, "groups").
            let mut named_captures = result.get_with_cache(
                vm,
                &vm.names.groups,
                cache(vm, CacheSite::RegExpPrototypeSymbolReplaceGroups),
            )?;

            // k. If functionalReplace is true, then
            let replacement = if replace_value.is_function() {
                // i. Let replacerArgs be the list-concatenation of « matched », captures, and « 𝔽(position), S ».
                let replacer_args: MarkedVec<'_, Value> = MarkedVec::new(vm);
                replacer_args.push(Value::from_string(matched));
                for capture_index in 0..captures.len() {
                    replacer_args.push(captures.get(capture_index).expect("the index is in bounds"));
                }
                replacer_args.push(Value::from_f64(position));
                replacer_args.push(Value::from_string(string));

                // ii. If namedCaptures is not undefined, then
                if !named_captures.is_undefined() {
                    // 1. Append namedCaptures as the last element of replacerArgs.
                    replacer_args.push(named_captures);
                }

                // iii. Let replValue be ? Call(replaceValue, undefined, replacerArgs).
                let replace_result = call_function_object(
                    vm,
                    replace_value.as_function(),
                    Value::UNDEFINED,
                    &replacer_args.to_vec(),
                )?;

                // iv. Let replacement be ? ToString(replValue).
                replace_result.to_utf16_string(vm)?
            }
            // l. Else,
            else {
                // i. If namedCaptures is not undefined, then
                if !named_captures.is_undefined() {
                    // 1. Set namedCaptures to ? ToObject(namedCaptures).
                    named_captures = Value::from_object(named_captures.to_object(vm)?);
                }

                // ii. Let replacement be ? GetSubstitution(matched, S, position, captures, namedCaptures, replaceValue).
                let replace_string = replace_string
                    .as_ref()
                    .expect("a replacement that is not a function was converted to a string");
                let matched_data = matched.utf16_string();
                get_substitution(
                    vm,
                    Utf16View::of_string(&matched_data),
                    string_view,
                    position as usize,
                    &captures.to_vec(),
                    named_captures,
                    Utf16View::of_string(replace_string),
                )?
            };

            // m. If position ≥ nextSourcePosition, then
            if position >= next_source_position as f64 {
                // i. NOTE: position should not normally move backwards. If it does, it is an indication of an ill-behaving RegExp subclass or use of an access triggered side-effect to change the global flag or other characteristics of rx. In such cases, the corresponding substitution is ignored.

                // ii. Set accumulatedResult to the string-concatenation of accumulatedResult, the substring of S from nextSourcePosition to position, and replacement.
                let position = position as usize;
                let substring = string_view.substring_view(next_source_position, position - next_source_position);
                accumulated_result_length = checked_js_string_length_sum(
                    vm,
                    accumulated_result_length,
                    substring.length_in_code_units(),
                    ErrorType::StringSizeMustNotOverflow,
                )?;
                accumulated_result.append(substring);
                let replacement_view = Utf16View::of_string(&replacement);
                accumulated_result_length = checked_js_string_length_sum(
                    vm,
                    accumulated_result_length,
                    replacement_view.length_in_code_units(),
                    ErrorType::StringSizeMustNotOverflow,
                )?;
                accumulated_result.append(replacement_view);

                // iii. Set nextSourcePosition to position + matchLength.
                next_source_position = position + matched_length;
            }
        }

        // 16. If nextSourcePosition ≥ lengthS, return accumulatedResult.
        if next_source_position >= string.length_in_utf16_code_units() {
            return Ok(Value::from_string(PrimitiveString::create(
                vm,
                accumulated_result.to_utf16_string(),
            )));
        }

        // 17. Return the string-concatenation of accumulatedResult and the substring of S from nextSourcePosition.
        let substring = string_view.substring_view(
            next_source_position,
            string_view.length_in_code_units() - next_source_position,
        );
        checked_js_string_length_sum(
            vm,
            accumulated_result_length,
            substring.length_in_code_units(),
            ErrorType::StringSizeMustNotOverflow,
        )?;
        accumulated_result.append(substring);

        Ok(Value::from_string(PrimitiveString::create(
            vm,
            accumulated_result.to_utf16_string(),
        )))
    }

    // 22.2.6.12 RegExp.prototype [ @@search ] ( string ), https://tc39.es/ecma262/#sec-regexp.prototype-@@search
    fn symbol_search(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let rx be the this value.
        // 2. If Type(rx) is not Object, throw a TypeError exception.
        let regexp_object = this_object(vm)?;

        // 3. Let S be ? ToString(string).
        let string = vm.argument(0).to_primitive_string(vm)?;

        // 4. Let previousLastIndex be ? Get(rx, "lastIndex").
        let previous_last_index = regexp_object.get_with_cache(
            vm,
            &vm.names.lastIndex,
            cache(vm, CacheSite::RegExpPrototypeSymbolSearchGetLastIndex),
        )?;

        // 5. If SameValue(previousLastIndex, +0𝔽) is false, then
        if !same_value(previous_last_index, Value::from_i32(0)) {
            // a. Perform ? Set(rx, "lastIndex", +0𝔽, true).
            regexp_object.set_with_cache(
                vm,
                &vm.names.lastIndex,
                Value::from_i32(0),
                cache(vm, CacheSite::RegExpPrototypeSymbolSearchResetLastIndex),
            )?;
        }

        // 6. Let result be ? RegExpExec(rx, S).
        let result = regexp_exec(vm, &regexp_object, string)?;

        // 7. Let currentLastIndex be ? Get(rx, "lastIndex").
        let current_last_index = regexp_object.get_with_cache(
            vm,
            &vm.names.lastIndex,
            cache(vm, CacheSite::RegExpPrototypeSymbolSearchGetCurrentLastIndex),
        )?;

        // 8. If SameValue(currentLastIndex, previousLastIndex) is false, then
        if !same_value(current_last_index, previous_last_index) {
            // a. Perform ? Set(rx, "lastIndex", previousLastIndex, true).
            regexp_object.set_with_cache(
                vm,
                &vm.names.lastIndex,
                previous_last_index,
                cache(vm, CacheSite::RegExpPrototypeSymbolSearchRestoreLastIndex),
            )?;
        }

        // 9. If result is null, return -1𝔽.
        if result.is_null() {
            return Ok(Value::from_i32(-1));
        }

        // 10. Return ? Get(result, "index").
        result.get_with_cache(
            vm,
            &vm.names.index,
            cache(vm, CacheSite::RegExpPrototypeSymbolSearchIndex),
        )
    }

    // 22.2.6.13 get RegExp.prototype.source, https://tc39.es/ecma262/#sec-get-regexp.prototype.source
    fn source(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("a builtin runs in a realm");

        // 1. Let R be the this value.
        // 2. If Type(R) is not Object, throw a TypeError exception.
        let regexp_object = this_object(vm)?;

        // 3. If R does not have an [[OriginalSource]] internal slot, then
        let Some(typed_regexp_object) = regexp_object.downcast::<RegExpObject>() else {
            // a. If SameValue(R, %RegExp.prototype%) is true, return "(?:)".
            if same_value(
                Value::from_object(regexp_object),
                Value::from_object(realm.intrinsics().regexp_prototype(vm)),
            ) {
                return Ok(Value::from_string(PrimitiveString::create_from_fly_string(
                    vm,
                    &Utf16FlyString::from_utf8("(?:)"),
                )));
            }

            // b. Otherwise, throw a TypeError exception.
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAnObjectOfType, &[&"RegExp"]);
        };

        // 4. Assert: R has an [[OriginalFlags]] internal slot.
        // 5. Let src be R.[[OriginalSource]].
        // 6. Let flags be R.[[OriginalFlags]].
        // 7. Return EscapeRegExpPattern(src, flags).
        Ok(Value::from_string(PrimitiveString::create(
            vm,
            typed_regexp_object.escape_regexp_pattern(),
        )))
    }

    // 22.2.6.14 RegExp.prototype [ @@split ] ( string, limit ), https://tc39.es/ecma262/#sec-regexp.prototype-@@split
    fn symbol_split(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let rx be the this value.
        // 2. If Type(rx) is not Object, throw a TypeError exception.
        let regexp_object = this_object(vm)?;

        // 3. Let S be ? ToString(string).
        let string = vm.argument(0).to_primitive_string(vm)?;

        Self::symbol_split_impl(vm, regexp_object, string, vm.argument(1))
    }

    /// The fast path of symbol_split_impl(): split with an unmodified RegExp, which bypasses the spec's
    /// SpeciesConstructor/Construct overhead and calls the regex directly with explicit start positions. Returns None
    /// when the regex cannot be compiled, in which case the generic path runs.
    fn symbol_split_fast_path(
        vm: &Vm,
        realm: Gc<Realm>,
        typed_regexp: Gc<RegExpObject>,
        string: Gc<PrimitiveString>,
        limit_value: Value,
    ) -> ThrowCompletionOr<Option<Value>> {
        let Some(compiled_regex) = get_or_compile_regex(&typed_regexp) else {
            return Ok(None);
        };

        let flag_bits = typed_regexp.flag_bits();
        let is_unicode = flag_bits.has(RegExpFlags::UNICODE);
        let is_unicode_sets = flag_bits.has(RegExpFlags::UNICODE_SETS);
        let unicode_matching = is_unicode || is_unicode_sets;

        let mut limit = u32::MAX;
        if !limit_value.is_undefined() {
            limit = limit_value.to_u32(vm)?;
        }

        let array = Array::create(vm, realm, 0, None).must();

        if limit == 0 {
            return Ok(Some(Value::from_object(array)));
        }

        let string_data = string.utf16_string();
        let utf16_view = Utf16View::of_string(&string_data);
        let size = utf16_view.length_in_code_units();

        // Empty string case.
        if size == 0 {
            let empty_result = compiled_regex.exec(utf16_view, 0);
            if empty_result == MatchResult::LimitExceeded {
                return throw_backtrack_limit_exceeded(vm);
            }
            if empty_result != MatchResult::Match {
                array.indexed_put(0, Value::from_string(string), DEFAULT_ATTRIBUTES);
            }
            return Ok(Some(Value::from_object(array)));
        }

        let mut array_length: u32 = 0;
        let mut last_match_end: usize = 0;
        let mut next_search_from: usize = 0;
        let n_capture_groups = compiled_regex.capture_count();
        let total_groups = compiled_regex.total_groups();

        let need_legacy = typed_regexp.legacy_features_enabled() && realm == typed_regexp.realm();

        while next_search_from < size {
            // The spec's split algorithm uses sticky semantics: match must
            // start at exactly next_search_from. We do a forward search and
            // then handle the case where the match starts later.
            let split_exec_result = compiled_regex.exec(utf16_view, next_search_from);
            if split_exec_result == MatchResult::LimitExceeded {
                return throw_backtrack_limit_exceeded(vm);
            }
            let matched = split_exec_result == MatchResult::Match;

            if !matched {
                break;
            }

            let match_start = compiled_regex.capture_slot(0) as usize;
            let match_end = compiled_regex.capture_slot(1) as usize;
            let last_index = match_end.min(size);

            // If the match starts at or past the end of string, it's not
            // a valid split point (spec's while q < size would have exited).
            if match_start >= size {
                break;
            }

            // Update legacy properties before split decides whether to
            // ignore this successful match.
            if need_legacy {
                let legacy_captures = legacy_capture_slots(&compiled_regex, n_capture_groups);
                update_legacy_regexp_static_properties_lazy(
                    vm,
                    realm.intrinsics().regexp_constructor(vm),
                    string,
                    match_start,
                    match_end,
                    legacy_captures.starts(),
                    legacy_captures.ends(),
                );
            }

            // If the match doesn't start at next_search_from, skip to
            // where it does start.
            if match_start > next_search_from {
                next_search_from = match_start;
            }

            // If match is zero-width at same position as last split, advance.
            if last_index == last_match_end {
                next_search_from = advance_string_index(utf16_view, next_search_from as u64, unicode_matching) as usize;
                continue;
            }

            // Add substring before this match.
            array.indexed_put(
                array_length,
                substring(vm, string, last_match_end, next_search_from - last_match_end),
                DEFAULT_ATTRIBUTES,
            );
            array_length += 1;
            if array_length == limit {
                return Ok(Some(Value::from_object(array)));
            }

            last_match_end = last_index;

            // Add captures.
            for i in 1..=n_capture_groups {
                let capture_start = if i < total_groups {
                    compiled_regex.capture_slot(i * 2)
                } else {
                    -1
                };
                let capture_end = if i < total_groups {
                    compiled_regex.capture_slot(i * 2 + 1)
                } else {
                    -1
                };

                if capture_start >= 0 && capture_end >= 0 {
                    array.indexed_put(
                        array_length,
                        substring(
                            vm,
                            string,
                            capture_start as usize,
                            (capture_end - capture_start) as usize,
                        ),
                        DEFAULT_ATTRIBUTES,
                    );
                } else {
                    array.indexed_put(array_length, Value::UNDEFINED, DEFAULT_ATTRIBUTES);
                }
                array_length += 1;
                if array_length == limit {
                    return Ok(Some(Value::from_object(array)));
                }
            }

            next_search_from = last_match_end;
        }

        // Add trailing substring.
        array.indexed_put(
            array_length,
            substring(vm, string, last_match_end, size - last_match_end),
            DEFAULT_ATTRIBUTES,
        );

        Ok(Some(Value::from_object(array)))
    }

    pub fn symbol_split_impl(
        vm: &Vm,
        regexp_object: Gc<Object>,
        string: Gc<PrimitiveString>,
        limit_value: Value,
    ) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("a builtin runs in a realm");

        // OPTIMIZATION: Fast path for split with regex.
        // When we have an unmodified RegExp, bypass the spec's SpeciesConstructor/Construct
        // overhead and call the regex directly with explicit start positions.
        if Self::split_is_fast_and_non_observable(vm, realm, &regexp_object)
            && let Some(typed_regexp) = regexp_object.downcast::<RegExpObject>()
            && typed_regexp.legacy_features_enabled()
            && (limit_value.is_undefined() || limit_value.is_number())
            && let Some(result) = Self::symbol_split_fast_path(vm, realm, typed_regexp, string, limit_value)?
        {
            return Ok(result);
        }

        // 4. Let C be ? SpeciesConstructor(rx, %RegExp%).
        let constructor = species_constructor(vm, &regexp_object, realm.intrinsics().regexp_constructor(vm).upcast())?;

        // 5. Let flags be ? ToString(? Get(rx, "flags")).
        let flags_value = regexp_object.get_with_cache(
            vm,
            &vm.names.flags,
            cache(vm, CacheSite::RegExpPrototypeSymbolSplitFlags),
        )?;
        let flags = flags_value.to_utf16_string(vm)?;

        // 6. If flags contains "u" or flags contains "v", let unicodeMatching be true.
        // 7. Else, let unicodeMatching be false.
        let unicode_matching = contains_code_unit(&flags, b'u') || contains_code_unit(&flags, b'v');

        // 8. If flags contains "y", let newFlags be flags.
        // 9. Else, let newFlags be the string-concatenation of flags and "y".
        let new_flags = if contains_code_unit(&flags, b'y') {
            flags
        } else {
            concatenate(&[Utf16View::of_string(&flags), Utf16View::Ascii(b"y")])
        };

        // 10. Let splitter be ? Construct(C, « rx, newFlags »).
        let splitter = construct(
            vm,
            constructor,
            &[
                Value::from_object(regexp_object),
                Value::from_string(PrimitiveString::create(vm, new_flags)),
            ],
            None,
        )?;

        // 11. Let A be ! ArrayCreate(0).
        let array = Array::create(vm, realm, 0, None).must();

        // 12. Let lengthA be 0.
        let mut array_length: u32 = 0;

        // 13. If limit is undefined, let lim be 2^32 - 1; else let lim be ℝ(? ToUint32(limit)).
        let mut limit = u32::MAX;
        if !limit_value.is_undefined() {
            limit = limit_value.to_u32(vm)?;
        }

        // 14. If lim is 0, return A.
        if limit == 0 {
            return Ok(Value::from_object(array));
        }

        // 15. If S is the empty String, then
        if string.is_empty() {
            // a. Let z be ? RegExpExec(splitter, S).
            let result = regexp_exec(vm, &splitter, string)?;

            // b. If z is not null, return A.
            if !result.is_null() {
                return Ok(Value::from_object(array));
            }

            // c. Perform ! CreateDataPropertyOrThrow(A, "0", S).
            array.indexed_put(0, Value::from_string(string), DEFAULT_ATTRIBUTES);

            // d. Return A.
            return Ok(Value::from_object(array));
        }

        // 16. Let size be the length of S.
        let size = string.length_in_utf16_code_units();
        let string_data = string.utf16_string();
        let string_view = Utf16View::of_string(&string_data);

        // 17. Let p be 0.
        let mut last_match_end: usize = 0;

        // 18. Let q be p.
        let mut next_search_from: usize = 0;

        // 19. Repeat, while q < size,
        while next_search_from < size {
            // a. Perform ? Set(splitter, "lastIndex", 𝔽(q), SplitBehavior::KeepEmpty).
            splitter.set_with_cache(
                vm,
                &vm.names.lastIndex,
                index_value(next_search_from),
                cache(vm, CacheSite::RegExpPrototypeSymbolSplitSetLastIndex),
            )?;

            // b. Let z be ? RegExpExec(splitter, S).
            let result = regexp_exec(vm, &splitter, string)?;

            // c. If z is null, set q to AdvanceStringIndex(S, q, unicodeMatching).
            if result.is_null() {
                next_search_from =
                    advance_string_index(string_view, next_search_from as u64, unicode_matching) as usize;
                continue;
            }

            // d. Else,

            // i. Let e be ℝ(? ToLength(? Get(splitter, "lastIndex"))).
            let last_index_value = splitter.get_with_cache(
                vm,
                &vm.names.lastIndex,
                cache(vm, CacheSite::RegExpPrototypeSymbolSplitGetLastIndex),
            )?;
            let last_index = last_index_value.to_length(vm)?;

            // ii. Set e to min(e, size).
            let last_index = last_index.min(size as u64) as usize;

            // iii. If e = p, set q to AdvanceStringIndex(S, q, unicodeMatching).
            if last_index == last_match_end {
                next_search_from =
                    advance_string_index(string_view, next_search_from as u64, unicode_matching) as usize;
                continue;
            }

            // iv. Else,

            // 1. Let T be the substring of S from p to q.
            // 2. Perform ! CreateDataPropertyOrThrow(A, ! ToString(𝔽(lengthA)), T).
            array.indexed_put(
                array_length,
                substring(vm, string, last_match_end, next_search_from - last_match_end),
                DEFAULT_ATTRIBUTES,
            );

            // 3. Set lengthA to lengthA + 1.
            array_length += 1;

            // 4. If lengthA = lim, return A.
            if array_length == limit {
                return Ok(Value::from_object(array));
            }

            // 5. Set p to e.
            last_match_end = last_index;

            // 6. Let numberOfCaptures be ? LengthOfArrayLike(z).
            let mut number_of_captures = length_of_array_like(vm, &result.as_object())?;

            // 7. Set numberOfCaptures to max(numberOfCaptures - 1, 0).
            number_of_captures = number_of_captures.saturating_sub(1);

            // 8. Let i be 1.
            // 9. Repeat, while i ≤ numberOfCaptures,
            for i in 1..=number_of_captures {
                // a. Let nextCapture be ? Get(z, ! ToString(𝔽(i))).
                let next_capture = result.get(vm, &PropertyKey::from_number(i))?;

                // b. Perform ! CreateDataPropertyOrThrow(A, ! ToString(𝔽(lengthA)), nextCapture).
                array.indexed_put(array_length, next_capture, DEFAULT_ATTRIBUTES);

                // c. Set i to i + 1.

                // d. Set lengthA to lengthA + 1.
                array_length += 1;

                // e. If lengthA = lim, return A.
                if array_length == limit {
                    return Ok(Value::from_object(array));
                }
            }

            // 10. Set q to p.
            next_search_from = last_match_end;
        }

        // 20. Let T be the substring of S from p to size.
        // 21. Perform ! CreateDataPropertyOrThrow(A, ! ToString(𝔽(lengthA)), T).
        array.indexed_put(
            array_length,
            substring(vm, string, last_match_end, size - last_match_end),
            DEFAULT_ATTRIBUTES,
        );

        // 22. Return A.
        Ok(Value::from_object(array))
    }

    // 22.2.6.16 RegExp.prototype.test ( S ), https://tc39.es/ecma262/#sec-regexp.prototype.test
    fn test(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let R be the this value.
        // 2. If Type(R) is not Object, throw a TypeError exception.
        let regexp_object = this_object(vm)?;

        // 3. Let string be ? ToString(S).
        let string = vm.argument(0).to_primitive_string(vm)?;

        // OPTIMIZATION: Fast path for test() on non-global, non-sticky RegExp objects.
        // Use the regex test() directly, avoiding result Array creation.
        let realm = vm.current_realm().expect("a builtin runs in a realm");
        if Self::test_is_fast_and_non_observable(vm, realm, &regexp_object) {
            let typed_regexp = regexp_object
                .downcast::<RegExpObject>()
                .expect("the fast path only runs on RegExp objects");
            let flag_bits = typed_regexp.flag_bits();
            let global = flag_bits.has(RegExpFlags::GLOBAL);
            let sticky = flag_bits.has(RegExpFlags::STICKY);

            // Only use fast path when we don't need to update lastIndex.
            if !global
                && !sticky
                && let Some(compiled_regex) = get_or_compile_regex(&typed_regexp)
            {
                let string_data = string.utf16_string();
                let utf16_view = Utf16View::of_string(&string_data);

                if !typed_regexp.legacy_features_enabled() {
                    // Fastest path: just test, no captures or legacy props.
                    let test_result = compiled_regex.test(utf16_view, 0);
                    if test_result == MatchResult::LimitExceeded {
                        return throw_backtrack_limit_exceeded(vm);
                    }
                    if test_result == MatchResult::Match && realm == typed_regexp.realm() {
                        invalidate_legacy_regexp_static_properties(realm.intrinsics().regexp_constructor(vm));
                    }
                    return Ok(Value::from_bool(test_result == MatchResult::Match));
                }

                // Fast path with legacy property updates: use exec to
                // get captures, then set legacy props lazily.
                let test_exec_result = compiled_regex.exec(utf16_view, 0);
                if test_exec_result == MatchResult::LimitExceeded {
                    return throw_backtrack_limit_exceeded(vm);
                }
                let matched = test_exec_result == MatchResult::Match;
                // NB: RegExpBuiltinExec only updates the legacy static properties after a match, and only for a RegExp
                //     of the current realm.
                if matched && realm == typed_regexp.realm() {
                    let n_capture_groups = compiled_regex.capture_count();
                    let match_start = compiled_regex.capture_slot(0) as usize;
                    let match_end = compiled_regex.capture_slot(1) as usize;
                    let legacy_captures = legacy_capture_slots(&compiled_regex, n_capture_groups);
                    update_legacy_regexp_static_properties_lazy(
                        vm,
                        realm.intrinsics().regexp_constructor(vm),
                        string,
                        match_start,
                        match_end,
                        legacy_captures.starts(),
                        legacy_captures.ends(),
                    );
                }
                return Ok(Value::from_bool(matched));
            }
        }

        // 4. Let match be ? RegExpExec(R, string).
        let match_ = regexp_exec(vm, &regexp_object, string)?;

        // 5. If match is not null, return true; else return false.
        Ok(Value::from_bool(!match_.is_null()))
    }

    // 22.2.6.17 RegExp.prototype.toString ( ), https://tc39.es/ecma262/#sec-regexp.prototype.tostring
    fn to_string(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let R be the this value.
        // 2. If Type(R) is not Object, throw a TypeError exception.
        let regexp_object = this_object(vm)?;

        // 3. Let pattern be ? ToString(? Get(R, "source")).
        let source_attribute = regexp_object.get_with_cache(
            vm,
            &vm.names.source,
            cache(vm, CacheSite::RegExpPrototypeToStringSource),
        )?;
        let pattern = source_attribute.to_utf16_string(vm)?;

        // 4. Let flags be ? ToString(? Get(R, "flags")).
        let flags_attribute =
            regexp_object.get_with_cache(vm, &vm.names.flags, cache(vm, CacheSite::RegExpPrototypeToStringFlags))?;
        let flags = flags_attribute.to_utf16_string(vm)?;

        // 5. Let result be the string-concatenation of "/", pattern, "/", and flags.
        // 6. Return result.
        Ok(Value::from_string(PrimitiveString::create(
            vm,
            concatenate(&[
                Utf16View::Ascii(b"/"),
                Utf16View::of_string(&pattern),
                Utf16View::Ascii(b"/"),
                Utf16View::of_string(&flags),
            ]),
        )))
    }

    // B.2.4.1 RegExp.prototype.compile ( pattern, flags ), https://tc39.es/ecma262/#sec-regexp.prototype.compile
    // B.2.4.1 RegExp.prototype.compile ( pattern, flags ), https://github.com/tc39/proposal-regexp-legacy-features#regexpprototypecompile--pattern-flags-
    fn compile(vm: &Vm) -> ThrowCompletionOr<Value> {
        let mut pattern = vm.argument(0);
        let mut flags = vm.argument(1);

        // 1. Let O be the this value.
        // 2. Perform ? RequireInternalSlot(O, [[RegExpMatcher]]).
        let regexp_object = typed_this_object::<RegExpObject>(vm, "RegExp")?;

        // 3. Let thisRealm be the current Realm Record.
        let this_realm = vm.current_realm();

        // 4. Let oRealm be the value of O’s [[Realm]] internal slot.
        let regexp_object_realm = Some(regexp_object.realm());

        // 5. If SameValue(thisRealm, oRealm) is false, throw a TypeError exception.
        if this_realm != regexp_object_realm {
            return vm.throw_completion(
                ErrorKind::TypeError,
                ErrorType::RegExpCompileError,
                &[&"thisRealm and oRealm is not same value"],
            );
        }

        // 6. If the value of R’s [[LegacyFeaturesEnabled]] internal slot is false, throw a TypeError exception.
        if !regexp_object.legacy_features_enabled() {
            return vm.throw_completion(
                ErrorKind::TypeError,
                ErrorType::RegExpCompileError,
                &[&"legacy features is not enabled"],
            );
        }

        // 7. If Type(pattern) is Object and pattern has a [[RegExpMatcher]] internal slot, then
        if pattern.is_object()
            && let Some(regexp_pattern) = pattern.as_object().downcast::<RegExpObject>()
        {
            // a. If flags is not undefined, throw a TypeError exception.
            if !flags.is_undefined() {
                return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotUndefined, &[&flags]);
            }

            // b. Let P be pattern.[[OriginalSource]].
            pattern = Value::from_string(PrimitiveString::create(vm, regexp_pattern.pattern()));

            // c. Let F be pattern.[[OriginalFlags]].
            flags = Value::from_string(PrimitiveString::create(vm, regexp_pattern.flags()));
        }
        // 8. Else,
        //     a. Let P be pattern.
        //     b. Let F be flags.

        // 9. Return ? RegExpInitialize(O, P, F).
        Ok(Value::from_object(regexp_object.regexp_initialize(vm, pattern, flags)?))
    }
}
