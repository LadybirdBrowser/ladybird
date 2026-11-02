/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The engine's custom property stores: the ones an element's custom declarations resolve to, and
//! what its other declarations substitute against them.

use super::super::custom_property_cascade::{
    PreparedCustomFunctions, SubstitutionAttributes, SubstitutionInputs, engine_resolution_context,
    substitute_against_stores, value_calls_custom_functions, value_reads_attributes, value_reads_conditions,
    value_reads_inherited_values, value_reads_substitution_subject,
};
use super::super::*;
use crate::css::cascaded_properties::resolve_declared_custom_properties;
use crate::css::custom_properties::{CustomPropertyFinalization, CustomPropertyStore};
use crate::css::style_value::RetainedStyleValueData;
use std::ffi::c_void;
use std::sync::Arc;

/// A written value, a longhand and the identities of the values the value's references resolve to.
type ReferencesKey = (usize, u16, smallvec::SmallVec<[usize; 4]>);

/// What a written value substituted to against two stores, with the stores, which the entry keeps
/// alive so that no other store takes the address the key names one of them by.
struct StoresSubstitution {
    substituted: RetainedStyleValueData,
    _stores: [Option<Arc<CustomPropertyStore>>; 2],
}

/// What a style thread substituted each written value to, keyed by the value, the longhand and
/// the stores it substituted against.
#[derive(Default)]
pub(in crate::css::style) struct SubstitutionCache {
    /// By the value, the longhand and the identities of the two stores.
    by_stores: HashMap<(usize, u16, usize, usize), StoresSubstitution>,
    /// By the value, the longhand and the identities of the values the custom properties it
    /// references resolve to, which elements holding stores of their own still share. Every
    /// entry is made alongside one of `by_stores`, whose stores keep those values alive.
    by_references: HashMap<ReferencesKey, RetainedStyleValueData>,
}

impl SubstitutionCache {
    fn insert_by_stores(
        &mut self,
        key: (usize, u16, usize, usize),
        stores: CustomPropertyStores<'_>,
        substituted: RetainedStyleValueData,
    ) {
        self.by_stores.insert(
            key,
            StoresSubstitution {
                substituted,
                _stores: [stores.own.cloned(), stores.inherited.cloned()],
            },
        );
    }
}

/// What the registered custom properties of a style element compute against once it computed:
/// its lengths, place among its siblings and color scheme, and the element whose random caching
/// keys its random functions draw from, with the keys not drawn yet.
struct RegisteredValueContext<'a, 'r> {
    length: &'a crate::css::style_compute::FfiLengthResolutionContext,
    tree_counting: Option<(u64, u64)>,
    color_scheme: u8,
    node: StyleNodeID,
    random_requests: &'a std::cell::RefCell<&'r mut Vec<super::random_bases::RandomBaseRequest>>,
    /// Whether a random function drew a base value.
    drew_random_base_values: &'a std::cell::Cell<bool>,
}

/// One custom property an element declares, as a store is cascaded from it: the name's identity and
/// text, its importance, and the declared value.
type DeclaredCustomProperty = (usize, bool, *const c_void);

/// The stores a style thread resolved, keyed by the stores the parent hands down and holds and
/// the declarations cascaded over them, each with the parent's stores kept alive so no key's
/// address is reused.
pub(in crate::css::style) type StoreCache = HashMap<(usize, usize, Vec<(usize, bool, usize)>), CachedStore>;

/// A store a style thread resolved, after the stores of the parent it resolved over.
type CachedStore = ([Option<Arc<CustomPropertyStore>>; 2], Option<Arc<CustomPropertyStore>>);

/// The custom-property stores a style element substitutes against: its own and the one it
/// inherits.
#[derive(Clone, Copy)]
pub(in crate::css::style) struct CustomPropertyStores<'a> {
    pub(in crate::css::style) own: Option<&'a Arc<CustomPropertyStore>>,
    pub(in crate::css::style) inherited: Option<&'a Arc<CustomPropertyStore>>,
    pub(in crate::css::style) substitutions: &'a std::cell::RefCell<SubstitutionCache>,
    pub(in crate::css::style) subject: &'a dyn SubstitutingSubject,
}

