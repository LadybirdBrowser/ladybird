/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::column::advance_epoch;
use super::*;

impl StyleEngine {
    /// Whether the locally evaluable compound containing an input changed truth on that element.
    fn route_origin_truth_flipped(
        &mut self,
        program: SelectorProgramID,
        origin: selector::SelectorNodeID,
        node: StyleNodeID,
    ) -> Option<bool> {
        let view = self.transaction_fact_view.as_ref()?;
        let compiled = self.programs.get(program);
        if !compiled.selector_node_reads_only_local_facts(origin) {
            return None;
        }
        let resident_facts = self.facts.primary();
        let old = MatchEvaluator::new(&self.tree, resident_facts)
            .with_transaction_fact_view(view, TransactionFactSide::Before)
            .matches_selector_node(compiled, origin, node, &mut self.counters)
            .ok()?;
        let new = MatchEvaluator::new(&self.tree, resident_facts)
            .with_transaction_fact_view(view, TransactionFactSide::After)
            .matches_selector_node(compiled, origin, node, &mut self.counters)
            .ok()?;
        Some(old != new)
    }

    /// Whether one attribute input changed the route's origin test. Presence and case-sensitive
    /// interned equality are answered directly; text operators use the selector evaluator.
    fn route_attribute_origin_truth_flipped(
        &mut self,
        input: &NormalizedInput,
        program: SelectorProgramID,
        origin: selector::SelectorNodeID,
        node: StyleNodeID,
    ) -> Option<bool> {
        let InputKey::LocalFeature(_, LocalFeatureKey::Attribute(_)) = input.key else {
            return None;
        };
        let compiled = self.programs.get(program);
        if let SelectorOp::Feature(selector::FeatureTest::Attribute(test)) = compiled.node(origin) {
            let value = |input: InputValue| match input {
                InputValue::Feature(value) => Some(value),
                _ => None,
            };
            let old = value(input.old)?;
            let new = value(input.new)?;
            if test.operator == selector::AttributeOperator::Presence {
                return Some(old.holds() != new.holds());
            }
            if test.operator == selector::AttributeOperator::Exact
                && test.case == selector::AttributeCase::Sensitive
                && !test.value_atom.is_none()
            {
                let matches = |value: FeatureValue| match value {
                    FeatureValue::Absent => Some(false),
                    FeatureValue::Atom(atom) => Some(atom == test.value_atom),
                    _ => None,
                };
                return Some(matches(old)? != matches(new)?);
            }
        }
        self.route_origin_truth_flipped(program, origin, node)
    }

    /// Route one non-program input to the region its transpose routes reach.
    ///
    /// The region a path folds to is often much wider than the subjects that can actually change:
    /// `.guard:hover .target` reaches a whole subtree, but only the `.target` elements inside it
    /// have a rule whose truth depends on that hover. So a region wider than a single node is
    /// narrowed to the candidates of the subject's own rightmost feature that lie inside it, which
    /// is a posting lookup plus a membership proof per candidate rather than a walk of the region.
    ///
    /// Narrowing is attempted only while the candidate source is small relative to the region. Past
    /// that, streaming the region is the cheaper plan and the region stands.
    #[allow(clippy::too_many_arguments)]
    pub(super) fn route_input(
        &mut self,
        input: &NormalizedInput,
        changed_sheets: &[SheetID],
        changed_programs: &[SelectorProgramID],
        tree_routing: TreeRoutingMode<'_>,
        sibling_entries: &[SiblingEntry],
        sibling_candidates: &mut SiblingCandidateWorkspace,
        pending_sibling_routes: &mut PendingSiblingRoutes,
        pending_routes: &mut PendingRoutes,
        pending_prefix_producers: &mut Vec<PendingPrefixProducer>,
        prefix_producer_admission: Option<&(ScopeProgramID, Rc<RuleDispatch>)>,
        prefix_producer_cache: &mut PrefixProducerCache,
        prefix_producer_seen: &mut Vec<u32>,
        sequences: &mut SequenceChanges,
        regions: &mut ImpactRegions,
    ) {
        let routing = Rc::clone(&self.routing);
        // A declaration sourced from the element changes what wins on that element and nothing
        // else. There is no selector to transpose: the region is the element itself.
        if let InputKey::ElementDeclaration(node, _) = input.key {
            self.record_selector_truth_refresh(node, None);
            regions.add(ImpactRegion::Node(node), &mut self.counters);
            return;
        }

        // Other element-owned style inputs do not change matching at all. Publish the retained
        // answer as an exact reaction without asking the selector evaluator to prove it again.
        if let InputKey::ElementStyleInput(node) = input.key {
            regions.add(ImpactRegion::Node(node), &mut self.counters);
            return;
        }

        // An element's part exposure moving is that element becoming addressable from somewhere it
        // was not, or ceasing to be. No selector names the exposure, so there is nothing to
        // transpose: the region is the element whose reach moved.
        if let InputKey::LocalFeature(node, LocalFeatureKey::PartExposure) = input.key {
            self.record_selector_truth_refresh(node, None);
            regions.add(ImpactRegion::Node(node), &mut self.counters);
            return;
        }

        // A tree delta is not a feature moving on a node: it is the node's place in the tree
        // moving, which changes positional truth across a whole child sequence.
        if let InputKey::TreeRelations(node) = input.key {
            self.route_tree_delta(
                node,
                input.old,
                input.new,
                tree_routing,
                sibling_entries,
                sibling_candidates,
                pending_sibling_routes,
                sequences,
                regions,
            );
            return;
        }

        let in_flux = Self::feature_in_flux(input);
        let is_arrival = matches!(input.key, InputKey::LocalFeature(_, LocalFeatureKey::ArrivingFacts));
        let keys = match input.key {
            InputKey::LocalFeature(node, LocalFeatureKey::ArrivingFacts) => self.routing_keys_of_arriving_facts(node),
            // A `[*|x]` rule registers under the any-namespace form of the name, which the pure
            // mapping cannot produce: it is a property of the atom rather than of the input. An
            // attribute change has to reach those rules as well as the ones naming its own namespace.
            InputKey::LocalFeature(_, LocalFeatureKey::Attribute(name)) => {
                let mut keys = routing_keys_for_input(input);
                for other in self.facts.attribute_name_keys(name) {
                    if other != name {
                        keys.push(RoutingKey::AttributeName(other));
                    }
                }
                keys
            }
            _ => routing_keys_for_input(input),
        };
        let route_liveness = routing.route_liveness(&self.program, &self.programs);
        for key in keys {
            // The tree delta already puts an arriving element and its subtree in the plan. Its
            // individual facts only have work left to do as possible witnesses of relational
            // selectors whose anchors live outside that subtree, or on the left side of sibling
            // selectors whose subjects follow it.
            let routes = match is_arrival {
                true => routing.arrival_routes_for(key),
                false => routing.routes_for(key),
            };
            self.counters.add(Counter::RoutedEntryPoints, routes.len() as u64);

            let Some(node) = input.key.style_node() else {
                continue;
            };
            for &route in routes {
                let route_is_live = route_liveness.contains(route.index());
                // A rule that decides nothing right now cannot be reached through. Its entry
                // points stay registered, because the rule still holds its position and its sheet
                // can come back.
                let rule = routing.rule_of(route);
                if !route_is_live
                    && !self.program.rule_can_decide(rule)
                    && !changed_sheets.contains(&self.program.rule_sheet(rule))
                {
                    continue;
                }
                let point = routing.route(route);
                let (selector_program, selector_entry) = self.programs.entry_location(point.entry);
                // A rule that was given a new selector list keeps its identity, so the routes
                // of the program it left behind are still registered under it. They describe a
                // selector the rule no longer has, and following them would route a mutation
                // through a selector nobody wrote.
                if !route_is_live
                    && self.program.rule_version(rule).selector_program != Some(selector_program)
                    && !changed_programs.contains(&selector_program)
                {
                    continue;
                }
                let path = routing.path_of(route);
                if !is_arrival
                    && point.anchor.is_none()
                    && !path.is_empty()
                    && matches!(input.key, InputKey::LocalFeature(_, LocalFeatureKey::Attribute(_)))
                {
                    let truth_flipped = self.route_attribute_origin_truth_flipped(
                        input,
                        selector_program,
                        point.selector_node.expect("attribute route has no selector node"),
                        node,
                    );
                    if truth_flipped == Some(false) {
                        self.counters.bump(Counter::OriginTruthRoutesSkipped);
                        continue;
                    }
                    self.counters.bump(Counter::OriginTruthRoutesFired);
                }
                // The element the input happened to must satisfy the rest of the compound the input
                // occurs in, or this route cannot be reached from it at all.
                if !self.node_carries_any(routing.origin_dispatch_of(route), node, in_flux) {
                    continue;
                }
                if !self.node_carries_all(routing.origin_required_of(route), node, in_flux) {
                    continue;
                }
                let exact_tree_evaluation = if is_arrival && tree_routing.use_exact {
                    if tree_routing.has_before_sibling_relations
                        && self.entry_can_use_before_sibling_relations(selector_program, selector_entry)
                    {
                        Some(
                            if matches!(path.first(), Some(InverseStep::FollowingSiblings))
                                && self.entry_is_monotone_in_the_tree(selector_program, selector_entry)
                            {
                                ExactTreeEvaluation::MonotonicArrival
                            } else {
                                ExactTreeEvaluation::BeforeSiblingRelations
                            },
                        )
                    } else if self.entry_is_monotone_in_the_tree(selector_program, selector_entry) {
                        Some(ExactTreeEvaluation::Arrival)
                    } else {
                        None
                    }
                } else {
                    None
                };
                let site = RoutingSite {
                    subject: routing.subject_dispatch_of(route),
                    subject_required: routing.subject_required_of(route),
                    position: routing.subject_position_of(route),
                    path,
                    waypoints: routing.waypoints_of(route),
                    in_flux,
                    exact_entry: Some((rule, point.entry)),
                    refresh_rule: Some((rule, point.entry)),
                    // One side decides for a monotonic tree change, and only where nothing the
                    // change carries can turn the entry the other way. A negation, `:empty` and a
                    // positional test each can, and each is answered on both sides as usual.
                    exact_tree_evaluation,
                };
                match point.anchor {
                    // A relational input resolves its anchors first. Folding the anchor step into
                    // the path would compose "the ancestors of the changed node" with whatever the
                    // outer selector adds, and ancestors-then-descendants is the document.
                    Some(anchor) => self.route_from_anchors(node, selector_program, anchor, &site, regions),
                    None => {
                        let region = ImpactRegion::follow(node, path, &self.tree);
                        let is_sibling_route = exact_tree_evaluation.is_some()
                            && matches!(
                                path.first(),
                                Some(InverseStep::NextSibling | InverseStep::FollowingSiblings)
                            )
                            && sibling_entries
                                .binary_search_by_key(&route, |entry| entry.route)
                                .is_ok();
                        let routes = if is_sibling_route {
                            pending_sibling_routes.regions_for(PendingSiblingRoute {
                                route,
                                kind: SiblingRouteKind::ExactUnion,
                                exact_tree_evaluation,
                            })
                        } else {
                            if let Some((scope_program, prefix_dispatch)) = prefix_producer_admission
                                && matches!(input.key, InputKey::LocalFeature(..) | InputKey::State(..))
                                && self.route_is_prefix_convergence_eligible(&routing, prefix_dispatch, route)
                            {
                                let producers = prefix_producer_cache.producers_for_route(
                                    route,
                                    prefix_dispatch.prefixes(),
                                    point.entry,
                                    routing.path_of(route).len(),
                                );
                                if !producers.is_empty()
                                    && let Lookup::Known(states) =
                                        self.prefix_caches.borrow().states.lookup(*scope_program)
                                {
                                    let node_stamp = node.raw();
                                    assert_ne!(node_stamp, 0, "a node identity used as an epoch stamp must be nonzero");
                                    for &producer in producers {
                                        if prefix_producer_seen.len() <= producer.index() {
                                            prefix_producer_seen.resize(producer.index() + 1, 0);
                                        }
                                        if prefix_producer_seen[producer.index()] == node_stamp {
                                            continue;
                                        }
                                        prefix_producer_seen[producer.index()] = node_stamp;
                                        if states.producer_is_active(prefix_dispatch.prefixes(), node, producer) {
                                            pending_prefix_producers.push(PendingPrefixProducer { node, producer });
                                        }
                                    }
                                }
                            }
                            pending_routes.regions_for(PendingRoute {
                                route,
                                exact_tree_evaluation,
                            })
                        };
                        self.push_pending_region(routes, region);
                    }
                }
            }
        }
    }

    /// Route a node's place in the tree changing.
    ///
    /// A node that is still connected brings its whole subtree: those nodes are new to this scope,
    /// or their ancestor context is. Positional truth changes across the whole child sequence the
    /// node joined or left, and emptiness and only-child truth change for its parent. Those matter
    /// only if some attached selector asks positional questions, which the registry answers in one
    /// lookup - so a document with no positional selectors pays nothing for the possibility.
    #[allow(clippy::too_many_arguments)]
    pub(super) fn route_tree_delta(
        &mut self,
        node: StyleNodeID,
        old: InputValue,
        new: InputValue,
        tree_routing: TreeRoutingMode<'_>,
        sibling_entries: &[SiblingEntry],
        sibling_candidates: &mut SiblingCandidateWorkspace,
        pending_sibling_routes: &mut PendingSiblingRoutes,
        sequences: &mut SequenceChanges,
        regions: &mut ImpactRegions,
    ) {
        let relations = |value: InputValue| match value {
            InputValue::TreeRelations(relations) => relations,
            _ => None,
        };
        let old = relations(old);
        let new = relations(new);

        if new.is_some()
            && (old.is_none()
                || old.and_then(|relations| relations.parent) != new.and_then(|relations| relations.parent))
        {
            regions.add(ImpactRegion::Subtree(node), &mut self.counters);
        }

        // A slottable's assigned slot is its parent in the flat tree, so a slot name change moves it
        // there without moving it in the DOM at all. Nothing about the child sequence it is in has
        // moved, and no feature of its own has changed - what reaches it is the element itself,
        // which is the subject `::slotted()` names and the element whose inheritance parent moved.
        if let (Some(old), Some(new)) = (old, new)
            && old.assigned_slot != new.assigned_slot
        {
            self.record_selector_truth_refresh(node, None);
            regions.add(ImpactRegion::Node(node), &mut self.counters);
            for slot in [old.assigned_slot, new.assigned_slot].into_iter().flatten() {
                for assigned in self.tree.assigned_nodes_of(slot) {
                    regions.add(ImpactRegion::Node(*assigned), &mut self.counters);
                }
            }
        }

        // A node crossing a parent boundary leaves one child sequence and joins another. A node
        // whose place inside one moved leaves and joins the same sequence, changing both seams
        // around it. A one-seam change instead describes a stationary neighbour, and counting it
        // would shift every count the sequence answers.
        let parent_of = |side: Option<TreeRelations>| side.and_then(|relations| relations.parent);
        let preceded_by = |side: Option<TreeRelations>| side.and_then(|relations| relations.previous_element_sibling);
        let followed_by = |side: Option<TreeRelations>| side.and_then(|relations| relations.next_element_sibling);
        let crossed_a_parent = parent_of(old) != parent_of(new);
        let place_moved = !crossed_a_parent
            && parent_of(old).is_some()
            && preceded_by(old) != preceded_by(new)
            && followed_by(old) != followed_by(new);

        // A node that stayed put and gained a new neighbour is what an adjacent- or general-sibling
        // selector constrains: `A + B` is a constraint on B, and what changed is B's relation. The
        // node that moved is the other side of the same constraint, and its old place is as much of
        // the change as its new one.
        if let (Some(old), Some(new)) = (old, new) {
            if old.previous_element_sibling != new.previous_element_sibling {
                self.route_sibling_relations_to_constrained_side(
                    node,
                    old,
                    new,
                    place_moved || crossed_a_parent,
                    tree_routing,
                    sibling_entries,
                    sibling_candidates,
                    pending_sibling_routes,
                );
            }
            if place_moved || crossed_a_parent {
                self.route_departure_to_siblings(
                    node,
                    old,
                    tree_routing,
                    sibling_entries,
                    sibling_candidates,
                    pending_sibling_routes,
                );
                self.route_arrival_to_siblings(
                    node,
                    tree_routing,
                    sibling_entries,
                    sibling_candidates,
                    pending_sibling_routes,
                );
            }
        }

        // A departing subtree takes its facts with it, and routing runs after it has already been
        // detached, so no walk from it finds anything and no feature is left to route from. Its old
        // relations are still on the delta, though, and they say where its anchors were.
        if new.is_none()
            && let Some(relations) = old
            && relations.parent.is_some()
        {
            self.route_departure_to_siblings(
                node,
                relations,
                tree_routing,
                sibling_entries,
                sibling_candidates,
                pending_sibling_routes,
            );
        }

        // An element joining, leaving, or moving within a sequence moves which of its members are
        // siblings of which, and a relational query reaching across a seam can gain or lose its
        // witness without any witness having changed. Nothing the moved element's features say
        // covers that, so the seam is recorded here and the anchors around it are asked once per
        // touched parent by the deferred relational sequence pass. A node crossing a parent
        // boundary departs one sequence and arrives in the other even when it stays connected.
        let relational_routes_exist = !self.routing.relational_routes().is_empty();

        for side in [old, new] {
            let Some(relations) = side else {
                continue;
            };
            // A removed node cannot be walked from, so its old neighbours anchor the sequence.
            if let Some(parent) = relations.parent {
                if crossed_a_parent || place_moved {
                    let change = sequences.get_or_insert_with(parent, || {
                        let workspace_before = self.match_workspace.capacity_bytes();
                        let children: Vec<StyleNodeID> = self.tree.children(parent).collect();
                        let (children, _, _) = self
                            .match_workspace
                            .publish_sibling_sequence(children, MatchEvaluationSide::Current);
                        let workspace_after = self.match_workspace.capacity_bytes();
                        self.memory
                            .reserve_required(MemoryCategory::BatchScratch, workspace_after - workspace_before);
                        SequenceChange::new(children)
                    });
                    let tag = self.facts.tag_of_node(node);
                    let changed_type = (!tag.is_none()).then(|| (tag, self.facts.namespace_of(node)));
                    let relocated = old.is_some() && new.is_some();
                    let relational = match (relational_routes_exist, relocated, side == new) {
                        (false, ..) => None,
                        (true, true, true) => Some(if crossed_a_parent {
                            SequenceSide::Arrived
                        } else {
                            SequenceSide::Moved
                        }),
                        (true, true, false) => Some(if crossed_a_parent {
                            SequenceSide::Departed
                        } else {
                            SequenceSide::Moved
                        }),
                        (true, false, true) => Some(SequenceSide::Arrived),
                        (true, false, false) => Some(SequenceSide::Departed),
                    };
                    change.record(relations, side == new, relocated, changed_type, node, relational);
                }
                continue;
            }
            let anchor = match side == new {
                true => Some(node),
                false => relations.previous_element_sibling.or(relations.next_element_sibling),
            };
            if let Some(anchor) = anchor {
                regions.add(ImpactRegion::SiblingSequence(anchor), &mut self.counters);
            }
        }
    }

    /// Route what the transaction did to child sequences into the relational queries it moves.
    ///
    /// A subtree leaving takes the witnesses with it, so the anchors cannot be found by walking
    /// from them. What is still true is that any anchor of a query the subtree witnessed is the old
    /// parent or one of its ancestors, and that it carries the anchor compound's own feature -
    /// which is the same filter that keeps an ordinary anchor set small.
    ///
    /// Joining is normally routed from the arriving element's own features, but two things no
    /// feature says. Which elements are siblings of which moved: `#subject:has(+ #s2)` stops
    /// matching when anything at all lands between the two, and that element need not be named by
    /// any selector in the document. And a query whose witness compound distinguishes nothing -
    /// `:has(:not(.test))`, `:has(*)` - has no feature for an arrival to carry, so the arriving
    /// element is asked directly whether it is a witness rather than waiting to be named as one.
    ///
    /// Runs once per touched parent rather than once per changed child. The anchors a sibling-axis
    /// query can reach from k seams in one sequence are the union of k predecessor prefixes, which
    /// is the single prefix of the rightmost seam - so a batch of arrivals pays one walk over the
    /// sequence instead of one per arrival.
    pub(super) fn route_relational_sequence_changes(
        &mut self,
        sequences: &SequenceChanges,
        regions: &mut ImpactRegions,
    ) {
        let routing = Rc::clone(&self.routing);
        if routing.relational_routes().is_empty() {
            return;
        }
        if sequences.iter().all(|(_, change)| change.relational_records.is_empty()) {
            return;
        }
        let live = routing.live_relational_routes(&self.program, &self.programs);

        for &LiveRelationalRoute { route, program, anchor } in live.iter() {
            // An argument that reaches its witness across a sibling relation of its own holds
            // because of an adjacency the query names nothing near. Any element landing in any
            // sequence breaks it while carrying nothing, so the query is asked from its witnesses
            // rather than from anything the change could be recognised as - and since that asks
            // nothing about the seam itself, once per transaction answers every seam.
            if anchor.argument_spans_siblings {
                let site = relational_route_site(&routing, route);
                self.route_from_possible_witnesses(program, anchor, &site, regions);
            }
        }

        for (parent, change) in sequences.iter() {
            if change.relational_records.is_empty() {
                continue;
            }
            self.route_relational_sequence_change(parent, change, &live, &routing, regions);
        }
    }

