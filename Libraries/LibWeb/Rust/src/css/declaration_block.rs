/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_string::CssString;
use crate::css::ffi_support::FfiUtf16View;
use crate::css::property_metadata::{property_is_logical_alias_including_shorthands, property_logical_group};
use crate::css::style_compute::expand_shorthands_with;
use crate::css::style_value::StyleValueData;
use std::cell::{Ref, RefCell};
use std::ffi::c_void;
use std::rc::Rc;
use std::sync::Arc;
use std::sync::atomic::{AtomicU64, Ordering};

#[derive(Clone)]
pub(crate) struct DeclaredProperty {
    pub(crate) property_id: u16,
    pub(crate) important: bool,
    pub(crate) value: Arc<StyleValueData>,
}

#[derive(Clone)]
pub(crate) struct CustomProperty {
    pub(crate) name: CssString,
    pub(crate) declaration: DeclaredProperty,
}

#[derive(Clone, Default)]
pub(crate) struct DeclarationBlockData {
    pub(crate) properties: Vec<DeclaredProperty>,
    pub(crate) custom_properties: Vec<CustomProperty>,
}

const _: () = {
    const fn assert_send_sync<T: Send + Sync>() {}
    assert_send_sync::<DeclarationBlockData>();
};

impl DeclarationBlockData {
    // https://drafts.csswg.org/cssom/#concept-declarations-specified-order
    pub(crate) fn append_in_specified_order(&mut self, declaration: DeclaredProperty) {
        // The specified order for declarations is the same as specified, but with shorthand properties expanded into their
        // longhand properties, in canonical order. If a property is specified more than once (after shorthand expansion), only
        // the one with greatest cascading order must be represented, at the same relative position as it was specified.
        expand_shorthands_with(
            declaration.property_id,
            Arc::as_ptr(&declaration.value).cast(),
            false,
            &mut |property_id, value, _| {
                self.append_normalized_property(DeclaredProperty {
                    property_id,
                    important: declaration.important,
                    value: unsafe { retain_value(value) },
                });
            },
        );
    }

    fn append_normalized_property(&mut self, declaration: DeclaredProperty) {
        if let Some(index) = self
            .properties
            .iter()
            .position(|property| property.property_id == declaration.property_id)
        {
            if self.properties[index].important && !declaration.important {
                return;
            }
            self.properties.remove(index);
        }
        self.properties.push(declaration);
    }

    pub(crate) fn append_block(&mut self, other: &Self) {
        for declaration in &other.properties {
            self.append_normalized_property(declaration.clone());
        }
        for property in &other.custom_properties {
            self.set_custom_property(property.name.clone(), property.declaration.clone());
        }
    }

    pub(crate) fn set_custom_property(&mut self, name: CssString, declaration: DeclaredProperty) {
        if let Some(property) = self.custom_properties.iter_mut().find(|property| property.name == name) {
            property.declaration = declaration;
        } else {
            self.custom_properties.push(CustomProperty { name, declaration });
        }
    }

    // https://drafts.csswg.org/cssom/#set-a-css-declaration
    fn set_declaration(&mut self, declaration: DeclaredProperty) -> bool {
        // NB: This follows the suggested algorithm at https://drafts.csswg.org/cssom/#example-a40690cb.
        // 1. If property is a case-sensitive match for a property name of a CSS declaration in declarations, follow these substeps:
        if let Some(index) = self
            .properties
            .iter()
            .position(|property| property.property_id == declaration.property_id)
        {
            // 1. Let target declaration be such CSS declaration.
            let target_declaration = &self.properties[index];
            // 2. Let needs append be false.
            let mut needs_append = false;
            if let Some(group) = property_logical_group(declaration.property_id) {
                let is_logical = property_is_logical_alias_including_shorthands(declaration.property_id);
                // 3. For each declaration in declarations after target declaration:
                for property in &self.properties[index + 1..] {
                    // 1. If declaration’s property name is not in the same logical property group as property, then continue.
                    if property_logical_group(property.property_id) != Some(group) {
                        continue;
                    }
                    // 2. If declaration’ property name has the same mapping logic as property, then continue.
                    if property_is_logical_alias_including_shorthands(property.property_id) == is_logical {
                        continue;
                    }
                    // 3. Let needs append be true.
                    needs_append = true;
                    // 4. Break.
                    break;
                }
            }
            // 4. If needs append is false, then:
            if !needs_append {
                // 1. Let needs update be false.
                // 2. If target declaration’s value is not equal to component value list, then let needs update be true.
                // 3. If target declaration’s important flag is not equal to whether important flag is set, then let needs update be true.
                let needs_update = target_declaration.value != declaration.value
                    || target_declaration.important != declaration.important;
                // 4. If needs update is false, then return false.
                if !needs_update {
                    return false;
                }
                // 5. Set target declaration’s value to component value list.
                // 6. If important flag is set, then set target declaration’s important flag, otherwise unset it.
                self.properties[index] = declaration;
                // 7. Return true.
                return true;
            }
            // 5. Otherwise, remove target declaration from declarations.
            self.properties.remove(index);
        }
        // 2. Append a new CSS declaration with property name property, value component value list, and important flag set
        //    if important flag is set to declarations.
        self.properties.push(declaration);
        // 3. Return true
        true
    }
}

