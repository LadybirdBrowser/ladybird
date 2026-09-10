/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_string::CssString;
use crate::css::descriptor_metadata::CUSTOM_DESCRIPTOR_ID;
use crate::css::ffi_support::FfiUtf16View;
use crate::css::serialize::{
    SerializationMode, StringUnits, TextSink, serialize_an_identifier, serialize_style_value, sink_into_raw,
};
use crate::css::style_value::StyleValueData;
use std::cell::RefCell;
use std::ffi::c_void;
use std::rc::Rc;
use std::sync::Arc;

#[derive(Clone)]
pub(crate) struct DescriptorData {
    pub(crate) name: CssString,
    pub(crate) id: u8,
    pub(crate) value: Arc<StyleValueData>,
}

#[derive(Clone, Default)]
pub(crate) struct DescriptorBlockData {
    pub(crate) descriptors: Vec<DescriptorData>,
}

const _: () = {
    const fn assert_send_sync<T: Send + Sync>() {}
    assert_send_sync::<DescriptorBlockData>();
};

type DescriptorBlockOwner = Arc<DescriptorBlockData>;

#[cfg(test)]
thread_local! {
    pub(crate) static DESCRIPTOR_OWNER_ALLOCATIONS: std::cell::Cell<u64> = const { std::cell::Cell::new(0) };
}

enum DescriptorBlockState {
    Immutable(Arc<DescriptorBlockData>),
    Mutable(Rc<RefCell<DescriptorBlockOwner>>),
}

// Native readers share immutable data without creating a mutable owner. Retained
// handles promote the same document-thread owner.
pub struct FfiDescriptorBlock {
    state: RefCell<DescriptorBlockState>,
}

#[repr(C)]
pub struct FfiDescriptor {
    pub name: FfiUtf16View,
    pub id: u8,
    pub value: *const c_void,
}

impl DescriptorData {
    #[cfg(test)]
    pub(crate) fn view(&self) -> FfiDescriptor {
        FfiDescriptor {
            name: FfiUtf16View {
                utf16: self.name.units().as_ptr(),
                length: self.name.units().len(),
                ..Default::default()
            },
            id: self.id,
            value: Arc::as_ptr(&self.value).cast(),
        }
    }
}

impl FfiDescriptorBlock {
    pub(crate) fn data(&self) -> Arc<DescriptorBlockData> {
        match &*self.state.borrow() {
            DescriptorBlockState::Immutable(data) => data.clone(),
            DescriptorBlockState::Mutable(owner) => owner.borrow().clone(),
        }
    }

    pub(crate) fn new(data: Arc<DescriptorBlockData>) -> Self {
        Self {
            state: RefCell::new(DescriptorBlockState::Immutable(data)),
        }
    }

    fn owner(&self) -> Rc<RefCell<DescriptorBlockOwner>> {
        let mut state = self.state.borrow_mut();
        if let DescriptorBlockState::Immutable(data) = &*state {
            #[cfg(test)]
            DESCRIPTOR_OWNER_ALLOCATIONS.with(|count| count.set(count.get().checked_add(1).unwrap()));
            *state = DescriptorBlockState::Mutable(Rc::new(RefCell::new(data.clone())));
        }
        let DescriptorBlockState::Mutable(owner) = &*state else {
            unreachable!()
        };
        owner.clone()
    }

    fn mutate(&mut self, mutation: impl FnOnce(&mut DescriptorBlockData) -> bool) -> bool {
        let owner = self.owner();
        let mut owner = owner.borrow_mut();
        mutation(Arc::make_mut(&mut owner))
    }
}