    /// One parent's share of the relational sequence routing: the candidate anchors each live
    /// route can reach from this sequence's seams, filtered by the anchor compound's own feature.
    pub(super) fn route_relational_sequence_change(
        &mut self,
        parent: StyleNodeID,
        change: &SequenceChange,
        live: &[LiveRelationalRoute],
        routing: &RoutingRegistry,
        regions: &mut ImpactRegions,
    ) {
        let children = &change.children;
        // Where the seams are, with a record nobody could place widened to the whole sequence.
        let clamp = |at: u32| match at == RELATIONAL_RECORD_UNLOCATED {
            true => children.len(),
            false => (at as usize).min(children.len()),
        };
        let mut any_arrived = false;
        let mut any_departed = false;
        let mut max_any = 0usize;
        let mut max_departed = 0usize;
        let mut max_arrived = 0usize;
        for &(at, side) in &change.relational_records {
            let at = clamp(at);
            max_any = max_any.max(at);
            match side {
                SequenceSide::Arrived => {
                    any_arrived = true;
                    max_arrived = max_arrived.max(at);
                }
                SequenceSide::Departed => {
                    any_departed = true;
                    max_departed = max_departed.max(at);
                }
                SequenceSide::Moved => {}
            }
        }

        // The anchor ranges that do not depend on where in the sequence a seam is, built at most
        // once per parent: the parent and its ancestors - which for an arrival are the arriving
        // element's own ancestors, exactly where a descendant or child query anchors on a witness -
        // and the elements before each of them, where `X:has(~ div .test)` anchors on an `X` that
        // precedes the div a departed witness sat in.
        let mut above: Option<Vec<StyleNodeID>> = None;
        let mut ancestor_predecessors: Option<Vec<StyleNodeID>> = None;

        for &LiveRelationalRoute { route, program, anchor } in live {
            let site = relational_route_site(routing, route);
            let anchor_posting = anchor
                .anchor_dispatch
                .has_selector_posting()
                .then_some(anchor.anchor_dispatch);
            // An element publishes every fact that holds on it as it connects, so a query resting
            // on one is reached from that fact and not from here. Only a query resting on none - a
            // negation, `*` - is invisible to an arrival, and only then are the anchors around an
            // arriving element asked whether it became their witness.
            let witnessing_arrival = any_arrived && anchor.witness_is_featureless;

            // An in-shadow-tree query anchors on the host of the tree the change happened in. A
            // child query's anchor hosts the tree the parent belongs to and a descendant query's
            // hosts the tree at the top of that chain, so either way there is at most one anchor.
            // A sibling axis leaves the hosted tree, so an in-shadow-tree query on one witnesses
            // nothing and is compiled away rather than reaching here.
            if anchor.match_in_shadow_tree {
                if !any_departed && !witnessing_arrival {
                    continue;
                }
                let host = match anchor.axis {
                    RelativeAxis::Child => self.tree.host_of(parent),
                    RelativeAxis::Descendant => {
                        let anchors_above = above.get_or_insert_with(|| {
                            std::iter::once(parent).chain(self.tree.ancestors(parent)).collect()
                        });
                        anchors_above.last().and_then(|&top| self.tree.host_of(top))
                    }
                    _ => None,
                };
                if let Some(host) = host {
                    self.route_relational_anchor(host, program, anchor.query, anchor_posting, &site, regions);
                }
                continue;
            }

            match anchor.axis {
                // A descendant stays a descendant of whatever it was a descendant of, and a move
                // inside one sequence changes no ancestor at all, so these reach only through the
                // element being a witness that arrived, or one that left and cannot say so.
                RelativeAxis::Descendant | RelativeAxis::Child => {
                    if !any_departed && !witnessing_arrival {
                        continue;
                    }
                    let anchors_above = above
                        .get_or_insert_with(|| std::iter::once(parent).chain(self.tree.ancestors(parent)).collect());
                    let candidates: &[StyleNodeID] = match (anchor.axis, any_departed) {
                        (RelativeAxis::Child, false) => &anchors_above[..1],
                        _ => anchors_above,
                    };
                    for &candidate in candidates {
                        self.route_relational_anchor(candidate, program, anchor.query, anchor_posting, &site, regions);
                    }
                }
                // The seam moves which elements are siblings of which, whoever made it, and the
                // anchors are as far back as the argument's leading chain reaches.
                RelativeAxis::NextSibling => {
                    for &(at, _) in &change.relational_records {
                        let at = clamp(at);
                        let reach = at.min(anchor.adjacent_reach as usize);
                        for &candidate in children[at - reach..at].iter().rev() {
                            self.route_relational_anchor(
                                candidate,
                                program,
                                anchor.query,
                                anchor_posting,
                                &site,
                                regions,
                            );
                        }
                    }
                }
                RelativeAxis::FollowingSibling => {
                    for &candidate in &children[..max_any] {
                        self.route_relational_anchor(candidate, program, anchor.query, anchor_posting, &site, regions);
                    }
                }
                // A witness under a sibling is not a sibling, so a witness that left, or arrived
                // carrying nothing, has to be looked for under a sibling of an ancestor as well as
                // under one of its own. A seam somebody else made still reaches back only as far as
                // the argument's leading chain does.
                RelativeAxis::NextSiblingSubtree => {
                    if any_departed || witnessing_arrival {
                        let wide_max = match anchor.witness_is_featureless {
                            true => max_departed.max(max_arrived),
                            false => max_departed,
                        };
                        for &candidate in &children[..wide_max] {
                            self.route_relational_anchor(
                                candidate,
                                program,
                                anchor.query,
                                anchor_posting,
                                &site,
                                regions,
                            );
                        }
                        self.route_relational_ancestor_predecessors(
                            parent,
                            &mut above,
                            &mut ancestor_predecessors,
                            program,
                            anchor.query,
                            anchor_posting,
                            &site,
                            regions,
                        );
                    }
                    for &(at, side) in &change.relational_records {
                        let seam_only = match side {
                            SequenceSide::Moved => true,
                            SequenceSide::Arrived => !anchor.witness_is_featureless,
                            SequenceSide::Departed => false,
                        };
                        if !seam_only {
                            continue;
                        }
                        let at = clamp(at);
                        let reach = at.min(anchor.adjacent_reach as usize);
                        for &candidate in children[at - reach..at].iter().rev() {
                            self.route_relational_anchor(
                                candidate,
                                program,
                                anchor.query,
                                anchor_posting,
                                &site,
                                regions,
                            );
                        }
                    }
                }
                RelativeAxis::FollowingSiblingSubtree => {
                    for &candidate in &children[..max_any] {
                        self.route_relational_anchor(candidate, program, anchor.query, anchor_posting, &site, regions);
                    }
                    if any_departed || witnessing_arrival {
                        self.route_relational_ancestor_predecessors(
                            parent,
                            &mut above,
                            &mut ancestor_predecessors,
                            program,
                            anchor.query,
                            anchor_posting,
                            &site,
                            regions,
                        );
                    }
                }
            }
        }
    }

    /// Route the elements before the parent and before each of its ancestors, which is where a
    /// sibling-subtree query anchors when its witness cannot be walked from.
    #[allow(clippy::too_many_arguments)]
    pub(super) fn route_relational_ancestor_predecessors(
        &mut self,
        parent: StyleNodeID,
        above: &mut Option<Vec<StyleNodeID>>,
        ancestor_predecessors: &mut Option<Vec<StyleNodeID>>,
        program: SelectorProgramID,
        query: RelativeQueryID,
        anchor_posting: Option<PostingKey>,
        site: &RoutingSite<'_>,
        regions: &mut ImpactRegions,
    ) {
        if ancestor_predecessors.is_none() {
            let anchors_above =
                above.get_or_insert_with(|| std::iter::once(parent).chain(self.tree.ancestors(parent)).collect());
            let mut found = Vec::new();
            for &ancestor in anchors_above.iter() {
                let mut sibling = self.tree.previous_element_sibling(ancestor);
                while let Some(previous) = sibling {
                    found.push(previous);
                    sibling = self.tree.previous_element_sibling(previous);
                }
            }
            *ancestor_predecessors = Some(found);
        }
        for &candidate in ancestor_predecessors.as_ref().unwrap() {
            self.route_relational_anchor(candidate, program, query, anchor_posting, site, regions);
        }
    }

    /// One candidate anchor: an element that does not carry the anchor compound's feature is not
    /// an anchor of this query at all, and the rest have the route's path applied from them.
    pub(super) fn route_relational_anchor(
        &mut self,
        candidate: StyleNodeID,
        program: SelectorProgramID,
        query: RelativeQueryID,
        anchor_posting: Option<PostingKey>,
        site: &RoutingSite<'_>,
        regions: &mut ImpactRegions,
    ) {
        if let Some(key) = anchor_posting {
            match self.facts.postings().lookup(key) {
                Lookup::Known(posting) if !posting.contains(candidate) => return,
                Lookup::KnownAbsent => return,
                Lookup::Known(_) | Lookup::Missing(_) => {}
            }
        }
        // An anchor whose retained witness still witnesses it was true and stays true: only
        // zero/nonzero witness transitions can affect selector truth, so nothing reached through
        // this anchor has moved and it drops out of the plan.
        if matches!(
            self.retained_witness_for_anchor(program, query, candidate),
            Lookup::Known(_)
        ) {
            self.counters.bump(Counter::RelationalAnchorsSkippedByWitness);
            return;
        }
        let region = ImpactRegion::follow(candidate, site.path, &self.tree);
        self.add_narrowed_region(region, site, regions);
    }

    /// Route a node whose neighbour moved to what a sibling combinator reaches through it.
    ///
    /// `A + B` and `A ~ B` are constraints on B, so B's truth moves when what precedes it does - and
    /// B need not be the subject. In `A + B .target` the sibling step is above the subject, and what
    /// the change reaches is what the rest of the path leads to from B. Both are one walk, because
    /// the node is exactly where the sibling step lands: the remaining steps run from it.
    ///
    /// An adjacent step names one element on either side of the change, so only a neighbour carrying
    /// the origin compound's own feature can have been the `A`. A general step reaches back through
    /// the whole sequence and is not bounded that way.
    #[allow(clippy::too_many_arguments)]
    pub(super) fn route_sibling_relations_to_constrained_side(
        &mut self,
        node: StyleNodeID,
        old: TreeRelations,
        new: TreeRelations,
        constrained_side_moved: bool,
        tree_routing: TreeRoutingMode<'_>,
        sibling_entries: &[SiblingEntry],
        sibling_candidates: &mut SiblingCandidateWorkspace,
        pending: &mut PendingSiblingRoutes,
    ) {
        let routing = Rc::clone(&self.routing);
        if constrained_side_moved {
            sibling_candidates.begin();
            sibling_candidates.candidates.extend(0..sibling_entries.len());
        } else {
            let origins: Vec<StyleNodeID> = [old.previous_element_sibling, new.previous_element_sibling]
                .into_iter()
                .flatten()
                .collect();
            self.sibling_entry_candidates(&origins, sibling_candidates);
        }
        for &index in &sibling_candidates.candidates {
            let entry = &sibling_entries[index];
            let point = routing.route(entry.route);
            let origin = routing.origin_dispatch_of(entry.route);
            let origin_required = routing.origin_required_of(entry.route);
            let path = routing.path_of(entry.route);
            let origin_can_match = |neighbour: Option<StyleNodeID>| {
                neighbour.is_some_and(|neighbour| {
                    self.node_carries_any(origin, neighbour, None)
                        && self.node_carries_all(origin_required, neighbour, None)
                })
            };
            let (origin_was_beside_it, exact_tree_evaluation) = match path.first() {
                Some(InverseStep::NextSibling) => {
                    let old_can_match = origin_can_match(old.previous_element_sibling);
                    let new_can_match = origin_can_match(new.previous_element_sibling);
                    // A predecessor that could not satisfy the compound leaves the whole entry
                    // unsatisfied, so an arrival can be decided from the new side alone. A
                    // departure-only transaction can instead compare the complete live entry with
                    // the reconstructed old sibling sequence, including when both predecessors pass
                    // this inexpensive compound prefilter.
                    let one_sided = !old_can_match
                        && new_can_match
                        && tree_routing.use_exact
                        && self.entry_is_monotone_in_the_tree_id(point.entry);
                    let exact = match (
                        tree_routing.use_exact
                            && tree_routing.has_before_sibling_relations
                            && self.entry_can_use_before_sibling_relations_id(point.entry),
                        one_sided,
                    ) {
                        (true, _) => Some(ExactTreeEvaluation::BeforeSiblingRelations),
                        (false, true) => Some(ExactTreeEvaluation::Arrival),
                        (false, false) => None,
                    };
                    (old_can_match || new_can_match, exact)
                }
                // A general-sibling constraint reads the set of preceding siblings, not which one
                // is immediately adjacent. A neighbour landing beside this stationary node changes
                // that set only through its own facts, which arrival or departure routing handles.
                // The seam itself matters only when the constrained node moved to another position.
                Some(InverseStep::FollowingSiblings) => (constrained_side_moved, None),
                _ => (false, None),
            };
            if !origin_was_beside_it {
                continue;
            }
            let region = ImpactRegion::follow(node, &path[1..], &self.tree);
            let routes = pending.regions_for(PendingSiblingRoute {
                route: entry.route,
                kind: exact_tree_evaluation.map_or(SiblingRouteKind::BeyondSibling, |_| SiblingRouteKind::ExactUnion),
                exact_tree_evaluation,
            });
            self.push_pending_region(routes, region);
        }
    }

    /// Route a node that moved to the siblings looking at it from where it landed.
    ///
    /// The mirror of routing its departure: `A + B` reaches B from A, so an `A` that moved is asked
    /// the same question from its new place as its old one. Nothing about its features changed, so
    /// no other path routes it.
    pub(super) fn route_arrival_to_siblings(
        &mut self,
        node: StyleNodeID,
        tree_routing: TreeRoutingMode<'_>,
        sibling_entries: &[SiblingEntry],
        sibling_candidates: &mut SiblingCandidateWorkspace,
        pending: &mut PendingSiblingRoutes,
    ) {
        let routing = Rc::clone(&self.routing);
        let candidate_entries = self.sibling_entry_candidates(std::slice::from_ref(&node), sibling_candidates);
        for &index in candidate_entries {
            let entry = &sibling_entries[index];
            let point = routing.route(entry.route);
            let origin = routing.origin_dispatch_of(entry.route);
            let origin_required = routing.origin_required_of(entry.route);
            let path = routing.path_of(entry.route);
            if !self.node_carries_any(origin, node, None) {
                continue;
            }
            if !self.node_carries_all(origin_required, node, None) {
                continue;
            }
            let region = ImpactRegion::follow(node, path, &self.tree);
            let exact_tree_evaluation = (tree_routing.use_exact
                && tree_routing.has_before_sibling_relations
                && self.entry_can_use_before_sibling_relations_id(point.entry))
            .then(|| {
                if matches!(path.first(), Some(InverseStep::FollowingSiblings))
                    && self.entry_is_monotone_in_the_tree_id(point.entry)
                {
                    ExactTreeEvaluation::MonotonicArrival
                } else {
                    ExactTreeEvaluation::BeforeSiblingRelations
                }
            });
            let routes = pending.regions_for(PendingSiblingRoute {
                route: entry.route,
                kind: exact_tree_evaluation.map_or(SiblingRouteKind::Full, |_| SiblingRouteKind::ExactUnion),
                exact_tree_evaluation,
            });
            self.push_pending_region(routes, region);
        }
    }

