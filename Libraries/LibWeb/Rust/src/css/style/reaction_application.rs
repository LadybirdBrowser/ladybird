/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Applying the reactions of a style update to the elements they name.
//!
//! The engine decides what each reaction installs: the record it settled, or a record it demands now. The host installs a record on its element, runs the element's animations and transitions,
//! invalidates its layout, and reports what the installation did, from which the engine derives the reactions of the
//! element's children.

use super::StyleNodeID;
use super::engine_calls::{has_deferred_element_style_input, with_engine};
use super::pipeline::install::{StyleInstallation, StyleSwap};
use super::pipeline::main_effects::InstalledElementEffects;
use crate::render_state::{BegunRead, DocumentHost};

/// A `Web::DOM::Element`, which Rust names by pointer only.
#[repr(C)]
pub struct HostElement {
    _opaque: [u8; 0],
    _not_send_or_sync: std::marker::PhantomData<*const ()>,
}

/// The host's application of style reactions to its document's elements, which Rust names by pointer only.
#[repr(C)]
pub struct HostStyleReactionApplication {
    _opaque: [u8; 0],
    _not_send_or_sync: std::marker::PhantomData<*const ()>,
}

/// The host's element held in a struct, which C++ sees as an opaque pointer: the GC plugin rejects a raw cell pointer
/// field, and a `GC::Ptr` is no C type.
#[derive(Clone, Copy)]
#[repr(transparent)]
pub struct HostElementPtr(pub *mut HostElement);

impl HostElementPtr {
    fn is_null(self) -> bool {
        self.0.is_null()
    }
}

/// The element a style node names, as the host holds it while it applies a reaction to it.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiReactionElement {
    /// The host's element, or null where no element has the style node.
    pub element: HostElementPtr,
    pub style_node: u32,
    /// Whether the element is connected to the document whose reactions are applied.
    pub connected: bool,
    pub has_style: bool,
    pub style_record: u64,
    /// Whether the style the element holds has display:none.
    pub display_none: bool,
}

/// How a targeted style update of an element reads its style.
#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiStyleUpdateMode {
    /// Every element the element inherits through computes the style it owes, under display:none ancestors too.
    Normal,
    /// Only an element without style, or one that owes a style input, computes its style.
    #[expect(
        dead_code,
        reason = "only the host asks for this mode, which the engine reads by elimination"
    )]
    OnlyIfNeeded,
    /// As `OnlyIfNeeded`, but a display:none ancestor stops the update: the element has no style to read.
    StopAtDisplayNone,
}

/// What a targeted style update answers a read of an element's style.
#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiTargetedStyleAnswer {
    /// The element has no style to read.
    Unstyled,
    /// The element has the style it reads.
    Styled,
    /// The element has the style it holds, if any.
    HeldStyle,
    /// The style the element reads needs a style update of its document first.
    NeedsStyleUpdate,
}

/// What the reaction passes of a style update did, for the host's counters.
#[derive(Clone, Copy, Default)]
#[repr(C)]
pub struct FfiStyleReactionCounts {
    /// Whether the update had reactions to apply at all.
    pub had_reactions: bool,
    pub batch_runs: u32,
    pub reaction_elements: u32,
    pub published_reactions: u32,
    pub pass_guard_hits: u32,
    pub apply_microseconds: u64,
}

unsafe extern "C" {
    fn web_css_style_inheritance_parent(
        application: *mut HostStyleReactionApplication,
        element: *mut HostElement,
    ) -> FfiReactionElement;
    fn web_css_run_style_update(application: *mut HostStyleReactionApplication, flown: bool);
    fn web_css_has_pending_style_update(application: *mut HostStyleReactionApplication) -> bool;
    fn web_css_note_style_update_has_reactions(application: *mut HostStyleReactionApplication);
    fn web_css_sample_animations_for_style_update(application: *mut HostStyleReactionApplication);
    fn web_css_begin_style_update_pass(application: *mut HostStyleReactionApplication, first: bool);
    fn web_css_sample_undisplayed_animations_for_read(
        application: *mut HostStyleReactionApplication,
        element: *mut HostElement,
    );
    fn web_css_style_reaction_element(application: *mut HostStyleReactionApplication, node: u32) -> FfiReactionElement;
    fn web_css_install_styles(
        application: *mut HostStyleReactionApplication,
        swaps: *const StyleSwap,
        swap_count: usize,
        effects: *const InstalledElementEffects,
        effect_count: usize,
        document_damage: u32,
    );
}