/// What a substitution reads of the element or pseudo-element it substitutes for,
/// beyond its custom property stores: the attributes an `attr()` reads, what the conditions of an
/// `if()` compute against, and the custom functions a call reaches.
pub(in crate::css::style) trait SubstitutingSubject {
    /// The element, and the kind of its pseudo-element the substitution is for, if any.
    fn target(&self) -> (StyleNodeID, Option<u8>);

    /// What the conditions of an `if()` compute against.
    fn style_query_inputs(&self) -> Option<crate::css::cascaded_properties::StyleQueryInputs>;

    /// The custom functions the element's calls reach, with the declarations their conditions
    /// select for it.
    fn custom_functions(&self) -> Option<PreparedCustomFunctions<'_>>;

    /// Resolve the container-relative lengths of `length` against the element's query
    /// containers.
    fn apply_container_unit_bases(&self, length: &mut crate::css::style_compute::FfiLengthResolutionContext);
}

use super::records::HeldCustomProperties;

/// What a drive of an element's longhands found of its registered custom properties.
pub(in crate::css::style) struct DrivenWithRegisteredProperties {
    pub(in crate::css::style) computed: super::compute::ComputedElement,
    /// What `registered_custom_property_store` answered.
    pub(in crate::css::style) registered: Option<(HeldCustomProperties, bool)>,
    /// Whether the element's computation read a style of its own it did not hold yet.
    pub(in crate::css::style) computes_again: bool,
}

/// Drives an element's longhands until its registered custom properties have settled, and answers
/// them with the store they computed to, if they did, in place of the one the element substitutes
/// before they did.
///
/// https://drafts.css-houdini.org/css-properties-values-api-1/#calculation-of-computed-values
/// A registered custom property computes against the element's own lengths, which the drive
/// settles, so the longhands substitute its value as parsed, which computes the same against the
/// same lengths. A random function in it draws its base value as it computes, which the longhands
/// substituting it read: they drive again against the computed values.
pub(in crate::css::style) fn drive_with_registered_properties<E>(
    mut drive: impl FnMut(Option<&HeldCustomProperties>) -> Result<DrivenWithRegisteredProperties, E>,
) -> Result<(super::compute::ComputedElement, Option<HeldCustomProperties>), E> {
    let mut computed_store = None;
    loop {
        let driven = drive(computed_store.as_ref())?;
        match driven.registered {
            Some((store, true)) if computed_store.is_none() => computed_store = Some(store),
            _ if driven.computes_again => {}
            Some((store, _)) => return Ok((driven.computed, Some(store))),
            None => return Ok((driven.computed, None)),
        }
    }
}