    pub(super) fn sibling_entry_candidates<'a>(
        &self,
        nodes: &[StyleNodeID],
        workspace: &'a mut SiblingCandidateWorkspace,
    ) -> &'a [usize] {
        workspace.begin();
        for &node in nodes {
            let mut append = |key| {
                for &route in self.routing.sibling_first_routes_for_origin(key) {
                    workspace.append(route);
                }
            };
            append(DispatchKey::Universal);
            self.for_each_dispatch_key_of_node(node, &mut append);
            for fact in self.facts.states_of_node(node).facts() {
                append(DispatchKey::State(fact));
            }
            if self.facts.heading_level_of(node) != 0 {
                append(DispatchKey::Heading);
            }
            if self.tree.parent(node).is_none() {
                append(DispatchKey::Root);
            }
        }
        &workspace.candidates
    }

    /// Route an element leaving to the siblings that were looking at it.
    ///
    /// `A + B` and `A ~ B` are constraints on B written in terms of an element beside it, so an `A`
    /// leaving changes B's truth. Those are also the only non-relational paths that reach outside
    /// the departing subtree: a descendant or child step from the element lands on nodes that left
    /// with it. The element's own facts say which selectors named it, and its old relations say
    /// where the path starts from now that it is gone.
    pub(super) fn route_departure_to_siblings(
        &mut self,
        node: StyleNodeID,
        relations: TreeRelations,
        tree_routing: TreeRoutingMode<'_>,
        sibling_entries: &[SiblingEntry],
        sibling_candidates: &mut SiblingCandidateWorkspace,
        pending: &mut PendingSiblingRoutes,
    ) {
        if sibling_entries.is_empty() {
            return;
        }
        let routing = Rc::clone(&self.routing);
        self.sibling_entry_candidates(std::slice::from_ref(&node), sibling_candidates);
        // The live state row may already reflect a clear recorded in this transaction, but the old
        // sibling route still depends on every state the node carried before it departed.
        let before_states = self
            .transaction_fact_view
            .as_ref()
            .and_then(|view| view.row_of(TransactionFactSide::Before, self.facts.primary(), node))
            .map(|(facts, row)| facts.states_of(row));
        if let Some(before_states) = before_states {
            for state in before_states.facts() {
                for &route in routing.sibling_first_routes_for_origin(DispatchKey::State(state)) {
                    sibling_candidates.append(route);
                }
            }
        }
        let candidate_entries = &sibling_candidates.candidates;
        let state_moved = before_states.is_some_and(|before| before != self.facts.states_of_node(node));
        let old_next_relocated = relations.next_element_sibling.is_some_and(|next| {
            self.tree.parent(next) == relations.parent
                && self.tree.previous_element_sibling(next) != relations.previous_element_sibling
                && tree_routing.tree_position_changed(next)
        });
        for &index in candidate_entries {
            let entry = &sibling_entries[index];
            let point = routing.route(entry.route);
            let origin = routing.origin_dispatch_of(entry.route);
            let origin_required = routing.origin_required_of(entry.route);
            let path = routing.path_of(entry.route);
            // The element has to have satisfied the compound the sibling combinator constrains, or
            // the selector never reached past it in the first place.
            if !self.node_carries_any(origin, node, None) {
                continue;
            }
            if !self.node_carries_all(origin_required, node, None) {
                continue;
            }
            let Some(region) = self.follow_from_departed_position(relations, path, old_next_relocated) else {
                continue;
            };
            // Exact old-tree evaluation shares current local facts. It cannot prove a route whose
            // departing origin also changed state because that state has already been committed.
            let departure_can_be_compared_exactly = tree_routing.use_exact
                && tree_routing.has_before_sibling_relations
                && !state_moved
                && self.entry_can_use_before_sibling_relations_id(point.entry);
            // The waypoints are the compounds the path passes through, checked by walking back from
            // a candidate. That walk arrives at the place the element had, which no longer leads to
            // it, so the candidate stands on its own features.
            let exact_tree_evaluation = departure_can_be_compared_exactly.then(|| {
                if matches!(path.first(), Some(InverseStep::FollowingSiblings))
                    && self.entry_is_monotone_in_the_tree_id(point.entry)
                {
                    ExactTreeEvaluation::MonotonicDeparture
                } else {
                    ExactTreeEvaluation::BeforeSiblingRelations
                }
            });
            let routes = pending.regions_for(PendingSiblingRoute {
                route: entry.route,
                kind: exact_tree_evaluation.map_or(SiblingRouteKind::Departure, |_| SiblingRouteKind::ExactUnion),
                exact_tree_evaluation,
            });
            self.push_pending_region(routes, region);
        }
    }

    /// Follow a transpose path from the place a departed element had.
    ///
    /// Only the first step needs the old relations: it is the one taken from the element itself, and
    /// the element is no longer on the tree to be stepped from. Every step after it starts from a
    /// node that is still there.
    pub(super) fn follow_from_departed_position(
        &self,
        relations: TreeRelations,
        path: &[InverseStep],
        old_next_relocated: bool,
    ) -> Option<ImpactRegion> {
        let first = match path.first()? {
            InverseStep::NextSibling => ImpactRegion::Node(relations.next_element_sibling?),
            // Everything past the place the element had starts at its old next sibling. Anchor
            // the range at that node's live position: the old previous sibling may itself have
            // moved to another parent in this transaction. If the next sibling moved too, the
            // old parent's current children are the narrowest live-tree range which still covers
            // every resident subject past the departure.
            InverseStep::FollowingSiblings => {
                let parent = relations.parent?;
                let next = relations.next_element_sibling?;
                if old_next_relocated || self.tree.parent(next) != Some(parent) {
                    ImpactRegion::Children(parent)
                } else if let Some(previous) = self.tree.previous_element_sibling(next) {
                    ImpactRegion::FollowingSiblings(previous)
                } else {
                    ImpactRegion::Children(parent)
                }
            }
            _ => return None,
        };
        Some(
            path[1..]
                .iter()
                .fold(first, |region, &step| region.step(step, &self.tree)),
        )
    }
    pub(super) fn sequence_origin_candidates<'a>(
        &self,
        node: StyleNodeID,
        index: &'a mut NthSequenceEntryIndex,
    ) -> &'a [usize] {
        advance_epoch(&mut index.epoch, 1, &mut [&mut index.candidate_epochs]);
        index.candidates.clear();
        let mut append_candidate = |candidate: usize| {
            if index.candidate_epochs.mark(candidate, index.epoch) {
                index.candidates.push(candidate);
            }
        };
        for candidate in index.unindexed.iter().copied() {
            append_candidate(candidate);
        }
        let mut append = |key| {
            if let Some(entries) = index.by_origin.get(&key) {
                for candidate in entries.iter().copied() {
                    append_candidate(candidate);
                }
            }
        };
        self.for_each_dispatch_key_of_node(node, &mut append);
        &index.candidates
    }

    pub(super) fn for_each_dispatch_key_of_node(&self, node: StyleNodeID, mut callback: impl FnMut(DispatchKey)) {
        let tag = self.facts.tag_of_node(node);
        if !tag.is_none() {
            callback(DispatchKey::TagName(tag));
        }
        let folded_tag = self.facts.folded_tag_of_node(node);
        if !folded_tag.is_none() && folded_tag != tag {
            callback(DispatchKey::TagName(folded_tag));
        }
        let id = self.facts.id_of_node(node);
        if !id.is_none() {
            callback(DispatchKey::Id(id));
        }
        for &class in self.facts.classes_of_node(node) {
            callback(DispatchKey::Class(class));
        }
        for attribute in self.facts.attributes_of_node(node) {
            for name in self.facts.attribute_name_keys(attribute) {
                callback(DispatchKey::AttributeName(name));
            }
        }
        let directionality = self.facts.directionality_of(node);
        if !directionality.is_none() {
            callback(DispatchKey::Directionality(directionality));
        }
        if let Some(row) = self.facts.primary().row_of(node) {
            for &state in self.facts.primary().custom_states_of(row) {
                callback(DispatchKey::CustomState(state));
            }
            for &part in self.facts.primary().parts_of(row) {
                callback(DispatchKey::Part(part));
            }
        }
        // The facts describe where the node ended up carrying what it ended up with. A key this
        // transaction is taking away named the node when a route still read it, so include that key.
        for &key in self.moved_features_of(node) {
            callback(key);
        }
    }

    /// Route what the transaction did to the child sequences it touched.
    ///
    /// The lookup that asks whether any attached selector depends on a child sequence at all is
    /// done once for the transaction rather than once per mutation, so a document with no
    /// positional selectors pays one lookup however much of its tree moved.
    pub(super) fn route_sequence_changes(
        &mut self,
        sequences: &SequenceChanges,
        changed_sheets: &[SheetID],
        tree_routing: TreeRoutingMode<'_>,
        regions: &mut ImpactRegions,
        workspace: &mut ImpactPlanningWorkspace,
    ) -> DeferredSequenceRoutes {
        if sequences.entries.is_empty() {
            return DeferredSequenceRoutes::default();
        }
        let routing = Rc::clone(&self.routing);
        let exact_before_sibling_relations = tree_routing.use_exact && tree_routing.has_before_sibling_relations;
        let use_cached_index = changed_sheets.is_empty();
        let mut entries = if use_cached_index {
            routing.live_sequence_entries(&self.program, &self.programs).to_vec()
        } else {
            let route_liveness = routing.route_liveness(&self.program, &self.programs);
            // A sheet changing attachment needs the routes from both sides of the transaction. The
            // current-program cache intentionally contains only the final side, so build this rare
            // union directly.
            let mut entries = Vec::new();
            for &route in routing.routes_for(RoutingKey::Structural) {
                let route_is_live = route_liveness.contains(route.index());
                let rule = routing.rule_of(route);
                if !route_is_live
                    && !self.program.rule_can_decide(rule)
                    && !changed_sheets.contains(&self.program.rule_sheet(rule))
                {
                    continue;
                }
                let point = routing.route(route);
                let (selector_program, _) = self.programs.entry_location(point.entry);
                if !route_is_live && self.program.rule_version(rule).selector_program != Some(selector_program) {
                    continue;
                }
                let operator = self
                    .programs
                    .get(selector_program)
                    .node(point.selector_node.expect("structural route has no operator"));
                if !matches!(operator, SelectorOp::Empty | SelectorOp::NthPosition(_)) {
                    continue;
                }
                entries.push(SequenceEntry {
                    route,
                    operator,
                    can_compare_exactly: self.entry_can_use_before_sibling_relations_id(point.entry),
                });
            }
            entries
        };
        for entry in &mut entries {
            entry.can_compare_exactly &= exact_before_sibling_relations;
        }
        if entries.is_empty() {
            return DeferredSequenceRoutes::default();
        }
        let entry_scratch_bytes = (entries.capacity() * size_of::<SequenceEntry>()) as u64;
        self.memory
            .reserve_required(MemoryCategory::BatchScratch, entry_scratch_bytes);
        // An entry the automaton answers exactly waits for the convergence walk: when the walk
        // re-compares every member of every touched sequence, its exact diffs subsume this
        // route's generic narrowing entirely, so routing it here would only be discarded. Shadow-
        // scope sequences stay on the immediate path, since the walk serves only the document
        // scope. Should the walk fail to cover, the held entries are routed afterwards.
        let sequences_are_document_scoped = sequences
            .iter()
            .all(|(parent, _)| self.tree.tree_scope(parent) == TreeScopeID::DOCUMENT);
        let (_, dispatch) = self.ranked_scope_program(TreeScopeID::DOCUMENT);
        let deferred_mask: Vec<bool> = entries
            .iter()
            .map(|entry| {
                sequences_are_document_scoped && dispatch.prefixes().contains_entry(routing.route(entry.route).entry)
            })
            .collect();
        let mask_bytes = deferred_mask.capacity() as u64;
        self.memory.reserve_required(MemoryCategory::BatchScratch, mask_bytes);
        let any_deferred = deferred_mask.iter().any(|&deferred| deferred);
        let any_immediate = deferred_mask.iter().any(|&deferred| !deferred);
        if any_immediate {
            let mut cached_entry_index =
                use_cached_index.then(|| routing.live_sequence_index(&self.program, &self.programs));
            let mut owned_entry_index = (!use_cached_index).then(|| SequenceEntryIndex::build(&entries, &routing));
            let entry_index_bytes = owned_entry_index.as_ref().map_or(0, SequenceEntryIndex::capacity_bytes);
            let entry_index = match (&mut cached_entry_index, &mut owned_entry_index) {
                (Some(cached), None) => &mut **cached,
                (None, Some(owned)) => owned,
                _ => unreachable!(),
            };
            self.memory
                .reserve_required(MemoryCategory::BatchScratch, entry_index_bytes);
            self.route_sequence_entries(
                sequences,
                &entries,
                entry_index,
                SequenceEntrySelection {
                    mask: &deferred_mask,
                    route_masked: false,
                },
                regions,
                workspace,
            );
            self.memory.release(MemoryCategory::BatchScratch, entry_index_bytes);
        }
        // The held entries and mask move onto the deferred lease, which charges them again.
        self.memory.release(MemoryCategory::BatchScratch, mask_bytes);
        self.memory.release(MemoryCategory::BatchScratch, entry_scratch_bytes);
        let mut deferred = DeferredSequenceRoutes::default();
        if any_deferred {
            deferred.entries = entries;
            deferred.deferred = deferred_mask;
        }
        let deferred_header_bytes = deferred.capacity_bytes();
        deferred
            .memory
            .resize_required_to(&mut self.memory, deferred_header_bytes);
        deferred
    }

    /// Route the selected sequence entries through every touched child sequence and narrow each
    /// entry's routed regions.
    fn route_sequence_entries(
        &mut self,
        sequences: &SequenceChanges,
        entries: &[SequenceEntry],
        entry_index: &mut SequenceEntryIndex,
        selection: SequenceEntrySelection<'_>,
        regions: &mut ImpactRegions,
        workspace: &mut ImpactPlanningWorkspace,
    ) {
        let routing = Rc::clone(&self.routing);
        let parent_emptiness = entry_index
            .empty
            .iter()
            .any(|&empty_index| selection.selects(empty_index));
        let mut pending_regions: Vec<Vec<ImpactRegion>> = (0..entries.len()).map(|_| Vec::new()).collect();
        let pending_outer_bytes = (pending_regions.capacity() * size_of::<Vec<ImpactRegion>>()) as u64;
        self.memory
            .reserve_required(MemoryCategory::BatchScratch, pending_outer_bytes);
        for (parent, change) in sequences.iter() {
            // The parent's own emptiness moved with the child count, and nothing else about it
            // did, so it is named only when something asks, attributed to the asking rules so a
            // retained answer refreshes those instead of poisoning to a full re-derivation.
            if parent_emptiness {
                for &empty_index in &entry_index.empty {
                    if !selection.selects(empty_index) {
                        continue;
                    }
                    let route = entries[empty_index].route;
                    let point = self.routing.route(route);
                    self.record_selector_truth_refresh(parent, Some((self.routing.rule_of(route), point.entry)));
                }
                regions.add(ImpactRegion::Node(parent), &mut self.counters);
            }
            self.add_sequence_region(
                parent,
                change,
                entries,
                entry_index,
                selection,
                &mut pending_regions,
                regions,
            );
        }
        let mut pending_inner_bytes = 0_u64;
        for (entry, routed_regions) in entries.iter().zip(&mut pending_regions) {
            if routed_regions.is_empty() {
                continue;
            }
            routed_regions.sort_unstable();
            routed_regions.dedup();
            pending_inner_bytes += (routed_regions.capacity() * size_of::<ImpactRegion>()) as u64;
            let site = entry.site(&routing);
            self.discard_regions_covered_by_subtree(routed_regions, &site, regions);
            self.add_narrowed_regions_with_workspace(routed_regions, &site, regions, workspace);
        }
        self.memory
            .release(MemoryCategory::BatchScratch, pending_outer_bytes + pending_inner_bytes);
    }

    /// Flush the sequence routes held back for the convergence walk: drop them when the walk's
    /// exact diffs covered every touched sequence, route and narrow them the ordinary way
    /// otherwise.
    pub(super) fn flush_deferred_sequence_routes(
        &mut self,
        mut deferred: DeferredSequenceRoutes,
        covered: bool,
        sequences: &SequenceChanges,
        regions: &mut ImpactRegions,
        workspace: &mut ImpactPlanningWorkspace,
    ) {
        let entries = std::mem::take(&mut deferred.entries);
        let deferred_mask = std::mem::take(&mut deferred.deferred);
        let covered = covered || regions.covers_document();
        if covered || entries.is_empty() {
            return;
        }
        let routing = Rc::clone(&self.routing);
        let mut entry_index = SequenceEntryIndex::build(&entries, &routing);
        let entry_index_bytes = entry_index.capacity_bytes();
        self.memory
            .reserve_required(MemoryCategory::BatchScratch, entry_index_bytes);
        self.route_sequence_entries(
            sequences,
            &entries,
            &mut entry_index,
            SequenceEntrySelection {
                mask: &deferred_mask,
                route_masked: true,
            },
            regions,
            workspace,
        );
        self.memory.release(MemoryCategory::BatchScratch, entry_index_bytes);
    }

    /// Whether an of-type position kept the same truth across this transaction.
    fn nth_of_type_truth_unchanged(
        &mut self,
        nth: NthPosition,
        node: StyleNodeID,
        workspace: &MatchEvaluationWorkspace,
    ) -> bool {
        let Some(view) = self
            .transaction_fact_view
            .as_ref()
            .filter(|view| view.before_sibling_relations_available)
        else {
            return false;
        };
        let resident_facts = self.facts.primary();
        let old_count = MatchEvaluator::new(&self.tree, resident_facts)
            .with_transaction_fact_view(view, TransactionFactSide::Before)
            .with_match_workspace(workspace, MatchEvaluationSide::OldTree)
            .indexed_sibling_position(nth, node, &mut self.counters)
            .ok()
            .flatten();
        let new_count = MatchEvaluator::new(&self.tree, resident_facts)
            .with_transaction_fact_view(view, TransactionFactSide::After)
            .with_match_workspace(workspace, MatchEvaluationSide::Current)
            .indexed_sibling_position(nth, node, &mut self.counters)
            .ok()
            .flatten();
        old_count
            .zip(new_count)
            .is_some_and(|(old, new)| nth.matches_count(old) == nth.matches_count(new))
    }

    /// Name the children whose position in the sequence actually moved.
    ///
    /// A child arriving or leaving at index `i` shifts the count-from-the-start of everything from
    /// `i` onwards, and the count-from-the-end of everything before it. Each positional selector
    /// then decides for itself which of those it can name: `:first-child` asks about one position
    /// and `:nth-child(3)` about three, and a selector with a step sees the whole sequence. A
    /// selector whose subject carries features narrows further, so an insertion between two divs
    /// costs nothing to a document whose positional rules are about table cells.
    #[allow(clippy::too_many_arguments)]
    pub(super) fn add_sequence_region(
        &mut self,
        parent: StyleNodeID,
        change: &SequenceChange,
        entries: &[SequenceEntry],
        entry_index: &mut SequenceEntryIndex,
        selection: SequenceEntrySelection<'_>,
        pending_regions: &mut [Vec<ImpactRegion>],
        regions: &mut ImpactRegions,
    ) {
        let routing = Rc::clone(&self.routing);
        let children = &change.children;
        if children.is_empty() {
            return;
        }
        let sequence_match_workspace = MatchEvaluationWorkspace::default();
        // What an entry's selector says about the parent of its positional test holds for the
        // whole sequence, so it is decided once per entry here rather than once per moved child.
        const PARENT_UNDECIDED: u8 = 0;
        const PARENT_ADMITS: u8 = 1;
        const PARENT_REJECTS: u8 = 2;
        let mut parent_verdicts = vec![PARENT_UNDECIDED; entries.len()];
        let parent_verdict_bytes = (parent_verdicts.capacity() * size_of::<u8>()) as u64;
        self.memory
            .reserve_required(MemoryCategory::BatchScratch, parent_verdict_bytes);
        // Where in the sequence the earliest and the latest change happened. A change nobody could
        // place leaves the whole sequence moved, because that is all that is still provable.
        let (first_moved, last_moved) = match change.span {
            Some(span) if !change.unlocated => span,
            _ => (0, children.len()),
        };

        // A child arriving or leaving changes whether the parent is empty, and nothing else about
        // it. The parent is the element that test is asked of, so it is the witness.
        for &entry_index in &entry_index.empty {
            if !selection.selects(entry_index) {
                continue;
            }
            let entry = &entries[entry_index];
            let point = routing.route(entry.route);
            let path = routing.path_of(entry.route);
            let site = RoutingSite {
                subject: routing.subject_dispatch_of(entry.route),
                subject_required: routing.subject_required_of(entry.route),
                position: routing.subject_position_of(entry.route),
                path,
                waypoints: routing.waypoints_of(entry.route),
                in_flux: None,
                exact_entry: None,
                exact_tree_evaluation: None,
                refresh_rule: Some((routing.rule_of(entry.route), point.entry)),
            };
            match point.anchor {
                Some(anchor) => {
                    let (program, _) = self.programs.entry_location(point.entry);
                    self.route_from_anchors(parent, program, anchor, &site, regions);
                }
                None => {
                    let region = ImpactRegion::follow(parent, path, &self.tree);
                    self.push_pending_region(&mut pending_regions[entry_index], region);
                }
            }
        }

        for group in &mut entry_index.nth {
            let nth = group.nth;
            // Only the side of the change that moved can have moved: counted from the start, the
            // children at or after the earliest change; counted from the end, those before the
            // latest one - plus the place an element relocated into, whose own count from the end
            // moved with it while nothing after it moved at all.
            let last_from_end = match change.last_relocation {
                Some(at) => last_moved.max(at.saturating_add(1)),
                None => last_moved,
            };
            let moved = match nth.from_end {
                true => 0..last_from_end.min(children.len()),
                false => first_moved.min(children.len())..children.len(),
            };
            // A test that counts a subsequence - `:nth-of-type()`, `:nth-child(... of ...)` - counts
            // something other than the places in this sequence, so a place cannot be turned into a
            // count and back.
            let counts_this_sequence = !nth.of_type && nth.of_selector.is_none();
            // `:nth-child(k)` matches at one position, so an element's answer flips only if its
            // count crossed k - which happens at k-1 and k, and nowhere else. A test with a step
            // matches at many positions, and every count on the moved side shifts.
            // The positional test is not always on the subject: `#list > :first-child .leaf` moves
            // for the descendants of the child whose count crossed. So the entry's own path is
            // followed from each such child to the subjects it reaches.
            let mut name = |engine: &mut Self, at: usize| {
                if !moved.contains(&at) {
                    return;
                }
                let Some(&child) = children.get(at) else {
                    return;
                };
                if nth.of_type && !change.has_unidentified_type {
                    let tag = engine.facts.tag_of_node(child);
                    if !tag.is_none() && !change.changed_types.contains(&(tag, engine.facts.namespace_of(child))) {
                        return;
                    }
                }
                let candidates = engine.sequence_origin_candidates(child, group);
                if !candidates.iter().any(|&candidate| selection.selects(candidate)) {
                    return;
                }
                // The moved side names every child whose count COULD have crossed the test, but
                // multiple mutations in one flush routinely cancel: a child shifted by a net even
                // amount keeps its `2n+1` truth. The retained before-side sequence holds the exact
                // old position, so evaluate the test on both sides and drop the children whose
                // truth provably did not move. A relative positional input stays routed: it is a
                // possible witness, and witness truth is judged at its anchors, not here.
                let truth_unchanged_on_both_sides = change.arriving_nodes.binary_search(&child).is_err()
                    && match counts_this_sequence {
                        true => engine.transaction_fact_view.as_ref().is_some_and(|view| {
                            view.before_sibling_relations_available
                                && view
                                    .before_sibling_geometry
                                    .sibling_positions(child)
                                    .is_some_and(|old| {
                                        let new_count = match nth.from_end {
                                            true => children.len() - at,
                                            false => at + 1,
                                        } as i64;
                                        let old_count = i64::from(match nth.from_end {
                                            true => old.from_end,
                                            false => old.from_start,
                                        });
                                        nth.matches_count(new_count) == nth.matches_count(old_count)
                                    })
                        }),
                        false if nth.of_type => {
                            engine.nth_of_type_truth_unchanged(nth, child, &sequence_match_workspace)
                        }
                        false => false,
                    };
                for &entry_index in candidates {
                    if !selection.selects(entry_index) {
                        continue;
                    }
                    let entry = &entries[entry_index];
                    let point = routing.route(entry.route);
                    if truth_unchanged_on_both_sides && point.anchor.is_none() {
                        continue;
                    }
                    let path = routing.path_of(entry.route);
                    // A whole sibling sequence shares one parent, so what the selector says the
                    // parent of the positional test is rejects the sequence in one lookup.
                    let parent_verdict = &mut parent_verdicts[entry_index];
                    if *parent_verdict == PARENT_UNDECIDED {
                        *parent_verdict =
                            match engine.node_carries_any(routing.parent_dispatch_of(entry.route), parent, None) {
                                true => PARENT_ADMITS,
                                false => PARENT_REJECTS,
                            };
                    }
                    if *parent_verdict == PARENT_REJECTS {
                        continue;
                    }
                    // A non-relational positional input sits in one compound of the selector. A
                    // child that cannot satisfy the rest of that compound cannot route through it.
                    //
                    // A relational position routes a possible witness to its anchors. Keep that
                    // conservative: a relocation has two witness positions, while this sequence
                    // scan sees only the final node and its facts.
                    if point.anchor.is_none()
                        && (!engine.node_carries_any(routing.origin_dispatch_of(entry.route), child, None)
                            || !engine.node_carries_all(routing.origin_required_of(entry.route), child, None))
                    {
                        continue;
                    }
                    let site = entry.site(&routing);
                    // A positional test inside a `:has()` argument makes the child a possible
                    // witness, not a subject: its anchors have to be resolved before the path
                    // continues.
                    match point.anchor {
                        Some(anchor) => {
                            let (program, _) = engine.programs.entry_location(point.entry);
                            engine.route_from_anchors(child, program, anchor, &site, regions);
                        }
                        None => {
                            let region = ImpactRegion::follow(child, path, &engine.tree);
                            engine.push_pending_region(&mut pending_regions[entry_index], region);
                        }
                    }
                }
            };
            // A subsequence-counting test names every element on the moved side of an affected
            // subsequence instead; the closure above restricts an `of-type` subsequence to the
            // qualified types that actually joined or left.
            match counts_this_sequence && nth.step == 0 && nth.offset >= 1 {
                true => {
                    // An arrival pushes counts up and a departure pulls them down, so an element
                    // whose count is `c` now held anything from `c - arrivals` to `c + departures`.
                    // It crossed the one count the selector names exactly when that lies in the
                    // range, which is what bounds the positions to name. One mutation of one
                    // sequence therefore names two of them, and nothing widens except by as much as
                    // the sequence actually moved.
                    let offset = nth.offset as usize;
                    let lowest = offset.saturating_sub(change.departures as usize).max(1);
                    let highest = offset.saturating_add(change.arrivals as usize);
                    for count in lowest..=highest {
                        let at = match nth.from_end {
                            true => match children.len().checked_sub(count) {
                                Some(at) => at,
                                None => continue,
                            },
                            false => count - 1,
                        };
                        name(self, at);
                    }
                }
                false => {
                    for at in moved.clone() {
                        if counts_this_sequence && change.last_relocation.is_none() {
                            let Some(&child) = children.get(at) else {
                                continue;
                            };
                            if change.arriving_nodes.binary_search(&child).is_err() {
                                let current = match nth.from_end {
                                    true => children.len() - at,
                                    false => at + 1,
                                } as i64;
                                let first = (current - i64::from(change.arrivals)).max(1);
                                let last = current + i64::from(change.departures);
                                if !nth.could_change_from_range(current, first, last) {
                                    continue;
                                }
                            }
                        }
                        name(self, at);
                    }
                }
            }
        }
        drop(parent_verdicts);
        self.memory.release(MemoryCategory::BatchScratch, parent_verdict_bytes);
        let sequence_match_workspace_bytes = sequence_match_workspace.capacity_bytes();
        self.memory
            .reserve_required(MemoryCategory::BatchScratch, sequence_match_workspace_bytes);
        drop(sequence_match_workspace);
        self.memory
            .release(MemoryCategory::BatchScratch, sequence_match_workspace_bytes);
    }

    /// Route a possible relational witness through the anchors whose truth it can flip.
    ///
    /// The anchor set is bounded by the inverse of the query's axis - one element for a child or
    /// adjacent-sibling query, the ancestor path for a descendant one - and then filtered to the
    /// ones that actually carry the anchor compound's own feature. `.card:has(.error) .button`
    /// therefore reaches the `.button` elements under the `.card` ancestors of the changed
    /// `.error`, rather than every descendant of every ancestor.
    pub(super) fn route_from_anchors(
        &mut self,
        witness: StyleNodeID,
        program: SelectorProgramID,
        anchor: RelativeAnchor,
        site: &RoutingSite<'_>,
        regions: &mut ImpactRegions,
    ) {
        if anchor.input_is_on_the_anchor {
            self.counters.bump(Counter::RelationalAnchorsConsidered);
            let region = ImpactRegion::follow(witness, site.path, &self.tree);
            self.add_narrowed_region(region, site, regions);
            return;
        }

        // The input is a step away from the witness, so no walk from it finds the anchors: the
        // step can leave the anchor's subtree, which puts the changed element above the anchor
        // rather than below it. What the query still says is what a witness of it must carry, and
        // the elements carrying that are enumerable. Route from each of them instead.
        if !anchor.input_is_on_the_witness {
            self.route_from_possible_witnesses(program, anchor, site, regions);
            return;
        }

        let mut anchors = Vec::new();
        match anchor.match_in_shadow_tree {
            true => possible_hosting_anchors(anchor.axis, witness, &self.tree, |candidate| {
                anchors.push(candidate);
            }),
            false => possible_anchors(anchor.axis, witness, &self.tree, None, |candidate| {
                anchors.push(candidate);
            }),
        }
        self.counters
            .add(Counter::RelationalAnchorsConsidered, anchors.len() as u64);

        let anchor_posting = anchor
            .anchor_dispatch
            .has_selector_posting()
            .then_some(anchor.anchor_dispatch);
        for candidate in anchors {
            // An ancestor that does not carry the anchor compound's feature is not an anchor of
            // this query at all.
            if let Some(key) = anchor_posting {
                match self.facts.postings().lookup(key) {
                    Lookup::Known(posting) if !posting.contains(candidate) => continue,
                    Lookup::KnownAbsent => continue,
                    Lookup::Known(_) | Lookup::Missing(_) => {}
                }
            }
            // An anchor whose retained witness still witnesses it was true and stays true: only
            // zero/nonzero witness transitions can affect selector truth, so nothing reached
            // through this anchor has moved and it drops out of the plan. A position-testing
            // argument is excluded: a sibling mutation can flip such a witness without ever
            // touching it, so its retained record proves nothing here.
            if !self
                .programs
                .get(program)
                .subtree_tests_position(self.programs.get(program).relative_query(anchor.query).compound)
                && matches!(
                    self.retained_witness_for_anchor(program, anchor.query, candidate),
                    Lookup::Known(_)
                )
            {
                self.counters.bump(Counter::RelationalAnchorsSkippedByWitness);
                continue;
            }
            let region = ImpactRegion::follow(candidate, site.path, &self.tree);
            self.add_narrowed_region(region, site, regions);
        }
    }

    /// Route a relational query from every element that could witness it.
    ///
    /// Used when no single changed element stands in for the witness: the input is a step away
    /// from one, or the change reshaped a child sequence the argument reaches across. What the
    /// query still says is what a witness of it must carry, and the elements carrying that are
    /// enumerable from the posting.
    pub(super) fn route_from_possible_witnesses(
        &mut self,
        program: SelectorProgramID,
        anchor: RelativeAnchor,
        site: &RoutingSite<'_>,
        regions: &mut ImpactRegions,
    ) {
        if !anchor.witness_dispatch.has_selector_posting() {
            self.add_narrowed_region(ImpactRegion::Document, site, regions);
            return;
        }
        let candidates: Vec<StyleNodeID> = match self.facts.postings().lookup(anchor.witness_dispatch) {
            Lookup::Known(posting) => posting.candidates().collect(),
            Lookup::KnownAbsent => Vec::new(),
            Lookup::Missing(_) => {
                self.add_narrowed_region(ImpactRegion::Document, site, regions);
                return;
            }
        };
        if candidates.len() > SMALL_CANDIDATE_SOURCE
            && candidates.len() * SELECTIVE_SHARE_DIVISOR > self.tree.connected_element_count().max(1) as usize
        {
            self.add_narrowed_region(ImpactRegion::Document, site, regions);
            return;
        }
        for candidate in candidates {
            self.route_from_anchors(
                candidate,
                program,
                RelativeAnchor {
                    input_is_on_the_witness: true,
                    input_is_on_the_anchor: false,
                    ..anchor
                },
                site,
                regions,
            );
        }
    }

    /// Whether `key` names the fact the input moved on `node`.
    ///
    /// An attribute has two names - its own, and the any-namespace form `[*|x]` dispatches under -
    /// and an input names only the first. The element is removed from both postings as the attribute
    /// leaves, so a `[*|x]` subject would find no candidate at all unless the shared form counts as
    /// in flux too. This is what keeps a removal routable: the posting no longer lists the element,
    /// and the fact that just moved is the reason it does not.
    pub(super) fn key_is_in_flux(
        &self,
        key: DispatchKey,
        node: StyleNodeID,
        in_flux: Option<(StyleNodeID, DispatchKey)>,
    ) -> bool {
        let Some((flux_node, flux_key)) = in_flux else {
            return false;
        };
        if flux_node != node {
            return false;
        }
        if flux_key == key {
            return true;
        }
        match (flux_key, key) {
            (DispatchKey::AttributeName(moved), DispatchKey::AttributeName(named)) => self
                .facts
                .attribute_name_keys(moved)
                .any(|candidate| candidate == named),
            _ => false,
        }
    }

    /// Whether `node` carries at least one of `keys`. An empty set distinguishes nothing, so it
    /// admits every node rather than rejecting every one. A feature moving in this transaction is
    /// admitted from either side: structural routing has no one feature input to pass as `in_flux`.
    pub(super) fn node_carries_any(
        &self,
        keys: &[DispatchKey],
        node: StyleNodeID,
        in_flux: Option<(StyleNodeID, DispatchKey)>,
    ) -> bool {
        if keys.is_empty() {
            return true;
        }
        keys.iter().any(|&key| self.node_carries(key, node, in_flux))
    }

    /// Whether `node` carries every independently necessary key of one compound.
    pub(super) fn node_carries_all(
        &self,
        keys: &[DispatchKey],
        node: StyleNodeID,
        in_flux: Option<(StyleNodeID, DispatchKey)>,
    ) -> bool {
        keys.iter().all(|&key| self.node_carries(key, node, in_flux))
    }

    pub(super) fn node_carries(
        &self,
        key: DispatchKey,
        node: StyleNodeID,
        in_flux: Option<(StyleNodeID, DispatchKey)>,
    ) -> bool {
        if self.key_is_in_flux(key, node, in_flux) || self.moved_features_of(node).contains(&key) {
            return true;
        }
        let is_root = self.tree.parent(node).is_none();
        let resident_facts = self.facts.primary();
        if resident_facts
            .row_of(node)
            .is_some_and(|row| resident_facts.carries_dispatch_key(row, key, is_root))
        {
            return true;
        }
        if self
            .transaction_fact_view
            .as_ref()
            .and_then(|view| view.row_of(TransactionFactSide::Before, resident_facts, node))
            .map(|(facts, row)| facts.carries_dispatch_key(row, key, is_root))
            == Some(true)
        {
            return true;
        }
        if resident_facts.row_of(node).is_some() {
            return false;
        }
        self.facts.carries_local_dispatch_key(node, key).unwrap_or_else(|| {
            !key.has_selector_posting()
                || match self.facts.postings().lookup(key) {
                    Lookup::Known(posting) => posting.contains(node),
                    Lookup::KnownAbsent => false,
                    Lookup::Missing(_) => true,
                }
        })
    }

    /// Whether the path a route takes to `node` can actually be walked back.
    ///
    /// A region says where a change reaches; it says nothing about what the selector required on
    /// the way. Walking the path backwards from a candidate and demanding each intermediate
    /// compound's key at the node that step lands on is a necessary condition of the selector, so a
    /// candidate that fails it could not have matched before the change either.
    ///
    /// The frontier is bounded: past a handful of nodes the membership checks cost more than the
    /// rejections save, and the candidate stands.
    pub(super) fn path_meets_waypoints(
        &self,
        path: &[InverseStep],
        waypoints: &[DispatchKey],
        node: StyleNodeID,
        in_flux: Option<(StyleNodeID, DispatchKey)>,
    ) -> bool {
        const MAX_FRONTIER: usize = 32;
        if waypoints.len() != path.len() || path.is_empty() {
            return true;
        }
        // A shadow or sequence step anywhere in the path breaks the walk back: a node inside a
        // shadow tree has no ancestor chain that reaches its host, so a later ancestral step would
        // find nothing and reject a candidate that does match.
        if !path.iter().all(|step| {
            matches!(
                step,
                InverseStep::Descendants
                    | InverseStep::Children
                    | InverseStep::NextSibling
                    | InverseStep::FollowingSiblings
            )
        }) {
            return true;
        }
        let mut frontier = [None; MAX_FRONTIER];
        frontier[0] = Some(node);
        let mut frontier_len = 1;
        for index in (1..path.len()).rev() {
            let mut next = [None; MAX_FRONTIER];
            let mut next_len = 0;
            let mut append = |candidate| {
                if next_len == MAX_FRONTIER {
                    return false;
                }
                next[next_len] = Some(candidate);
                next_len += 1;
                true
            };
            for current in frontier[..frontier_len].iter().flatten().copied() {
                match path[index] {
                    InverseStep::Descendants => {
                        for ancestor in self.tree.ancestors(current) {
                            if !append(ancestor) {
                                return true;
                            }
                        }
                    }
                    InverseStep::Children => {
                        if let Some(parent) = self.tree.parent(current)
                            && !append(parent)
                        {
                            return true;
                        }
                    }
                    InverseStep::NextSibling => {
                        if let Some(previous) = self.tree.previous_element_sibling(current)
                            && !append(previous)
                        {
                            return true;
                        }
                    }
                    InverseStep::FollowingSiblings => {
                        let mut sibling = self.tree.previous_element_sibling(current);
                        while let Some(previous) = sibling {
                            if !append(previous) {
                                return true;
                            }
                            sibling = self.tree.previous_element_sibling(previous);
                        }
                    }
                    _ => return true,
                }
            }
            let key = waypoints[index - 1];
            if key != DispatchKey::Universal {
                let mut kept = 0;
                for read in 0..next_len {
                    let candidate = next[read].unwrap();
                    if self.node_carries_any(std::slice::from_ref(&key), candidate, in_flux) {
                        next[kept] = Some(candidate);
                        kept += 1;
                    }
                }
                next_len = kept;
            }
            if next_len == 0 {
                return false;
            }
            frontier = next;
            frontier_len = next_len;
        }
        true
    }

    /// Whether an entry is monotonic under a positive tree arrival.
    ///
    /// An arriving element is routed from the facts it publishes, so the new side alone decides
    /// unless a negation, `:empty`, or a positional test can turn another element's answer off.
    #[must_use]
    pub(super) fn entry_is_monotone_in_the_tree(&self, program: SelectorProgramID, entry: u32) -> bool {
        let compiled = self.programs.get(program);
        compiled
            .entries()
            .get(entry as usize)
            .is_some_and(|entry| entry.is_monotone_under_arrivals())
    }

    #[must_use]
    fn entry_is_monotone_in_the_tree_id(&self, entry: EntryID) -> bool {
        let (program, index) = self.programs.entry_location(entry);
        self.entry_is_monotone_in_the_tree(program, index)
    }

    #[must_use]
    pub(super) fn entry_can_use_before_sibling_relations(&self, program: SelectorProgramID, entry: u32) -> bool {
        let compiled = self.programs.get(program);
        compiled
            .entries()
            .get(entry as usize)
            .is_some_and(|entry| entry.can_use_before_sibling_relations() && entry.observes_sibling_relation())
    }

    #[must_use]
    fn entry_can_use_before_sibling_relations_id(&self, entry: EntryID) -> bool {
        let (program, index) = self.programs.entry_location(entry);
        self.entry_can_use_before_sibling_relations(program, index)
    }

    pub(super) fn candidate_changes_exact_tree(
        &mut self,
        node: StyleNodeID,
        program: SelectorProgramID,
        entry: u32,
        evaluation: ExactTreeEvaluation,
    ) -> ExactEntryResult {
        let mut covered = std::mem::take(&mut self.exact_covered_scratch);
        covered.clear();
        covered.push(node);
        let mut sibling_window = INITIAL_SIBLING_FACT_WINDOW;
        let old_matches = match evaluation {
            ExactTreeEvaluation::Arrival => None,
            ExactTreeEvaluation::MonotonicArrival
            | ExactTreeEvaluation::MonotonicDeparture
            | ExactTreeEvaluation::BeforeSiblingRelations => self
                .transaction_fact_view
                .as_ref()
                .filter(|view| view.retained_truth_available)
                .and_then(|_| self.retained_entry_matches(node, program, entry)),
        };
        let change = loop {
            let result = {
                let compiled = self.programs.get(program);
                let exact_entry = &compiled.entries()[entry as usize];
                evaluation.candidate_changes(
                    &self.tree,
                    self.facts.primary(),
                    (compiled, exact_entry),
                    node,
                    old_matches,
                    self.transaction_fact_view.as_ref(),
                    &self.match_workspace,
                    &mut self.counters,
                )
            };
            match result {
                Ok(change) => break change,
                Err(incomplete) => {
                    if self
                        .widen_fact_coverage(&mut covered, incomplete, &mut sibling_window)
                        .is_err()
                    {
                        break Lookup::Missing(ExactEntryGap);
                    }
                }
            }
        };
        self.exact_covered_scratch = covered;
        change
    }

    /// Whether one routed entry actually changes truth on this candidate.
    ///
    /// Transpose routing deliberately produces a bounded superset. When the transaction only moved
    /// local facts, both sides of the exact match program are available: the fact store is the
    /// new side and the journal retained the old values. This rejects redundant witnesses such as
    /// adding `.guard` above a `.target` that already had a nearer `.guard` ancestor.
    pub(super) fn candidate_changes_exact_entry(
        &mut self,
        node: StyleNodeID,
        site: &RoutingSite<'_>,
    ) -> ExactEntryResult {
        let Some((rule, entry_id)) = site.exact_entry else {
            return Lookup::Missing(ExactEntryGap);
        };
        let (program, entry) = self.programs.entry_location(entry_id);
        if self.transaction_fact_view.is_none() {
            return Lookup::Missing(ExactEntryGap);
        }
        {
            let compiled = self.programs.get(program);
            let sheet = self.program.rule_sheet(rule);
            if self.tree.tree_scope(node) != TreeScopeID::DOCUMENT
                || compiled.can_leave_its_scope()
                || !self
                    .program
                    .sheet_attachments(sheet)
                    .iter()
                    .any(|attachment| attachment.tree_scope == TreeScopeID::DOCUMENT)
            {
                return Lookup::Missing(ExactEntryGap);
            }
            if compiled.entries().get(entry as usize).is_none() {
                return Lookup::Missing(ExactEntryGap);
            }
        }

        if let Some(exact_tree_evaluation) = site.exact_tree_evaluation {
            return self.candidate_changes_exact_tree(node, program, entry, exact_tree_evaluation);
        }

        if !self
            .transaction_fact_view
            .as_ref()
            .is_some_and(|view| view.retained_truth_available)
        {
            return Lookup::Missing(ExactEntryGap);
        }

        let retained_old_matches = self
            .transaction_fact_view
            .as_ref()
            .filter(|view| view.retained_truth_available)
            .and_then(|_| self.retained_entry_matches(node, program, entry));
        let mut covered = std::mem::take(&mut self.exact_covered_scratch);
        covered.clear();
        covered.push(node);
        let mut sibling_window = INITIAL_SIBLING_FACT_WINDOW;
        let changed = loop {
            let result = {
                let resident_facts = self.facts.primary();
                let view = self
                    .transaction_fact_view
                    .as_ref()
                    .expect("exact evaluation has a transaction fact view");
                let compiled = self.programs.get(program);
                let exact_entry = &compiled.entries()[entry as usize];
                let new_matches = MatchEvaluator::new(&self.tree, resident_facts)
                    .with_transaction_fact_view(view, TransactionFactSide::After)
                    .matches_entry_for_program(program, compiled, exact_entry, node, &mut self.counters);
                let old_matches = match retained_old_matches {
                    Some(old_matches) => Ok(old_matches),
                    None => MatchEvaluator::new(&self.tree, resident_facts)
                        .with_transaction_fact_view(view, TransactionFactSide::Before)
                        .matches_entry_for_program(program, compiled, exact_entry, node, &mut self.counters),
                };
                (old_matches, new_matches)
            };
            match result {
                (Ok(old_matches), Ok(new_matches)) if old_matches != new_matches => {
                    break Lookup::Known(if new_matches {
                        SetChange::Added
                    } else {
                        SetChange::Removed
                    });
                }
                (Ok(_), Ok(_)) => break Lookup::KnownAbsent,
                (Err(incomplete), _) | (_, Err(incomplete)) => {
                    if self
                        .widen_fact_coverage(&mut covered, incomplete, &mut sibling_window)
                        .is_err()
                    {
                        break Lookup::Missing(ExactEntryGap);
                    }
                }
            }
        };
        self.exact_covered_scratch = covered;
        changed
    }

    #[must_use]
    pub(super) fn exact_batch_is_available(&self, routed_regions: &[ImpactRegion], site: &RoutingSite<'_>) -> bool {
        site.exact_entry.is_some()
            && (site.exact_tree_evaluation.is_some()
                || self
                    .transaction_fact_view
                    .as_ref()
                    .is_some_and(|view| view.retained_truth_available))
            && !routed_regions
                .iter()
                .any(|region| matches!(region, ImpactRegion::Document | ImpactRegion::TreeScope(_)))
    }

    pub(super) fn compile_region_batch(
        &mut self,
        routed_regions: &[ImpactRegion],
        plan: &ImpactRegions,
        workspace: Option<&mut ImpactPlanningWorkspace>,
    ) -> Rc<ImpactRegionBatch> {
        if let Some(workspace) = workspace {
            if let Some(batch) = workspace.batches.get(routed_regions) {
                return Rc::clone(batch);
            }

            let batch = Rc::new(plan.compile_union(routed_regions, &self.tree, None));
            self.record_compiled_region_batch(&batch);
            workspace.insert_batch(routed_regions, Rc::clone(&batch));
            workspace.settle_memory(&mut self.memory);
            return batch;
        }

        let batch = Rc::new(plan.compile_union(routed_regions, &self.tree, None));
        self.record_compiled_region_batch(&batch);
        batch
    }

    pub(super) fn record_compiled_region_batch(&mut self, batch: &ImpactRegionBatch) {
        if let Some(intervals) = batch.interval_count() {
            self.counters.add(Counter::ExactRegionBatchIntervals, intervals as u64);
        }
        self.counters
            .add(Counter::ExactRegionBatchNodes, batch.node_count() as u64);
    }

    /// Run one exact candidate pass over the union of several transpose regions.
    pub(super) fn add_exact_batch_regions(
        &mut self,
        routed_regions: &[ImpactRegion],
        site: &RoutingSite<'_>,
        regions: &mut ImpactRegions,
        workspace: Option<&mut ImpactPlanningWorkspace>,
        compiled_batch: Option<Rc<ImpactRegionBatch>>,
    ) -> bool {
        if !self.exact_batch_is_available(routed_regions, site) {
            return false;
        }

        let batch_is_cached = workspace.is_some();
        let batch = compiled_batch.unwrap_or_else(|| self.compile_region_batch(routed_regions, regions, workspace));
        let mut candidates = Vec::new();
        let attributed_rule = site.attribution();
        regions.for_each_batch(&batch, |candidate| {
            // A node the entry's subject cannot name matches it on neither side, so it has no
            // delta to contribute - whether or not an earlier route already planned it. Filtering
            // before the already-planned check keeps every entry whose batch merely contains a
            // planned node from queueing a full exact evaluation of that node later.
            if !(self.node_carries_any(site.subject, candidate, site.in_flux)
                && self.node_carries_all(site.subject_required, candidate, site.in_flux)
                && self.path_meets_waypoints(site.path, site.waypoints, candidate, site.in_flux))
            {
                return;
            }
            if regions.contains_node(candidate) {
                self.record_already_planned_selector_truth(candidate, site);
                return;
            }
            candidates.push(candidate);
        });
        let charged_bytes = if batch_is_cached {
            (candidates.capacity() * size_of::<StyleNodeID>()) as u64
        } else {
            (batch.storage_bytes() + candidates.capacity() * size_of::<StyleNodeID>()) as u64
        };
        self.memory
            .reserve_required(MemoryCategory::BatchScratch, charged_bytes);

        let (rule, entry_id) = site.exact_entry.unwrap();
        let (program, entry) = self.programs.entry_location(entry_id);
        let sheet = self.program.rule_sheet(rule);
        let compiled = self.programs.get(program);
        if compiled.can_leave_its_scope()
            || !self
                .program
                .sheet_attachments(sheet)
                .iter()
                .any(|attachment| attachment.tree_scope == TreeScopeID::DOCUMENT)
            || compiled.entries().get(entry as usize).is_none()
        {
            for candidate in candidates {
                self.record_selector_truth_refresh(candidate, attributed_rule);
                regions.add(ImpactRegion::Node(candidate), &mut self.counters);
            }
            self.memory.release(MemoryCategory::BatchScratch, charged_bytes);
            return true;
        }

        candidates.retain(|&candidate| {
            if self.tree.tree_scope(candidate) == TreeScopeID::DOCUMENT {
                true
            } else {
                self.record_selector_truth_refresh(candidate, attributed_rule);
                regions.add(ImpactRegion::Node(candidate), &mut self.counters);
                false
            }
        });
        if candidates.is_empty() {
            self.memory.release(MemoryCategory::BatchScratch, charged_bytes);
            return true;
        }

        let workspace_before = self.match_workspace.capacity_bytes();
        for candidate in candidates {
            if let Some(exact_tree_evaluation) = site.exact_tree_evaluation {
                let change = self.candidate_changes_exact_tree(candidate, program, entry, exact_tree_evaluation);
                if exact_entry_changed(change) {
                    self.record_exact_selector_truth_change(candidate, site, change);
                    regions.add(ImpactRegion::Node(candidate), &mut self.counters);
                }
                continue;
            }

            let change = self.candidate_changes_exact_entry(candidate, site);
            if exact_entry_changed(change) {
                self.record_exact_selector_truth_change(candidate, site, change);
                regions.add(ImpactRegion::Node(candidate), &mut self.counters);
            }
        }
        let workspace_after = self.match_workspace.capacity_bytes();
        self.memory
            .reserve_required(MemoryCategory::BatchScratch, workspace_after - workspace_before);
        self.memory.release(MemoryCategory::BatchScratch, charged_bytes);
        true
    }

    /// Narrow the union of all regions reached through one transpose route.
    ///
    /// Transaction impact is the union of every normalized input's route. Keeping that union
    /// grouped by route means a common subject posting is enumerated once, and a broad exact
    /// plan streams overlapping regions once, regardless of how many inputs reached the entry.
    pub(super) fn add_narrowed_regions_with_workspace(
        &mut self,
        routed_regions: &[ImpactRegion],
        site: &RoutingSite<'_>,
        regions: &mut ImpactRegions,
        workspace: &mut ImpactPlanningWorkspace,
    ) {
        if routed_regions.is_empty() {
            return;
        }
        if site.position.bound != u32::MAX {
            let mut expanded_children = false;
            let mut has_other_regions = false;
            for &region in routed_regions {
                match region {
                    ImpactRegion::Children(_) => {
                        self.add_position_bounded_region(region, site, regions);
                        expanded_children = true;
                    }
                    _ => has_other_regions = true,
                }
            }
            if !has_other_regions {
                return;
            }
            if expanded_children {
                for region in routed_regions
                    .iter()
                    .filter(|region| !matches!(region, ImpactRegion::Children(_)))
                {
                    self.add_narrowed_regions_with_workspace(std::slice::from_ref(region), site, regions, workspace);
                }
                return;
            }
        }

        let has_usable_subject = !site.subject.is_empty() && site.subject.iter().all(|key| key.has_selector_posting());
        if !self.selector_truth_changes_active
            && has_usable_subject
            && site
                .subject
                .iter()
                .all(|&key| !matches!(self.facts.postings().lookup(key), Lookup::Missing(_)))
            && routed_regions
                .iter()
                .all(|region| matches!(region, ImpactRegion::Node(_)))
        {
            self.add_narrowed_node_regions(routed_regions, site, regions);
            return;
        }
        let cardinality = if has_usable_subject {
            site.subject.iter().copied().try_fold(0_usize, |total, posting| {
                match self.facts.postings().lookup(posting) {
                    Lookup::Known(posting) => Some(total.saturating_add(posting.len())),
                    Lookup::KnownAbsent => Some(total),
                    Lookup::Missing(_) => None,
                }
            })
        } else {
            None
        };
        // A tiny dense posting is already the cheaper side of the join. Prove each of its nodes
        // against the named regions directly instead of materializing every node in those regions
        // merely to discover that the posting was tiny. Larger sources still compile the union so
        // the selective-versus-batch choice has an exact region cardinality.
        let use_direct_region_membership = cardinality.is_some_and(|cardinality| cardinality <= SMALL_CANDIDATE_SOURCE);
        let compiled_regions = if use_direct_region_membership {
            None
        } else {
            let compiled_regions = self.compile_region_batch(routed_regions, regions, Some(workspace));
            let region_nodes = compiled_regions.node_count();
            match choose_plan(
                region_nodes,
                cardinality,
                self.exact_batch_is_available(routed_regions, site),
            ) {
                Plan::ExactBatch { .. } => {
                    let executed = self.add_exact_batch_regions(
                        routed_regions,
                        site,
                        regions,
                        Some(workspace),
                        Some(compiled_regions),
                    );
                    if executed {
                        return;
                    }
                    unreachable!("an available exact batch plan must execute");
                }
                Plan::Conservative => {
                    for &region in routed_regions {
                        if let ImpactRegion::Node(node) = region {
                            self.record_selector_truth_refresh(node, site.attribution());
                        }
                        self.add_region_for_site(region, site, regions);
                    }
                    return;
                }
                Plan::Selective { .. } => {}
            }
            Some(compiled_regions)
        };

        let mut candidates = Vec::new();
        let mut pruned_nodes = Vec::new();
        for &posting in site.subject {
            let Ok((_, reused, copied, inspected, _)) = workspace.extend_remaining_posting(
                posting,
                self.facts.postings(),
                regions,
                &mut candidates,
                self.selector_truth_changes_active.then_some(&mut pruned_nodes),
            ) else {
                unreachable!("the cardinality lookup proved every selective posting present")
            };
            workspace.settle_memory(&mut self.memory);
            self.counters.bump(match reused {
                true => Counter::RemainingPostingReuses,
                false => Counter::RemainingPostingBuilds,
            });
            self.counters.add(Counter::RemainingPostingRowsCopied, copied as u64);
            self.counters
                .add(Counter::RemainingPostingRowsInspected, inspected as u64);
        }
        let mut already_planned_moved_nodes = Vec::new();
        let moved_features = &self
            .transaction_fact_view
            .as_ref()
            .expect("routing requires a transaction fact view")
            .moved_features;
        for &key in site.subject {
            moved_features.all_nodes_with_key(key, |node| {
                match regions.contains_node(node) {
                    true => already_planned_moved_nodes.push(node),
                    false => candidates.push(node),
                }
                true
            });
        }
        already_planned_moved_nodes.sort_unstable();
        already_planned_moved_nodes.dedup();
        // A node the posting dropped as already planned, and a moved node already in the plan,
        // still need this route's contribution reflected in their retained-answer patch. When
        // the route names its exact rule, one attribution of its extent carries that for all of
        // them symbolically instead of materializing the (node, rule) product; the routes whose
        // input this is were measured pushing hundreds of thousands of such rows per flush.
        pruned_nodes.sort_unstable();
        pruned_nodes.dedup();
        let already_planned_candidates = (already_planned_moved_nodes.len() + pruned_nodes.len()) as u64;
        if already_planned_candidates > 0 && self.selector_truth_changes_active {
            match site.attribution() {
                Some(key) => {
                    for &region in routed_regions {
                        regions.attribute_extent(region, key, &mut self.counters);
                    }
                }
                None => {
                    for &node in &already_planned_moved_nodes {
                        self.record_already_planned_selector_truth(node, site);
                    }
                    for &pruned in &pruned_nodes {
                        self.record_already_planned_selector_truth(pruned, site);
                    }
                }
            }
        }
        candidates.sort_unstable();
        candidates.dedup();
        let workspace_before = self.match_workspace.capacity_bytes();
        for candidate in candidates {
            let candidate_is_in_routed_regions = compiled_regions.as_ref().map_or_else(
                || regions.union_contains_node(routed_regions, candidate, &self.tree),
                |compiled_regions| regions.batch_contains_node(compiled_regions, candidate),
            );
            if candidate_is_in_routed_regions
                && self.node_carries_all(site.subject_required, candidate, site.in_flux)
                && self.path_meets_waypoints(site.path, site.waypoints, candidate, site.in_flux)
            {
                let change = self.candidate_changes_exact_entry(candidate, site);
                if exact_entry_changed(change) {
                    self.record_exact_selector_truth_change(candidate, site, change);
                    regions.add(ImpactRegion::Node(candidate), &mut self.counters);
                }
            }
        }
        let workspace_after = self.match_workspace.capacity_bytes();
        self.memory
            .reserve_required(MemoryCategory::BatchScratch, workspace_after - workspace_before);
    }

    /// Narrow a union of exact-node regions from the nodes themselves.
    ///
    /// A subject posting can contain many nodes while this route's region contains only one. The
    /// local fact rows answer membership directly, so walking and copying the whole posting merely
    /// to intersect it back down to those named nodes reverses the cheaper side of the join.
    fn add_narrowed_node_regions(
        &mut self,
        routed_regions: &[ImpactRegion],
        site: &RoutingSite<'_>,
        regions: &mut ImpactRegions,
    ) {
        let mut candidates: Vec<_> = routed_regions
            .iter()
            .filter_map(|region| match region {
                ImpactRegion::Node(node) => Some(*node),
                _ => None,
            })
            .collect();
        candidates.sort_unstable();
        candidates.dedup();

        let mut already_planned = Vec::new();
        let workspace_before = self.match_workspace.capacity_bytes();
        for candidate in candidates {
            if !self.node_carries_any(site.subject, candidate, site.in_flux) {
                continue;
            }
            if regions.contains_node(candidate) {
                already_planned.push(candidate);
                continue;
            }
            if self.node_carries_all(site.subject_required, candidate, site.in_flux)
                && self.path_meets_waypoints(site.path, site.waypoints, candidate, site.in_flux)
            {
                let change = self.candidate_changes_exact_entry(candidate, site);
                if exact_entry_changed(change) {
                    self.record_exact_selector_truth_change(candidate, site, change);
                    regions.add(ImpactRegion::Node(candidate), &mut self.counters);
                }
            }
        }
        let workspace_after = self.match_workspace.capacity_bytes();
        self.memory
            .reserve_required(MemoryCategory::BatchScratch, workspace_after - workspace_before);

        if already_planned.is_empty() || !self.selector_truth_changes_active {
            return;
        }
        match site.attribution() {
            Some(key) => {
                for &region in routed_regions {
                    regions.attribute_extent(region, key, &mut self.counters);
                }
            }
            None => {
                for node in already_planned {
                    self.record_already_planned_selector_truth(node, site);
                }
            }
        }
    }

    /// Finish local routes after all normalized inputs have contributed their regions.
    ///
    /// Ordinary selector chains over all four axes share one top-down automaton. When every
    /// pending route is answerable by that automaton, compare its old and new states once from
    /// every changed root. A node whose local matches differ is dirty; descendants need
    /// inspection only while the continuation inherited by children differs, and later siblings
    /// only while the rightward state entering them differs. Successfully compared routes are
    /// removed from the route-by-route planner below.
    #[must_use]
    pub(super) fn route_is_prefix_convergence_eligible(
        &self,
        routing: &RoutingRegistry,
        dispatch: &RuleDispatch,
        route: RouteID,
    ) -> bool {
        let point = routing.route(route);
        routing.entry_has_prefix_chain(route)
            && routing.entry_prefix_chain_has_only_local_facts(route)
            && self
                .program
                .sheet_origin(self.program.rule_sheet(routing.rule_of(route)))
                == CascadeOrigin::Author
            && dispatch.prefixes().contains_entry(point.entry)
    }

    pub(super) fn add_prefix_convergence_regions(
        &mut self,
        pending: &mut PendingRoutes,
        regions: &mut ImpactRegions,
        retained_winners_are_current: bool,
        transaction: &StyleTransaction,
        sequences: &SequenceChanges,
        pending_prefix_producers: &[PendingPrefixProducer],
    ) -> PrefixConvergenceOutcome {
        let Some((root, mut pending_nodes, departures, tree_relations_changed, sibling_frontier)) =
            self.transaction_fact_view.as_ref().and_then(|transition| {
                transition.prefix.as_ref().map(|prefix| {
                    (
                        transition.root,
                        prefix.roots.clone(),
                        prefix.departures.clone(),
                        prefix.tree_relations_changed,
                        prefix.sibling_frontier.clone(),
                    )
                })
            })
        else {
            return PrefixConvergenceOutcome::default();
        };
        let routing = Rc::clone(&self.routing);
        let (scope_program, dispatch) = self.ranked_scope_program(TreeScopeID::DOCUMENT);
        if dispatch.prefixes().is_empty() {
            return PrefixConvergenceOutcome::default();
        }
        let mut local_fact_changes = pending_nodes.clone();
        local_fact_changes.sort_unstable();
        local_fact_changes.dedup();
        let automaton_has_sibling_steps = dispatch.prefixes().has_sibling_steps();
        // A tree delta moves the rightward context entering the nodes that follow it, so a
        // sibling-bearing automaton walks the frontier those deltas name: the retained walk
        // compares each frontier node's cached transition against one computed from the current
        // topology and propagates right and down until both outputs converge.
        if automaton_has_sibling_steps && tree_relations_changed {
            pending_nodes.extend(sibling_frontier);
            pending_nodes.sort_unstable();
            pending_nodes.dedup();
        }
        // Structural truth is per index or per child list, not per fact cohort: one tree
        // mutation moves the an+b answers of untouched siblings on both sides of the mutation
        // point and its parent's emptiness, with no local fact change and no rightward state
        // to carry the flip. When the automaton holds structural tests, the walk therefore
        // re-compares every remaining child of every parent a tree delta touched, plus that
        // parent itself for emptiness. An of-type answer additionally reads sibling tag facts,
        // so a tag change re-compares the changed node's whole sequence too.
        let positional_tests = dispatch.prefixes().positional_tests();
        // The parents whose child sequences this flush touched: only their children can have
        // moved positional truth, so every other node's retained positional bits still hold.
        let mut positional_touched_parents: Vec<StyleNodeID> = Vec::new();
        if !positional_tests.is_empty() {
            let has_of_type_tests = positional_tests
                .iter()
                .any(|&(_, test)| matches!(test, selector::PrefixStructuralTest::Nth(nth) if nth.of_type));
            let automaton_has_empty_tests = positional_tests
                .iter()
                .any(|&(_, test)| test == selector::PrefixStructuralTest::Empty);
            let mut touched_parents: Vec<StyleNodeID> = Vec::new();
            let mut touch = |tree: &StyleNodeTree, parent: Option<StyleNodeID>| {
                if let Some(parent) = parent
                    && tree.is_live(parent)
                    && tree.tree_scope(parent) == TreeScopeID::DOCUMENT
                {
                    touched_parents.push(parent);
                }
            };
            for input in &transaction.inputs {
                match input.key {
                    InputKey::TreeRelations(_) => {
                        for value in [input.old, input.new] {
                            let InputValue::TreeRelations(Some(relations)) = value else {
                                continue;
                            };
                            touch(&self.tree, relations.parent);
                        }
                    }
                    InputKey::LocalFeature(node, LocalFeatureKey::TagName | LocalFeatureKey::FoldedTagName)
                        if has_of_type_tests =>
                    {
                        touch(&self.tree, self.tree.parent(node));
                    }
                    _ => {}
                }
            }
            touched_parents.sort_unstable();
            touched_parents.dedup();
            if !touched_parents.is_empty() {
                if automaton_has_empty_tests {
                    pending_nodes.extend(touched_parents.iter().copied());
                }
                for &parent in &touched_parents {
                    let child_count = self.tree.children(parent).count();
                    let before_geometry = self
                        .transaction_fact_view
                        .as_ref()
                        .filter(|view| view.before_sibling_relations_available);
                    let sequence_change = sequences.get(parent);
                    let can_narrow = before_geometry.is_some() && sequence_change.is_some();
                    for (index, child) in self.tree.children(parent).enumerate() {
                        let plain_truth_changed = can_narrow
                            && positional_tests.iter().any(|&(_, test)| {
                                let selector::PrefixStructuralTest::Nth(nth) = test else {
                                    return false;
                                };
                                if nth.of_type {
                                    return false;
                                }
                                let new_count = match nth.from_end {
                                    true => child_count - index,
                                    false => index + 1,
                                } as i64;
                                let new_matches = nth.matches_count(new_count);
                                let old_matches = before_geometry
                                    .and_then(|view| view.before_sibling_geometry.sibling_positions(child))
                                    .is_some_and(|old| {
                                        let old_count = match nth.from_end {
                                            true => old.from_end,
                                            false => old.from_start,
                                        };
                                        nth.matches_count(i64::from(old_count))
                                    });
                                old_matches != new_matches
                            });
                        let of_type_may_have_changed = can_narrow
                            && has_of_type_tests
                            && sequence_change.is_some_and(|change| {
                                if change.has_unidentified_type {
                                    return true;
                                }
                                let tag = self.facts.tag_of_node(child);
                                tag.is_none() || change.changed_types.contains(&(tag, self.facts.namespace_of(child)))
                            });
                        if !can_narrow || plain_truth_changed || of_type_may_have_changed {
                            pending_nodes.push(child);
                        }
                    }
                }
                pending_nodes.sort_unstable();
                pending_nodes.dedup();
            }
            positional_touched_parents = touched_parents;
        }

        // No fact root moved: the cache's own book-keeping is the whole maintenance. Departed
        // transitions are forgotten, an arrival simply has no transition until something asks,
        // and cross-parent moves already refused the transition above. Pure tree flushes used to
        // discard the cache here, which re-cooled every later plan into coarse arrivals.
        if pending_nodes.is_empty() {
            // Currency can only be claimed for inputs the prefix book-keeping accounts for. A
            // change in another tree scope contributes no roots here yet still ages the caches
            // serving that scope, and a non-node input reshapes the program itself.
            let all_inputs_accounted = transaction.inputs.iter().all(|input| {
                input
                    .key
                    .style_node()
                    .is_some_and(|node| self.tree.tree_scope(node) == TreeScopeID::DOCUMENT)
            });
            if !all_inputs_accounted {
                return PrefixConvergenceOutcome::default();
            }
            let prefix_caches = Rc::clone(&self.prefix_caches);
            let mut caches = prefix_caches.borrow_mut();
            if !caches.states.is_retained() {
                return PrefixConvergenceOutcome::default();
            }
            for node in departures {
                caches.states.forget_transition(scope_program, node);
            }
            // With positional tests registered, an empty pending set on a tree flush means
            // every touched sequence lost its last member, so there is no positional truth
            // left to move and coverage holds vacuously.
            return PrefixConvergenceOutcome {
                sibling_routes_are_covered: false,
                positional_routes_are_covered: tree_relations_changed
                    && !dispatch.prefixes().positional_tests().is_empty(),
            };
        }
        if !pending_nodes
            .iter()
            .all(|&node| regions.region_contains_node(ImpactRegion::Subtree(root), node, &self.tree))
        {
            return PrefixConvergenceOutcome::default();
        }

        if retained_winners_are_current {
            let before = pending.len();
            let mut released_route_bytes = 0;
            let mut inexact_answer_regions = Vec::new();
            pending.retain(|key, routed_regions| {
                let route = routing.route(key.route);
                let (program, entry) = self.programs.entry_location(route.entry);
                let keep = !self.prefix_route_cannot_change_any_retained_winner(
                    routing.rule_of(key.route),
                    program,
                    entry,
                    transaction,
                );
                if !keep {
                    released_route_bytes += (routed_regions.capacity() * size_of::<ImpactRegion>()) as u64;
                    inexact_answer_regions.extend(routed_regions.iter().copied());
                }
                keep
            });
            self.memory.release(MemoryCategory::BatchScratch, released_route_bytes);
            let inexact_answer_region_bytes = (inexact_answer_regions.capacity() * size_of::<ImpactRegion>()) as u64;
            self.memory
                .reserve_required(MemoryCategory::BatchScratch, inexact_answer_region_bytes);
            if self.selector_truth_changes_active {
                let refreshes = &mut self.selector_truth_changes.refreshes;
                regions.for_each_union(&inexact_answer_regions, &self.tree, Some(root), |node| {
                    refreshes.push(SelectorTruthRefresh { node, rule: None });
                });
            }
            self.memory
                .release(MemoryCategory::BatchScratch, inexact_answer_region_bytes);
            self.counters.add(
                Counter::SelectorRoutesRejectedByCascade,
                before.saturating_sub(pending.len()) as u64,
            );
            if pending.is_empty() {
                return PrefixConvergenceOutcome::default();
            }
        }

        // The pass consumes the routes the automaton can answer and leaves the rest pending: a
        // mixed transaction no longer forfeits the retained cache, it just narrows its other
        // routes the ordinary way.
        let mut eligible_keys: Vec<PendingRoute> = pending
            .keys()
            .filter(|key| self.route_is_prefix_convergence_eligible(&routing, &dispatch, key.route))
            .collect();
        eligible_keys.sort_unstable();
        self.counters.add(Counter::PendingSelectorRoutes, pending.len() as u64);
        self.counters
            .add(Counter::PrefixEligibleRoutes, eligible_keys.len() as u64);
        // Consumption still asks for the whole pending set: at current transition unit cost a
        // maintenance walk on a mixed flush loses to the generic narrowing it would subsidize.
        // Partial consumption was measured again after positional admission and lost about a
        // fifth of the whole bench, spread across every suite, so widening admission until
        // whole flushes qualify is the lever, not walking to subsidize a subset.
        let consuming = eligible_keys.len() == pending.len();
        if !consuming {
            return PrefixConvergenceOutcome::default();
        }

        // A retained prefix pass is a top-down walk until its continuation converges. When every
        // route has a small indexed subject set, exact selective routing can ask that union directly
        // instead of visiting an otherwise unchanged document to reach it. A warm retained cache
        // changes that arithmetic: its maintenance walk is bounded by the change and keeping it
        // current is what keeps every later plan narrow, so it is never bypassed away.
        const PREFIX_CONVERGENCE_SELECTIVE_MINIMUM_DOCUMENT: u32 = 1024;
        const PREFIX_CONVERGENCE_SELECTIVE_RATIO: usize = 16;
        let mut subject_candidates = HashSet::default();
        let mut subjects_are_indexed = true;
        for key in pending.keys() {
            let subject = routing.subject_dispatch_of(key.route);
            if subject.is_empty() {
                subjects_are_indexed = false;
                break;
            }
            for dispatch in subject {
                if !dispatch.has_selector_posting() {
                    subjects_are_indexed = false;
                    break;
                }
                let candidates = match self.facts.postings().lookup(*dispatch) {
                    Lookup::Known(posting) => posting.candidates(),
                    Lookup::KnownAbsent => continue,
                    Lookup::Missing(_) => {
                        subjects_are_indexed = false;
                        break;
                    }
                };
                for candidate in candidates {
                    subject_candidates.insert(candidate);
                    if subject_candidates.len() > SMALL_CANDIDATE_SOURCE {
                        break;
                    }
                }
                if !subjects_are_indexed || subject_candidates.len() > SMALL_CANDIDATE_SOURCE {
                    break;
                }
            }
            if !subjects_are_indexed || subject_candidates.len() > SMALL_CANDIDATE_SOURCE {
                break;
            }
        }
        if !self.prefix_caches.borrow().states.is_retained()
            && self.tree.connected_element_count() >= PREFIX_CONVERGENCE_SELECTIVE_MINIMUM_DOCUMENT
            && subjects_are_indexed
            && subject_candidates.len() <= SMALL_CANDIDATE_SOURCE
            && subject_candidates
                .len()
                .saturating_mul(PREFIX_CONVERGENCE_SELECTIVE_RATIO)
                < self.tree.connected_element_count() as usize
        {
            self.counters.bump(Counter::PrefixConvergenceBypasses);
            return PrefixConvergenceOutcome::default();
        }

        let selection = dispatch.prefixes().select_entries(
            eligible_keys.iter().map(|key| {
                let route = routing.route(key.route);
                route.entry
            }),
            self.programs.entry_capacity(),
        );
        let had_retained_prefix_states = self.prefix_caches.borrow().states.is_retained();
        let mut local_prefix_producers: HashMap<StyleNodeID, Vec<PrefixProducer>> = HashMap::default();
        if had_retained_prefix_states {
            for producer in pending_prefix_producers {
                local_prefix_producers
                    .entry(producer.node)
                    .or_default()
                    .push(producer.producer);
            }
            for producers in local_prefix_producers.values_mut() {
                producers.sort_unstable_by_key(|producer| producer.step);
                producers.dedup_by_key(|producer| producer.step);
            }
        }
        let selection_bytes = selection.capacity_bytes()
            + (local_prefix_producers.capacity() * (size_of::<StyleNodeID>() + size_of::<Vec<PrefixProducer>>() + 1)
                + local_prefix_producers.values().map(Vec::capacity).sum::<usize>() * size_of::<PrefixProducer>())
                as u64;

        if had_retained_prefix_states {
            let prefix_caches = Rc::clone(&self.prefix_caches);
            let mut caches = prefix_caches.borrow_mut();
            let PrefixCaches {
                states: retained,
                answers,
            } = &mut *caches;
            let cache_is_batch_sparse = retained.is_sparse();
            retained.make_scratch(&mut self.memory);
            retained.prepare_to_mutate(&mut self.memory);
            // NB: An incomplete cache under departures used to refuse reuse outright, which
            //     released the cache and stranded every later flush on the from-scratch compare
            //     loop, since nothing reseeds the retained states between full batches. The walk
            //     already forgets departed transitions and routes every unprovable comparison
            //     through its gap and abandon exits, so reuse is safe and keeps the automaton
            //     warm across departure-bearing flushes.
            let can_reuse = retained.lookup_mut(scope_program).sparse().is_ok();
            if can_reuse {
                let view = self
                    .transaction_fact_view
                    .as_ref()
                    .expect("prefix planning has a transaction fact view");
                let resident_facts = self.facts.primary();
                self.counters.bump(Counter::PrefixTransitionCacheHits);
                let nodes_in_preorder = regions.sort_nodes_for_top_down_walk(&mut pending_nodes, &self.tree);
                if automaton_has_sibling_steps && !nodes_in_preorder {
                    retained.release();
                    answers.release(&mut self.match_answers);
                    return PrefixConvergenceOutcome::default();
                }
                let mut visited = Vec::new();
                let mut changed_nodes = Vec::new();
                let mut prefix_delta_arena = PrefixDeltaArena::default();
                let old_evaluator = MatchEvaluator::new(&self.tree, resident_facts)
                    .with_transaction_fact_view(view, TransactionFactSide::Before);
                let old_evaluation = PrefixEvaluation::new(
                    dispatch.prefixes(),
                    &self.tree,
                    resident_facts,
                    &self.programs,
                    &old_evaluator,
                    None,
                    None,
                );
                let new_evaluator = MatchEvaluator::new(&self.tree, resident_facts)
                    .with_transaction_fact_view(view, TransactionFactSide::After);
                let new_evaluation = PrefixEvaluation::new(
                    dispatch.prefixes(),
                    &self.tree,
                    resident_facts,
                    &self.programs,
                    &new_evaluator,
                    None,
                    None,
                );
                let workspace_bytes = |pending_node_capacity: usize,
                                       visited_capacity: usize,
                                       changed_node_capacity: usize,
                                       delta_capacity_bytes: u64| {
                    (pending_node_capacity * size_of::<PendingPrefixNode>()) as u64
                        + (local_fact_changes.capacity() * size_of::<StyleNodeID>()) as u64
                        + visited_capacity.div_ceil(8) as u64
                        + (changed_node_capacity * size_of::<StyleNodeID>()) as u64
                        + selection_bytes
                        + delta_capacity_bytes
                };
                let mut charged_bytes = workspace_bytes(
                    pending_nodes.capacity(),
                    visited.capacity(),
                    changed_nodes.capacity(),
                    prefix_delta_arena.capacity_bytes(),
                );
                self.memory
                    .reserve_required(MemoryCategory::BatchScratch, charged_bytes);
                let mut complete = true;
                let mut prefix_completion_budget = PREFIX_TRANSITION_CACHE_COMPLETION_BUDGET;
                let mut sibling_walk_abandoned = false;
                {
                    let states = match retained.lookup_mut(scope_program) {
                        Lookup::Known(states) => states,
                        Lookup::KnownAbsent | Lookup::Missing(_) => {
                            unreachable!("prefix cache reuse proved the program present")
                        }
                    };
                    // A batch-warmed sparse cache holds transitions only for the spines that
                    // were asked. A root without one has no old truth to diff, and turning it
                    // into an upquery would trade its route's exact delta for a cold rematch,
                    // so leave such roots to their routes and walk only where the cache
                    // compares.
                    if cache_is_batch_sparse {
                        pending_nodes.retain(|&node| states.has_transition(node));
                    }
                    let mut pending_prefix_nodes: Vec<_> = pending_nodes
                        .drain(..)
                        .map(|node| PendingPrefixNode {
                            node,
                            entering_deltas: PrefixEnteringDeltas::default(),
                        })
                        .collect();
                    while let Some(pending_node) = pending_prefix_nodes.pop() {
                        let node = pending_node.node;
                        if !automaton_has_sibling_steps
                            && regions.is_covered_by_subtree(ImpactRegion::Node(node), &self.tree)
                        {
                            // NB: The transaction needs no narrower answer below an existing
                            //     subtree. A sibling-bearing automaton processes the node anyway:
                            //     its rightward output can reach later siblings outside the
                            //     covering subtree.
                            //     A covering subtree that is only attributed narrows the node's
                            //     patch to the covering rules, so the undiffed node must then
                            //     poison its patch to a full re-derivation.
                            if self.selector_truth_changes_active
                                && !regions.is_covered_by_full_subtree(ImpactRegion::Node(node), &self.tree)
                            {
                                self.selector_truth_changes
                                    .refreshes
                                    .push(SelectorTruthRefresh { node, rule: None });
                            }
                            // The skipped subtree's resident transitions describe pre-flush facts.
                            // In a down-only automaton the subtree is exactly the set of states
                            // that depend on the skipped node, so forget those transitions and
                            // keep the rest of the cache warm instead of surrendering all of it.
                            for stale in self.elements_under(node) {
                                states.forget_transition(stale);
                            }
                            continue;
                        }
                        if !mark_element_visited(&mut visited, node) {
                            continue;
                        }
                        // A topology flush disables the routed selection for difference
                        // detection: a tree change can move matches no route named.
                        let difference_selection = match tree_relations_changed {
                            true => None,
                            false => Some(&selection),
                        };
                        let local_facts_changed = local_fact_changes.binary_search(&node).is_ok();
                        let local_prefix_candidates = local_facts_changed
                            .then(|| local_prefix_producers.get(&node))
                            .flatten()
                            .map(Vec::as_slice)
                            .filter(|steps| !steps.is_empty());
                        // A node's positional bits move when its own sequence moved; its emptiness
                        // moves when its children did.
                        let positional_truth_stable = positional_tests.is_empty()
                            || (positional_touched_parents.binary_search(&node).is_err()
                                && self
                                    .tree
                                    .parent(node)
                                    .is_none_or(|parent| positional_touched_parents.binary_search(&parent).is_err()));
                        let difference = match states.compare_and_update(
                            &new_evaluation,
                            &old_evaluation,
                            difference_selection,
                            node,
                            local_facts_changed,
                            local_prefix_candidates,
                            pending_node.entering_deltas,
                            positional_truth_stable,
                            &mut prefix_delta_arena,
                            &mut self.counters,
                        ) {
                            PrefixTransitionLookup::Known(difference) => difference,
                            PrefixTransitionLookup::Missing(_) => {
                                complete = false;
                                sibling_walk_abandoned = automaton_has_sibling_steps;
                                break;
                            }
                        };
                        self.counters.bump(Counter::PrefixConvergenceNodes);
                        if difference.arrived {
                            self.counters.bump(Counter::PrefixConvergenceUpqueries);
                            // NB: An arrival has no old state to diff against, so the retained
                            //     answer must be re-derived cold. An exact node region alone does
                            //     not force that once attributed regions cover the node, so poison
                            //     its patch explicitly.
                            if self.selector_truth_changes_active
                                && !regions.is_covered_by_full_subtree(ImpactRegion::Node(node), &self.tree)
                            {
                                self.selector_truth_changes
                                    .refreshes
                                    .push(SelectorTruthRefresh { node, rule: None });
                            }
                            regions.add(ImpactRegion::Node(node), &mut self.counters);
                        } else if difference.matches_changed {
                            changed_nodes.push((node, difference.old_matches, difference.new_matches));
                        }
                        if automaton_has_sibling_steps
                            && difference.right_changed
                            && let Some(next) = self.tree.next_element_sibling(node)
                        {
                            // The next sibling pops after this node's whole changed subtree: it is
                            // pushed first, and children go on top of it in document order.
                            if states.has_transition(next) {
                                pending_prefix_nodes.push(PendingPrefixNode {
                                    node: next,
                                    entering_deltas: PrefixEnteringDeltas {
                                        parent: pending_node.entering_deltas.parent,
                                        previous: difference.right_delta,
                                        parent_endpoints: pending_node.entering_deltas.parent_endpoints,
                                    },
                                });
                            } else if prefix_completion_budget != 0 {
                                prefix_completion_budget -= 1;
                                pending_prefix_nodes.push(PendingPrefixNode {
                                    node: next,
                                    entering_deltas: PrefixEnteringDeltas {
                                        parent: pending_node.entering_deltas.parent,
                                        previous: difference.right_delta,
                                        parent_endpoints: pending_node.entering_deltas.parent_endpoints,
                                    },
                                });
                            } else {
                                complete = false;
                                sibling_walk_abandoned = true;
                                break;
                            }
                        }
                        if difference.continuation_changed {
                            let first_new_child = pending_prefix_nodes.len();
                            for child in self.tree.children(node) {
                                if states.has_transition(child) {
                                    pending_prefix_nodes.push(PendingPrefixNode {
                                        node: child,
                                        entering_deltas: PrefixEnteringDeltas {
                                            parent: difference.continuation_delta,
                                            previous: None,
                                            parent_endpoints: difference.continuation_endpoints,
                                        },
                                    });
                                } else if prefix_completion_budget != 0 {
                                    prefix_completion_budget -= 1;
                                    pending_prefix_nodes.push(PendingPrefixNode {
                                        node: child,
                                        entering_deltas: PrefixEnteringDeltas {
                                            parent: difference.continuation_delta,
                                            previous: None,
                                            parent_endpoints: difference.continuation_endpoints,
                                        },
                                    });
                                } else {
                                    regions.add(ImpactRegion::Subtree(child), &mut self.counters);
                                }
                            }
                            // Pops must see siblings in document order, or a later sibling would
                            // read its predecessor's rightward state before it is recomputed.
                            pending_prefix_nodes[first_new_child..].reverse();
                        } else {
                            self.counters.bump(Counter::PrefixConvergenceStops);
                        }

                        let current_bytes = workspace_bytes(
                            pending_prefix_nodes.capacity(),
                            visited.capacity(),
                            changed_nodes.capacity(),
                            prefix_delta_arena.capacity_bytes(),
                        );
                        if current_bytes > charged_bytes {
                            self.memory
                                .reserve_required(MemoryCategory::BatchScratch, current_bytes - charged_bytes);
                            charged_bytes = current_bytes;
                        }
                    }
                }
                retained.settle_memory(&mut self.memory);
                if complete {
                    {
                        let states = match retained.lookup_mut(scope_program) {
                            Lookup::Known(states) => states,
                            Lookup::KnownAbsent | Lookup::Missing(_) => {
                                unreachable!("prefix cache reuse proved the program present")
                            }
                        };
                        for node in departures {
                            states.forget_transition(node);
                        }
                    }
                    {
                        let states = match retained.lookup_mut(scope_program) {
                            Lookup::Known(states) => states,
                            Lookup::KnownAbsent | Lookup::Missing(_) => {
                                unreachable!("prefix cache reuse proved the program present")
                            }
                        };
                        for &(node, old_matches, new_matches) in &changed_nodes {
                            // The interned sets name exactly which entries moved: those rules
                            // join the node's attribution, so its patch stays that narrow.
                            record_match_set_difference(
                                &mut self.selector_truth_changes,
                                self.selector_truth_changes_active,
                                node,
                                states.matches_in(old_matches),
                                states.matches_in(new_matches),
                                &dispatch,
                            );
                        }
                    }
                    for (node, _, _) in changed_nodes {
                        regions.add(ImpactRegion::Node(node), &mut self.counters);
                    }
                    // Subsumption speaks for a route's whole candidate space, so a walk over a
                    // batch-warmed sparse cache keeps every route; the warmth still cut the
                    // spine re-derivation. The walk has now refreshed the cache, so the sparse
                    // origin clears either way.
                    if !cache_is_batch_sparse {
                        let mut released_route_bytes = 0;
                        pending.retain(|key, routed_regions| {
                            let keep = eligible_keys.binary_search(&key).is_err();
                            if !keep {
                                released_route_bytes += (routed_regions.capacity() * size_of::<ImpactRegion>()) as u64;
                            }
                            keep
                        });
                        self.memory.release(MemoryCategory::BatchScratch, released_route_bytes);
                    }
                    self.counters.bump(Counter::PrefixConvergencePasses);
                    self.memory.release(MemoryCategory::BatchScratch, charged_bytes);
                    // The walk refreshed only the nodes it visited, so a batch-sparse cache
                    // stays sparse until a completing full-batch retention says otherwise.
                    retained.mark_current();
                    return PrefixConvergenceOutcome {
                        sibling_routes_are_covered: !cache_is_batch_sparse
                            && automaton_has_sibling_steps
                            && tree_relations_changed,
                        positional_routes_are_covered: !cache_is_batch_sparse
                            && !dispatch.prefixes().positional_tests().is_empty()
                            && tree_relations_changed,
                    };
                }
                self.memory.release(MemoryCategory::BatchScratch, charged_bytes);
                retained.release();
                if sibling_walk_abandoned {
                    // The abandoned walk already drained part of the pending stack, so the
                    // transaction pass below would under-visit; leave the whole flush to the
                    // conservative planner instead.
                    answers.release(&mut self.match_answers);
                    return PrefixConvergenceOutcome::default();
                }
            } else {
                retained.release();
            }
        }
        // NB: Prefix answers are keyed by identities minted by the transition cache. They are one
        //     coherent cache and cannot outlive a transition cache that failed to become current.
        if had_retained_prefix_states {
            self.prefix_caches.borrow_mut().answers.release(&mut self.match_answers);
        }

        PrefixConvergenceOutcome::default()
    }
    #[must_use]
    pub(super) fn rule_has_complete_element_winners(&self, rule: RuleID, entry: &SelectorEntry) -> bool {
        entry.pseudo_element.is_none()
            && entry.scope_root.is_none()
            && self.program.declarations_are_complete_for(rule)
            && self.program.sheet_origin(self.program.rule_sheet(rule)) == CascadeOrigin::Author
    }

    /// Whether a prefix route changes no retained winner in any resident cascade class.
    #[must_use]
    pub(super) fn prefix_route_cannot_change_any_retained_winner(
        &self,
        rule: RuleID,
        program: SelectorProgramID,
        entry_index: u32,
        transaction: &StyleTransaction,
    ) -> bool {
        let winner_program_version = self.program.version();
        if !matches!(
            self.winner_groups
                .coverage_at_least(winner_program_version, self.tree.connected_element_count() as usize),
            Lookup::Known(())
        ) {
            return false;
        }
        let compiled = self.programs.get(program);
        let Some(entry) = compiled.entries().get(entry_index as usize) else {
            return false;
        };
        if !self.rule_has_complete_element_winners(rule, entry) {
            return false;
        }
        let dispatch_key = compiled.dispatch_key(entry);
        if !dispatch_key.has_selector_posting() {
            return false;
        }
        // Rules dispatched under one key all walk the same posting, and the winner column is
        // stable for the routing pass, so the distinct-state collection is shared through a
        // generation-keyed cache: the first asker pays the posting walk, and every further rule
        // under the key pays only the distinct-state count.
        let winner_generation = self.winner_groups.generation();
        let cached = self
            .route_pruning_states
            .borrow()
            .get(&(dispatch_key, winner_generation))
            .cloned();
        let posting_states = match cached {
            Some(Some(states)) => states,
            Some(None) => return false,
            None => {
                let mut collected: Vec<CascadeStateID> = Vec::new();
                let complete = match self.facts.postings().lookup(dispatch_key) {
                    Lookup::Known(posting) => posting.candidates().all(|node| {
                        self.winner_groups
                            .lookup(WinnerGroupKey::current(node, winner_program_version))
                            .sparse()
                            .is_ok_and(|&(state, _)| {
                                if let Err(index) = collected.binary_search(&state) {
                                    collected.insert(index, state);
                                }
                                true
                            })
                    }),
                    Lookup::KnownAbsent => true,
                    Lookup::Missing(_) => false,
                };
                let entry_value = complete.then(|| Rc::new(collected));
                self.route_pruning_states
                    .borrow_mut()
                    .insert((dispatch_key, winner_generation), entry_value.clone());
                match entry_value {
                    Some(states) => states,
                    None => return false,
                }
            }
        };
        let mut states_with_moved_features: Option<Vec<CascadeStateID>> = None;
        let moved_features_are_complete = self
            .transaction_fact_view
            .as_ref()
            .expect("routing requires a transaction fact view")
            .moved_features
            .all_nodes_with_key(dispatch_key, |node| {
                let Some(&(state, _)) = self
                    .winner_groups
                    .lookup(WinnerGroupKey::current(node, winner_program_version))
                    .sparse()
                    .ok()
                else {
                    return false;
                };
                let states = states_with_moved_features.get_or_insert_with(|| posting_states.as_ref().clone());
                if let Err(index) = states.binary_search(&state) {
                    states.insert(index, state);
                }
                true
            });
        if !moved_features_are_complete {
            return false;
        }
        let states = states_with_moved_features.as_deref().unwrap_or(posting_states.as_ref());

        // The proof has a cheap half and an expensive half. The cheap half compares the changed
        // rule against the exact retained priority; only when it can not win is the expensive half
        // needed, which walks the winner's transpose sites to prove the transaction does not touch
        // that winner. Ask the cheap question first.
        let retained_winner_program = |previous: PropertyWinner, property| {
            let WinnerSource::Rule(winner_rule) = previous.source else {
                return None;
            };
            if !self
                .program
                .declared_properties_of(winner_rule)
                .iter()
                .any(|declaration| declaration.property == property)
            {
                return None;
            }
            let program = self.program.rule_version(winner_rule).selector_program?;
            let compiled = self.programs.get(program);
            if compiled.can_leave_its_scope() || compiled.entries().iter().any(|entry| entry.scope_root.is_some()) {
                return None;
            }

            Some(program)
        };
        let retained_winner_ignores_transaction = |program: SelectorProgramID| {
            let compiled = self.programs.get(program);
            for (entry_index, entry) in compiled.entries().iter().enumerate() {
                if entry.pseudo_element.is_some() {
                    continue;
                }
                let mut reads_changed_input = false;
                compiled.collect_transpose_entry_points(entry_index, |site| {
                    reads_changed_input |= self.transaction_changes_routing_key(transaction, site.key);
                });
                if reads_changed_input {
                    return false;
                }
            }
            true
        };
        states.iter().all(|&state| {
            self.program.declared_properties_of(rule).iter().all(|declaration| {
                let Some(previous) = self.winner_groups.winner_in_state(state, declaration.property) else {
                    return false;
                };
                let changed_priority = self.cascade_priority_of(
                    rule,
                    TreeScopeID::DOCUMENT,
                    entry.specificity,
                    u32::MAX,
                    declaration.important,
                );
                let Some(previous_program) = retained_winner_program(previous, declaration.property) else {
                    return false;
                };
                changed_priority <= previous.priority && retained_winner_ignores_transaction(previous_program)
            })
        })
    }

    #[must_use]
    pub(super) fn transaction_changes_routing_key(&self, transaction: &StyleTransaction, key: RoutingKey) -> bool {
        transaction.inputs.iter().any(|input| {
            if input_routes_on_key(input, key) {
                return true;
            }
            let InputKey::LocalFeature(_, LocalFeatureKey::Attribute(changed)) = input.key else {
                return false;
            };
            let RoutingKey::AttributeName(required) = key else {
                return false;
            };
            self.facts
                .attribute_name_keys(changed)
                .any(|candidate| candidate == required)
        })
    }

    /// Finish local routes after all normalized inputs have contributed their regions.
    #[allow(clippy::too_many_arguments)]
    pub(super) fn flush_pending_routes(
        &mut self,
        pending: &mut PendingRoutes,
        regions: &mut ImpactRegions,
        workspace: &mut ImpactPlanningWorkspace,
        retained_winners_are_current: bool,
        transaction: &StyleTransaction,
        sequences: &SequenceChanges,
        pending_prefix_producers: &[PendingPrefixProducer],
    ) -> PrefixConvergenceOutcome {
        let prefix_convergence = self.add_prefix_convergence_regions(
            pending,
            regions,
            retained_winners_are_current,
            transaction,
            sequences,
            pending_prefix_producers,
        );
        let routing = Rc::clone(&self.routing);
        // Several changed facts can reach different transpose points of the same selector entry.
        // Once both exact fact sides and the retained answer are complete, their region union can
        // be compared by evaluating that entry once per candidate. This replaces repeated partial
        // path walks with the exact question the planner ultimately needs to answer. Keep initial
        // and incomplete state on the original route-order path: its allocation lifetimes are part
        // of retained-cache admission under memory pressure.
        let can_group_exact_entries = retained_winners_are_current
            && transaction.inputs.iter().all(|input| {
                let (InputKey::LocalFeature(node, _) | InputKey::State(node, _)) = input.key else {
                    return false;
                };
                matches!(
                    self.winner_groups
                        .lookup(WinnerGroupKey::current(node, self.program.version())),
                    Lookup::Known(_)
                ) && matches!(self.retained_match_answer(node), Lookup::Known(_))
            })
            && transaction
                .inputs
                .iter()
                .all(|input| !matches!(input.key, InputKey::LocalFeature(_, LocalFeatureKey::ArrivingFacts)))
            && self.selector_incidence_is_current
            && self
                .transaction_fact_view
                .as_ref()
                .is_some_and(|view| view.retained_truth_available)
            && matches!(
                self.winner_groups
                    .coverage_at_least(self.program.version(), self.tree.connected_element_count() as usize),
                Lookup::Known(())
            );
        if !can_group_exact_entries {
            for (key, routed_regions) in pending.iter_mut() {
                let route = key.route;
                routed_regions.sort_unstable();
                routed_regions.dedup();
                let rule = routing.rule_of(route);
                let point = routing.route(route);
                let site = RoutingSite {
                    subject: routing.subject_dispatch_of(route),
                    subject_required: routing.subject_required_of(route),
                    position: routing.subject_position_of(route),
                    path: routing.path_of(route),
                    waypoints: routing.waypoints_of(route),
                    in_flux: None,
                    exact_entry: Some((rule, point.entry)),
                    exact_tree_evaluation: key.exact_tree_evaluation,
                    refresh_rule: Some((rule, point.entry)),
                };
                self.discard_regions_covered_by_subtree(routed_regions, &site, regions);
                self.add_narrowed_regions_with_workspace(routed_regions, &site, regions, workspace);
            }
            return prefix_convergence;
        }
        pending.entries.sort_unstable_by_key(|entry| {
            let point = routing.route(entry.key.route);
            (
                routing.rule_of(entry.key.route),
                point.entry,
                entry.key.exact_tree_evaluation,
            )
        });
        let mut first = 0;
        while first < pending.entries.len() {
            let key = pending.entries[first].key;
            let point = routing.route(key.route);
            let exact_entry = (routing.rule_of(key.route), point.entry);
            let mut end = first + 1;
            while end < pending.entries.len() {
                let next = pending.entries[end].key;
                let next_point = routing.route(next.route);
                if (routing.rule_of(next.route), next_point.entry) != exact_entry
                    || next.exact_tree_evaluation != key.exact_tree_evaluation
                {
                    break;
                }
                debug_assert_eq!(
                    routing.subject_dispatch_of(next.route),
                    routing.subject_dispatch_of(key.route)
                );
                debug_assert_eq!(
                    routing.subject_required_of(next.route),
                    routing.subject_required_of(key.route)
                );
                debug_assert_eq!(
                    routing.subject_position_of(next.route),
                    routing.subject_position_of(key.route)
                );
                end += 1;
            }
            self.counters.add(
                Counter::GroupedExactSelectorRoutes,
                end.saturating_sub(first + 1) as u64,
            );
            let original_region_capacity = pending.entries[first..end]
                .iter()
                .map(|entry| entry.regions.capacity())
                .sum::<usize>();
            let mut merged_regions = std::mem::take(&mut pending.entries[first].regions);
            for entry in &mut pending.entries[first + 1..end] {
                merged_regions.extend(std::mem::take(&mut entry.regions));
            }
            let additional_region_bytes = merged_regions
                .capacity()
                .saturating_sub(original_region_capacity)
                .saturating_mul(size_of::<ImpactRegion>()) as u64;
            self.memory
                .reserve_required(MemoryCategory::BatchScratch, additional_region_bytes);
            let routed_regions = &mut merged_regions;
            let route = key.route;
            routed_regions.sort_unstable();
            routed_regions.dedup();
            let rule = routing.rule_of(route);
            let point = routing.route(route);
            let site = RoutingSite {
                subject: routing.subject_dispatch_of(route),
                subject_required: routing.subject_required_of(route),
                position: routing.subject_position_of(route),
                path: if end - first == 1 { routing.path_of(route) } else { &[] },
                waypoints: if end - first == 1 {
                    routing.waypoints_of(route)
                } else {
                    &[]
                },
                in_flux: None,
                exact_entry: Some((rule, point.entry)),
                exact_tree_evaluation: key.exact_tree_evaluation,
                refresh_rule: Some((rule, point.entry)),
            };
            self.discard_regions_covered_by_subtree(routed_regions, &site, regions);
            self.add_narrowed_regions_with_workspace(routed_regions, &site, regions, workspace);
            self.memory
                .release(MemoryCategory::BatchScratch, additional_region_bytes);
            first = end;
        }
        prefix_convergence
    }

    pub(super) fn flush_pending_sibling_routes(
        &mut self,
        pending: &mut PendingSiblingRoutes,
        entries: &[SiblingEntry],
        regions: &mut ImpactRegions,
        workspace: &mut ImpactPlanningWorkspace,
        prefix_convergence_covers_sibling_routes: bool,
    ) {
        if prefix_convergence_covers_sibling_routes {
            let (_, dispatch) = self.ranked_scope_program(TreeScopeID::DOCUMENT);
            let routing = Rc::clone(&self.routing);
            let mut released_route_bytes = 0;
            pending.retain(|key, routed_regions| {
                let keep = !self.route_is_prefix_convergence_eligible(&routing, &dispatch, key.route);
                if !keep {
                    released_route_bytes += (routed_regions.capacity() * size_of::<ImpactRegion>()) as u64;
                }
                keep
            });
            self.memory.release(MemoryCategory::BatchScratch, released_route_bytes);
        }
        let routing = Rc::clone(&self.routing);
        for (key, routed_regions) in pending.iter_mut() {
            let Ok(index) = entries.binary_search_by_key(&key.route, |entry| entry.route) else {
                continue;
            };
            let entry = &entries[index];
            routed_regions.sort_unstable();
            routed_regions.dedup();
            let site = match key.kind {
                SiblingRouteKind::Full => entry.site(&routing, key.exact_tree_evaluation),
                SiblingRouteKind::BeyondSibling => {
                    entry.site_beyond_the_sibling_step(&routing, key.exact_tree_evaluation)
                }
                SiblingRouteKind::Departure | SiblingRouteKind::ExactUnion => {
                    let point = routing.route(entry.route);
                    let exact_entry = (key.kind == SiblingRouteKind::ExactUnion || key.exact_tree_evaluation.is_some())
                        .then_some((routing.rule_of(entry.route), point.entry));
                    RoutingSite {
                        subject: routing.subject_dispatch_of(entry.route),
                        subject_required: routing.subject_required_of(entry.route),
                        position: routing.subject_position_of(entry.route),
                        path: if key.kind == SiblingRouteKind::ExactUnion {
                            &[]
                        } else {
                            routing.path_of(entry.route)
                        },
                        waypoints: &[],
                        in_flux: None,
                        exact_entry,
                        exact_tree_evaluation: key.exact_tree_evaluation,
                        refresh_rule: Some((routing.rule_of(entry.route), point.entry)),
                    }
                }
            };
            self.discard_regions_covered_by_subtree(routed_regions, &site, regions);
            self.add_narrowed_regions_with_workspace(routed_regions, &site, regions, workspace);
        }
    }

    /// Add a region, replacing it with the subject's candidates inside it where that is cheaper.
    /// Add a planned region, carrying the site's exact rule as attribution when it names one, so
    /// retained answers covered only by such regions can patch against the covering rules
    /// instead of fully re-deriving.
    pub(super) fn add_region_for_site(
        &mut self,
        region: ImpactRegion,
        site: &RoutingSite<'_>,
        regions: &mut ImpactRegions,
    ) {
        match site.attribution() {
            Some(key) => regions.add_attributed(region, key, &mut self.counters),
            None => regions.add(region, &mut self.counters),
        }
    }

    /// Preserve a route's contribution to the retained-answer patch when its region is discarded
    /// because an already planned subtree covers it. Coverage alone stopped being a sound reason
    /// to forget the route once patch narrowing existed: the covering subtree may itself be
    /// attributed to another route's rule, in which case a covered node's patch narrows to the
    /// covering attributions and would silently lose this route's rule. A site that names a rule
    /// records its extent symbolically over the dropped region; a site with no rule poisons the
    /// region to the full re-derivation cover, per the `refresh_rule` contract.
    pub(super) fn attribute_region_covered_by_subtree(
        &mut self,
        region: ImpactRegion,
        site: &RoutingSite<'_>,
        regions: &mut ImpactRegions,
    ) {
        if !self.selector_truth_changes_active || regions.is_covered_by_full_subtree(region, &self.tree) {
            return;
        }
        match site.attribution() {
            Some(key) => regions.attribute_extent(region, key, &mut self.counters),
            None => match region {
                ImpactRegion::Node(node) => self.record_selector_truth_refresh(node, None),
                _ => regions.add(region, &mut self.counters),
            },
        }
    }

    /// Drop the routed regions an already planned subtree covers, carrying each dropped region's
    /// contribution into the patch cover first.
    pub(super) fn discard_regions_covered_by_subtree(
        &mut self,
        routed_regions: &mut Vec<ImpactRegion>,
        site: &RoutingSite<'_>,
        regions: &mut ImpactRegions,
    ) {
        let mut kept = 0;
        for index in 0..routed_regions.len() {
            let region = routed_regions[index];
            if !regions.is_covered_by_subtree(region, &self.tree) {
                routed_regions[kept] = region;
                kept += 1;
                continue;
            }
            self.attribute_region_covered_by_subtree(region, site, regions);
        }
        routed_regions.truncate(kept);
    }

    pub(super) fn add_narrowed_region(
        &mut self,
        region: ImpactRegion,
        site: &RoutingSite<'_>,
        regions: &mut ImpactRegions,
    ) {
        let mut workspace = ImpactPlanningWorkspace::default();
        self.add_narrowed_regions_with_workspace(std::slice::from_ref(&region), site, regions, &mut workspace);
    }

    pub(super) fn add_position_bounded_region(
        &mut self,
        region: ImpactRegion,
        site: &RoutingSite<'_>,
        regions: &mut ImpactRegions,
    ) {
        if regions.is_covered_by_subtree(region, &self.tree) {
            self.attribute_region_covered_by_subtree(region, site, regions);
            return;
        }
        if let ImpactRegion::Node(node) = region
            && regions.contains_node(node)
        {
            match site.attribution() {
                Some(key) if self.selector_truth_changes_active => {
                    regions.attribute_extent(region, key, &mut self.counters);
                }
                _ => self.record_already_planned_selector_truth(node, site),
            }
            return;
        }

        // A subject with no feature to enumerate by can still be bounded by where in a sequence it
        // is allowed to be: `:first-child` names one element of a sequence however long it is.
        if let ImpactRegion::Children(parent) = region
            && site.position.bound != u32::MAX
        {
            let unbounded = RoutingSite {
                position: SubjectPosition::UNBOUNDED,
                ..*site
            };
            let bound = site.position.bound as usize;
            if bound == 0 {
                return;
            }
            if site.position.from_end {
                let mut named = VecDeque::new();
                for child in self.tree.children(parent) {
                    if named.len() == bound {
                        named.pop_front();
                    }
                    named.push_back(child);
                }
                let named_bytes = (named.capacity() * size_of::<StyleNodeID>()) as u64;
                self.memory.reserve_required(MemoryCategory::BatchScratch, named_bytes);
                for child in named {
                    self.add_narrowed_region(ImpactRegion::Node(child), &unbounded, regions);
                }
                self.memory.release(MemoryCategory::BatchScratch, named_bytes);
            } else {
                let named: Vec<StyleNodeID> = self.tree.children(parent).take(bound).collect();
                let named_bytes = (named.capacity() * size_of::<StyleNodeID>()) as u64;
                self.memory.reserve_required(MemoryCategory::BatchScratch, named_bytes);
                for child in named {
                    self.add_narrowed_region(ImpactRegion::Node(child), &unbounded, regions);
                }
                self.memory.release(MemoryCategory::BatchScratch, named_bytes);
            }
            return;
        }
        unreachable!("position-bounded routing only expands child regions");
    }

    /// Lower one normalized program input into rows of the active-rule-match join.
    pub(super) fn append_program_join_deltas(&self, input: &NormalizedInput, output: &mut Vec<ProgramJoinDelta>) {
        let (rules, kind) = match input.key {
            InputKey::RuleField(rule, field) => {
                let kind = match field {
                    RuleField::Existence | RuleField::Selector | RuleField::Activation | RuleField::Scope => {
                        ProgramJoinDeltaKind::ActiveRuleMatch
                    }
                    RuleField::Declarations => ProgramJoinDeltaKind::Declarations,
                    RuleField::Layer => ProgramJoinDeltaKind::Priority,
                };
                (vec![rule], kind)
            }
            InputKey::SheetAttachment(sheet, _) | InputKey::SheetActivation(sheet) => (
                self.program.rules_in_sheet(sheet),
                ProgramJoinDeltaKind::ActiveRuleMatch,
            ),
            InputKey::CascadeTopology(TopologyAxis::SheetOrder(tree_scope)) => (
                self.program
                    .sheets_in_scope(tree_scope)
                    .into_iter()
                    .flat_map(|sheet| self.program.rules_in_sheet(sheet))
                    .collect(),
                ProgramJoinDeltaKind::Priority,
            ),
            InputKey::CascadeTopology(TopologyAxis::LayerOrder(tree_scope)) => (
                self.program.rules_in_a_layer_in_scope(tree_scope),
                ProgramJoinDeltaKind::Priority,
            ),
            _ => return,
        };

        output.extend(rules.into_iter().map(|rule| {
            let current_program = self.program.rule_version(rule).selector_program;
            let (before_program, after_program, before_contributes, after_contributes) = match input.key {
                InputKey::RuleField(_, RuleField::Selector) => {
                    let before = match input.old {
                        InputValue::SelectorProgram(program) => program,
                        _ => current_program,
                    };
                    let after = match input.new {
                        InputValue::SelectorProgram(program) => program,
                        _ => current_program,
                    };
                    (before, after, before.is_some(), after.is_some())
                }
                InputKey::RuleField(_, RuleField::Existence | RuleField::Activation)
                | InputKey::SheetAttachment(..)
                | InputKey::SheetActivation(_) => {
                    let before = match input.old {
                        InputValue::Flag(value) => value,
                        _ => true,
                    };
                    let after = match input.new {
                        InputValue::Flag(value) => value,
                        _ => true,
                    };
                    (current_program, current_program, before, after)
                }
                InputKey::RuleField(_, RuleField::Scope | RuleField::Declarations | RuleField::Layer)
                | InputKey::CascadeTopology(_) => (current_program, current_program, true, true),
                _ => unreachable!("program join lowering accepted a non-program input"),
            };
            ProgramJoinDelta {
                input: input.key,
                rule,
                before_program,
                after_program,
                before_contributes,
                after_contributes,
                kind,
            }
        }));
    }

    /// Route a program change to the elements the affected rules could match.
    ///
    /// Inserting a sheet invalidates no existing selector truth, so the region is not the document:
    /// it is the candidates of the rules that arrived or left. A sheet of `@font-face` rules
    /// contributes no style rule and therefore reaches nothing at all, and a sheet whose rules all
    /// name a class reaches only the elements carrying it. An entry with no selective rightmost
    /// feature has no posting to drive from, and only then does the region widen.
    pub(super) fn route_program_input(
        &mut self,
        input: &NormalizedInput,
        program_joins: &[ProgramJoinDelta],
        arriving_rules: &[RuleID],
        departed_sheet_scopes: &[(SheetID, TreeScopeID)],
        context: ProgramRoutingContext<'_>,
        regions: &mut ImpactRegions,
    ) {
        let ProgramRoutingContext {
            resident_nodes,
            winner_program_version,
            document_root,
            attachment_scopes,
            removed_rules_requiring_refresh,
        } = context;
        let key = input.key;
        if program_joins.is_empty() {
            return;
        }

        // A rule that is retargeted stops matching what it matched. Both sides of the change have
        // to be reached, or the declarations the old selector was winning are never taken back.
        // A sheet decides only in the scopes it is attached to. Its rules' features are indexed
        // across the whole document, so a sheet adopted by one shadow root would otherwise reach
        // every element anywhere carrying the same class. A program using `:host`, `::slotted()` or
        // `::part()` does reach out of its scope, and only that program skips the check.
        let mut scopes = match key {
            // An attachment change names the scope it happened in, which is the only place it can
            // reach - and the only answer that still holds once the sheet has left, since by routing
            // time the attachment it is about is gone.
            InputKey::SheetAttachment(_, tree_scope) => {
                attachment_scopes.map_or_else(|| vec![tree_scope], |scopes| scopes.to_vec())
            }
            InputKey::SheetActivation(sheet) => self.scopes_of_sheet(sheet, departed_sheet_scopes),
            InputKey::RuleField(rule, _) => self.scopes_of_sheet(self.program.rule_sheet(rule), departed_sheet_scopes),
            InputKey::CascadeTopology(TopologyAxis::SheetOrder(tree_scope) | TopologyAxis::LayerOrder(tree_scope)) => {
                vec![tree_scope]
            }
            _ => Vec::new(),
        };
        scopes.sort_unstable();
        scopes.dedup();

        // A sheet attached nowhere decides nothing, so neither do its rules. Re-inserting a `<style>`
        // element builds a new sheet and drops the old one, and the dropped one's rules are still
        // recorded as having arrived - answering for them with the whole document would make every
        // detach and reattach of a subtree holding a sheet a full restyle.
        if matches!(key, InputKey::SheetActivation(_) | InputKey::RuleField(..)) && scopes.is_empty() {
            return;
        }

        let resident_count = resident_nodes.map_or(self.tree.connected_element_count() as usize, |nodes| nodes.len());

        let mut programs: Vec<(RuleID, SelectorProgramID)> = program_joins
            .iter()
            .filter(|delta| delta.before_program != delta.after_program)
            .flat_map(|delta| {
                [delta.before_program, delta.after_program]
                    .into_iter()
                    .flatten()
                    .map(|program| (delta.rule, program))
            })
            .collect();
        let changes_selector_truth = program_joins
            .iter()
            .any(|delta| delta.kind == ProgramJoinDeltaKind::ActiveRuleMatch);

        for delta in program_joins {
            let rule = delta.rule;
            // Liveness is not asked about, because a rule being deleted is the change: it is no
            // longer live, and what it was winning still has to be repaired.
            //
            // Its activation is asked about: a rule behind a group whose condition does not hold
            // decides nothing, so a sheet arriving, leaving, being enabled or being reordered moves
            // nothing it could have won. A rule whose own activation is the change is the one case
            // where the answer has to come from the other side of it, and that is decided below.
            if !matches!(key, InputKey::RuleField(..)) && !self.program.rule_conditions_hold(rule) {
                continue;
            }
            let version = self.program.rule_version(rule);
            // A rule that reaches its consumers by name matches nothing, so what finds them is the
            // index of who uses that name: the elements running an animation for `@keyframes`, the
            // elements declaring or referencing a custom property for `@property`.
            // A registration carrying an initial value gives the property a value on every element,
            // whether or not the element mentions it. There is no index of elements that never named
            // it, so the answer is every element the sheet decides for.
            if version.kind == RuleKind::Property && version.registration_has_initial_value {
                regions.widen_to_document(&mut self.counters);
                return;
            }
            if let Some(name) = version.declared_name {
                let consumers = match version.kind {
                    RuleKind::Keyframes => {
                        match self.facts.postings().lookup(DependencyPostingKey::AnimationName(name)) {
                            Lookup::Known(posting) => Ok(posting.candidates().collect()),
                            Lookup::KnownAbsent => Ok(Vec::new()),
                            Lookup::Missing(gap) => Err(gap),
                        }
                    }
                    // A registration reaches every element whose own cascade declares the name, and
                    // also the elements whose custom-property use could not be named, because one of
                    // them may be using this very property.
                    RuleKind::Property => self.facts.custom_property_candidates(name).and_then(|mut nodes| {
                        match self.facts.postings().lookup(DependencyPostingKey::AnyCustomProperty) {
                            Lookup::Known(posting) => nodes.extend(posting.candidates()),
                            Lookup::KnownAbsent => {}
                            Lookup::Missing(gap) => return Err(gap),
                        }
                        Ok(nodes)
                    }),
                    _ => continue,
                };
                let Ok(mut consumers) = consumers else {
                    match self.regions_reachable_for_named_consumers(&scopes) {
                        Some(reachable) => {
                            for region in reachable {
                                regions.add_if_not_covered(region, &self.tree, &mut self.counters);
                            }
                            continue;
                        }
                        None => {
                            regions.widen_to_document(&mut self.counters);
                            return;
                        }
                    }
                };
                consumers.sort_unstable();
                consumers.dedup();
                // The index is document-wide, but a scope-local sheet decides only in the tree it is
                // attached to, its host, and the nodes slotted into it. An element elsewhere running
                // an animation of the same name is not this sheet's business.
                // A named rule reaches consumers, not selector subjects. Consumers can be inside
                // the attached tree or outside it through `:host` and `::slotted()` declarations.
                if let Some(reachable) = self.regions_reachable_for_named_consumers(&scopes) {
                    consumers.retain(|&node| reachable.iter().any(|region| region.contains_node(node, &self.tree)));
                }
                for node in consumers {
                    regions.add_if_not_covered(ImpactRegion::Node(node), &self.tree, &mut self.counters);
                }
                continue;
            }
            // Registering a custom property changes how every element that declares or references it
            // computes, and changes what it inherits. Which elements those are is not something
            // selector matching can say, and a registration only moves when a sheet holding an
            // `@property` rule is attached or `CSS.registerProperty()` is called - so the document is
            // the answer, the same one the old invalidation reached for the same reason.
            if version.kind == RuleKind::Property {
                regions.widen_to_document(&mut self.counters);
                return;
            }
            // A custom function reaches the elements that called one. Which function they called is
            // not reported by the substitution machinery, so it is all of them - bounded by having
            // called a function at all, rather than by the document.
            if version.kind == RuleKind::Function {
                let consumers: Vec<StyleNodeID> =
                    match self.facts.postings().lookup(DependencyPostingKey::AnyCustomFunction) {
                        Lookup::Known(posting) => posting.candidates().collect(),
                        Lookup::KnownAbsent => Vec::new(),
                        Lookup::Missing(_) => match self.regions_reachable_for_named_consumers(&scopes) {
                            Some(reachable) => {
                                for region in reachable {
                                    regions.add_if_not_covered(region, &self.tree, &mut self.counters);
                                }
                                continue;
                            }
                            None => {
                                regions.widen_to_document(&mut self.counters);
                                return;
                            }
                        },
                    };
                for node in consumers {
                    regions.add_if_not_covered(ImpactRegion::Node(node), &self.tree, &mut self.counters);
                }
                continue;
            }
            // `@font-feature-values` changes how a font feature name resolves for every element that
            // uses one, and nothing indexes which elements those are: the names are resolved deep in
            // value computation, not by selector matching. So it reaches everything the sheet decides
            // for, the same shape as a registration that carries an initial value.
            if version.kind == RuleKind::FontFeatureValues {
                regions.widen_to_document(&mut self.counters);
                return;
            }
            // A counter style can be referenced through list markers and generated content. Those
            // consumers are resolved while computing values and are not indexed by rule name.
            if version.kind == RuleKind::CounterStyle {
                regions.widen_to_document(&mut self.counters);
                return;
            }
            // A rule that does not contribute declarations through selector matching reaches no
            // element: an @font-face rule arrives at its own consumers instead.
            if !version.kind.matches_elements() {
                continue;
            }
            if let Some(selector_program) = version.selector_program {
                let compiled = self.programs.get(selector_program);
                let removes_contribution = matches!(
                    (key, input.old, input.new),
                    (
                        InputKey::SheetAttachment(..)
                            | InputKey::SheetActivation(_)
                            | InputKey::RuleField(_, RuleField::Existence | RuleField::Activation),
                        InputValue::Flag(true),
                        InputValue::Flag(false)
                    )
                );
                let winner_inventory_is_complete = removes_contribution
                    && self.program.declarations_are_complete_for(rule)
                    && compiled.entries().iter().all(|entry| entry.pseudo_element.is_none())
                    && winner_program_version.is_some_and(|version| {
                        matches!(
                            self.winner_groups.coverage_at_least(version, resident_count),
                            Lookup::Known(())
                        )
                    });
                if winner_inventory_is_complete {
                    if !self.winner_groups.rule_is_a_winner(rule) {
                        if self.selector_truth_changes_active {
                            removed_rules_requiring_refresh.push(rule);
                        }
                        self.counters.bump(Counter::ProgramCandidatesRejectedByCascade);
                        continue;
                    }
                    let winning_nodes = (!compiled.subject_can_leave_its_scope()
                        && compiled
                            .entries()
                            .iter()
                            .all(|entry| compiled.dispatch_key(entry).has_selector_posting()))
                    .then(|| {
                        self.winner_groups.winning_nodes(rule).map(|nodes| {
                            nodes
                                .filter(|&node| scopes.binary_search(&self.tree.tree_scope(node)).is_ok())
                                .collect::<Vec<_>>()
                        })
                    })
                    .flatten();
                    if let Some(nodes) = winning_nodes {
                        if self.selector_truth_changes_active {
                            removed_rules_requiring_refresh.push(rule);
                        }
                        for node in nodes {
                            regions.add_if_not_covered(ImpactRegion::Node(node), &self.tree, &mut self.counters);
                        }
                        continue;
                    }
                }
                programs.push((rule, selector_program));
            }
        }
        programs.sort_unstable_by_key(|(rule, program)| (program.0, rule.0));
        programs.dedup();

        // A rule behind a group whose condition does not hold decides nothing, so a rule arriving
        // inside one reaches nothing. Its activation is normally exempt from that - a condition
        // turning off is precisely what has to reach the rule's matches to take its declarations
        // back - but not when the rule arrived in this same transaction, because then there is
        // nothing to take back.
        if let InputKey::RuleField(rule, field) = key
            && !self.program.rule_conditions_hold(rule)
            && (field != RuleField::Activation || arriving_rules.contains(&rule))
        {
            return;
        }

        let mut first_program_rule = 0;
        while first_program_rule < programs.len() {
            let selector_program = programs[first_program_rule].1;
            let end_program_rule = programs[first_program_rule..]
                .partition_point(|&(_, program)| program == selector_program)
                + first_program_rule;
            let program_rules = &programs[first_program_rule..end_program_rule];
            let can_leave_scope = self.programs.get(selector_program).can_leave_its_scope();
            let subject_can_leave_scope = self.programs.get(selector_program).subject_can_leave_its_scope();
            let bounded_by_scope = !scopes.is_empty() && !subject_can_leave_scope;
            // Activation is one input of an active rule match; selector truth is the other. The
            // dispatch posting is only a candidate source, so test the complete selector before
            // admitting one of its nodes. Turning a rule off reads the old side and turning it on
            // reads the new side. If that side cannot be reconstructed, keep the candidate.
            // A sheet attaching, detaching, or toggling activation is the same shape sheet-wide:
            // on the side where the sheet contributed nothing it decided nothing, so only the side
            // where it contributes has to be read, and a candidate its selector does not match
            // there is not a subject of the change.
            let can_filter_exactly = matches!(
                key,
                InputKey::RuleField(_, RuleField::Activation)
                    | InputKey::SheetAttachment(..)
                    | InputKey::SheetActivation(_)
            ) && scopes.as_slice() == [TreeScopeID::DOCUMENT]
                && !can_leave_scope;
            let retained_incidence = if can_filter_exactly
                && self.selector_incidence_is_current
                && matches!(key, InputKey::RuleField(_, RuleField::Activation))
            {
                self.retained_selector_incidences
                    .lookup(selector_program)
                    .or_else(|| {
                        program_joins
                            .iter()
                            .any(|delta| delta.before_contributes && delta.before_program == Some(selector_program))
                            .then(|| self.retained_selector_incidence(selector_program, document_root))
                            .flatten()
                    })
                    .or_else(|| self.materialize_current_selector_incidence(selector_program))
            } else {
                None
            };
            if let Some(incidences) = retained_incidence {
                // Selector incidence is invariant under activation. The retained relation was
                // materialized while this program contributed, so route its exact subjects and
                // leave liveness to the active-rule join downstream. In particular, an empty
                // relation is a complete answer and performs no selector work.
                let change = match (input.old, input.new) {
                    (InputValue::Flag(false), InputValue::Flag(true)) => SetChange::Added,
                    (InputValue::Flag(true), InputValue::Flag(false)) => SetChange::Removed,
                    _ => unreachable!("an activation input must flip its boolean value"),
                };
                for incidence in incidences.iter().copied() {
                    let node = incidence.node;
                    if resident_nodes.is_some_and(|resident| resident.binary_search(&node).is_err()) {
                        continue;
                    }
                    if bounded_by_scope && scopes.binary_search(&self.tree.tree_scope(node)).is_err() {
                        continue;
                    }
                    let entry = &self.programs.get(selector_program).entries()[incidence.entry as usize];
                    if program_rules.iter().all(|&(rule, _)| {
                        self.program_change_cannot_change_retained_winners(
                            input,
                            rule,
                            entry,
                            winner_program_version,
                            RetainedWinnerProbe::Node(node),
                        )
                    }) {
                        self.record_selector_truth_refresh(node, None);
                        self.counters
                            .add(Counter::ProgramCandidatesRejectedByCascade, program_rules.len() as u64);
                        continue;
                    }
                    if self.selector_truth_changes_active && changes_selector_truth {
                        for &(rule, _) in program_rules {
                            self.selector_truth_changes.deltas.push(SelectorTruthDelta {
                                node,
                                rule,
                                entry: self.programs.entry_id(selector_program, incidence.entry),
                                change,
                                selector_truth_changed: false,
                            });
                        }
                    }
                    regions.add_if_not_covered(ImpactRegion::Node(node), &self.tree, &mut self.counters);
                }
                first_program_rule = end_program_rule;
                continue;
            }
            let activation_fact_sides = can_filter_exactly.then_some(match (input.old, input.new) {
                (InputValue::Flag(false), InputValue::Flag(true)) => (None, Some(TransactionFactSide::After)),
                (InputValue::Flag(true), InputValue::Flag(false)) => (Some(TransactionFactSide::Before), None),
                _ => (Some(TransactionFactSide::Before), Some(TransactionFactSide::After)),
            });
            let compiled = self.programs.get(selector_program);
            for (entry_index, entry) in compiled.entries().iter().enumerate() {
                if scopes.as_slice() == [TreeScopeID::DOCUMENT]
                    && !can_leave_scope
                    && program_rules.iter().all(|&(rule, _)| {
                        self.program_change_cannot_change_retained_winners(
                            input,
                            rule,
                            entry,
                            winner_program_version,
                            RetainedWinnerProbe::AllResident { resident_count },
                        )
                    })
                {
                    let dispatch = compiled.dispatch_key(entry);
                    let rejected = match dispatch
                        .has_selector_posting()
                        .then(|| self.facts.postings().lookup(dispatch))
                    {
                        Some(Lookup::Known(posting)) => {
                            if self.selector_truth_changes_active {
                                let refreshes = &mut self.selector_truth_changes.refreshes;
                                for node in posting.candidates() {
                                    refreshes.push(SelectorTruthRefresh { node, rule: None });
                                }
                            }
                            posting.len()
                        }
                        Some(Lookup::KnownAbsent) => 0,
                        Some(Lookup::Missing(_)) | None => {
                            if self.selector_truth_changes_active {
                                let refreshes = &mut self.selector_truth_changes.refreshes;
                                self.retained_match_answers.for_each_answer_node(|node| {
                                    refreshes.push(SelectorTruthRefresh { node, rule: None });
                                });
                            }
                            1
                        }
                    };
                    self.counters.add(
                        Counter::ProgramCandidatesRejectedByCascade,
                        rejected as u64 * program_rules.len() as u64,
                    );
                    continue;
                }
                let dispatch = compiled.dispatch_key(entry);
                let posting = dispatch
                    .has_selector_posting()
                    .then(|| self.facts.postings().lookup(dispatch));
                match posting {
                    Some(Lookup::Known(posting)) => {
                        // A subject bounded to a prefix or suffix of its sibling sequence is not
                        // every element carrying its feature: `.row:first-child` names one row of a
                        // table however many rows carry the class.
                        let position = compiled.subject_position(entry_index);
                        let posting_cardinality = posting.len();
                        // An arriving subtree is already in the plan. When most of a posting belongs
                        // to one, probe the smaller set of resident nodes instead of enumerating all
                        // of the new nodes once for every arriving selector.
                        let candidates: Vec<StyleNodeID> = match resident_nodes {
                            Some(residents) => {
                                let candidates: Vec<StyleNodeID> = if residents.len() < posting_cardinality {
                                    residents
                                        .iter()
                                        .copied()
                                        .filter(|&node| posting.contains(node))
                                        .collect()
                                } else {
                                    posting
                                        .candidates()
                                        .filter(|node| residents.binary_search(node).is_ok())
                                        .collect()
                                };
                                candidates
                            }
                            None => posting.candidates().collect(),
                        };
                        for node in candidates {
                            if bounded_by_scope && scopes.binary_search(&self.tree.tree_scope(node)).is_err() {
                                continue;
                            }
                            if !self.node_is_within_subject_position(node, position) {
                                continue;
                            }
                            if let Some((old_side, new_side)) = activation_fact_sides {
                                let mut may_match = false;
                                for side in [old_side, new_side].into_iter().flatten() {
                                    let view = self
                                        .transaction_fact_view
                                        .as_ref()
                                        .expect("exact activation filtering has a transaction fact view");
                                    match MatchEvaluator::new(&self.tree, self.facts.primary())
                                        .with_transaction_fact_view(view, side)
                                        .matches_entry(compiled, entry, node, &mut self.counters)
                                    {
                                        Ok(false) => {}
                                        Ok(true) | Err(_) => {
                                            may_match = true;
                                            break;
                                        }
                                    }
                                }
                                if !may_match {
                                    if !matches!(key, InputKey::RuleField(..)) {
                                        self.counters.bump(Counter::SheetChangeCandidatesRejected);
                                    }
                                    continue;
                                }
                            }
                            if program_rules.iter().all(|&(rule, _)| {
                                self.program_change_cannot_change_retained_winners(
                                    input,
                                    rule,
                                    entry,
                                    winner_program_version,
                                    RetainedWinnerProbe::Node(node),
                                )
                            }) {
                                if self.selector_truth_changes_active {
                                    self.selector_truth_changes
                                        .refreshes
                                        .push(SelectorTruthRefresh { node, rule: None });
                                }
                                self.counters
                                    .add(Counter::ProgramCandidatesRejectedByCascade, program_rules.len() as u64);
                                continue;
                            }
                            // Program routing and DOM routing share one union of impact regions. If
                            // another route already covered this node, its narrower attribution must
                            // not hide the program rule from the retained-answer patch.
                            if self.selector_truth_changes_active && changes_selector_truth {
                                for &(rule, _) in program_rules {
                                    self.selector_truth_changes.refreshes.push(SelectorTruthRefresh {
                                        node,
                                        rule: Some((
                                            rule,
                                            self.programs.entry_id(
                                                selector_program,
                                                u32::try_from(entry_index)
                                                    .expect("selector program entry space exhausted"),
                                            ),
                                        )),
                                    });
                                }
                            }
                            regions.add_if_not_covered(ImpactRegion::Node(node), &self.tree, &mut self.counters);
                        }
                    }
                    // Nothing to enumerate the subject by. It may still say where its subjects are:
                    // `:root` names one element, and a child combinator names a parent that does
                    // have a posting. Only when neither holds is the scope the answer, and a sheet
                    // in a shadow root is bounded by the tree it decides in where the document's own
                    // scope has no bound but the document.
                    Some(Lookup::KnownAbsent) => continue,
                    Some(Lookup::Missing(_)) | None => {
                        if let Some(narrowed) = self.regions_from_subject_position(
                            compiled,
                            entry_index,
                            document_root,
                            bounded_by_scope.then_some(scopes.as_slice()),
                        ) {
                            for region in narrowed {
                                regions.add_if_not_covered(region, &self.tree, &mut self.counters);
                            }
                            continue;
                        }
                        match self.regions_reachable_from_scopes(
                            &scopes,
                            compiled.subject_can_leave_its_scope(),
                            compiled.host_is_a_subject(),
                        ) {
                            Some(reachable) => {
                                for region in reachable {
                                    regions.add_if_not_covered(region, &self.tree, &mut self.counters);
                                }
                            }
                            None => {
                                regions.widen_to_document(&mut self.counters);
                                return;
                            }
                        }
                    }
                }
            }
            first_program_rule = end_program_rule;
        }
    }

    /// Whether the retained cascade answers prove that a program change cannot change a winner.
    ///
    /// The winner column is the answer from the last completed cascade, not a prediction. Adding a
    /// declaration below every retained winner leaves that answer unchanged. Editing a winner to
    /// the same canonical specified value does too, provided its priority does not fall and expose
    /// a contender that the winner column deliberately does not retain.
    #[must_use]
    pub(super) fn program_change_cannot_change_retained_winners(
        &self,
        input: &NormalizedInput,
        rule: RuleID,
        entry: &SelectorEntry,
        winner_program_version: Option<ProgramVersion>,
        probe: RetainedWinnerProbe,
    ) -> bool {
        if !self.rule_has_complete_element_winners(rule, entry) {
            return false;
        }
        // The completeness the gate above reads is the new side's. A declarations edit is judged
        // against the old winners, and a block that was incomplete before the edit can have
        // declared custom properties, which the winner columns never hold - so the proof cannot
        // see what the rule used to contribute, and pruning would strand its old declarations.
        if matches!(input.key, InputKey::RuleField(changed, RuleField::Declarations) if changed == rule)
            && self
                .program_staging
                .rules_with_incomplete_old_declarations
                .contains(&rule)
        {
            return false;
        }
        let Some(winner_program_version) = winner_program_version else {
            return false;
        };
        let node_state = match probe {
            RetainedWinnerProbe::Node(node) => {
                let retained_winner_key = WinnerGroupKey::retained(node, winner_program_version);
                if !matches!(self.winner_groups.lookup(retained_winner_key), Lookup::Known(_)) {
                    return false;
                }
                if matches!(
                    input.key,
                    InputKey::CascadeTopology(TopologyAxis::SheetOrder(_) | TopologyAxis::LayerOrder(_))
                ) {
                    return self.program.declared_properties_of(rule).iter().all(|current| {
                        matches!(
                            self.winner_groups.winner(retained_winner_key, current.property),
                            Lookup::Known(previous) if previous.key == Self::retained_rule_winner_key(*current)
                        )
                    });
                }
                let current_winner_key = WinnerGroupKey::current(node, winner_program_version);
                let state = match self.winner_groups.lookup(current_winner_key) {
                    Lookup::Known(&(state, _)) => state,
                    Lookup::KnownAbsent | Lookup::Missing(_) => return false,
                };
                if matches!(
                    input.key,
                    InputKey::RuleField(changed, RuleField::Declarations) if changed == rule
                ) {
                    let declared = self.program.declared_properties_of(rule);

                    // A property this rule used to win but no longer declares exposes a runner-up
                    // that this compact column intentionally does not retain.
                    if self.winner_groups.winners_in_state(state).any(|previous| {
                        previous.source == WinnerSource::Rule(rule)
                            && !declared.iter().any(|current| current.property == previous.property)
                    }) {
                        return false;
                    }

                    return declared.iter().all(|current| {
                        let Some(previous) = self.winner_groups.winner_in_state(state, current.property) else {
                            return false;
                        };
                        let key = Self::retained_rule_winner_key(*current);

                        if previous.source == WinnerSource::Rule(rule) {
                            // Raising the same winner cannot expose anything below it. Lowering it
                            // can, even when its own value is unchanged.
                            (!previous.important || current.important) && key == previous.key
                        } else {
                            // A declaration that remains below the retained winner is inert. One
                            // that reaches or passes it is inert only for the same semantic value.
                            key == previous.key
                        }
                    });
                }
                Some((state, self.tree.tree_scope(node)))
            }
            RetainedWinnerProbe::AllResident { resident_count } => {
                if !matches!(
                    self.winner_groups
                        .coverage_at_least(winner_program_version, resident_count),
                    Lookup::Known(())
                ) {
                    return false;
                }
                None
            }
        };
        if matches!(
            (input.key, input.old, input.new),
            (
                InputKey::RuleField(changed, RuleField::Activation),
                InputValue::Flag(true),
                InputValue::Flag(false)
            ) if changed == rule
        ) {
            let Some((state, _)) = node_state else {
                return false;
            };
            return self.program.declared_properties_of(rule).iter().all(|declared| {
                !matches!(
                    self.winner_groups.winner_in_state(state, declared.property),
                    Some(previous) if previous.source == WinnerSource::Rule(rule)
                )
            });
        }
        match (input.key, input.old, input.new) {
            (
                InputKey::SheetAttachment(..)
                | InputKey::SheetActivation(_)
                | InputKey::RuleField(_, RuleField::Existence | RuleField::Activation),
                InputValue::Flag(false),
                InputValue::Flag(true),
            ) => {}
            _ => return false,
        }

        let declarations_are_inert = |state, tree_scope| {
            self.program.declared_properties_of(rule).iter().all(|declared| {
                let Some(previous) = self.winner_groups.winner_in_state(state, declared.property) else {
                    return false;
                };
                let WinnerSource::Rule(previous_rule) = previous.source else {
                    return false;
                };
                let Some(previous_program) = self.program.rule_version(previous_rule).selector_program else {
                    return false;
                };
                if self.programs.get(previous_program).can_leave_its_scope() {
                    return false;
                }
                let priority =
                    self.cascade_priority_of(rule, tree_scope, entry.specificity, u32::MAX, declared.important);
                priority <= previous.priority
            })
        };
        match node_state {
            Some((state, tree_scope)) => declarations_are_inert(state, tree_scope),
            None => self
                .winner_groups
                .active_states()
                .all(|state| declarations_are_inert(state, TreeScopeID::DOCUMENT)),
        }
    }

    /// Whether a node is near enough to either end of its sibling sequence to be a bounded subject.
    ///
    /// The bound is how far in the sequence the subject can be, so the walk is at most that many
    /// steps: `:first-child` asks whether anything precedes the node, and stops there.
    #[must_use]
    pub(super) fn node_is_within_subject_position(&self, node: StyleNodeID, position: SubjectPosition) -> bool {
        if position.bound == u32::MAX {
            return true;
        }
        let mut remaining = position.bound;
        let mut cursor = Some(node);
        while let Some(current) = cursor {
            if remaining == 0 {
                return false;
            }
            remaining -= 1;
            cursor = match position.from_end {
                true => self.tree.next_element_sibling(current),
                false => self.tree.previous_element_sibling(current),
            };
        }
        true
    }
}