/// The host's application of a batch of reactions to its document's elements, which the host names by pointer.
#[derive(Clone, Copy)]
struct Application<'a> {
    host: &'a DocumentHost,
    read: &'a BegunRead,
    host_application: *mut HostStyleReactionApplication,
}

impl Application<'_> {
    fn element(self, node: u32) -> FfiReactionElement {
        // SAFETY: The host lends its application for the call that applies the batch.
        unsafe { web_css_style_reaction_element(self.host_application, node) }
    }

    /// A targeted update of `node`: the engine computes the record it demands for the element now, and the host takes
    /// the installation of that record as it takes a style update's. Answers whether the engine could: where it could
    /// not, the element keeps what it holds until the next style update.
    fn demand(self, node: StyleNodeID) -> bool {
        let Some(installation) = with_engine(self.read, self.host, |engine| engine.demand_installation(node)) else {
            return false;
        };
        self.install_styles(&installation);
        with_engine(self.read, self.host, |engine| {
            for &record in &installation.pinned_records {
                engine.unpin_layout_style_record(record);
            }
        });
        true
    }
}

/// A style update applies at most this many passes of reactions that publish new style, beyond which it stops: a
/// style change that keeps feeding back into itself would never settle.
const MAX_STYLE_UPDATE_PASSES: u32 = 8;

impl Application<'_> {
    /// Takes the next style update of the host's update_style(), the one computed beside the event loop first where it
    /// drains it, and answers the style installation the render side made of it for the host to swap in.
    fn take_transaction(self, flown: bool) -> StyleInstallation {
        {
            let _taking = self.host.engine_memo().take_transactions_to_apply();
            // SAFETY: The host lends its application for the call that updates the style.
            unsafe { web_css_run_style_update(self.host_application, flown) };
        }
        self.host.take_transaction_installation().unwrap_or_default()
    }

    /// Has the host swap in the styles of `installation`, a style installation, and perform their side effects.
    fn install_styles(self, installation: &StyleInstallation) {
        // SAFETY: The host lends its application for the call that updates the style, and the slices live for it.
        unsafe {
            web_css_install_styles(
                self.host_application,
                installation.swaps.as_ptr(),
                installation.swaps.len(),
                installation.effects.as_ptr(),
                installation.effects.len(),
                installation.document_damage,
            );
        }
    }

    fn has_pending_transaction(self) -> bool {
        // SAFETY: As above.
        unsafe { web_css_has_pending_style_update(self.host_application) }
    }
}

/// Applies the reactions of the style updates of one update_style(), from the first, computed beside the event
/// loop where it drains one, until the reactions they feed back settle. Each pass applies one transaction's
/// reactions, closed over the elements they inherit through, and the consequences produced while applying them become
/// the next transaction.
fn update_style(application: Application<'_>, drains_flown_transaction: bool) -> FfiStyleReactionCounts {
    let mut counts = FfiStyleReactionCounts::default();
    let mut taken = application.take_transaction(drains_flown_transaction);
    let is_empty = |installation: &StyleInstallation| installation.swaps.is_empty() && installation.effects.is_empty();
    // SAFETY: The host lends its application for the call that updates the style.
    unsafe {
        if !is_empty(&taken) {
            web_css_note_style_update_has_reactions(application.host_application);
        }
        web_css_sample_animations_for_style_update(application.host_application);
    }
    if is_empty(&taken) && application.has_pending_transaction() {
        taken = application.take_transaction(false);
    }
    // SAFETY: As above.
    unsafe { web_css_begin_style_update_pass(application.host_application, true) };
    if is_empty(&taken) {
        return counts;
    }
    counts.had_reactions = true;

    let mut style_update_passes = 0;
    let mut first_pass = true;
    while !is_empty(&taken) {
        let apply_started_at = std::time::Instant::now();
        if !std::mem::take(&mut first_pass) {
            // SAFETY: As above.
            unsafe { web_css_begin_style_update_pass(application.host_application, false) };
        }
        let published_reactions = taken.swaps.len() as u32;
        if published_reactions > 0 {
            style_update_passes += 1;
            if style_update_passes > MAX_STYLE_UPDATE_PASSES {
                counts.pass_guard_hits += 1;
                // NB: The render side noted the records the installation swaps in as the host's, so the host takes it
                //     before it stops.
                application.install_styles(&taken);
                counts.apply_microseconds += apply_started_at.elapsed().as_micros() as u64;
                break;
            }
        }
        counts.batch_runs += 1;
        counts.reaction_elements += published_reactions;
        application.install_styles(&taken);
        counts.apply_microseconds += apply_started_at.elapsed().as_micros() as u64;

        // The consequences produced while installing become the next transaction in this stabilization epoch.
        taken = match application.has_pending_transaction() {
            true => application.take_transaction(false),
            false => StyleInstallation::default(),
        };
    }
    counts
}