impl RetainedState {
    /// The engine: the custom-property store of an element whose custom declarations cascaded to
    /// `cascaded`, resolved over the store its parent hands down, with `inherit` reading the
    /// store the parent holds. The parent's store where the element declares nothing new. A
    /// value of a custom property registered with a syntax stays as parsed, since the element's
    /// own lengths are not known yet: see `registered_custom_property_store`. A value reading
    /// `subject` itself, through `attr()`, `if()` or a custom function, makes the store the
    /// element's alone.
    pub(in crate::css::style) fn custom_property_store(
        &self,
        parent_store: Option<&Arc<CustomPropertyStore>>,
        parent_held: Option<&Arc<CustomPropertyStore>>,
        cascaded: &[(CustomDeclaration, RetainedStyleValueData)],
        stores: &mut StoreCache,
        subject: &dyn SubstitutingSubject,
    ) -> Option<Option<Arc<CustomPropertyStore>>> {
        if cascaded.is_empty() {
            return Some(parent_store.cloned());
        }
        let values = self.declared_custom_properties(parent_store, cascaded)?;
        if values.is_empty() {
            return Some(parent_store.cloned());
        }
        // A store holding what the element's environment held before gives way to that environment's store, which the
        // environment is named by.
        let (node, pseudo) = subject.target();
        let previous = |store: Option<Arc<CustomPropertyStore>>| {
            store.map(|store| {
                self.store_resolving_to_the_same_values(node, pseudo, &store)
                    .unwrap_or(store)
            })
        };
        if cascaded
            .iter()
            .any(|(_, value)| value_reads_substitution_subject(value.data()))
        {
            let (store, _) = self.resolve_custom_property_store(parent_store, parent_held, values, None, Some(subject));
            return Some(previous(store));
        }
        // What the store resolves to reads nothing of the element but these declarations and the
        // parent's store, so elements declaring the same values under the same store share one.
        let key = (
            parent_store.map_or(0, |store| Arc::as_ptr(store) as usize),
            parent_held.map_or(0, |store| Arc::as_ptr(store) as usize),
            values
                .iter()
                .map(|(name, important, value)| (*name, *important, *value as usize))
                .collect::<Vec<_>>(),
        );
        if let Some((_, store)) = stores.get(&key) {
            return Some(previous(store.clone()));
        }
        let (resolved, _) = self.resolve_custom_property_store(parent_store, parent_held, values, None, None);
        stores.insert(key, ([parent_store.cloned(), parent_held.cloned()], resolved.clone()));
        Some(previous(resolved))
    }

    /// The engine: the store an element holds once it computed to `computed`, where its custom
    /// declarations `cascaded` name a custom property registered with a syntax: the store
    /// `custom_property_store` resolves, with each such value computed against the element's
    /// own lengths, place among its siblings and color scheme, as its computed value is. What
    /// those values read joins what `computed` reads. Answers the store with whether its values
    /// drew random base values, which the element's longhands substituting them read. None where
    /// they name no such property, and the element holds the store it substituted against.
    #[allow(clippy::too_many_arguments)]
    pub(in crate::css::style) fn registered_custom_property_store(
        &self,
        node: StyleNodeID,
        parent_store: Option<&Arc<CustomPropertyStore>>,
        parent_held: Option<&Arc<CustomPropertyStore>>,
        cascaded: &[(CustomDeclaration, RetainedStyleValueData)],
        computed: &mut super::compute::ComputedElement,
        subject: &dyn SubstitutingSubject,
        random_requests: &mut Vec<super::random_bases::RandomBaseRequest>,
    ) -> Option<(Option<Arc<CustomPropertyStore>>, bool)> {
        let inputs = &self.document_style_computation_inputs;
        let registry = inputs.custom_property_registry();
        if !registry.has_registrations()
            || !cascaded.iter().any(|(declared, _)| {
                self.declared_custom_property_name(declared.name)
                    .is_some_and(|name| name.with_fly_string(|name| registry.has_syntax(name)))
            })
        {
            return None;
        }
        let values = self.declared_custom_properties(parent_store, cascaded)?;
        if values.is_empty() {
            return Some((parent_store.cloned(), false));
        }
        let tree_counting = self
            .element_sibling_position(node)
            .map(|(count, index)| (u64::from(count), u64::from(index)));
        let color_scheme =
            u8::try_from(computed.value_facts.effective_color_scheme()).unwrap_or(inputs.preferred_color_scheme);
        // A container-relative length resolves against the element's query containers, which the
        // element's own lengths read only where its longhands use one.
        let mut length = computed.length;
        subject.apply_container_unit_bases(&mut length);
        let random_requests = std::cell::RefCell::new(random_requests);
        let drew_random_base_values = std::cell::Cell::new(false);
        let (store, reads) = self.resolve_custom_property_store(
            parent_store,
            parent_held,
            values,
            Some(RegisteredValueContext {
                length: &length,
                tree_counting,
                color_scheme,
                node,
                random_requests: &random_requests,
                drew_random_base_values: &drew_random_base_values,
            }),
            cascaded
                .iter()
                .any(|(_, value)| value_reads_substitution_subject(value.data()))
                .then_some(subject),
        );
        // A value reading the element's place among its siblings or its containers is the
        // element's alone.
        computed.reads.reads_sibling_position |= reads.sibling_position;
        computed.reads.container_units |= reads.container_unit_mask;
        computed.reads.reads_root_font = computed.reads.reads_root_font.max(reads.root_font);
        computed.reads.reads_font_metric_lengths |= reads.font_metrics;
        if reads.color_scheme {
            computed.reads.environment_reads |= crate::css::style::input_types::StyleEnvironment::COLOR_SCHEME;
        }
        if reads.viewport {
            computed.reads.environment_reads |= crate::css::style::input_types::StyleEnvironment::VIEWPORT;
        }
        computed.element_dependent |= reads.sibling_position || reads.container_unit_mask != 0;
        // A store holding what the element's environment held before gives way to that environment's store.
        let (target, pseudo) = subject.target();
        let store = store.map(|store| {
            self.store_resolving_to_the_same_values(target, pseudo, &store)
                .unwrap_or(store)
        });
        Some((store, drew_random_base_values.get()))
    }

