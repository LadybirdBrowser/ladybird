/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSSMediaRule, https://drafts.csswg.org/cssom/#the-cssmediarule-interface

use super::css_condition_rule::CSSConditionRule;
use super::css_grouping_rule::CSSGroupingRule;
use super::css_rule::impl_rule_wrappable;
use super::css_rule::{CSSRule, RuleText, serialize_block_of_rules};
use super::media_list::MediaList;
use crate::bindings::cpp_cell::CppCell;
use crate::bindings::interface::{Inherits, Ref, allocate};
use crate::bindings::{CallContext, WebIdlResult};
use ak::Utf16String;
use libjs_rust::api::GcRefCell;
use libjs_rust::api::{Trace, Vm};
use libweb_rust::NativeRule;
use std::rc::Rc;

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CSSMediaRule {
    condition_rule: CSSConditionRule,
    media: GcRefCell<Option<Ref<MediaList>>>,
}

impl_rule_wrappable!(CSSMediaRule);

impl Inherits<CSSConditionRule> for CSSMediaRule {
    fn parent(&self) -> &CSSConditionRule {
        &self.condition_rule
    }
}

impl CSSMediaRule {
    pub(crate) fn create(vm: &Vm, native_rule: Rc<NativeRule>) -> Ref<Self> {
        allocate(vm, |_| Self {
            condition_rule: CSSConditionRule::new(native_rule),
            media: GcRefCell::new(None),
        })
    }

    pub(crate) fn rule(&self) -> &CSSRule {
        self.condition_rule.rule()
    }

    pub(crate) fn grouping_rule(&self) -> &CSSGroupingRule {
        self.condition_rule.grouping_rule()
    }

    // https://drafts.csswg.org/cssom/#dom-cssmediarule-media
    pub(crate) fn media(&self, cx: &mut CallContext) -> WebIdlResult<Ref<MediaList>> {
        if let Some(media) = *self.media.borrow() {
            return Ok(media);
        }
        let vm = cx.vm();
        let native_media = self.rule().native_rule().media_list();
        let media = allocate(vm, |defer_gc| MediaList::new(defer_gc, native_media));
        // SAFETY: The cell is that of this object, which is live.
        media.set_associated_rule(vm, unsafe { CppCell::from_raw(self.rule().object().cell()) });
        *self.media.borrow_mut(vm) = Some(media);
        Ok(media)
    }

    // https://drafts.csswg.org/css-conditional-5/#dom-cssmediarule-matches
    pub(crate) fn matches(&self, _: &mut CallContext) -> WebIdlResult<bool> {
        Ok(self.rule().native_rule().media_list().matches())
    }

    pub(crate) fn serialized_condition_text(&self) -> Utf16String {
        self.rule().native_rule().media_list().media_text()
    }

    // https://drafts.csswg.org/cssom-1/#serialize-a-css-rule
    pub(crate) fn serialized(&self, vm: &Vm) -> Utf16String {
        // The result of concatenating the following:
        // 1. The string "@media", followed by a single SPACE (U+0020).
        let mut text = RuleText::new("@media ");

        // 2. The result of performing serialize a media query list on rule’s media query list.
        text.push(&self.serialized_condition_text());
        serialize_block_of_rules(text, vm, self.grouping_rule().child_rules(vm))
    }
}