/// Whether `LIBWEB_STYLE_SELFCHECK` asks for what `style_engine_self_check` does.
#[unsafe(no_mangle)]
pub extern "C" fn style_engine_self_check_is_enabled(host: &DocumentHost) -> bool {
    host.engine_memo().style_self_check.is_enabled()
}

/// Checks what the engine holds for the host's document under `root`, its document element's style node or zero,
/// against a cold pass, as `LIBWEB_STYLE_SELFCHECK` asks, once a style update has settled.
#[unsafe(no_mangle)]
pub extern "C" fn style_engine_self_check(host: &DocumentHost, read: &BegunRead, root: u32) {
    let settings = host.engine_memo().style_self_check;
    if let Some(root) = StyleNodeID::from_raw(root)
        && settings.is_enabled()
    {
        with_engine(read, host, |engine| engine.run_self_check(root, settings));
    }
}

/// Applies the reactions of the style updates of one update_style() of the host's document, from the one computed
/// beside the event loop
/// where `drains_flown_transaction`, through `host_application`, the host's state for applying them, and answers what
/// they did for its counters.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread, and `host_application` must be live for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_update_style(
    host: &DocumentHost,
    read: &BegunRead,
    host_application: *mut HostStyleReactionApplication,
    drains_flown_transaction: bool,
) -> FfiStyleReactionCounts {
    let application = Application {
        host,
        read,
        host_application,
    };
    update_style(application, drains_flown_transaction)
}

/// What an inheritance chain holds, by depth from its first element.
#[derive(Default)]
struct InheritanceChainScan {
    /// The topmost element that has to compute its style: it has none, or it owes a style input.
    topmost_requiring_style: Option<usize>,
    topmost_display_none: Option<usize>,
    nearest_display_none: Option<usize>,
}

impl InheritanceChainScan {
    fn display_none_above_every_element_requiring_style(&self) -> bool {
        self.topmost_display_none.is_some_and(|display_none| {
            self.topmost_requiring_style
                .is_none_or(|requiring| display_none > requiring)
        })
    }
}