unsafe fn descriptor_from_view(view: &FfiDescriptor) -> DescriptorData {
    let name = CssString::from_utf16(&unsafe { view.name.to_utf16() }.unwrap());
    assert!(!view.value.is_null());
    let value = view.value.cast::<StyleValueData>();
    let value = unsafe {
        Arc::increment_strong_count(value);
        Arc::from_raw(value)
    };
    DescriptorData {
        name,
        id: view.id,
        value,
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_descriptor_block_create(
    descriptors: *const FfiDescriptor,
    count: usize,
) -> *mut FfiDescriptorBlock {
    let descriptors = (0..count)
        .map(|index| unsafe { descriptor_from_view(&*descriptors.add(index)) })
        .collect();
    Box::into_raw(Box::new(FfiDescriptorBlock::new(Arc::new(DescriptorBlockData {
        descriptors,
    }))))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_descriptor_block_share(block: &FfiDescriptorBlock) -> *mut FfiDescriptorBlock {
    Box::into_raw(Box::new(FfiDescriptorBlock::new(block.data())))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_descriptor_block_retain(block: &FfiDescriptorBlock) -> *mut FfiDescriptorBlock {
    Box::into_raw(Box::new(FfiDescriptorBlock {
        state: RefCell::new(DescriptorBlockState::Mutable(block.owner())),
    }))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_descriptor_block_replace(block: &mut FfiDescriptorBlock, source: &FfiDescriptorBlock) {
    let data = source.data();
    let owner = block.owner();
    let mut owner = owner.borrow_mut();
    *owner = data;
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_descriptor_block_destroy(block: *mut FfiDescriptorBlock) {
    if !block.is_null() {
        drop(unsafe { Box::from_raw(block) });
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_descriptor_block_length(block: &FfiDescriptorBlock) -> usize {
    block.data().descriptors.len()
}

// The block owns the returned value until its next mutation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_descriptor_block_get(
    block: &FfiDescriptorBlock,
    id: u8,
    name: FfiUtf16View,
) -> *const c_void {
    let name = unsafe { name.units() }.unwrap();
    block
        .data()
        .descriptors
        .iter()
        .find(|descriptor| {
            descriptor.id == id
                && (id != CUSTOM_DESCRIPTOR_ID
                    || (descriptor.name.units().len() == name.len()
                        && descriptor
                            .name
                            .units()
                            .iter()
                            .enumerate()
                            .all(|(index, unit)| *unit == name.code_unit_at(index))))
        })
        .map_or(std::ptr::null(), |descriptor| Arc::as_ptr(&descriptor.value).cast())
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_descriptor_block_item(block: &FfiDescriptorBlock, index: usize) -> usize {
    let data = block.data();
    let name = data
        .descriptors
        .get(index)
        .map_or(&[][..], |descriptor| descriptor.name.units());
    ak::Utf16String::from_utf16(name).into_raw()
}

// https://drafts.csswg.org/cssom/#serialize-a-css-declaration-block
#[unsafe(no_mangle)]
pub extern "C" fn rust_descriptor_block_serialize(block: &FfiDescriptorBlock, result: &mut usize) -> bool {
    let Some(sink) = serialize_descriptors(&block.data()) else {
        return false;
    };
    *result = sink_into_raw(sink);
    true
}

fn serialize_descriptors(data: &DescriptorBlockData) -> Option<TextSink> {
    let mut sink = TextSink::new();
    for (index, descriptor) in data.descriptors.iter().enumerate() {
        if index != 0 {
            sink.push_ascii(" ");
        }
        // AD-HOC: Descriptors have no shorthand serialization or important flags.
        // Escape names consistently with serialize a CSS declaration.
        serialize_an_identifier(&mut sink, &StringUnits::Utf16(descriptor.name.units()));
        sink.push_ascii(": ");
        let mut value = TextSink::new();
        if !serialize_style_value(&mut value, &descriptor.value, SerializationMode::Normal) {
            return None;
        }
        if !value.is_ascii_whitespace() {
            sink.push_sink(&value);
        }
        sink.push_ascii(";");
    }
    Some(sink)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_descriptor_block_set(block: &mut FfiDescriptorBlock, descriptor: &FfiDescriptor) -> bool {
    let descriptor = unsafe { descriptor_from_view(descriptor) };
    block.mutate(|data| {
        if let Some(index) = data.descriptors.iter().position(|existing| {
            existing.id == descriptor.id && (descriptor.id != CUSTOM_DESCRIPTOR_ID || existing.name == descriptor.name)
        }) {
            if data.descriptors[index].value.as_ref() == descriptor.value.as_ref() {
                return false;
            }
            data.descriptors[index] = descriptor;
        } else {
            data.descriptors.push(descriptor);
        }
        true
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_descriptor_block_remove(
    block: &mut FfiDescriptorBlock,
    id: u8,
    name: FfiUtf16View,
) -> bool {
    let name = unsafe { name.to_utf16() }.unwrap();
    let Some(index) =
        block.data().descriptors.iter().position(|descriptor| {
            descriptor.id == id && (id != CUSTOM_DESCRIPTOR_ID || descriptor.name.units() == name)
        })
    else {
        return false;
    };
    block.mutate(|data| {
        data.descriptors.remove(index);
        true
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_descriptor_block_external_memory_size(block: &FfiDescriptorBlock) -> usize {
    let data = block.data();
    let mut size = size_of::<FfiDescriptorBlock>().saturating_add(size_of::<DescriptorBlockData>());
    if matches!(*block.state.borrow(), DescriptorBlockState::Mutable(_)) {
        size = size.saturating_add(size_of::<DescriptorBlockOwner>());
    }
    size = size.saturating_add(data.descriptors.capacity().saturating_mul(size_of::<DescriptorData>()));
    for descriptor in &data.descriptors {
        size = size.saturating_add(descriptor.name.units().len().saturating_mul(size_of::<u16>()));
    }
    size
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::css::parser::syntax_parser::parse_shared_stylesheet;
    use crate::css::parser::value_parser::ParseContext;
    use crate::css::rule::{rust_rule_list_at, rust_rule_payload};

    #[test]
    fn descriptor_snapshots_can_be_mutated_on_a_worker() {
        let descriptor = |name: &str, value| DescriptorData {
            name: CssString::from_utf16(&name.encode_utf16().collect::<Vec<_>>()),
            id: CUSTOM_DESCRIPTOR_ID,
            value: Arc::new(StyleValueData::Number { value }),
        };
        let data = Arc::new(DescriptorBlockData {
            descriptors: vec![descriptor("--first", 13.0), descriptor("--色", 29.0)],
        });
        let snapshot = data.clone();
        std::thread::spawn(move || {
            let mut block = FfiDescriptorBlock::new(data.clone());
            let mut first = data.descriptors[0].view();
            first.value = Arc::as_ptr(&data.descriptors[1].value).cast();
            assert!(unsafe { rust_descriptor_block_set(&mut block, &first) });
            let second = data.descriptors[1].view();
            assert!(unsafe { rust_descriptor_block_remove(&mut block, second.id, second.name) });
            assert_eq!(block.data().descriptors.len(), 1);
            assert!(Arc::ptr_eq(
                &block.data().descriptors[0].value,
                &data.descriptors[1].value
            ));
            assert_eq!(crate::css::ffi_stats::CPP_CALLBACK_COUNT.get(), 0);
        })
        .join()
        .unwrap();
        assert_eq!(snapshot.descriptors.len(), 2);
        assert!(matches!(
            &*snapshot.descriptors[0].value,
            StyleValueData::Number { value: 13.0 }
        ));
    }

    #[test]
    fn native_descriptors_keep_readers_immutable_and_retained_handles_live() {
        let units: Vec<_> = "@font-face { font-family: 文字; src: url(font.woff); }"
            .encode_utf16()
            .collect();
        let input = crate::css::css_tokenizer::TokenizerInput::Utf16(&units);
        // All context fields are booleans, integers, or nullable pointers.
        let context: ParseContext = unsafe { std::mem::zeroed() };
        let parse = unsafe { parse_shared_stylesheet(input, &context) };
        let rules = crate::css::rule::NativeRuleList::from_parsed(parse);
        let rule = unsafe { &*rust_rule_list_at(&rules, 0) };
        let block = unsafe { &*rust_rule_payload(rule).descriptors };
        let snapshot = block.data();
        let old_data = Arc::downgrade(&snapshot);
        assert_eq!(snapshot.descriptors.len(), 2);
        assert_eq!(rust_descriptor_block_length(block), 2);
        let allocations = DESCRIPTOR_OWNER_ALLOCATIONS.get();
        let descriptor = snapshot.descriptors[0].view();
        let value = unsafe { rust_descriptor_block_get(block, descriptor.id, descriptor.name) };
        assert_eq!(value, descriptor.value);
        assert_eq!(
            serialize_descriptors(&block.data()).unwrap().into_utf16(),
            "font-family: 文字; src: url(\"font.woff\");"
                .encode_utf16()
                .collect::<Vec<_>>()
        );
        assert_eq!(DESCRIPTOR_OWNER_ALLOCATIONS.get(), allocations);
        assert!(matches!(*block.state.borrow(), DescriptorBlockState::Immutable(_)));
        assert!(rust_descriptor_block_external_memory_size(block) > 0);
        let mut fork = unsafe { Box::from_raw(rust_descriptor_block_share(block)) };
        assert!(!unsafe { rust_descriptor_block_remove(&mut fork, CUSTOM_DESCRIPTOR_ID, FfiUtf16View::default()) });
        assert!(matches!(*block.state.borrow(), DescriptorBlockState::Immutable(_)));
        assert!(matches!(*fork.state.borrow(), DescriptorBlockState::Immutable(_)));
        assert!(Arc::ptr_eq(&snapshot, &fork.data()));

        let mut retained = unsafe { Box::from_raw(rust_descriptor_block_retain(block)) };
        assert!(Rc::ptr_eq(&block.owner(), &retained.owner()));
        let descriptor = snapshot.descriptors[0].view();
        assert!(unsafe { rust_descriptor_block_remove(&mut retained, descriptor.id, descriptor.name) });
        assert_eq!(rust_descriptor_block_length(block), 1);
        // The pinned snapshot still owns the removed descriptor.
        assert_eq!(
            unsafe { descriptor.name.to_utf16().unwrap() },
            "font-family".encode_utf16().collect::<Vec<_>>()
        );
        assert_eq!(fork.data().descriptors.len(), 2);
        rust_descriptor_block_replace(&mut retained, &fork);
        assert_eq!(rust_descriptor_block_length(block), 2);
        assert!(matches!(*fork.state.borrow(), DescriptorBlockState::Immutable(_)));
        drop(rules);
        let first = &snapshot.descriptors[0];
        assert!(unsafe { rust_descriptor_block_remove(&mut fork, first.id, FfiUtf16View::default()) });
        assert!(matches!(*fork.state.borrow(), DescriptorBlockState::Mutable(_)));
        assert_eq!(fork.data().descriptors.len(), 1);
        assert_eq!(retained.data().descriptors.len(), 2);
        assert_eq!(snapshot.descriptors.len(), 2);
        drop(retained);
        drop(snapshot);
        assert!(old_data.upgrade().is_none());
    }
}