#[repr(C)]
pub struct FfiDeclaredProperty {
    pub property_id: u16,
    pub important: bool,
    pub value: *const c_void,
    pub name: FfiUtf16View,
}

impl DeclaredProperty {
    fn view(&self, name: FfiUtf16View) -> FfiDeclaredProperty {
        FfiDeclaredProperty {
            property_id: self.property_id,
            important: self.important,
            value: Arc::as_ptr(&self.value).cast(),
            name,
        }
    }
}

struct DeclarationBlockViews {
    _data: Arc<DeclarationBlockData>,
    revision: u64,
    properties: Box<[FfiDeclaredProperty]>,
    custom_properties: Box<[FfiDeclaredProperty]>,
}

struct DeclarationBlockOwner {
    data: Arc<DeclarationBlockData>,
    identity: u64,
    revision: u64,
}

static NEXT_DECLARATION_BLOCK_IDENTITY: AtomicU64 = AtomicU64::new(1);

// Each consumer owns its FFI views. Retained handles observe the same document-thread
// owner, while shared blocks fork an independent owner of the immutable native data.
// Views retain their snapshot until the next view or mutation call on that handle.
pub struct FfiDeclarationBlock {
    owner: Rc<RefCell<DeclarationBlockOwner>>,
    views: RefCell<Option<DeclarationBlockViews>>,
}

impl FfiDeclarationBlock {
    pub(crate) fn new(data: Arc<DeclarationBlockData>) -> Self {
        Self {
            owner: Rc::new(RefCell::new(DeclarationBlockOwner {
                data,
                identity: NEXT_DECLARATION_BLOCK_IDENTITY
                    .fetch_update(Ordering::Relaxed, Ordering::Relaxed, |value| value.checked_add(1))
                    .expect("declaration block identity overflow"),
                revision: 0,
            })),
            views: RefCell::new(None),
        }
    }