    /// The engine: the custom properties `cascaded` declares over the store its element's parent
    /// hands down, leaving out each value the parent already holds by identity.
    fn declared_custom_properties(
        &self,
        parent_store: Option<&Arc<CustomPropertyStore>>,
        cascaded: &[(CustomDeclaration, RetainedStyleValueData)],
    ) -> Option<Vec<DeclaredCustomProperty>> {
        let parent = parent_store.map(|store| &**store);
        let mut values = Vec::with_capacity(cascaded.len());
        for (declared, value) in cascaded {
            let name = self.declared_custom_property_name(declared.name)?;
            if parent.is_some_and(|parent| parent.value_is_identical(name.raw(), value.pointer().cast())) {
                continue;
            }
            values.push((name.raw(), declared.important(), value.pointer().cast()));
        }
        Some(values)
    }

    /// Resolve the store of an element declaring `values`, with its registered values computed
    /// against the element's lengths, place among its siblings and color scheme where `computed`
    /// gives them, and left as parsed otherwise. Values substituting something of the element
    /// itself read it from `subject`.
    fn resolve_custom_property_store(
        &self,
        parent_store: Option<&Arc<CustomPropertyStore>>,
        parent_held: Option<&Arc<CustomPropertyStore>>,
        values: Vec<DeclaredCustomProperty>,
        computed: Option<RegisteredValueContext<'_, '_>>,
        subject: Option<&dyn SubstitutingSubject>,
    ) -> (
        Option<Arc<CustomPropertyStore>>,
        crate::css::custom_properties::RegisteredValueReads,
    ) {
        let inputs = &self.document_style_computation_inputs;
        // https://drafts.csswg.org/css-cascade-5/#inheriting
        // `inherit` takes the parent's value even of a property that does not inherit.
        let parent = parent_held.or(parent_store).map(|store| &**store);
        // SAFETY: The values are the program's and the element's interned values, live for the call.
        let cascaded_store = unsafe { CustomPropertyStore::cascaded_child(parent_store.cloned(), values) };
        let registry = inputs.custom_property_registry();
        let random_function_index = crate::css::parser::value_parser::RandomFunctionIndex::default();
        let mut parse_context = registry.parse_context(&random_function_index);
        parse_context.in_quirks_mode = inputs.in_quirks_mode;
        let media_environment = self.document_media.environment();
        // https://drafts.csswg.org/css-values-5/#attr-notation
        // https://drafts.csswg.org/css-values-5/#if-notation
        // https://drafts.csswg.org/css-mixins/#using-custom-functions
        let attributes = subject.map(|subject| {
            let (node, pseudo) = subject.target();
            SubstitutionAttributes::of(
                &self.projection,
                self.substitution_attribute_element(node, pseudo),
                self.html_element_namespace,
            )
        });
        let style_query = subject.and_then(SubstitutingSubject::style_query_inputs);
        let functions = subject.and_then(SubstitutingSubject::custom_functions);
        let mut resolution_context = engine_resolution_context(
            &parse_context,
            Some(&cascaded_store),
            parent,
            registry,
            attributes.as_ref(),
            &media_environment,
            style_query.as_ref(),
            functions.as_ref(),
        );
        // https://drafts.csswg.org/css-values-5/#random-caching
        // A random function in a registered value computes against the base value its random
        // caching key draws once. The keys not drawn yet are drawn between passes, and the
        // element waits.
        let draw_random_base_value = |name: &[u16], element_shared: bool| {
            let Some(context) = &computed else {
                return 0.0;
            };
            context.drew_random_base_values.set(true);
            self.drawn_random_base_value(context.node, name, element_shared)
                .unwrap_or_else(|| {
                    context
                        .random_requests
                        .borrow_mut()
                        .push(super::random_bases::RandomBaseRequest {
                            node: context.node,
                            name: name.into(),
                            element_shared,
                        });
                    0.0
                })
        };
        let finalization = CustomPropertyFinalization {
            length: computed.as_ref().map(|context| context.length),
            tree_counting: computed.as_ref().and_then(|context| context.tree_counting),
            random_base_values: &[],
            color_scheme: computed.as_ref().map_or(0, |context| context.color_scheme),
            draw_random_base_value: computed.is_some().then_some(&draw_random_base_value as _),
        };
        let (store, reads) = resolve_declared_custom_properties(
            &cascaded_store,
            parent_store,
            parent_store.is_some(),
            &mut resolution_context,
            finalization,
        );
        (store.or_else(|| parent_store.cloned()), reads)
    }

