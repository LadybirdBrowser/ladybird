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
use std::cell::RefCell;
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
pub struct DeclarationBlockData {
    pub(crate) properties: Vec<DeclaredProperty>,
    pub(crate) custom_properties: Vec<CustomProperty>,
    custom_property_references: std::sync::OnceLock<(Vec<Vec<u16>>, bool)>,
}

/// Visit cached custom-property reads without creating declaration owners or C++ value views.
///
/// # Safety
/// The callback must accept the context and borrowed UTF-16 names for the duration of the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_declaration_data_visit_custom_property_references(
    data: &DeclarationBlockData,
    context: *mut c_void,
    visit: unsafe extern "C" fn(*mut c_void, *const u16, usize),
) -> bool {
    let (names, complete) = data.custom_property_references();
    for name in names {
        unsafe { visit(context, name.as_ptr(), name.len()) };
    }
    *complete
}

/// Facts used by style-sharing keys, read directly from the immutable declaration storage.
#[derive(Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct FfiDeclarationBlockDependencies {
    pub has_custom_properties: bool,
    pub has_unresolved_values: bool,
    pub has_custom_functions: bool,
    pub reads_style_scope: bool,
    pub declares_animation_name: bool,
}

const _: () = {
    const fn assert_send_sync<T: Send + Sync>() {}
    assert_send_sync::<DeclarationBlockData>();
};

impl DeclarationBlockData {
    fn custom_property_references(&self) -> &(Vec<Vec<u16>>, bool) {
        self.custom_property_references.get_or_init(|| {
            let mut names = Vec::new();
            let mut complete = true;
            for value in self.properties.iter().map(|property| &*property.value).chain(
                self.custom_properties
                    .iter()
                    .map(|property| &*property.declaration.value),
            ) {
                if matches!(value, StyleValueData::Unresolved { presence_var: true, .. }) {
                    let (references, visible) = crate::css::style_value::custom_property_references(value).unwrap();
                    names.extend(references);
                    complete &= visible;
                }
            }
            use crate::css::style_compute::keyword;
            for property in &self.custom_properties {
                if matches!(
                    &*property.declaration.value,
                    StyleValueData::Keyword {
                        keyword: keyword::INHERIT | keyword::UNSET | keyword::REVERT | keyword::REVERT_LAYER
                    }
                ) {
                    names.push(property.name.units().to_vec());
                }
            }
            names.sort_unstable();
            names.dedup();
            (names, complete)
        })
    }

    fn dependencies(&self) -> FfiDeclarationBlockDependencies {
        use crate::css::property_metadata::property_id;
        let mut dependencies = FfiDeclarationBlockDependencies {
            has_custom_properties: !self.custom_properties.is_empty(),
            ..Default::default()
        };
        for property in &self.properties {
            let unresolved = matches!(&*property.value, StyleValueData::Unresolved { .. });
            dependencies.has_unresolved_values |= unresolved;
            dependencies.has_custom_functions |= matches!(
                &*property.value,
                StyleValueData::Unresolved {
                    presence_dashed_function: true,
                    ..
                }
            );
            dependencies.reads_style_scope |= unresolved
                || matches!(
                    property.property_id,
                    property_id::CONTENT | property_id::LIST_STYLE_TYPE
                );
            dependencies.declares_animation_name |= property.property_id == property_id::ANIMATION_NAME;
        }
        for custom_property in &self.custom_properties {
            dependencies.has_custom_functions |= matches!(
                &*custom_property.declaration.value,
                StyleValueData::Unresolved {
                    presence_dashed_function: true,
                    ..
                }
            );
        }
        dependencies
    }

