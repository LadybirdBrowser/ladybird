/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! URLSearchParams, https://url.spec.whatwg.org/#interface-urlsearchparams

use core::cell::{self, Cell, RefCell};
use core::ops::Deref;

use ak::{Utf16String, Utf16StringUnits};
use encoding_rs::UTF_8;
use libjs_rust::api::Trace;
use liburl_rust::url::form_urlencoded;

use super::url::URL;
use super::url_input;
use crate::bindings::conversions::UsvString;
use crate::bindings::interface::{Ref, Wrappable};
use crate::bindings::iterable::PairIterable;
use crate::bindings::{CallContext, Exception, WebIdlResult};

/// The union (sequence<sequence<USVString>> or record<USVString, USVString> or USVString) of
/// DOMURL/URLSearchParams.idl, whose members the bindings convert.
pub(crate) enum USVStringSequenceSequenceOrUSVStringUSVStringRecordOrUSVString {
    Sequence(Vec<Vec<UsvString>>),
    Record(Vec<(UsvString, UsvString)>),
    USVString(UsvString),
}

type Init = USVStringSequenceSequenceOrUSVStringUSVStringRecordOrUSVString;

/// A name-value tuple.
pub(crate) type Tuple = (UsvString, UsvString);

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct URLSearchParams {
    // https://url.spec.whatwg.org/#concept-urlsearchparams-list
    #[gc(untraced)]
    list: RefCell<Vec<Tuple>>,
    // https://url.spec.whatwg.org/#concept-urlsearchparams-url-object
    url_object: Cell<Option<Ref<URL>>>,
}

impl Wrappable for URLSearchParams {}

fn code_units(string: &UsvString) -> impl Iterator<Item = u16> + '_ {
    let (ascii, utf16) = match string.units() {
        Utf16StringUnits::Ascii(bytes) => (bytes, &[][..]),
        Utf16StringUnits::Utf16(code_units) => (&[][..], code_units),
    };
    ascii.iter().map(|byte| u16::from(*byte)).chain(utf16.iter().copied())
}

// https://url.spec.whatwg.org/#concept-urlencoded-string-parser
pub(crate) fn parse_query(input: &str) -> Vec<Tuple> {
    form_urlencoded::parse(input.as_bytes())
        .into_iter()
        .map(|(name, value)| (UsvString::from_utf8(&name), UsvString::from_utf8(&value)))
        .collect()
}

impl URLSearchParams {
    pub(crate) fn new(list: Vec<Tuple>, url_object: Option<Ref<URL>>) -> Self {
        Self {
            list: RefCell::new(list),
            url_object: Cell::new(url_object),
        }
    }

    pub(crate) fn set_list(&self, list: Vec<Tuple>) {
        *self.list.borrow_mut() = list;
    }

    // https://url.spec.whatwg.org/#dom-urlsearchparams-urlsearchparams
    pub(crate) fn construct(_: &mut CallContext, init: Init) -> WebIdlResult<Self> {
        // 1. If init is a string and starts with U+003F (?), then remove the first code point from init.
        let init = match init {
            Init::USVString(string) => Init::USVString(string.strip_ascii_prefix(b'?').unwrap_or(string)),
            init => init,
        };

        // 2. Initialize this with init.
        Ok(Self::new(Self::initialize(init)?, None))
    }

    // https://url.spec.whatwg.org/#urlsearchparams-initialize
    fn initialize(init: Init) -> WebIdlResult<Vec<Tuple>> {
        match init {
            // 1. If init is a sequence, then for each innerSequence of init:
            Init::Sequence(sequence) => sequence
                .into_iter()
                .map(|inner_sequence| {
                    // 1. If innerSequence's size is not 2, then throw a TypeError.
                    let [name, value]: [UsvString; 2] = inner_sequence.try_into().map_err(|_| {
                        Exception::TypeError(Utf16String::from_utf8("Expected each pair to contain exactly 2 items"))
                    })?;

                    // 2. Append (innerSequence[0], innerSequence[1]) to query's list.
                    Ok((name, value))
                })
                .collect(),
            // 2. Otherwise, if init is a record, then for each name → value of init, append (name, value) to query's
            //    list.
            Init::Record(record) => Ok(record),
            // 3. Otherwise:
            //     1. Assert: init is a string.
            //     2. Set query's list to the result of parsing init.
            Init::USVString(string) => Ok(parse_query(&string.to_utf8())),
        }
    }

    // https://url.spec.whatwg.org/#concept-urlsearchparams-update
    fn update(&self) {
        // 1. If query's URL object is null, then return.
        let Some(url_object) = self.url_object.get() else {
            return;
        };

        // 2. Let serializedQuery be the serialization of query's list.
        let serialized_query = self.serialize();

        // 3. If serializedQuery is the empty string, then set serializedQuery to null.
        let serialized_query = (!serialized_query.is_empty()).then_some(serialized_query.as_str());

        // 4. Set query's URL object's URL's query to serializedQuery.
        url_object.set_query(serialized_query);
    }

    // https://url.spec.whatwg.org/#concept-urlencoded-serializer
    pub(crate) fn serialize(&self) -> String {
        let list = self.list.borrow();
        form_urlencoded::serialize(
            list.iter()
                .map(|(name, value)| (url_input(name.units()), url_input(value.units()))),
            UTF_8,
        )
    }