    /// The engine: what the children of an element holding `store` inherit, where the element was
    /// handed `handed_down`: the store without the custom properties a registration keeps from
    /// inheriting.
    pub(in crate::css::style) fn inheritable_custom_property_store<'store>(
        &self,
        store: Option<&'store Arc<CustomPropertyStore>>,
        handed_down: Option<&'store Arc<CustomPropertyStore>>,
    ) -> Option<std::borrow::Cow<'store, Arc<CustomPropertyStore>>> {
        let store = store?;
        // A store handed down holds only what inherits.
        if handed_down.is_some_and(|handed_down| Arc::ptr_eq(handed_down, store)) {
            return Some(std::borrow::Cow::Borrowed(store));
        }
        let registry = self.document_style_computation_inputs.custom_property_registry();
        if !registry.has_non_inheriting_registrations() {
            return Some(std::borrow::Cow::Borrowed(store));
        }
        let Some(inheritable) = store.inheritable(registry) else {
            return Some(std::borrow::Cow::Borrowed(store));
        };
        // A store whose own values all stay with the element hands down what it was handed, which
        // keeps one store through a chain of elements declaring only such values.
        if inheritable.own_values.is_empty() {
            return handed_down.map(std::borrow::Cow::Borrowed);
        }
        Some(std::borrow::Cow::Owned(inheritable))
    }

    /// The engine: what a written value that waits on substitution substitutes to for `property`,
    /// against the element's custom-property store and the one it inherits.
    pub(in crate::css::style) fn substitute_value(
        &self,
        stores: CustomPropertyStores<'_>,
        property: u16,
        written: &RetainedStyleValueData,
    ) -> RetainedStyleValueData {
        // A value reading the element itself substitutes for the element alone.
        if value_reads_substitution_subject(written.data()) {
            return self.substitute_for_subject(stores, property, written);
        }
        // A substitution reads nothing of the element but its stores, which its descendants
        // mostly share, so each thread remembers what each value substituted to against them.
        let key = (
            std::ptr::from_ref(written.data()) as usize,
            property,
            stores.own.map_or(0, |store| Arc::as_ptr(store) as usize),
            stores.inherited.map_or(0, |store| Arc::as_ptr(store) as usize),
        );
        if let Some(entry) = stores.substitutions.borrow().by_stores.get(&key) {
            return entry.substituted.clone();
        }
        // What the value references, where every reference is visible and resolves to a value
        // whose identity is all there is to it. An `inherit()` reads the inherited store too.
        let references = (!value_reads_inherited_values(written.data()))
            .then(|| crate::css::style_value::custom_property_references(written.data()))
            .flatten()
            .filter(|(_, all_visible)| *all_visible)
            .and_then(|(names, _)| {
                names
                    .iter()
                    .map(|name| {
                        stores.own.map_or(Some(0), |store| {
                            store.resolved_value_identity(&ak::Utf16FlyString::from_utf16(name))
                        })
                    })
                    .collect::<Option<smallvec::SmallVec<[usize; 4]>>>()
            })
            .map(|identities| (key.0, property, identities));
        let found = references
            .as_ref()
            .and_then(|references| stores.substitutions.borrow().by_references.get(references).cloned());
        if let Some(substituted) = found {
            stores
                .substitutions
                .borrow_mut()
                .insert_by_stores(key, stores, substituted.clone());
            return substituted;
        }
        let substituted = self.substitute_uncached(
            stores.own.map(Arc::as_ref),
            stores.inherited.map(Arc::as_ref),
            property,
            written,
        );
        let mut substitutions = stores.substitutions.borrow_mut();
        substitutions.insert_by_stores(key, stores, substituted.clone());
        if let Some(references) = references {
            substitutions.by_references.insert(references, substituted.clone());
        }
        substituted
    }

    fn substitute_uncached(
        &self,
        store: Option<&CustomPropertyStore>,
        inheritance_store: Option<&CustomPropertyStore>,
        property: u16,
        written: &RetainedStyleValueData,
    ) -> RetainedStyleValueData {
        let inputs = SubstitutionInputs {
            document: &self.document_style_computation_inputs,
            media: &self.document_media,
            attributes: None,
            style_query: None,
            style_query_references: None,
            functions: None,
        };
        substitute_against_stores(store, inheritance_store, &inputs, property, &[], written)
    }

    /// The engine: what a written value reading the element itself through `attr()`, `if()` or a
    /// custom function substitutes to for `property`, against the element's stores.
    fn substitute_for_subject(
        &self,
        stores: CustomPropertyStores<'_>,
        property: u16,
        written: &RetainedStyleValueData,
    ) -> RetainedStyleValueData {
        let (node, pseudo) = stores.subject.target();
        let functions = value_calls_custom_functions(written.data())
            .then(|| stores.subject.custom_functions())
            .flatten();
        let reads_attributes = value_reads_attributes(written.data())
            || functions.as_ref().is_some_and(|functions| functions.reads_attributes);
        let attributes = reads_attributes.then(|| {
            SubstitutionAttributes::of(
                &self.projection,
                self.substitution_attribute_element(node, pseudo),
                self.html_element_namespace,
            )
        });
        let style_query = (functions.is_some() || value_reads_conditions(written.data()))
            .then(|| stores.subject.style_query_inputs())
            .flatten();
        let inputs = SubstitutionInputs {
            document: &self.document_style_computation_inputs,
            media: &self.document_media,
            attributes: attributes.as_ref(),
            style_query: style_query.as_ref(),
            style_query_references: None,
            functions: functions.as_ref(),
        };
        substitute_against_stores(
            stores.own.map(Arc::as_ref),
            stores.inherited.map(Arc::as_ref),
            &inputs,
            property,
            &[],
            written,
        )
    }
}