    pub(crate) fn external_memory_size(&self) -> usize {
        let mut size = size_of::<Self>()
            .saturating_add(self.properties.capacity().saturating_mul(size_of::<DeclaredProperty>()))
            .saturating_add(
                self.custom_properties
                    .capacity()
                    .saturating_mul(size_of::<CustomProperty>()),
            );
        for property in &self.custom_properties {
            size = size.saturating_add(size_of_val(property.name.units()));
        }
        if let Some((names, _)) = self.custom_property_references.get() {
            size = size.saturating_add(names.capacity().saturating_mul(size_of::<Vec<u16>>()));
            for name in names {
                size = size.saturating_add(name.capacity().saturating_mul(size_of::<u16>()));
            }
        }
        size
    }

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

struct DeclarationBlockOwner {
    data: Arc<DeclarationBlockData>,
    identity: u64,
    revision: u64,
}

static NEXT_DECLARATION_BLOCK_IDENTITY: AtomicU64 = AtomicU64::new(1);

#[cfg(test)]
thread_local! {
    pub(crate) static DECLARATION_OWNER_ALLOCATIONS: std::cell::Cell<u64> = const { std::cell::Cell::new(0) };
}

enum DeclarationBlockState {
    Immutable(Arc<DeclarationBlockData>),
    Mutable(Rc<RefCell<DeclarationBlockOwner>>),
}

// Native reads use immutable data without an owner allocation. Retaining a live handle
// or editing a block promotes its document-thread owner.
// Shared blocks start independently from the current immutable data.
pub struct DeclarationBlock {
    state: RefCell<DeclarationBlockState>,
}

impl Clone for DeclarationBlock {
    fn clone(&self) -> Self {
        Self {
            state: RefCell::new(DeclarationBlockState::Mutable(self.owner())),
        }
    }
}

impl DeclarationBlock {
    pub(crate) fn new(data: Arc<DeclarationBlockData>) -> Self {
        Self {
            state: RefCell::new(DeclarationBlockState::Immutable(data)),
        }
    }

    fn owner(&self) -> Rc<RefCell<DeclarationBlockOwner>> {
        let mut state = self.state.borrow_mut();
        if let DeclarationBlockState::Immutable(data) = &*state {
            #[cfg(test)]
            DECLARATION_OWNER_ALLOCATIONS.with(|count| count.set(count.get().checked_add(1).unwrap()));
            *state = DeclarationBlockState::Mutable(Rc::new(RefCell::new(DeclarationBlockOwner {
                data: data.clone(),
                identity: NEXT_DECLARATION_BLOCK_IDENTITY
                    .fetch_update(Ordering::Relaxed, Ordering::Relaxed, |value| value.checked_add(1))
                    .expect("declaration block identity overflow"),
                revision: 0,
            })));
        }
        let DeclarationBlockState::Mutable(owner) = &*state else {
            unreachable!()
        };
        owner.clone()
    }

    pub(crate) fn data(&self) -> Arc<DeclarationBlockData> {
        match &*self.state.borrow() {
            DeclarationBlockState::Immutable(data) => data.clone(),
            DeclarationBlockState::Mutable(owner) => owner.borrow().data.clone(),
        }
    }

    #[cfg(test)]
    pub(crate) fn is_immutable(&self) -> bool {
        matches!(&*self.state.borrow(), DeclarationBlockState::Immutable(_))
    }

    pub(crate) fn identity(&self) -> u64 {
        self.owner().borrow().identity
    }

    pub(crate) fn replace(&mut self, source: &Self) {
        let data = source.data();
        let owner = self.owner();
        let mut owner = owner.borrow_mut();
        owner.data = data;
        owner.revision = owner
            .revision
            .checked_add(1)
            .expect("declaration block revision overflow");
    }

    pub(crate) fn remove(&mut self, property_id: u16) -> bool {
        let Some(index) = self
            .data()
            .properties
            .iter()
            .position(|property| property.property_id == property_id)
        else {
            return false;
        };
        self.mutate(|data| {
            data.properties.remove(index);
            true
        })
    }

