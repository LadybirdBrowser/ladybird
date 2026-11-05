/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSSFontFeatureValuesMap, https://drafts.csswg.org/css-fonts-4/#cssfontfeaturevaluesmap

use ak::Utf16String;
use libjs_rust::api::{Array, Gc, Map, MarkedVec, Object, Trace, Value, Vm};
use libweb_rust::FontFeatureValuesRuleKind;

use super::css_font_feature_values_rule::CSSFontFeatureValuesRule;
use crate::bindings::conversions::ToJs;
use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::maplike::context_of_map;
use crate::bindings::maplike::{Maplike, WritableMaplike};
use crate::bindings::setlike::WorldEntries;
use crate::bindings::{CallContext, Exception, WebIdlResult};
// NB: VibratePattern is the only typedef of (unsigned long or sequence<unsigned long>), so the bindings name the union
//     after it.
use crate::notifications_api::notification::VibratePattern;

fn create_map_value(cx: &CallContext, values: &[u32]) -> Value {
    let list = MarkedVec::with_capacity(cx.vm(), values.len());
    for value in values {
        list.push(Value::from_f64(f64::from(*value)));
    }
    Value::from_object(Array::create_from(cx.vm(), cx.realm(), &list.values()).upcast::<Object>())
}

fn set_map_entry(cx: &CallContext, map: Gc<Map>, name: &Utf16String, values: &[u32]) {
    let key = name.clone().to_js(cx).expect("a string converts to a JS value");
    map.map_set(cx.vm(), key, create_map_value(cx, values));
}

// https://drafts.csswg.org/css-fonts-4/#cssfontfeaturevaluesmap
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CSSFontFeatureValuesMap {
    #[gc(untraced)]
    kind: FontFeatureValuesRuleKind,
    parent_rule: Ref<CSSFontFeatureValuesRule>,
    map_entries: WorldEntries<Map>,
}

impl Wrappable for CSSFontFeatureValuesMap {}

impl CSSFontFeatureValuesMap {
    pub(crate) fn create(
        vm: &Vm,
        kind: FontFeatureValuesRuleKind,
        parent_rule: Ref<CSSFontFeatureValuesRule>,
    ) -> Ref<Self> {
        allocate(vm, |_| Self {
            kind,
            parent_rule,
            map_entries: WorldEntries::default(),
        })
    }

    fn max_value_count(&self) -> usize {
        match self.kind {
            FontFeatureValuesRuleKind::CharacterVariant => 2,
            FontFeatureValuesRuleKind::Styleset => usize::MAX,
            _ => 1,
        }
    }

    fn entries(&self) -> Vec<(Utf16String, Vec<u32>)> {
        self.parent_rule
            .rule()
            .native_rule()
            .font_feature_values_entries(self.kind)
    }

    // https://drafts.csswg.org/css-fonts-4/#cssfontfeaturevaluesmap
    pub(crate) fn set(
        &self,
        cx: &mut CallContext,
        feature_value_name: Utf16String,
        values: VibratePattern,
    ) -> WebIdlResult<()> {
        // The CSSFontFeatureValuesMap interface uses the default map class methods but the set method has different
        // behavior. It takes a sequence of unsigned integers and associates it with a given featureValueName. The
        // method behaves the same as the default map class method except that

        // A single unsigned long value is treated as a sequence of a single value.
        let values = match values {
            VibratePattern::UnsignedLong(value) => vec![value],
            VibratePattern::Sequence(values) => values,
        };

        // The method throws an exception if an invalid number of values is passed in.
        if values.is_empty() {
            return Err(Exception::Dom(
                "InvalidAccessError",
                Utf16String::from_utf8("CSSFontFeatureValuesMap.set requires at least one value."),
            ));
        }

        // If the associated feature value block only allows a limited number of values, the set method throws an
        // InvalidAccessError exception when the input sequence to set contains more than the limited number of values.
        // See the description of multi-valued feature value definitions for details on the maximum number of values
        // allowed for a given type of feature value block.
        if values.len() > self.max_value_count() {
            return Err(Exception::Dom(
                "InvalidAccessError",
                Utf16String::from_utf8(&format!(
                    "CSSFontFeatureValuesMap.set only allows a maximum of {} values for the associated feature",
                    self.max_value_count()
                )),
            ));
        }

        self.parent_rule
            .rule()
            .native_rule()
            .set_font_feature_value(self.kind, &feature_value_name, values.clone());
        self.parent_rule.clear_caches();
        self.map_entries.for_each_live(cx.vm(), |map| {
            set_map_entry(&context_of_map(cx, map), map, &feature_value_name, &values);
        });
        Ok(())
    }
}

impl Maplike for CSSFontFeatureValuesMap {
    fn map_entries(&self, cx: &CallContext) -> Gc<Map> {
        self.map_entries.get_or_create(cx, || {
            let map = Map::create(cx.vm(), cx.realm());
            for (name, values) in self.entries() {
                set_map_entry(cx, map, &name, &values);
            }
            map
        })
    }

    /// NB: Each get() converts the sequence of the entry to a new JS array.
    fn map_value(&self, cx: &CallContext, key: &Utf16String) -> Option<Value> {
        self.entries()
            .into_iter()
            .find(|(name, _)| name == key)
            .map(|(_, values)| create_map_value(cx, &values))
    }
}

impl WritableMaplike for CSSFontFeatureValuesMap {
    fn map_set(&self, _: &CallContext, _: Utf16String, _: Value) {
        unreachable!("the interface declares its own set operation");
    }

    fn map_remove(&self, cx: &CallContext, key: &Utf16String) -> bool {
        let removed = self
            .parent_rule
            .rule()
            .native_rule()
            .remove_font_feature_value(self.kind, key);
        if removed {
            self.parent_rule.clear_caches();
            self.map_entries.for_each_live(cx.vm(), |map| {
                let key = key.clone().to_js(cx).expect("a string converts to a JS value");
                map.map_remove(cx.vm(), key);
            });
        }
        removed
    }

    fn map_clear(&self, cx: &CallContext) {
        self.parent_rule
            .rule()
            .native_rule()
            .clear_font_feature_values(self.kind);
        self.parent_rule.clear_caches();
        self.map_entries.for_each_live(cx.vm(), |map| {
            map.map_clear(cx.vm());
        });
    }
}
