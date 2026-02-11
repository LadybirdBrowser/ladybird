/*
 * Copyright (c) 2025, Psychpsyo <psychpsyo@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/ImmutableBitmap.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/CSS/CSSKeyframesRule.h>
#include <LibWeb/CSS/CSSStyleRule.h>
#include <LibWeb/CSS/CSSStyleSheet.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/Navigable.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Painting/PaintableBox.h>
#include <LibWeb/ViewTransition/ViewTransition.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::ViewTransition {

GC_DEFINE_ALLOCATOR(NamedViewTransitionPseudoElement);
GC_DEFINE_ALLOCATOR(ReplacedNamedViewTransitionPseudoElement);
GC_DEFINE_ALLOCATOR(CapturedElement);
GC_DEFINE_ALLOCATOR(ViewTransition);

NamedViewTransitionPseudoElement::NamedViewTransitionPseudoElement(CSS::PseudoElement type, FlyString view_transition_name)
    : m_type(type)
    , m_view_transition_name(view_transition_name)
{
}

ReplacedNamedViewTransitionPseudoElement::ReplacedNamedViewTransitionPseudoElement(CSS::PseudoElement type, FlyString view_transition_name, RefPtr<Gfx::ImmutableBitmap> content = {})
    : NamedViewTransitionPseudoElement(type, view_transition_name)
{
    m_content = content;
}

GC::Ref<ViewTransition> ViewTransition::create(JS::Realm& realm)
{
    auto const& finished_promise = WebIDL::create_promise(realm);
    WebIDL::mark_promise_as_handled(finished_promise);
    return realm.create<ViewTransition>(realm, WebIDL::create_promise(realm), WebIDL::create_promise(realm), finished_promise);
}

ViewTransition::ViewTransition(JS::Realm& realm, GC::Ref<WebIDL::Promise> ready_promise, GC::Ref<WebIDL::Promise> update_callback_done_promise, GC::Ref<WebIDL::Promise> finished_promise)
    : PlatformObject(realm)
    , m_ready_promise(ready_promise)
    , m_update_callback_done_promise(update_callback_done_promise)
    , m_finished_promise(finished_promise)
    , m_transition_root_pseudo_element(heap().allocate<DOM::PseudoElementTreeNode>())

{
}

void ViewTransition::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(ViewTransition);
    Base::initialize(realm);
}

void ViewTransition::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);

    for (auto captured_element : m_named_elements) {
        visitor.visit(captured_element.value);
    }
    visitor.visit(m_update_callback);
    visitor.visit(m_ready_promise);
    visitor.visit(m_update_callback_done_promise);
    visitor.visit(m_finished_promise);
    visitor.visit(m_transition_root_pseudo_element);
}

void CapturedElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);

    visitor.visit(new_element);

    visitor.visit(group_keyframes);
    visitor.visit(group_animation_name_rule);
    visitor.visit(group_styles_rule);
    visitor.visit(image_pair_isolation_rule);
    visitor.visit(image_animation_name_rule);
}

// https://drafts.csswg.org/css-view-transitions-1/#dom-viewtransition-skiptransition
void ViewTransition::skip_transition()
{
    // The method steps for skipTransition() are:

    // 1. If this's phase is not "done", then skip the view transition for this with an "AbortError" DOMException.
    if (m_phase != Phase::Done) {
        skip_the_view_transition(WebIDL::AbortError::create(realm(), "ViewTransition.skip_transition() was called"_utf16));
    }
}

// https://drafts.csswg.org/css-view-transitions-1/#setup-view-transition
void ViewTransition::setup_view_transition()
{
    auto& realm = this->realm();
    // To setup view transition for a ViewTransition transition, perform the following steps:

    // 1. Let document be transition’s relevant global object’s associated document.
    auto& document = as<HTML::Window>(HTML::relevant_global_object(*this)).associated_document();

    // 2. Flush the update callback queue.
    // AD-HOC: Spec doesn't say what document to flush it for.
    //         Lets just use the one we have.
    //         (see https://github.com/w3c/csswg-drafts/issues/11986 )
    document.flush_the_update_callback_queue();

    // 3. Capture the old state for transition.
    auto result = capture_the_old_state();
    //    If failure is returned,
    if (result.is_error()) {
        // then skip the view transition for transition with an "InvalidStateError" DOMException in transition’s relevant Realm,
        skip_the_view_transition(WebIDL::InvalidStateError::create(realm, "Failed to capture old state"_utf16));
        // and return.
        return;
    }

    // 4. Set document’s rendering suppression for view transitions to true.
    document.set_rendering_suppression_for_view_transitions(true);

    // 5. Queue a global task on the DOM manipulation task source, given transition’s relevant global object, to
    //    perform the following steps:
    HTML::queue_global_task(HTML::Task::Source::DOMManipulation, HTML::relevant_global_object(*this), GC::create_function(realm.heap(), [&] {
        HTML::TemporaryExecutionContext context(realm);
        // 1. If transition’s phase is "done", then abort these steps.
        if (m_phase == Phase::Done)
            return;

        // 2. schedule the update callback for transition.
        schedule_the_update_callback();

        // 3. Flush the update callback queue.
        // AD-HOC: Spec doesn't say what document to flush it for.
        //         Lets just use the one we have.
        //         (see https://github.com/w3c/csswg-drafts/issues/11986 )
        //         Also, scheduling the update callback should already do this, see https://github.com/w3c/csswg-drafts/issues/11987
        document.flush_the_update_callback_queue();
    }));
}

// https://drafts.csswg.org/css-view-transitions-1/#activate-view-transition
void ViewTransition::activate_view_transition()
{
    auto& realm = this->realm();
    // To activate view transition for a ViewTransition transition, perform the following steps:

    // 1. If transition’s phase is "done", then return.
    // NOTE: This happens if transition was skipped before this point.
    if (m_phase == Phase::Done)
        return;

    // 2. Set transition’s relevant global object’s associated document’s rendering suppression for view transitions to
    //    false.
    auto& document = as<HTML::Window>(HTML::relevant_global_object(*this)).associated_document();
    document.set_rendering_suppression_for_view_transitions(false);

    // 3. If transition’s initial snapshot containing block size is not equal to the snapshot containing block size, then
    //    skip transition with an "InvalidStateError" DOMException in transition’s relevant Realm, and return.
    auto snapshot_containing_block_size = document.navigable()->snapshot_containing_block_size();
    if (m_initial_snapshot_containing_block_size != snapshot_containing_block_size) {
        skip_the_view_transition(WebIDL::InvalidStateError::create(realm, "Transition's initial snapshot containing block size is not equal to the snapshot containing block size"_utf16));
        return;
    }

    // 4. Capture the new state for transition.
    auto result = capture_the_new_state();
    //    If failure is returned,
    if (result.is_error()) {
        // then skip the view transition for transition with an "InvalidStateError" DOMException in transition’s relevant Realm,
        skip_the_view_transition(WebIDL::InvalidStateError::create(realm, "Failed to capture new state"_utf16));
        // and return.
        return;
    }

    // 5. For each capturedElement of transition’s named elements' values:
    for (auto captured_element : m_named_elements) {
        // 1. If capturedElement’s new element is not null, then set capturedElement’s new element’s captured in a
        //    view transition to true.
        if (captured_element.value->new_element) {
            captured_element.value->new_element->set_captured_in_a_view_transition(true);
        }
    }

    // 6. Setup transition pseudo-elements for transition.
    setup_transition_pseudo_elements();

    // 7. Update pseudo-element styles for transition.
    result = update_pseudo_element_styles();
    //    If failure is returned,
    if (result.is_error()) {
        // then skip the view transition for transition with an "InvalidStateError" DOMException in transition’s relevant Realm,
        skip_the_view_transition(WebIDL::InvalidStateError::create(realm, "Failed to update pseudo-element styles"_utf16));
        // and return.
        return;
    }
    // NOTE: The above steps will require running document lifecycle phases, to compute information
    // calculated during style/layout.
    // FIXME: Figure out what this entails.

    // 8. Set transition’s phase to "animating".
    m_phase = Phase::Animating;

    // 9. Resolve transition’s ready promise.
    WebIDL::resolve_promise(realm, m_ready_promise);
}

// https://drafts.csswg.org/css-view-transitions-1/#capture-the-old-state
ErrorOr<void> ViewTransition::capture_the_old_state()
{
    // To capture the old state for ViewTransition transition:

    // 1. Let document be transition’s relevant global object’s associated document.
    auto& document = as<HTML::Window>(HTML::relevant_global_object(*this)).associated_document();

    // 2. Let namedElements be transition’s named elements.
    auto& named_elements = m_named_elements;

    // 3. Let usedTransitionNames be a new set of strings.
    auto used_transition_names = AK::OrderedHashTable<FlyString>();

    // 4. Let captureElements be a new list of elements.
    auto capture_elements = AK::Vector<DOM::Element&>();

    // 5. If the snapshot containing block size exceeds an implementation-defined maximum, then return failure.
    auto snapshot_containing_block = document.navigable()->snapshot_containing_block();
    if (snapshot_containing_block.width() > NumericLimits<int>::max() || snapshot_containing_block.height() > NumericLimits<int>::max())
        return Error::from_string_literal("The snapshot containing block is too large.");

    // 6. Set transition’s initial snapshot containing block size to the snapshot containing block size.
    m_initial_snapshot_containing_block_size = snapshot_containing_block.size();

    // 7. For each element of every element that is connected, and has a node document equal to document, in paint
    //    order:
    // FIXME: Actually do this in paint order
    auto result = document.document_element()->for_each_in_inclusive_subtree_of_type<DOM::Element>([&](auto& element) {
        // NOTE: Step 1 is handled at the end of this function.

        // 2. If element has more than one box fragment, then continue.
        // FIXME: Implement this once we have fragments.

        // 3. Let transitionName be the element’s document-scoped view transition name.
        auto transition_name = element.document_scoped_view_transition_name();

        // 4. If transitionName is none, or element is not rendered, then continue.
        if (!transition_name.has_value() || element.not_rendered())
            return TraversalDecision::Continue;

        // 5. If usedTransitionNames contains transitionName, then:
        if (used_transition_names.contains(transition_name.value())) {
            // 1. For each element in captureElements:
            for (auto& element : capture_elements)
                // 1. Set element’s captured in a view transition to false.
                element.set_captured_in_a_view_transition(false);

            // 2. Return failure
            return TraversalDecision::Break;
        }

        // 6. Append transitionName to usedTransitionNames.
        used_transition_names.set(transition_name.value());

        // 7. Set element’s captured in a view transition to true.
        element.set_captured_in_a_view_transition(true);

        // 8. Append element to captureElements.
        capture_elements.append(element);

        // 1. If any flat tree ancestor of this element skips its contents, then continue.
        if (element.skips_its_contents())
            return TraversalDecision::SkipChildrenAndContinue;

        return TraversalDecision::Continue;
    });
    if (result == TraversalDecision::Break)
        return Error::from_string_literal("Cannot include multiple elements with the same view-transition-name in a view transition.");

    // 8. For each element in captureElements:
    for (auto& element : capture_elements) {
        // 1. Let capture be a new captured element struct.
        auto capture = heap().allocate<CapturedElement>();

        // 2. Set capture’s old image to the result of capturing the image of element.
        capture->old_image = element.capture_the_image();

        // 3. Let originalRect be snapshot containing block if element is the document element, otherwise, the
        //    element's border box.
        auto original_rect = element.is_document_element() ? snapshot_containing_block : element.paintable_box()->absolute_border_box_rect();

        // 4. Set capture’s old width to originalRect’s width.
        capture->old_width = original_rect.width();

        // 5. Set capture’s old height to originalRect’s height.
        capture->old_height = original_rect.height();

        // 6. Set capture’s old transform to a <transform-function> that would map element’s border box from the
        //    snapshot containing block origin to its current visual position.
        // FIXME: Actually compute the right transform here.
        capture->old_transform = CSS::TransformationStyleValue::create(CSS::PropertyID::Transform, CSS::TransformFunction::Translate,
            CSS::StyleValueVector {
                CSS::LengthStyleValue::create(CSS::Length(0, CSS::LengthUnit::Px)),
                CSS::LengthStyleValue::create(CSS::Length(0, CSS::LengthUnit::Px)),
            });

        // 7. Set capture’s old writing-mode to the computed value of writing-mode on element.
        capture->old_writing_mode = element.layout_node()->computed_values().writing_mode();

        // 8. Set capture’s old direction to the computed value of direction on element.
        capture->old_direction = element.layout_node()->computed_values().direction();

        // 9. Set capture’s old text-orientation to the computed value of text-orientation on element.
        // FIXME: Implement this once we have text-orientation.

        // 10. Set capture’s old mix-blend-mode to the computed value of mix-blend-mode on element.
        capture->old_mix_blend_mode = element.layout_node()->computed_values().mix_blend_mode();

        // 11. Set capture’s old backdrop-filter to the computed value of backdrop-filter on element.
        capture->old_backdrop_filter = element.layout_node()->computed_values().backdrop_filter();

        // 12. Set capture’s old color-scheme to the computed value of color-scheme on element.
        capture->old_color_scheme = element.layout_node()->computed_values().color_scheme();

        // 13. Let transitionName be the computed value of view-transition-name for element.
        auto transition_name = element.layout_node()->computed_values().view_transition_name();

        // 14. Set namedElements[transitionName] to capture.
        named_elements.set(transition_name.value(), capture);
    }

    // 9. For each element in captureElements:
    for (auto& element : capture_elements) {
        // 1. Set element’s captured in a view transition to false.
        element.set_captured_in_a_view_transition(false);
    }

    return {};
}

// https://drafts.csswg.org/css-view-transitions-1/#capture-the-new-state
ErrorOr<void> ViewTransition::capture_the_new_state()
{
    // To capture the new state for ViewTransition transition:

    // 1. Let document be transition’s relevant global object’s associated document.
    auto& document = as<HTML::Window>(HTML::relevant_global_object(*this)).associated_document();

    // 2. Let namedElements be transition’s named elements.
    // NOTE: We just use m_named_elements

    // 3. Let usedTransitionNames be a new set of strings.
    auto used_transition_names = AK::OrderedHashTable<FlyString>();

    // 4. For each element of every element that is connected, and has a node document equal to document, in paint
    //    order:
    // FIXME: Actually do this in paint order
    auto result = document.document_element()->for_each_in_inclusive_subtree_of_type<DOM::Element>([&](auto& element) {
        // NOTE: Step 1 is handled at the end of this function.

        // 2. Let transitionName be the element’s document-scoped view transition name.
        auto transition_name = element.document_scoped_view_transition_name();

        // 3. If transitionName is none, or element is not rendered, then continue.
        if (!transition_name.has_value() || element.not_rendered())
            return TraversalDecision::Continue;

        // 4. If element has more than one box fragment, then continue.
        // FIXME: Implement this once we have fragments

        // 5. If usedTransitionNames contains transitionName, then return failure.
        if (used_transition_names.contains(transition_name.value()))
            return TraversalDecision::Break;

        // 6. Append transitionName to usedTransitionNames.
        used_transition_names.set(transition_name.value());

        // 7. If namedElements[transitionName] does not exist, then set namedElements[transitionName] to a new captured element struct.
        if (!m_named_elements.contains(transition_name.value())) {
            auto captured_element = heap().allocate<CapturedElement>();
            m_named_elements.set(transition_name.value(), captured_element);
        }

        // 8. Set namedElements[transitionName]'s new element to element.
        m_named_elements.get(transition_name.value()).value()->new_element = element;

        // 1. If any flat tree ancestor of this element skips its contents, then continue.
        if (element.skips_its_contents())
            return TraversalDecision::SkipChildrenAndContinue;

        return TraversalDecision::Continue;
    });
    if (result == TraversalDecision::Break)
        return Error::from_string_literal("Cannot include multiple elements with the same view-transition-name in a view transition.");

    return {};
}

// https://drafts.csswg.org/css-view-transitions-1/#setup-transition-pseudo-elements
void ViewTransition::setup_transition_pseudo_elements()
{
    // To setup transition pseudo-elements for a ViewTransition transition:

    // 1. Let document be this’s relevant global object’s associated document.
    auto& document = as<HTML::Window>(HTML::relevant_global_object(*this)).associated_document();

    // 2. Set document’s show view transition tree to true.
    document.set_show_view_transition_tree(true);
    // Note: stylesheet is not a variable in the spec but ends up being referenced a lot in this algorithm.
    auto stylesheet = document.dynamic_view_transition_style_sheet();

    // 3. For each transitionName → capturedElement of transition’s named elements:
    for (auto [transition_name, captured_element] : m_named_elements) {
        // 1. Let group be a new '::view-transition-group()', with its view transition name set to transitionName.
        auto group = heap().allocate<NamedViewTransitionPseudoElement>(CSS::PseudoElement::ViewTransitionGroup, transition_name);

        // 2. Append group to transition’s transition root pseudo-element.
        m_transition_root_pseudo_element->append_child(group);

        // 3. Let imagePair be a new '::view-transition-image-pair()', with its view transition name set to
        //    transitionName.
        auto image_pair = heap().allocate<NamedViewTransitionPseudoElement>(CSS::PseudoElement::ViewTransitionImagePair, transition_name);

        // 4. Append imagePair to group.
        group->append_child(image_pair);

        // 5. If capturedElement’s old image is not null, then:
        if (captured_element->old_image) {
            // 1. Let old be a new '::view-transition-old()', with its view transition name set to transitionName,
            //    displaying capturedElement’s old image as its replaced content.
            auto old = heap().allocate<ReplacedNamedViewTransitionPseudoElement>(CSS::PseudoElement::ViewTransitionOld, transition_name, captured_element->old_image);

            // 2. Append old to imagePair.
            image_pair->append_child(old);
        }

        // 6. If capturedElement’s new element is not null, then:
        if (captured_element->new_element) {
            // 1. Let new be a new ::view-transition-new(), with its view transition name set to transitionName.
            //    NOTE: The styling of this pseudo is handled in update pseudo-element styles.
            auto new_ = heap().allocate<ReplacedNamedViewTransitionPseudoElement>(CSS::PseudoElement::ViewTransitionNew, transition_name);

            // 2. Append new to imagePair.
            image_pair->append_child(new_);
        }

        // 7. If capturedElement’s old image is null, then:
        if (!captured_element->old_image) {
            // 1. Assert: capturedElement’s new element is not null.
            VERIFY(captured_element->new_element);

            // 2. Set capturedElement’s image animation name rule to a new CSSStyleRule representing the
            //    following CSS, and append it to document’s dynamic view transition style sheet:
            //     :root::view-transition-new(transitionName) {
            //       animation-name: -ua-view-transition-fade-in;
            //     }
            //    NOTE: The above code example contains variables to be replaced.
            unsigned index = MUST(stylesheet->insert_rule(MUST(String::formatted(R"(
                :root::view-transition-new({}) {{
                    animation-name: -ua-view-transition-fade-in;
                }}
            )",
                                                              transition_name)),
                stylesheet->rules().length()));
            captured_element->image_animation_name_rule = as<CSS::CSSStyleRule>(stylesheet->css_rules()->item(index));
        }

        // 8. If capturedElement’s new element is null, then:
        if (!captured_element->new_element) {
            // 1. Assert: capturedElement’s old image is not null.
            VERIFY(captured_element->old_image);

            // 2. Set capturedElement’s image animation name rule to a new CSSStyleRule representing the
            //    following CSS, and append it to document’s dynamic view transition style sheet:
            //     :root::view-transition-old(transitionName) {
            //       animation-name: -ua-view-transition-fade-out;
            //     }
            //    NOTE: The above code example contains variables to be replaced.
            unsigned index = MUST(stylesheet->insert_rule(MUST(String::formatted(R"(
                :root::view-transition-old({}) {{
                    animation-name: -ua-view-transition-fade-out;
                }}
            )",
                                                              transition_name)),
                stylesheet->rules().length()));
            captured_element->image_animation_name_rule = as<CSS::CSSStyleRule>(stylesheet->css_rules()->item(index));
        }

        // 9. If both of capturedElement’s old image and new element are not null, then:
        if (captured_element->old_image && captured_element->new_element) {
            // 1. Let transform be capturedElement’s old transform.
            auto& transform = captured_element->old_transform;
            // FIXME: Remove this once tranform gets used in step 5 below.
            (void)transform;

            // 2. Let width be capturedElement’s old width.
            auto& width = captured_element->old_width;

            // 3. Let height be capturedElement’s old height.
            auto& height = captured_element->old_height;

            // 4. Let backdropFilter be capturedElement’s old backdrop-filter.
            auto& backdrop_filter = captured_element->old_backdrop_filter;
            // FIXME: Remove this once tranform gets used in step 5 below.
            (void)backdrop_filter;

            // 5. Set capturedElement’s group keyframes to a new CSSKeyframesRule representing the following
            //    CSS, and append it to document’s dynamic view transition style sheet:
            //     @keyframes -ua-view-transition-group-anim-transitionName {
            //       from {
            //         transform: transform;
            //         width: width;
            //         height: height;
            //         backdrop-filter: backdropFilter;
            //       }
            //     }
            //    NOTE: The above code example contains variables to be replaced.
            unsigned index = MUST(stylesheet->insert_rule(MUST(String::formatted(R"(
                @keyframes -ua-view-transition-group-anim-{} {{
                    from {{
                        transform: {};
                        width: {};
                        height: {};
                        backdrop-filter: {};
                    }}
                }}
            )",
                                                              transition_name, "transform", width, height, "backdrop_filter")),
                stylesheet->rules().length()));
            // FIXME: all the strings above should be the identically named variables, serialized somehow.
            captured_element->group_keyframes = as<CSS::CSSKeyframesRule>(stylesheet->css_rules()->item(index));

            // 6. Set capturedElement’s group animation name rule to a new CSSStyleRule representing the
            //    following CSS, and append it to document’s dynamic view transition style sheet:
            //     :root::view-transition-group(transitionName) {
            //       animation-name: -ua-view-transition-group-anim-transitionName;
            //     }
            //    NOTE: The above code example contains variables to be replaced.
            index = MUST(stylesheet->insert_rule(MUST(String::formatted(R"(
                :root::view-transition-group({0}) {{
                    animation-name: -ua-view-transition-group-anim-{0};
                }}
            )",
                                                     transition_name)),
                stylesheet->rules().length()));
            captured_element->group_animation_name_rule = as<CSS::CSSStyleRule>(stylesheet->css_rules()->item(index));

            // 7. Set capturedElement’s image pair isolation rule to a new CSSStyleRule representing the
            //    following CSS, and append it to document’s dynamic view transition style sheet:
            //     :root::view-transition-image-pair(transitionName) {
            //       isolation: isolate;
            //     }
            //    NOTE: The above code example contains variables to be replaced.
            index = MUST(stylesheet->insert_rule(MUST(String::formatted(R"(
                :root::view-transition-image-pair({}) {{
                    isolation: isolate;
                }}
            )",
                                                     transition_name)),
                stylesheet->rules().length()));
            captured_element->image_pair_isolation_rule = as<CSS::CSSStyleRule>(stylesheet->css_rules()->item(index));

            // 8. Set capturedElement’s image animation name rule to a new CSSStyleRule representing the
            //    following CSS, and append it to document’s dynamic view transition style sheet:
            //     :root::view-transition-old(transitionName) {
            //       animation-name: -ua-view-transition-fade-out, -ua-mix-blend-mode-plus-lighter;
            //     }
            //     :root::view-transition-new(transitionName) {
            //       animation-name: -ua-view-transition-fade-in, -ua-mix-blend-mode-plus-lighter;
            //     }
            //    NOTE: The above code example contains variables to be replaced.
            //    NOTE: mix-blend-mode: plus-lighter ensures that the blending of identical pixels from the
            //    old and new images results in the same color value as those pixels, and achieves a “correct”
            //    cross-fade.
            // AD-HOC: We can't use the given CSS exactly since it is two rules, not one.
            //         Instead we turn it into one rule, with both of them nested inside.
            index = MUST(stylesheet->insert_rule(MUST(String::formatted(R"(
                :root {{
                    &::view-transition-old({0}) {{
                        animation-name: -ua-view-transition-fade-out, -ua-mix-blend-mode-plus-lighter;
                    }}
                    &::view-transition-new({0}) {{
                        animation-name: -ua-view-transition-fade-in, -ua-mix-blend-mode-plus-lighter;
                    }}
                }}
            )",
                                                     transition_name)),
                stylesheet->rules().length()));
            captured_element->image_animation_name_rule = as<CSS::CSSStyleRule>(stylesheet->css_rules()->item(index));
        }
    }
}

// https://drafts.csswg.org/css-view-transitions-1/#call-the-update-callback
void ViewTransition::call_the_update_callback()
{
    auto& realm = this->realm();
    // To call the update callback of a ViewTransition transition:

    // 1. Assert: transition’s phase is "done", or before "update-callback-called".
    VERIFY(m_phase == Phase::Done || to_underlying(m_phase) < to_underlying(Phase::UpdateCallbackCalled));

    // 2. If transition’s phase is not "done", then set transition’s phase to "update-callback-called".
    if (m_phase != Phase::Done)
        m_phase = Phase::UpdateCallbackCalled;

    // 3. Let callbackPromise be null.
    WebIDL::Promise* callback_promise;

    // 4. If transition’s update callback is null, then set callbackPromise to a promise resolved with undefined, in
    //    transition’s relevant Realm.
    if (!m_update_callback) {
        auto& relevant_realm = HTML::relevant_realm(*this);
        callback_promise = WebIDL::create_promise(relevant_realm);
        WebIDL::resolve_promise(relevant_realm, *callback_promise, JS::js_undefined());
    }

    // 5. Otherwise, set callbackPromise to the result of invoking transition’s update callback.
    else {
        auto promise = MUST(WebIDL::invoke_callback(*m_update_callback, {}, {}));
        // FIXME: since WebIDL::invoke_callback does not yet convert the value for us,
        // We need to do it here manually.
        // https://webidl.spec.whatwg.org/#js-promise

        HTML::TemporaryExecutionContext context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
        // 1. Let promiseCapability be ? NewPromiseCapability(%Promise%).
        auto promise_capability = WebIDL::create_promise(realm);
        // 2. Perform ? Call(promiseCapability.[[Resolve]], undefined, « V »).
        MUST(JS::call(realm.vm(), *promise_capability->resolve(), JS::js_undefined(), promise));
        // 3. Return promiseCapability.
        callback_promise = GC::make_root(promise_capability);
    }

    // 6. Let fulfillSteps be to following steps:
    auto fulfill_steps = GC::create_function(realm.heap(), [this, &realm](JS::Value) -> WebIDL::ExceptionOr<JS::Value> {
        HTML::TemporaryExecutionContext context(realm);
        // 1. Resolve transition’s update callback done promise with undefined.
        WebIDL::resolve_promise(realm, m_update_callback_done_promise, JS::js_undefined());

        // 2. Activate transition.
        activate_view_transition();

        return JS::js_undefined();
    });

    // 7. Let rejectSteps be the following steps given reason:
    auto reject_steps = GC::create_function(realm.heap(), [this, &realm](JS::Value reason) -> WebIDL::ExceptionOr<JS::Value> {
        HTML::TemporaryExecutionContext context(realm);
        // 1. Reject transition’s update callback done promise with reason.
        WebIDL::reject_promise(realm, m_update_callback_done_promise, reason);

        // 2. If transition’s phase is "done", then return.
        // NOTE: This happens if transition was skipped before this point.
        if (m_phase == Phase::Done)
            return JS::js_undefined();

        // 3. Mark as handled transition’s ready promise.
        // NOTE: transition’s update callback done promise will provide the unhandledrejection. This
        // step avoids a duplicate.
        WebIDL::mark_promise_as_handled(m_update_callback_done_promise);

        // 4. Skip the view transition transition with reason.
        skip_the_view_transition(reason);

        return JS::js_undefined();
    });

    // 8. React to callbackPromise with fulfillSteps and rejectSteps.
    HTML::TemporaryExecutionContext context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
    WebIDL::react_to_promise(*callback_promise, fulfill_steps, reject_steps);

    // 9. To skip a transition after a timeout, the user agent may perform the following steps in parallel:
    // FIXME: Figure out if we want to do this.
}

// https://drafts.csswg.org/css-view-transitions-1/#schedule-the-update-callback
void ViewTransition::schedule_the_update_callback()
{
    // To schedule the update callback given a ViewTransition transition:

    // 1. Append transition to transition’s relevant settings object’s update callback queue.
    // AD-HOC: The update callback queue is a property on document, not a settings object.
    //         For now we'll just put it on the relevant global object's associated document.
    //         Spec bug is filed at https://github.com/w3c/csswg-drafts/issues/11986
    as<HTML::Window>(HTML::relevant_global_object(*this)).associated_document().update_callback_queue().append(this);

    // 2. Queue a global task on the DOM manipulation task source, given transition’s relevant global object, to flush
    //    the update callback queue.
    HTML::queue_global_task(HTML::Task::Source::DOMManipulation, HTML::relevant_global_object(*this), GC::create_function(realm().heap(), [&] {
        // AD-HOC: Spec doesn't say what document to flush it for.
        //         Lets just use the one we use elsewhere.
        //         (see https://github.com/w3c/csswg-drafts/issues/11986 )
        as<HTML::Window>(HTML::relevant_global_object(*this)).associated_document().flush_the_update_callback_queue();
    }));
}

// https://drafts.csswg.org/css-view-transitions-1/#skip-the-view-transition
void ViewTransition::skip_the_view_transition(JS::Value reason)
{
    auto& realm = this->realm();
    // To skip the view transition for ViewTransition transition with reason reason:

    // 1. Let document be transition’s relevant global object’s associated document.
    auto& document = as<HTML::Window>(HTML::relevant_global_object(*this)).associated_document();

    // 2. Assert: transition’s phase is not "done".
    VERIFY(m_phase != Phase::Done);

    // 3. If transition’s phase is before "update-callback-called", then schedule the update callback for transition.
    if (to_underlying(m_phase) < to_underlying(Phase::UpdateCallbackCalled)) {
        schedule_the_update_callback();
    }

    // 4. Set rendering suppression for view transitions to false.
    document.set_rendering_suppression_for_view_transitions(false);

    // 5. If document’s active view transition is transition, Clear view transition transition.
    if (document.active_view_transition() == this)
        clear_view_transition();

    // 6. Set transition’s phase to "done".
    m_phase = Phase::Done;

    // 7. Reject transition’s ready promise with reason.
    WebIDL::reject_promise(realm, m_ready_promise, reason);

    // 8. Resolve transition’s finished promise with the result of reacting to transition’s update callback done promise:
    //    - If the promise was fulfilled, then return undefined.
    HTML::TemporaryExecutionContext context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
    WebIDL::resolve_promise(realm, m_finished_promise, WebIDL::react_to_promise(m_update_callback_done_promise, GC::create_function(realm.heap(), [](JS::Value) -> WebIDL::ExceptionOr<JS::Value> { return JS::js_undefined(); }), nullptr)->promise());
}

// https://drafts.csswg.org/css-view-transitions-1/#handle-transition-frame
void ViewTransition::handle_transition_frame()
{
    auto& realm = this->realm();
    // To handle transition frame given a ViewTransition transition

    // 1. Let document be transition’s relevant global object’s associated document.
    auto& document = as<HTML::Window>(HTML::relevant_global_object(*this)).associated_document();

    // 2. Let hasActiveAnimations be a boolean, initially false.
    bool has_active_animations = false;

    // 3. For each element of transition’s transition root pseudo-element’s inclusive descendants:
    m_transition_root_pseudo_element->for_each_in_inclusive_subtree([&](DOM::PseudoElementTreeNode&) {
        // For each animation whose timeline is a document timeline associated with document, and contains at
        // least one associated effect whose effect target is element, set hasActiveAnimations to true if any of the
        // following conditions are true:
        // FIXME: Implement this.

        // - animation’s play state is paused or running.
        // FIXME: Implement this.

        // - document’s pending animation event queue has any events associated with animation.
        // FIXME: Implement this.

        return TraversalDecision::Continue;
    });

    // 4. If hasActiveAnimations is false:
    if (!has_active_animations) {
        // 1. Set transition’s phase to "done".
        m_phase = Phase::Done;

        // 2. Clear view transition transition.
        clear_view_transition();

        // 3. Resolve transition’s finished promise.
        // FIXME: Without this TemporaryExecutionContext, this would fail an assert later on about missing one.
        //        Figure out why and where this actually needs to be handled.
        HTML::TemporaryExecutionContext context(realm);
        WebIDL::resolve_promise(realm, m_finished_promise);

        // 4. Return.
        return;
    }

    // 5. If transition’s initial snapshot containing block size is not equal to the snapshot containing block size,
    auto snapshot_containing_block_size = document.navigable()->snapshot_containing_block_size();
    if (m_initial_snapshot_containing_block_size != snapshot_containing_block_size) {
        // then skip the view transition for transition with an "InvalidStateError" DOMException in transition’s relevant Realm,
        skip_the_view_transition(WebIDL::InvalidStateError::create(realm, "Transition's initial snapshot containing block size is not equal to the snapshot containing block size"_utf16));
        // and return.
        return;
    }

    // 6. Update pseudo-element styles for transition.
    auto result = update_pseudo_element_styles();
    //    If failure is returned,
    if (result.is_error()) {
        // then skip the view transition for transition with an "InvalidStateError" DOMException in transition’s relevant Realm,
        skip_the_view_transition(WebIDL::InvalidStateError::create(realm, "Failed to update pseudo-element styles"_utf16));
        // and return.
        return;
    }
}

// https://drafts.csswg.org/css-view-transitions-1/#update-pseudo-element-styles
ErrorOr<void> ViewTransition::update_pseudo_element_styles()
{
    // To update pseudo-element styles for a ViewTransition transition:

    // 1. For each transitionName → capturedElement of transition’s named elements:
    for (auto [transition_name, captured_element] : m_named_elements) {
        // 1. Let width, height, transform, writingMode, direction, textOrientation, mixBlendMode, backdropFilter and
        //    colorScheme be null.
        Optional<CSSPixels> width = {};
        Optional<CSSPixels> height = {};
        RefPtr<CSS::TransformationStyleValue const> transform = {};
        Optional<CSS::WritingMode> writing_mode = {};
        Optional<CSS::Direction> direction = {};
        // FIXME: Implement this once we have text-orientation.
        Optional<CSS::MixBlendMode> mix_blend_mode = {};
        Optional<CSS::Filter> backdrop_filter = {};
        Optional<CSS::PreferredColorScheme> color_scheme = {};

        // 2. If capturedElement’s new element is null, then:
        if (!captured_element->new_element) {
            // 1. Set width to capturedElement’s old width.
            width = captured_element->old_width;

            // 2. Set height to capturedElement’s old height.
            height = captured_element->old_height;

            // 3. Set transform to capturedElement’s old transform.
            transform = captured_element->old_transform;

            // 4. Set writingMode to capturedElement’s old writing-mode.
            writing_mode = captured_element->old_writing_mode;

            // 5. Set direction to capturedElement’s old direction.
            direction = captured_element->old_direction;

            // 6. Set textOrientation to capturedElement’s old text-orientation.
            // FIXME: Implement this once we have text-orientation.

            // 7. Set mixBlendMode to capturedElement’s old mix-blend-mode.
            mix_blend_mode = captured_element->old_mix_blend_mode;

            // 8. Set backdropFilter to capturedElement’s old backdrop-filter.
            backdrop_filter = captured_element->old_backdrop_filter;

            // 9. Set colorScheme to capturedElement’s old color-scheme.
            color_scheme = captured_element->old_color_scheme;
        }

        // 3. Otherwise:
        else {
            // 1. Return failure if any of the following conditions is true:

            //    - capturedElement’s new element has a flat tree ancestor that skips its contents.
            for (auto ancestor = captured_element->new_element->flat_tree_parent_element(); ancestor; ancestor = ancestor->flat_tree_parent_element()) {
                if (ancestor->skips_its_contents())
                    return Error::from_string_literal("capturedElement’s new element has a flat tree ancestor that skips its contents.");
            }

            //    - capturedElement’s new element is not rendered.
            if (captured_element->new_element->not_rendered())
                return Error::from_string_literal("capturedElement’s new element is not rendered.");

            //    - capturedElement has more than one box fragment.
            // FIXME: Implement this once we have fragments.
            // FIXME: capturedElement would not have box fragments. Update this once the spec issue for that has been resolved:
            //        https://github.com/w3c/csswg-drafts/issues/11991

            // NOTE: Other rendering constraints are enforced via capturedElement’s new element being
            //       captured in a view transition.

            // 2. Let newRect be the snapshot containing block if capturedElement’s new element is the
            //    document element, otherwise, capturedElement’s border box.
            auto new_rect = captured_element->new_element->is_document_element() ? captured_element->new_element->navigable()->snapshot_containing_block() : captured_element->new_element->paintable_box()->absolute_border_box_rect();

            // 3. Set width to the current width of newRect.
            width = new_rect.width();

            // 4. Set height to the current height of newRect.
            height = new_rect.height();

            // 5. Set transform to a transform that would map newRect from the snapshot containing block origin
            //    to its current visual position.
            auto offset = new_rect.location() - captured_element->new_element->navigable()->snapshot_containing_block().location();
            transform = CSS::TransformationStyleValue::create(CSS::PropertyID::Transform, CSS::TransformFunction::Translate,
                CSS::StyleValueVector {
                    CSS::LengthStyleValue::create(CSS::Length::make_px(offset.x())),
                    CSS::LengthStyleValue::create(CSS::Length::make_px(offset.y())),
                });

            // 6. Set writingMode to the computed value of writing-mode on capturedElement’s new element.
            writing_mode = captured_element->new_element->layout_node()->computed_values().writing_mode();

            // 7. Set direction to the computed value of direction on capturedElement’s new element.
            direction = captured_element->new_element->layout_node()->computed_values().direction();

            // 8. Set textOrientation to the computed value of text-orientation on capturedElement’s new
            //    element.
            // FIXME: Implement this.

            // 9. Set mixBlendMode to the computed value of mix-blend-mode on capturedElement’s new
            //    element.
            mix_blend_mode = captured_element->new_element->layout_node()->computed_values().mix_blend_mode();

            // 10. Set backdropFilter to the computed value of backdrop-filter on capturedElement’s new element.
            backdrop_filter = captured_element->new_element->layout_node()->computed_values().backdrop_filter();

            // 11. Set colorScheme to the computed value of color-scheme on capturedElement’s new element.
            color_scheme = captured_element->new_element->layout_node()->computed_values().color_scheme();
        }

        // 4. If capturedElement’s group styles rule is null, then set capturedElement’s group styles rule to a new
        //    CSSStyleRule representing the following CSS, and append it to transition’s relevant global object’s
        //    associated document’s dynamic view transition style sheet.
        if (!captured_element->group_styles_rule) {
            // :root::view-transition-group(transitionName) {
            //   width: width;
            //   height: height;
            //   transform: transform;
            //   writing-mode: writingMode;
            //   direction: direction;
            //   text-orientation: textOrientation;
            //   mix-blend-mode: mixBlendMode;
            //   backdrop-filter: backdropFilter;
            //   color-scheme: colorScheme;
            // }
            // NOTE: The above code example contains variables to be replaced.
            auto stylesheet = as<HTML::Window>(HTML::relevant_global_object(*this)).associated_document().dynamic_view_transition_style_sheet();
            unsigned index = MUST(stylesheet->insert_rule(MUST(String::formatted(R"(
                :root::view-transition-group({}) {{
                    width: {};
                    height: {};
                    transform: {};
                    writing-mode: {};
                    direction: {};
                    text-orientation: {};
                    mix-blend-mode: {};
                    backdrop-filter: {};
                    color-scheme: {};
                }}
            )",
                                                              transition_name, width, height, "transform", "writing_mode", "direction", "text_orientation", "mix_blend_mode", "backdrop_filter", "color_scheme")),
                stylesheet->rules().length()));
            // FIXME: all the strings above should be the identically named variables, serialized somehow.
            captured_element->group_styles_rule = as<CSS::CSSStyleRule>(stylesheet->css_rules()->item(index));
        }
        // Otherwise, update capturedElement’s group styles rule to match the following CSS:
        // :root::view-transition-group(transitionName) {
        //   width: width;
        //   height: height;
        //   transform: transform;
        //   writing-mode: writingMode;
        //   direction: direction;
        //   text-orientation: textOrientation;
        //   mix-blend-mode: mixBlendMode;
        //   backdrop-filter: backdropFilter;
        //   color-scheme: colorScheme;
        // }
        // NOTE: The above code example contains variables to be replaced.
        else {
            captured_element->group_styles_rule->set_selector_text(MUST(String::formatted(":root::view-transition-group({0})", transition_name)));
            captured_element->group_styles_rule->set_css_text(MUST(String::formatted(R"(
                width: {};
                height: {};
                transform: {};
                writing-mode: {};
                direction: {};
                text-orientation: {};
                mix-blend-mode: {};
                backdrop-filter: {};
                color-scheme: {};
            )",
                width, height, "transform", "writing_mode", "direction", "text_orientation", "mix_blend_mode", "backdrop_filter", "color_scheme")));
            // FIXME: all the strings above should be the identically named variables, serialized somehow.
        }

        // 5. If capturedElement’s new element is not null, then:
        if (captured_element->new_element) {
            // 1. Let new be the ::view-transition-new() with the view transition name transitionName.
            ReplacedNamedViewTransitionPseudoElement* new_;
            m_transition_root_pseudo_element->for_each_in_inclusive_subtree_of_type<ReplacedNamedViewTransitionPseudoElement>([&](auto& element) {
                if (element.m_type == CSS::PseudoElement::ViewTransitionNew && element.m_view_transition_name == transition_name) {
                    new_ = &element;
                    return TraversalDecision::Break;
                }
                return TraversalDecision::Continue;
            });

            // 2. Set new’s replaced element content to the result of capturing the image of capturedElement’s
            //    new element.
            new_->m_content = captured_element->new_element->capture_the_image();
        }
    }
    return {};
}

// https://drafts.csswg.org/css-view-transitions-1/#clear-view-transition
void ViewTransition::clear_view_transition()
{
    // To clear view transition of a ViewTransition transition:

    // 1. Let document be transition’s relevant global object’s associated document.
    auto& document = as<HTML::Window>(HTML::relevant_global_object(*this)).associated_document();

    // 2. Assert: document’s active view transition is transition.
    VERIFY(document.active_view_transition() == this);

    // 3. For each capturedElement of transition’s named elements' values:
    for (auto captured_element : m_named_elements) {
        // 1. If capturedElement’s new element is not null, then set capturedElement’s new element's captured in a
        //    view transition to false.
        if (captured_element.value->new_element) {
            captured_element.value->new_element->set_captured_in_a_view_transition(false);
        }

        // 2. For each style of capturedElement’s style definitions:
        auto steps = [&](GC::Ptr<CSS::CSSRule> style) {
            // 1. If style is not null, and style is in document’s dynamic view transition style sheet, then remove
            //    style from document’s dynamic view transition style sheet.
            if (style) {
                auto stylesheet = document.dynamic_view_transition_style_sheet();
                auto rules = stylesheet->css_rules();
                for (u32 i = 0; i < rules->length(); i++) {
                    if (rules->item(i) == style) {
                        MUST(stylesheet->delete_rule(i));
                        break;
                    }
                }
            }
        };

        steps(captured_element.value->group_keyframes);
        steps(captured_element.value->group_animation_name_rule);
        steps(captured_element.value->group_styles_rule);
        steps(captured_element.value->image_pair_isolation_rule);
        steps(captured_element.value->image_animation_name_rule);
    }

    // 4. Set document’s show view transition tree to false.
    document.set_show_view_transition_tree(false);

    // 5. Set document’s active view transition to null.
    document.set_active_view_transition(nullptr);
}

}