    fn mutate(&mut self, mutation: impl FnOnce(&mut DeclarationBlockData) -> bool) -> bool {
        let owner = self.owner();
        let mut owner = owner.borrow_mut();
        let data = Arc::make_mut(&mut owner.data);
        data.custom_property_references.take();
        if !mutation(data) {
            return false;
        }
        owner.revision = owner
            .revision
            .checked_add(1)
            .expect("declaration block revision overflow");
        true
    }
}

// Resource registration belongs to stylesheet attachment on the document thread,
// not parsing. Keep the traversal native and expose only image-bearing values.
pub(crate) fn visit_declaration_images(data: &DeclarationBlockData, visit: &mut impl FnMut(&StyleValueData)) {
    fn visit_images(value: &StyleValueData, visit: &mut impl FnMut(&StyleValueData)) {
        match value {
            // Leave image-set type filtering to the document's image decoder support.
            StyleValueData::Image { .. } | StyleValueData::ImageSet { .. } => visit(value),
            StyleValueData::ValueList { values, .. } | StyleValueData::Shorthand { values, .. } => {
                for value in values.as_slice() {
                    visit_images(value.data(), visit);
                }
            }
            StyleValueData::Content { content, alt_text } => {
                visit_images(content.data(), visit);
                if let Some(alt_text) = alt_text.optional_data() {
                    visit_images(alt_text, visit);
                }
            }
            _ => {}
        }
    }
    for property in &data.properties {
        visit_images(&property.value, visit);
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_declaration_block_is_empty(block: *const DeclarationBlock) -> bool {
    let block = unsafe { &*block };
    let data = block.data();
    data.properties.is_empty() && data.custom_properties.is_empty()
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_declaration_block_identity(block: &DeclarationBlock) -> u64 {
    block.identity()
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_declaration_block_revision(block: &DeclarationBlock) -> u64 {
    match &*block.state.borrow() {
        DeclarationBlockState::Immutable(_) => 0,
        DeclarationBlockState::Mutable(owner) => owner.borrow().revision,
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_declaration_block_retain(block: &DeclarationBlock) -> *mut DeclarationBlock {
    Box::into_raw(Box::new(block.clone()))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_declaration_block_replace(block: &mut DeclarationBlock, source: &DeclarationBlock) {
    block.replace(source);
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
) -> *mut DeclarationBlock {
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
    Box::into_raw(Box::new(DeclarationBlock::new(Arc::new(data))))
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_declaration_block_share(block: &DeclarationBlock) -> *mut DeclarationBlock {
    Box::into_raw(Box::new(DeclarationBlock::new(block.data())))
}

/// Pin the current declarations without creating a mutable owner or property views.
#[unsafe(no_mangle)]
pub extern "C" fn rust_declaration_block_snapshot(block: &DeclarationBlock) -> *const DeclarationBlockData {
    Arc::into_raw(block.data())
}

/// Retain an immutable declaration snapshot without creating a live declaration owner.
///
/// # Safety
/// `data` must be a live, Arc-owned declaration snapshot.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_declaration_data_retain(
    data: *const DeclarationBlockData,
) -> *const DeclarationBlockData {
    unsafe {
        Arc::increment_strong_count(data);
    }
    data
}

/// Release an immutable declaration snapshot returned by the style engine or retained from a view.
///
/// # Safety
/// `data` must be null or own an Arc reference to declaration data.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_declaration_data_release(data: *const DeclarationBlockData) {
    if !data.is_null() {
        drop(unsafe { Arc::from_raw(data) });
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_declaration_data_dependencies(data: &DeclarationBlockData) -> FfiDeclarationBlockDependencies {
    data.dependencies()
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_declaration_block_dependencies(block: &DeclarationBlock) -> FfiDeclarationBlockDependencies {
    match &*block.state.borrow() {
        DeclarationBlockState::Immutable(data) => data.dependencies(),
        DeclarationBlockState::Mutable(owner) => owner.borrow().data.dependencies(),
    }
}

/// Visit borrowed declaration values without materializing mutable owners or FFI arrays.
#[unsafe(no_mangle)]
pub extern "C" fn rust_declaration_data_visit(
    data: &DeclarationBlockData,
    context: *mut c_void,
    visit: extern "C" fn(*mut c_void, &FfiDeclaredProperty),
) {
    for property in &data.properties {
        visit(context, &property.view(FfiUtf16View::default()));
    }
    for property in &data.custom_properties {
        visit(
            context,
            &property.declaration.view(FfiUtf16View {
                utf16: property.name.units().as_ptr(),
                length: property.name.units().len(),
                ..Default::default()
            }),
        );
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_declaration_block_destroy(block: *mut DeclarationBlock) {
    if !block.is_null() {
        drop(unsafe { Box::from_raw(block) });
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_declaration_block_visit(
    block: &DeclarationBlock,
    context: *mut c_void,
    visit: extern "C" fn(*mut c_void, &FfiDeclaredProperty),
) {
    rust_declaration_data_visit(&block.data(), context, visit);
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_declaration_block_set(
    block: &mut DeclarationBlock,
    property: &FfiDeclaredProperty,
) -> bool {
    let declaration = unsafe { declaration_from_view(property) };
    block.mutate(|data| data.set_declaration(declaration))
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_declaration_block_append(block: &mut DeclarationBlock, property: &FfiDeclaredProperty) {
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
    block: &mut DeclarationBlock,
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
pub unsafe extern "C" fn rust_declaration_block_remove(block: &mut DeclarationBlock, property_id: u16) -> bool {
    block.remove(property_id)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_declaration_block_remove_custom(
    block: &mut DeclarationBlock,
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
pub unsafe extern "C" fn rust_declaration_block_external_memory_size(block: &DeclarationBlock) -> usize {
    let data = block.data();
    let mut size = size_of::<DeclarationBlock>().saturating_add(data.external_memory_size());
    if matches!(*block.state.borrow(), DeclarationBlockState::Mutable(_)) {
        size = size.saturating_add(size_of::<DeclarationBlockOwner>());
    }
    size
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::css::parser::syntax_parser::parse_shared_stylesheet;
    use crate::css::parser::value_parser::ParseContext;
    use crate::css::property_metadata::property_id;
    use crate::css::rule::{rust_rule_children, rust_rule_list_at, rust_rule_payload};

    fn parsed_declarations(text: &str) -> Arc<DeclarationBlockData> {
        let units: Vec<_> = format!(".target {{ {text} }}").encode_utf16().collect();
        let input = crate::css::css_tokenizer::TokenizerInput::Utf16(&units);
        // All context fields are booleans, integers, or nullable pointers.
        let context: ParseContext = unsafe { std::mem::zeroed() };
        let parse = unsafe { parse_shared_stylesheet(input, &context) };
        let rules = crate::css::rule::NativeRuleList::from_parsed(parse);
        let rule = unsafe { &*rust_rule_list_at(&rules, 0) };
        rule.cascade_declarations().unwrap()
    }

    #[test]
    fn cached_custom_property_reads_follow_copy_on_write_mutation() {
        let data = parsed_declarations("width: var(--幅); --色: inherit");
        let expected = ["--幅", "--色"].map(|name| name.encode_utf16().collect::<Vec<_>>());
        assert_eq!(data.custom_property_references(), &(expected.to_vec(), true));
        let mut block = DeclarationBlock::new(data.clone());
        assert!(block.remove(property_id::WIDTH));
        assert_eq!(
            block.data().custom_property_references(),
            &(vec![expected[1].clone()], true)
        );
        assert_eq!(data.custom_property_references(), &(expected.to_vec(), true));
    }

    #[test]
    fn declaration_snapshots_can_be_forked_and_mutated_on_a_worker() {
        let data = parsed_declarations("width: 13px; --色: green");
        let snapshot = data.clone();
        std::thread::spawn(move || {
            let mut block = DeclarationBlock::new(data);
            let property = &block.data().properties[0];
            let mut view = property.view(FfiUtf16View::default());
            view.important = true;
            assert!(unsafe { rust_declaration_block_set(&mut block, &view) });
            let name = block.data().custom_properties[0].name.clone();
            assert!(unsafe {
                rust_declaration_block_remove_custom(
                    &mut block,
                    FfiUtf16View {
                        utf16: name.units().as_ptr(),
                        length: name.units().len(),
                        ..Default::default()
                    },
                )
            });
            assert!(block.data().properties[0].important);
            assert!(block.data().custom_properties.is_empty());
            assert_eq!(crate::css::ffi_stats::CPP_CALLBACK_COUNT.get(), 0);
        })
        .join()
        .unwrap();
        assert!(!snapshot.properties[0].important);
        assert_eq!(snapshot.custom_properties.len(), 1);
    }

    #[test]
    fn inline_cascade_snapshots_borrow_values_without_views_and_survive_mutation() {
        let mut owner = DeclarationBlock::new(parsed_declarations("width: var(--幅); --幅: 30px; color: red"));
        let owners = DECLARATION_OWNER_ALLOCATIONS.get();
        let snapshot = unsafe { Arc::from_raw(rust_declaration_block_snapshot(&owner)) };
        assert_eq!(DECLARATION_OWNER_ALLOCATIONS.get(), owners);
        let cascade = crate::css::cascaded_properties::FfiCascadeBlock {
            is_inline_style: true,
            native_declarations: Arc::as_ptr(&snapshot),
            ..unsafe { std::mem::zeroed() }
        };
        let values: Vec<_> = cascade
            .declarations()
            .map(|property| {
                assert!(!property.has_style_sheet_context);
                property.data
            })
            .collect();
        assert_eq!(
            values,
            snapshot
                .properties
                .iter()
                .map(|property| Arc::as_ptr(&property.value).cast())
                .collect::<Vec<_>>()
        );
        let replacement = DeclarationBlock::new(parsed_declarations("width: 40px; --幅: 50px; color: blue"));
        rust_declaration_block_replace(&mut owner, &replacement);
        assert!(!Arc::ptr_eq(&owner.data(), &snapshot));
        assert_eq!(
            values,
            cascade.declarations().map(|property| property.data).collect::<Vec<_>>()
        );
    }

    #[test]
    fn dependency_reads_borrow_native_declarations_without_owners_or_views() {
        for (text, expected) in [
            ("width: 13px", FfiDeclarationBlockDependencies::default()),
            (
                "--色: red",
                FfiDeclarationBlockDependencies {
                    has_custom_properties: true,
                    ..Default::default()
                },
            ),
            (
                "width: var(--幅)",
                FfiDeclarationBlockDependencies {
                    has_unresolved_values: true,
                    reads_style_scope: true,
                    ..Default::default()
                },
            ),
            (
                "content: '文字'; list-style-type: disc",
                FfiDeclarationBlockDependencies {
                    reads_style_scope: true,
                    ..Default::default()
                },
            ),
            (
                "animation: movement 1s",
                FfiDeclarationBlockDependencies {
                    declares_animation_name: true,
                    ..Default::default()
                },
            ),
        ] {
            let data = parsed_declarations(text);
            let block = DeclarationBlock::new(data.clone());
            let owners = DECLARATION_OWNER_ALLOCATIONS.with(|count| count.get());
            let references = Arc::strong_count(&data);
            assert_eq!(rust_declaration_data_dependencies(&data), expected, "{text}");
            assert_eq!(rust_declaration_block_dependencies(&block), expected, "{text}");
            // Zero is Author; the other fields are integers, booleans, and nullable pointers.
            let cascade = crate::css::cascaded_properties::FfiCascadeBlock {
                native_declarations: Arc::as_ptr(&data),
                ..unsafe { std::mem::zeroed() }
            };
            assert_eq!(cascade.declarations().count(), data.properties.len());
            for (declaration, property) in cascade.declarations().zip(&data.properties) {
                assert_eq!(declaration.data, Arc::as_ptr(&property.value).cast());
                assert_eq!(declaration.property_id, property.property_id);
                assert_eq!(declaration.important, property.important);
            }
            assert_eq!(Arc::strong_count(&data), references);
            assert_eq!(DECLARATION_OWNER_ALLOCATIONS.with(|count| count.get()), owners);
            assert!(matches!(*block.state.borrow(), DeclarationBlockState::Immutable(_)));
        }
    }

    #[test]
    fn dependency_reads_follow_mutation_without_changing_shared_snapshots() {
        let data = parsed_declarations("width: var(--幅); animation-name: movement; --幅: 13px");
        let original = DeclarationBlock::new(data.clone());
        let mut fork = unsafe { Box::from_raw(rust_declaration_block_share(&original)) };
        let expected = rust_declaration_data_dependencies(&data);
        assert!(unsafe { rust_declaration_block_remove(&mut fork, property_id::WIDTH) });
        assert!(unsafe { rust_declaration_block_remove(&mut fork, property_id::ANIMATION_NAME) });
        let mut retained = unsafe { Box::from_raw(rust_declaration_block_retain(&fork)) };
        let owners = DECLARATION_OWNER_ALLOCATIONS.with(|count| count.get());
        assert_eq!(
            rust_declaration_block_dependencies(&retained),
            FfiDeclarationBlockDependencies {
                has_custom_properties: true,
                ..Default::default()
            }
        );
        let name: Vec<_> = "--幅".encode_utf16().collect();
        assert!(unsafe {
            rust_declaration_block_remove_custom(
                &mut retained,
                FfiUtf16View {
                    utf16: name.as_ptr(),
                    length: name.len(),
                    ..Default::default()
                },
            )
        });
        assert_eq!(
            rust_declaration_block_dependencies(&fork),
            FfiDeclarationBlockDependencies::default()
        );
        assert_eq!(rust_declaration_block_dependencies(&original), expected);
        assert_eq!(rust_declaration_data_dependencies(&data), expected);
        assert_eq!(DECLARATION_OWNER_ALLOCATIONS.with(|count| count.get()), owners);
    }

    #[test]
    fn native_nested_declarations_promote_only_for_live_handles_or_edits() {
        let units: Vec<_> = ".親 { .子 {} width: 13px; --幅: 19px; }".encode_utf16().collect();
        let input = crate::css::css_tokenizer::TokenizerInput::Utf16(&units);
        // All context fields are booleans, integers, or nullable pointers.
        let context: ParseContext = unsafe { std::mem::zeroed() };
        let parse = unsafe { parse_shared_stylesheet(input, &context) };
        let rules = crate::css::rule::NativeRuleList::from_parsed(parse);
        let rule = unsafe { &*rust_rule_list_at(&rules, 0) };
        let children = unsafe { &*rust_rule_children(rule) };
        let nested = unsafe { &*rust_rule_list_at(children, 1) };
        let block = unsafe { &*rust_rule_payload(nested).declarations };
        let snapshot = nested.cascade_declarations().unwrap();
        let old_data = Arc::downgrade(&snapshot);
        let borrowed = block.data();
        assert_eq!(borrowed.properties.len(), 1);
        assert_eq!(borrowed.custom_properties.len(), 1);
        assert!(!unsafe { rust_declaration_block_is_empty(block) });
        assert_eq!(rust_declaration_block_revision(block), 0);
        assert!(unsafe { rust_declaration_block_external_memory_size(block) } > 0);
        let mut fork = unsafe { Box::from_raw(rust_declaration_block_share(block)) };
        assert!(!unsafe { rust_declaration_block_remove(&mut fork, property_id::HEIGHT) });
        assert!(matches!(*block.state.borrow(), DeclarationBlockState::Immutable(_)));
        assert!(matches!(*fork.state.borrow(), DeclarationBlockState::Immutable(_)));
        assert!(Arc::ptr_eq(&snapshot, &fork.data()));

        let mut retained = unsafe { Box::from_raw(rust_declaration_block_retain(block)) };
        assert!(Rc::ptr_eq(&block.owner(), &retained.owner()));
        assert!(unsafe { rust_declaration_block_remove(&mut retained, property_id::WIDTH) });
        assert!(block.data().properties.is_empty());
        assert_eq!(rust_declaration_block_revision(block), 1);
        // A different handle's mutation cannot invalidate a pinned snapshot.
        assert_eq!(borrowed.properties[0].property_id, property_id::WIDTH);
        assert_eq!(
            borrowed.custom_properties[0].name.units(),
            "--幅".encode_utf16().collect::<Vec<_>>()
        );
        assert_eq!(fork.data().properties.len(), 1);
        rust_declaration_block_replace(&mut retained, &fork);
        assert_eq!(block.data().properties.len(), 1);
        assert_eq!(rust_declaration_block_revision(block), 2);
        assert!(matches!(*fork.state.borrow(), DeclarationBlockState::Immutable(_)));
        drop(rules);
        assert!(unsafe { rust_declaration_block_remove(&mut fork, property_id::WIDTH) });
        assert!(matches!(*fork.state.borrow(), DeclarationBlockState::Mutable(_)));
        assert!(fork.data().properties.is_empty());
        assert_eq!(retained.data().properties.len(), 1);
        assert_eq!(snapshot.properties.len(), 1);
        drop(retained);
        drop(borrowed);
        drop(snapshot);
        assert!(old_data.upgrade().is_none());
    }
}