    pub(crate) fn data(&self) -> Ref<'_, Arc<DeclarationBlockData>> {
        Ref::map(self.owner.borrow(), |owner| &owner.data)
    }

    fn mutate(&mut self, mutation: impl FnOnce(&mut DeclarationBlockData) -> bool) -> bool {
        self.views.get_mut().take();
        let mut owner = self.owner.borrow_mut();
        if !mutation(Arc::make_mut(&mut owner.data)) {
            return false;
        }
        owner.revision = owner
            .revision
            .checked_add(1)
            .expect("declaration block revision overflow");
        true
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_declaration_block_is_empty(block: *const FfiDeclarationBlock) -> bool {
    let block = unsafe { &*block };
    let data = block.data();
    data.properties.is_empty() && data.custom_properties.is_empty()
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_declaration_block_identity(block: &FfiDeclarationBlock) -> u64 {
    block.owner.borrow().identity
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_declaration_block_revision(block: &FfiDeclarationBlock) -> u64 {
    block.owner.borrow().revision
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_declaration_block_retain(block: &FfiDeclarationBlock) -> *mut FfiDeclarationBlock {
    Box::into_raw(Box::new(FfiDeclarationBlock {
        owner: block.owner.clone(),
        views: RefCell::new(None),
    }))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_declaration_block_replace(block: &mut FfiDeclarationBlock, source: &FfiDeclarationBlock) {
    let data = source.data().clone();
    block.views.get_mut().take();
    let mut owner = block.owner.borrow_mut();
    owner.data = data;
    owner.revision = owner
        .revision
        .checked_add(1)
        .expect("declaration block revision overflow");
}

#[repr(C)]
pub struct FfiDeclarationBlockView {
    pub properties: *const FfiDeclaredProperty,
    pub property_count: usize,
    pub custom_properties: *const FfiDeclaredProperty,
    pub custom_property_count: usize,
}

unsafe fn retain_value(value: *const c_void) -> Arc<StyleValueData> {
    let value = value.cast::<StyleValueData>();
    assert!(!value.is_null());
    unsafe {
        Arc::increment_strong_count(value);
        Arc::from_raw(value)
    }
}

pub(crate) unsafe fn declaration_from_view(property: &FfiDeclaredProperty) -> DeclaredProperty {
    DeclaredProperty {
        property_id: property.property_id,
        important: property.important,
        value: unsafe { retain_value(property.value) },
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_declaration_block_create(
    properties: *const FfiDeclaredProperty,
    property_count: usize,
    custom_properties: *const FfiDeclaredProperty,
    custom_property_count: usize,
) -> *mut FfiDeclarationBlock {
    let mut data = DeclarationBlockData::default();
    for index in 0..property_count {
        data.append_in_specified_order(unsafe { declaration_from_view(&*properties.add(index)) });
    }
    for index in 0..custom_property_count {
        let property = unsafe { &*custom_properties.add(index) };
        data.set_custom_property(
            CssString::from_utf16(&unsafe { property.name.to_utf16() }.unwrap()),
            unsafe { declaration_from_view(property) },
        );
    }
    Box::into_raw(Box::new(FfiDeclarationBlock::new(Arc::new(data))))
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_declaration_block_share(block: &FfiDeclarationBlock) -> *mut FfiDeclarationBlock {
    Box::into_raw(Box::new(FfiDeclarationBlock::new(block.data().clone())))
}

// Retain an immutable block borrowed from a parsed stylesheet, with independent COW ownership.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_declaration_block_from_data(
    data: *const DeclarationBlockData,
) -> *mut FfiDeclarationBlock {
    assert!(!data.is_null());
    let data = unsafe {
        Arc::increment_strong_count(data);
        Arc::from_raw(data)
    };
    Box::into_raw(Box::new(FfiDeclarationBlock::new(data)))
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_declaration_block_destroy(block: *mut FfiDeclarationBlock) {
    if !block.is_null() {
        drop(unsafe { Box::from_raw(block) });
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_declaration_block_view(block: &FfiDeclarationBlock) -> FfiDeclarationBlockView {
    let owner = block.owner.borrow();
    let mut cached_views = block.views.borrow_mut();
    if cached_views
        .as_ref()
        .is_none_or(|views| views.revision != owner.revision)
    {
        *cached_views = Some(DeclarationBlockViews {
            _data: owner.data.clone(),
            revision: owner.revision,
            properties: owner
                .data
                .properties
                .iter()
                .map(|property| property.view(FfiUtf16View::default()))
                .collect(),
            custom_properties: owner
                .data
                .custom_properties
                .iter()
                .map(|property| {
                    property.declaration.view(FfiUtf16View {
                        ascii: std::ptr::null(),
                        utf16: property.name.units().as_ptr(),
                        length: property.name.units().len(),
                    })
                })
                .collect(),
        });
    }
    let views = cached_views.as_ref().unwrap();
    FfiDeclarationBlockView {
        properties: views.properties.as_ptr(),
        property_count: views.properties.len(),
        custom_properties: views.custom_properties.as_ptr(),
        custom_property_count: views.custom_properties.len(),
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_declaration_block_set(
    block: &mut FfiDeclarationBlock,
    property: &FfiDeclaredProperty,
) -> bool {
    let declaration = unsafe { declaration_from_view(property) };
    block.mutate(|data| data.set_declaration(declaration))
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_declaration_block_append(
    block: &mut FfiDeclarationBlock,
    property: &FfiDeclaredProperty,
) {
    let declaration = unsafe { declaration_from_view(property) };
    block.mutate(|data| {
        data.properties
            .retain(|existing| existing.property_id != declaration.property_id);
        data.properties.push(declaration);
        true
    });
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_declaration_block_set_custom(
    block: &mut FfiDeclarationBlock,
    property: &FfiDeclaredProperty,
) {
    let name = CssString::from_utf16(&unsafe { property.name.to_utf16() }.unwrap());
    let declaration = unsafe { declaration_from_view(property) };
    block.mutate(|data| {
        data.set_custom_property(name, declaration);
        true
    });
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_declaration_block_remove(block: &mut FfiDeclarationBlock, property_id: u16) -> bool {
    let Some(index) = block
        .data()
        .properties
        .iter()
        .position(|property| property.property_id == property_id)
    else {
        return false;
    };
    block.mutate(|data| {
        data.properties.remove(index);
        true
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_declaration_block_remove_custom(
    block: &mut FfiDeclarationBlock,
    name: FfiUtf16View,
) -> bool {
    let name = unsafe { name.to_utf16() }.unwrap();
    let Some(index) = block
        .data()
        .custom_properties
        .iter()
        .position(|property| property.name.units() == name)
    else {
        return false;
    };
    block.mutate(|data| {
        data.custom_properties.remove(index);
        true
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_declaration_block_external_memory_size(block: &FfiDeclarationBlock) -> usize {
    let data = block.data();
    let mut size = size_of::<FfiDeclarationBlock>()
        .saturating_add(size_of::<DeclarationBlockOwner>())
        .saturating_add(size_of::<DeclarationBlockData>());
    size = size.saturating_add(data.properties.capacity().saturating_mul(size_of::<DeclaredProperty>()));
    size = size.saturating_add(
        data.custom_properties
            .capacity()
            .saturating_mul(size_of::<CustomProperty>()),
    );
    for property in &data.custom_properties {
        size = size.saturating_add(property.name.units().len().saturating_mul(size_of::<u16>()));
    }
    if let Some(views) = block.views.borrow().as_ref() {
        size = size.saturating_add(
            views
                .properties
                .len()
                .saturating_add(views.custom_properties.len())
                .saturating_mul(size_of::<FfiDeclaredProperty>()),
        );
    }
    size
}