    // https://url.spec.whatwg.org/#dom-urlsearchparams-size
    pub(crate) fn size(&self, _: &mut CallContext) -> WebIdlResult<u32> {
        // The size getter steps are to return this's list's size.
        Ok(u32::try_from(self.list.borrow().len()).unwrap_or(u32::MAX))
    }

    // https://url.spec.whatwg.org/#dom-urlsearchparams-append
    pub(crate) fn append(&self, _: &mut CallContext, name: UsvString, value: UsvString) -> WebIdlResult<()> {
        // 1. Append (name, value) to this's list.
        self.list.borrow_mut().push((name, value));

        // 2. Update this.
        self.update();
        Ok(())
    }

    // https://url.spec.whatwg.org/#dom-urlsearchparams-delete
    pub(crate) fn delete(&self, _: &mut CallContext, name: UsvString, value: Option<UsvString>) -> WebIdlResult<()> {
        match value {
            // 1. If value is given, then remove all tuples whose name is name and value is value from this's list.
            Some(value) => self
                .list
                .borrow_mut()
                .retain(|(tuple_name, tuple_value)| *tuple_name != name || *tuple_value != value),
            // 2. Otherwise, remove all tuples whose name is name from this's list.
            None => self.list.borrow_mut().retain(|(tuple_name, _)| *tuple_name != name),
        }

        // 3. Update this.
        self.update();
        Ok(())
    }

    // https://url.spec.whatwg.org/#dom-urlsearchparams-get
    pub(crate) fn get(&self, _: &mut CallContext, name: UsvString) -> WebIdlResult<Option<UsvString>> {
        // The get(name) method steps are to return the value of the first tuple whose name is name in this's list, if
        // there is such a tuple; otherwise null.
        Ok(self
            .list
            .borrow()
            .iter()
            .find(|(tuple_name, _)| *tuple_name == name)
            .map(|(_, tuple_value)| tuple_value.clone()))
    }

    // https://url.spec.whatwg.org/#dom-urlsearchparams-getall
    pub(crate) fn get_all(&self, _: &mut CallContext, name: UsvString) -> WebIdlResult<Vec<UsvString>> {
        // The getAll(name) method steps are to return the values of all tuples whose name is name in this's list, in
        // list order; otherwise the empty sequence.
        Ok(self
            .list
            .borrow()
            .iter()
            .filter(|(tuple_name, _)| *tuple_name == name)
            .map(|(_, tuple_value)| tuple_value.clone())
            .collect())
    }

    // https://url.spec.whatwg.org/#dom-urlsearchparams-has
    pub(crate) fn has(&self, _: &mut CallContext, name: UsvString, value: Option<UsvString>) -> WebIdlResult<bool> {
        let list = self.list.borrow();
        match value {
            // 1. If value is given and there is a tuple whose name is name and value is value in this's list, then
            //    return true.
            Some(value)
                if list
                    .iter()
                    .any(|(tuple_name, tuple_value)| *tuple_name == name && *tuple_value == value) =>
            {
                return Ok(true);
            }
            // 2. If value is not given and there is a tuple whose name is name in this's list, then return true.
            None if list.iter().any(|(tuple_name, _)| *tuple_name == name) => return Ok(true),
            _ => {}
        }

        // 3. Return false.
        Ok(false)
    }

    // https://url.spec.whatwg.org/#dom-urlsearchparams-set
    pub(crate) fn set(&self, _: &mut CallContext, name: UsvString, value: UsvString) -> WebIdlResult<()> {
        {
            let mut list = self.list.borrow_mut();

            // 1. If this's list contains any tuples whose name is name, then set the value of the first such tuple
            //    to value and remove the others.
            if list.iter().any(|(tuple_name, _)| *tuple_name == name) {
                let mut is_first = true;
                list.retain_mut(|(tuple_name, tuple_value)| {
                    if *tuple_name != name {
                        return true;
                    }
                    if is_first {
                        is_first = false;
                        *tuple_value = value.clone();
                        return true;
                    }
                    false
                });
            }
            // 2. Otherwise, append (name, value) to this's list.
            else {
                list.push((name, value));
            }
        }

        // 3. Update this.
        self.update();
        Ok(())
    }

    // https://url.spec.whatwg.org/#dom-urlsearchparams-sort
    pub(crate) fn sort(&self, _: &mut CallContext) -> WebIdlResult<()> {
        // 1. Set this's list to the result of sorting in ascending order this's list, with a being less than b if a's
        //    name is code unit less than b's name.
        self.list
            .borrow_mut()
            .sort_by(|(a_name, _), (b_name, _)| code_units(a_name).cmp(code_units(b_name)));

        // 2. Update this.
        self.update();
        Ok(())
    }

    // https://url.spec.whatwg.org/#urlsearchparams-stringification-behavior
    pub(crate) fn to_string(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        // The stringification behavior steps are to return the serialization of this's list.
        Ok(Utf16String::from_ascii(self.serialize().as_bytes()))
    }
}

impl PairIterable for URLSearchParams {
    type Key = UsvString;
    type Value = UsvString;

    // https://url.spec.whatwg.org/#ref-for-dfn-value-pairs-to-iterate-over
    fn value_pairs_to_iterate_over(&self) -> impl Deref<Target = [(UsvString, UsvString)]> + '_ {
        // The value pairs to iterate over are this's list's tuples with the key being the name and the value being the
        // value.
        cell::Ref::map(self.list.borrow(), Vec::as_slice)
    }
}