impl Application<'_> {
    /// The elements an element inherits its style through, from `first` up to the root.
    fn inheritance_chain(self, first: FfiReactionElement) -> impl Iterator<Item = FfiReactionElement> {
        let present = |element: FfiReactionElement| (!element.element.is_null()).then_some(element);
        std::iter::successors(present(first), move |element| {
            // SAFETY: The host lends its application for the call, and the element is live while it is read.
            present(unsafe { web_css_style_inheritance_parent(self.host_application, element.element.0) })
        })
    }

    /// Scans the chain an element inherits its style through, from `first`, where every element may owe a style
    /// input unless `may_owe_style_inputs` is false.
    fn scan_inheritance_chain(self, first: FfiReactionElement, may_owe_style_inputs: bool) -> InheritanceChainScan {
        let mut scan = InheritanceChainScan::default();
        for (depth, element) in self.inheritance_chain(first).enumerate() {
            if !element.has_style
                || (may_owe_style_inputs
                    && StyleNodeID::from_raw(element.style_node)
                        .is_some_and(|node| has_deferred_element_style_input(self.host, self.read, node)))
            {
                scan.topmost_requiring_style = Some(depth);
            }
            if element.display_none {
                scan.topmost_display_none = Some(depth);
                scan.nearest_display_none.get_or_insert(depth);
            }
        }
        scan
    }

    /// Answers a read of an element's style, whose chain of elements it inherits through starts at `first`, from the
    /// styles they hold, where neither its document nor an embedding one has style work pending.
    fn read_settled_inheritance_chain(
        self,
        first: FfiReactionElement,
        mode: FfiStyleUpdateMode,
    ) -> FfiTargetedStyleAnswer {
        let scan = self.scan_inheritance_chain(first, false);
        if mode == FfiStyleUpdateMode::StopAtDisplayNone && scan.display_none_above_every_element_requiring_style() {
            FfiTargetedStyleAnswer::Unstyled
        } else if scan.topmost_requiring_style.is_none() {
            FfiTargetedStyleAnswer::Styled
        } else {
            FfiTargetedStyleAnswer::NeedsStyleUpdate
        }
    }

    /// Updates the style of the chain of elements an element inherits through, from `first`: re-cascades from the
    /// rootmost element on the chain that has to compute its style back down to the element.
    fn update_style_for_inheritance_chain(
        self,
        first: FfiReactionElement,
        mode: FfiStyleUpdateMode,
        embedding_document_layout_was_stale: bool,
    ) -> FfiTargetedStyleAnswer {
        let scan = self.scan_inheritance_chain(first, true);
        if mode == FfiStyleUpdateMode::StopAtDisplayNone && scan.display_none_above_every_element_requiring_style() {
            return FfiTargetedStyleAnswer::Unstyled;
        }
        // NB: The records the engine installs for a document it owns are current once the style update ran, but
        //     for those below an element that is not displayed, which no style update styles: a read in normal mode
        //     demands them again, as their inputs may have moved since they were demanded last.
        if scan.topmost_requiring_style.is_none()
            && (mode != FfiStyleUpdateMode::Normal || scan.nearest_display_none.is_none())
        {
            return FfiTargetedStyleAnswer::HeldStyle;
        }
        // NB: No style update samples the animations of an element that is not displayed. What a read below one sees of
        //     them is sampled here, from the top down, for the demands below to compute from.
        if mode == FfiStyleUpdateMode::Normal && scan.nearest_display_none.is_some() {
            let elements: smallvec::SmallVec<[_; 32]> =
                self.inheritance_chain(first).map(|element| element.element).collect();
            for element in elements.into_iter().rev() {
                // SAFETY: The host lends its application for the call, and the element is live while it is read.
                unsafe { web_css_sample_undisplayed_animations_for_read(self.host_application, element.0) };
            }
        }
        // Normal mode also re-cascades the target path under its display:none ancestors.
        let mut topmost_element_to_recompute = scan.topmost_requiring_style;
        if mode == FfiStyleUpdateMode::Normal && topmost_element_to_recompute.is_none() {
            topmost_element_to_recompute = scan.nearest_display_none.and_then(|depth| depth.checked_sub(1));
        }
        let topmost_element_to_recompute = match topmost_element_to_recompute {
            Some(depth) => depth,
            None if mode != FfiStyleUpdateMode::Normal && !embedding_document_layout_was_stale => {
                return FfiTargetedStyleAnswer::HeldStyle;
            }
            None => 0,
        };

        // The chain is named by style node: an element it names is looked up again as the walk reaches it.
        let chain: smallvec::SmallVec<[u32; 32]> = self
            .inheritance_chain(first)
            .take(topmost_element_to_recompute + 1)
            .map(|element| element.style_node)
            .collect();
        for &style_node in chain.iter().rev() {
            let element = self.element(style_node);
            let Some(node) = StyleNodeID::from_raw(style_node).filter(|_| !element.element.is_null()) else {
                return FfiTargetedStyleAnswer::HeldStyle;
            };
            if !self.demand(node) {
                return FfiTargetedStyleAnswer::HeldStyle;
            }

            let element = self.element(style_node);
            if element.display_none && mode == FfiStyleUpdateMode::StopAtDisplayNone {
                return FfiTargetedStyleAnswer::Unstyled;
            }
        }
        FfiTargetedStyleAnswer::HeldStyle
    }
}

/// Answers a read of an element's style from the styles held by the chain of elements it inherits through, from
/// `first`, where neither its document nor an embedding one has style work pending, or that it needs a style update.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread, and `host_application` and the element `first`
/// names must be live for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_read_settled_inheritance_chain(
    host: &DocumentHost,
    read: &BegunRead,
    host_application: *mut HostStyleReactionApplication,
    first: FfiReactionElement,
    mode: FfiStyleUpdateMode,
) -> FfiTargetedStyleAnswer {
    let application = Application {
        host,
        read,
        host_application,
    };
    application.read_settled_inheritance_chain(first, mode)
}

/// Updates the style of the chain of elements an element inherits through, from `first`, for a read of the element's
/// style, and answers the read.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread, and `host_application` and the element `first`
/// names must be live for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_update_style_for_inheritance_chain(
    host: &DocumentHost,
    read: &BegunRead,
    host_application: *mut HostStyleReactionApplication,
    first: FfiReactionElement,
    mode: FfiStyleUpdateMode,
    embedding_document_layout_was_stale: bool,
) -> FfiTargetedStyleAnswer {
    let application = Application {
        host,
        read,
        host_application,
    };
    application.update_style_for_inheritance_chain(first, mode, embedding_document_layout_was_stale)
}
