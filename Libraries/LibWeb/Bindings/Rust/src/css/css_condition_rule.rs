/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSSConditionRule, https://drafts.csswg.org/css-conditional-3/#the-cssconditionrule-interface

use std::rc::Rc;

use ak::Utf16String;
use libjs_rust::api::Trace;
use libweb_rust::NativeRule;

use super::css_grouping_rule::CSSGroupingRule;
use super::css_rule::{AnyCSSRule, CSSRule};
use crate::bindings::interface::{Inherits, Wrappable};
use crate::bindings::{CallContext, WebIdlResult};

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CSSConditionRule {
    grouping_rule: CSSGroupingRule,
}

impl Wrappable for CSSConditionRule {}

impl Inherits<CSSGroupingRule> for CSSConditionRule {
    fn parent(&self) -> &CSSGroupingRule {
        &self.grouping_rule
    }
}

impl CSSConditionRule {
    pub(crate) fn new(native_rule: Rc<NativeRule>) -> Self {
        Self {
            grouping_rule: CSSGroupingRule::new(native_rule),
        }
    }

    pub(crate) fn grouping_rule(&self) -> &CSSGroupingRule {
        &self.grouping_rule
    }

    pub(crate) fn rule(&self) -> &CSSRule {
        self.grouping_rule.rule()
    }

    // https://drafts.csswg.org/css-conditional-3/#dom-cssconditionrule-conditiontext
    pub(crate) fn condition_text(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(match self.rule().object() {
            AnyCSSRule::CSSContainerRule(object) => object.serialized_condition_text(),
            AnyCSSRule::CSSMediaRule(object) => object.serialized_condition_text(),
            AnyCSSRule::CSSSupportsRule(object) => object.serialized_condition_text(),
            _ => unreachable!("only these rules are condition rules"),
        })
    }
}
