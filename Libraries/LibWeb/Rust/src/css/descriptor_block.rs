/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_string::CssString;
use crate::css::descriptor_metadata::CUSTOM_DESCRIPTOR_ID;
use crate::css::ffi_support::FfiUtf16View;
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

struct DescriptorBlockOwner {
    data: Arc<DescriptorBlockData>,
    revision: u64,
}

struct DescriptorBlockViews {
    _data: Arc<DescriptorBlockData>,
    revision: u64,
    descriptors: Box<[FfiDescriptor]>,
}

// Retained handles observe the same document-thread owner. Independent consumers
// share only immutable data, and each handle keeps its borrowed views alive.
pub struct FfiDescriptorBlock {
    owner: Rc<RefCell<DescriptorBlockOwner>>,
    views: RefCell<Option<DescriptorBlockViews>>,
}

#[repr(C)]
pub struct FfiDescriptor {
    pub name: FfiUtf16View,
    pub id: u8,
    pub value: *const c_void,
}

#[repr(C)]
pub struct FfiDescriptorBlockView {
    pub descriptors: *const FfiDescriptor,
    pub count: usize,
}

impl FfiDescriptorBlock {
    pub(crate) fn new(data: Arc<DescriptorBlockData>) -> Self {
        Self {
            owner: Rc::new(RefCell::new(DescriptorBlockOwner { data, revision: 0 })),
            views: RefCell::new(None),
        }
    }

    fn mutate(&mut self, mutation: impl FnOnce(&mut DescriptorBlockData) -> bool) -> bool {
        self.views.get_mut().take();
        let mut owner = self.owner.borrow_mut();
        if !mutation(Arc::make_mut(&mut owner.data)) {
            return false;
        }
        owner.revision = owner
            .revision
            .checked_add(1)
            .expect("descriptor block revision overflow");
        true
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
pub unsafe extern "C" fn rust_descriptor_block_from_data(data: *const DescriptorBlockData) -> *mut FfiDescriptorBlock {
    assert!(!data.is_null());
    let data = unsafe {
        Arc::increment_strong_count(data);
        Arc::from_raw(data)
    };
    Box::into_raw(Box::new(FfiDescriptorBlock::new(data)))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_descriptor_block_share(block: &FfiDescriptorBlock) -> *mut FfiDescriptorBlock {
    Box::into_raw(Box::new(FfiDescriptorBlock::new(block.owner.borrow().data.clone())))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_descriptor_block_retain(block: &FfiDescriptorBlock) -> *mut FfiDescriptorBlock {
    Box::into_raw(Box::new(FfiDescriptorBlock {
        owner: block.owner.clone(),
        views: RefCell::new(None),
    }))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_descriptor_block_revision(block: &FfiDescriptorBlock) -> u64 {
    block.owner.borrow().revision
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_descriptor_block_replace(block: &mut FfiDescriptorBlock, source: &FfiDescriptorBlock) {
    let data = source.owner.borrow().data.clone();
    block.views.get_mut().take();
    let mut owner = block.owner.borrow_mut();
    owner.data = data;
    owner.revision = owner
        .revision
        .checked_add(1)
        .expect("descriptor block revision overflow");
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_descriptor_block_destroy(block: *mut FfiDescriptorBlock) {
    if !block.is_null() {
        drop(unsafe { Box::from_raw(block) });
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_descriptor_block_length(block: &FfiDescriptorBlock) -> usize {
    block.owner.borrow().data.descriptors.len()
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_descriptor_block_view(block: &FfiDescriptorBlock) -> FfiDescriptorBlockView {
    let owner = block.owner.borrow();
    let mut cached_views = block.views.borrow_mut();
    if cached_views
        .as_ref()
        .is_none_or(|views| views.revision != owner.revision)
    {
        *cached_views = Some(DescriptorBlockViews {
            _data: owner.data.clone(),
            revision: owner.revision,
            descriptors: owner
                .data
                .descriptors
                .iter()
                .map(|descriptor| FfiDescriptor {
                    name: FfiUtf16View {
                        ascii: std::ptr::null(),
                        utf16: descriptor.name.units().as_ptr(),
                        length: descriptor.name.units().len(),
                    },
                    id: descriptor.id,
                    value: Arc::as_ptr(&descriptor.value).cast(),
                })
                .collect(),
        });
    }
    let views = cached_views.as_ref().unwrap();
    FfiDescriptorBlockView {
        descriptors: views.descriptors.as_ptr(),
        count: views.descriptors.len(),
    }
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
        block.owner.borrow().data.descriptors.iter().position(|descriptor| {
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
    let owner = block.owner.borrow();
    let mut size = size_of::<FfiDescriptorBlock>()
        .saturating_add(size_of::<DescriptorBlockOwner>())
        .saturating_add(size_of::<DescriptorBlockData>());
    size = size.saturating_add(
        owner
            .data
            .descriptors
            .capacity()
            .saturating_mul(size_of::<DescriptorData>()),
    );
    for descriptor in &owner.data.descriptors {
        size = size.saturating_add(descriptor.name.units().len().saturating_mul(size_of::<u16>()));
    }
    if let Some(views) = block.views.borrow().as_ref() {
        size = size.saturating_add(views.descriptors.len().saturating_mul(size_of::<FfiDescriptor>()));
    }
    size
}
