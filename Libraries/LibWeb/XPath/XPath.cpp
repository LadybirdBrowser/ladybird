/*
 * Copyright (c) 2025, Johannes Gustafsson <johannesgu@outlook.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashTable.h>
#include <AK/QuickSort.h>
#include <AK/StringConversions.h>
#include <AK/Utf16StringBuilder.h>
#include <LibGC/Heap.h>
#include <LibGC/RootHashTable.h>
#include <LibGC/RootVector.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibJS/Runtime/Value.h>
#include <LibWeb/DOM/Attr.h>
#include <LibWeb/DOM/CharacterData.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/NamedNodeMap.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/DOM/ParentNode.h>
#include <LibWeb/DOM/ProcessingInstruction.h>
#include <LibWeb/HTML/AttributeNames.h>
#include <LibWeb/HTML/Scripting/ExceptionReporter.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibXML/XPath/Program.h>
#include <math.h>

#include "XPath.h"
#include "XPathExpression.h"
#include "XPathNSResolver.h"
#include "XPathResult.h"

namespace Web::XPath {

namespace FFI = XML::FFI;

// A set of nodes without duplicates. If it isn't sorted, its order means nothing.
struct NodeSet {
    GC::RootVector<GC::Ref<DOM::Node>> nodes;
    bool is_in_document_order { true };
};

using Value = Variant<bool, double, Utf16String, NodeSet>;

struct Context {
    GC::Ref<DOM::Node> node;
    size_t position { 1 };
    size_t size { 1 };
};

// https://www.w3.org/TR/xml/#NT-S
// Not ASCII whitespace as HTML has it: a form feed is no white space in XML.
static constexpr auto XML_WHITESPACE = u" \t\n\r"sv;

// An attribute isn't a child of its element, but its element is its parent.
static DOM::Node* parent_of(DOM::Node& node)
{
    if (auto* attribute = as_if<DOM::Attr>(node))
        return attribute->owner_element();
    return node.parent();
}

static DOM::Node& root_of(DOM::Node& node)
{
    auto* parent = parent_of(node);
    return parent ? parent->root() : node;
}

// Whether `a` comes before `b` in document order, where an element's attributes come after it and before its children.
static bool is_before(DOM::Node& a, DOM::Node& b)
{
    return b.compare_document_position(a) & DOM::Node::DOCUMENT_POSITION_PRECEDING;
}

static void sort_in_document_order(NodeSet& set)
{
    if (set.is_in_document_order)
        return;
    set.is_in_document_order = true;

    // Comparing two nodes costs their depth plus the width of the tree where their paths part, so sorting by
    // comparison is only for sets small enough that walking the tree would cost more.
    static constexpr size_t LARGEST_SET_TO_SORT_BY_COMPARING = 16;
    if (set.nodes.size() <= LARGEST_SET_TO_SORT_BY_COMPARING) {
        quick_sort(set.nodes, [](auto& a, auto& b) { return is_before(a, b); });
        return;
    }

    // OPTIMIZATION: Walk the trees the nodes are in once, and pick the set's nodes out as they come by.
    GC::RootHashTable<DOM::Node*> members;
    GC::RootVector<DOM::Node*> roots;
    bool has_attributes = false;
    for (auto& node : set.nodes) {
        members.set(node.ptr());
        has_attributes |= node->is_attribute();
        auto* root = &root_of(node);
        if (!roots.contains_slow(root))
            roots.append(root);
    }
    quick_sort(roots);

    auto remaining = set.nodes.size();
    set.nodes.clear_with_capacity();
    auto visit = [&](DOM::Node& node) {
        if (members.contains(&node)) {
            set.nodes.unchecked_append(node);
            --remaining;
        }
    };
    for (auto* root : roots) {
        for (auto* node = root; node && remaining > 0; node = node->next_in_pre_order()) {
            visit(*node);
            if (!has_attributes)
                continue;
            if (auto* element = as_if<DOM::Element>(*node)) {
                for (size_t i = 0; i < element->attribute_list_size(); ++i)
                    visit(*element->attributes()->item(i));
            }
        }
    }
    VERIFY(remaining == 0);
}

static GC::Ptr<DOM::Node> first_in_document_order(NodeSet const& set)
{
    if (set.nodes.is_empty())
        return nullptr;
    auto first = set.nodes[0];
    if (!set.is_in_document_order) {
        for (auto& node : set.nodes) {
            if (is_before(node, first))
                first = node;
        }
    }
    return first;
}

// https://www.w3.org/TR/1999/REC-xpath-19991116/#data-model
static Utf16String string_value(DOM::Node& node)
{
    // A document's text content is null, but the string-value of a root node is all the text in it.
    if (node.is_document())
        return node.descendant_text_content();
    return node.text_content().value_or({});
}

// https://www.w3.org/TR/1999/REC-xpath-19991116/#function-number
static double string_to_number(Utf16View string)
{
    // Number ::= Digits ('.' Digits?)? | '.' Digits, with an optional minus sign before it and white space around it.
    // That is what a number is to parse_number() too, once exponents and plus signs are out of the way.
    string = string.trim(XML_WHITESPACE);
    for (size_t i = 0; i < string.length_in_code_units(); ++i) {
        auto code_unit = string.code_unit_at(i);
        if (!is_ascii_digit(code_unit) && code_unit != '.' && code_unit != '-')
            return NAN;
    }
    return parse_number<double>(string, TrimWhitespace::No).value_or(NAN);
}

// https://www.w3.org/TR/1999/REC-xpath-19991116/#function-normalize-space
static Utf16String normalize_space(Utf16View string)
{
    Utf16StringBuilder builder;
    bool pending_space = false;
    for (size_t i = 0; i < string.length_in_code_units(); ++i) {
        auto code_unit = string.code_unit_at(i);
        if (XML_WHITESPACE.contains(code_unit)) {
            pending_space = !builder.is_empty();
            continue;
        }
        if (pending_space)
            builder.append_code_unit(' ');
        pending_space = false;
        builder.append_code_unit(code_unit);
    }
    return builder.to_string();
}

static bool to_boolean(Value const& value)
{
    return value.visit(
        [](bool boolean) { return boolean; },
        [](double number) { return number != 0 && !isnan(number); },
        [](Utf16String const& string) { return !string.is_empty(); },
        [](NodeSet const& set) { return !set.nodes.is_empty(); });
}

static Utf16String to_string(Value const& value)
{
    return value.visit(
        [](bool boolean) { return boolean ? "true"_utf16 : "false"_utf16; },
        // A number is written the way script writes one, except that it never has an exponent.
        [](double number) { return JS::number_to_utf16_string(number, JS::NumberToStringMode::WithoutExponent); },
        [](Utf16String const& string) { return string; },
        [](NodeSet const& set) {
            auto first = first_in_document_order(set);
            return first ? string_value(*first) : Utf16String {};
        });
}

static double to_number(Value const& value)
{
    return value.visit(
        [](bool boolean) { return boolean ? 1.0 : 0.0; },
        [](double number) { return number; },
        [](Utf16String const& string) { return string_to_number(string); },
        [&](NodeSet const&) { return string_to_number(to_string(value)); });
}

// https://www.w3.org/TR/1999/REC-xpath-19991116/#function-round
static double round_number(double number)
{
    if (!isfinite(number))
        return number;
    if (signbit(number) && number >= -0.5)
        return -0.0;
    // Not floor(number + 0.5): the sum is rounded itself, which takes the number just below a half up to one and an
    // odd integer too large to have a fraction up to the next even one.
    auto rounded = floor(number);
    return number - rounded >= 0.5 ? rounded + 1 : rounded;
}

static bool compare_numbers(FFI::XPathKind kind, double left, double right)
{
    switch (kind) {
    case FFI::XPathKind::Equal:
        return left == right;
    case FFI::XPathKind::NotEqual:
        return left != right;
    case FFI::XPathKind::Less:
        return left < right;
    case FFI::XPathKind::LessOrEqual:
        return left <= right;
    case FFI::XPathKind::Greater:
        return left > right;
    case FFI::XPathKind::GreaterOrEqual:
        return left >= right;
    default:
        VERIFY_NOT_REACHED();
    }
}

static bool is_equality(FFI::XPathKind kind)
{
    return kind == FFI::XPathKind::Equal || kind == FFI::XPathKind::NotEqual;
}

// Compares two values of which neither is a node-set.
static bool compare_simple_values(FFI::XPathKind kind, Value const& left, Value const& right)
{
    if (is_equality(kind)) {
        bool equal;
        if (left.has<bool>() || right.has<bool>())
            equal = to_boolean(left) == to_boolean(right);
        else if (left.has<double>() || right.has<double>())
            equal = to_number(left) == to_number(right);
        else
            equal = left.get<Utf16String>() == right.get<Utf16String>();
        return equal == (kind == FFI::XPathKind::Equal);
    }
    return compare_numbers(kind, to_number(left), to_number(right));
}

// The comparison with its operands the other way around: a < b is b > a.
static FFI::XPathKind mirrored(FFI::XPathKind kind)
{
    switch (kind) {
    case FFI::XPathKind::Less:
        return FFI::XPathKind::Greater;
    case FFI::XPathKind::LessOrEqual:
        return FFI::XPathKind::GreaterOrEqual;
    case FFI::XPathKind::Greater:
        return FFI::XPathKind::Less;
    case FFI::XPathKind::GreaterOrEqual:
        return FFI::XPathKind::LessOrEqual;
    default:
        return kind;
    }
}

// https://www.w3.org/TR/1999/REC-xpath-19991116/#booleans
static bool compare_values(FFI::XPathKind kind, Value const& left, Value const& right)
{
    if (!left.has<NodeSet>() && !right.has<NodeSet>())
        return compare_simple_values(kind, left, right);
    if (!left.has<NodeSet>())
        return compare_values(mirrored(kind), right, left);

    // A node-set compares true to a value if one of its nodes does.
    auto const& left_set = left.get<NodeSet>();
    if (right.has<bool>())
        return compare_simple_values(kind, to_boolean(left), right);
    if (right.has<double>()) {
        for (auto& node : left_set.nodes) {
            if (compare_numbers(kind, string_to_number(string_value(node)), right.get<double>()))
                return true;
        }
        return false;
    }
    if (right.has<Utf16String>()) {
        for (auto& node : left_set.nodes) {
            if (compare_simple_values(kind, string_value(node), right))
                return true;
        }
        return false;
    }

    // Two node-sets compare true if a node of one and a node of the other do.
    auto const& right_set = right.get<NodeSet>();
    if (left_set.nodes.is_empty() || right_set.nodes.is_empty())
        return false;
    if (kind == FFI::XPathKind::Equal) {
        // OPTIMIZATION: Look each string of one set up in the other, which is the common way to join two sets.
        HashTable<Utf16String> right_strings;
        for (auto& node : right_set.nodes)
            right_strings.set(string_value(node));
        for (auto& node : left_set.nodes) {
            if (right_strings.contains(string_value(node)))
                return true;
        }
        return false;
    }
    if (kind == FFI::XPathKind::NotEqual) {
        // OPTIMIZATION: Some two strings differ unless every node of both sets has the same one, so each node is
        //               looked at once, and no pair of them is.
        auto first = string_value(left_set.nodes[0]);
        auto has_another_string = [&](NodeSet const& set) {
            for (auto& node : set.nodes) {
                if (string_value(node) != first)
                    return true;
            }
            return false;
        };
        return has_another_string(left_set) || has_another_string(right_set);
    }

    // OPTIMIZATION: Some number of one set is less than some number of the other exactly if its smallest is less than
    //               the other's largest, so each node is looked at once, and no pair of them is. A node that is no
    //               number compares false to everything, and so does a set of nothing but such nodes.
    struct Range {
        double smallest { NAN };
        double largest { NAN };
    };
    auto range_of = [](NodeSet const& set) {
        Range range;
        for (auto& node : set.nodes) {
            auto number = string_to_number(string_value(node));
            if (isnan(number))
                continue;
            if (isnan(range.smallest) || number < range.smallest)
                range.smallest = number;
            if (isnan(range.largest) || number > range.largest)
                range.largest = number;
        }
        return range;
    };
    auto left_range = range_of(left_set);
    auto right_range = range_of(right_set);
    if (kind == FFI::XPathKind::Less || kind == FFI::XPathKind::LessOrEqual)
        return compare_numbers(kind, left_range.smallest, right_range.largest);
    return compare_numbers(kind, left_range.largest, right_range.smallest);
}

static bool is_reverse_axis(FFI::XPathAxis axis)
{
    return first_is_one_of(axis, FFI::XPathAxis::Ancestor, FFI::XPathAxis::AncestorOrSelf, FFI::XPathAxis::Preceding, FFI::XPathAxis::PrecedingSibling);
}

// Calls `callback` with each node on `axis` of `context`, nearest first: in document order for a forward axis and in
// reverse document order for a reverse one.
template<typename Callback>
static void for_each_node_on_axis(FFI::XPathAxis axis, DOM::Node& context, Callback callback)
{
    auto* attribute = as_if<DOM::Attr>(context);

    switch (axis) {
    case FFI::XPathAxis::Self_:
        callback(context);
        return;
    case FFI::XPathAxis::Child:
        for (auto* child = context.first_child(); child; child = child->next_sibling())
            callback(*child);
        return;
    case FFI::XPathAxis::DescendantOrSelf:
        callback(context);
        [[fallthrough]];
    case FFI::XPathAxis::Descendant:
        for (auto* node = context.first_child(); node; node = node->next_in_pre_order(&context))
            callback(*node);
        return;
    case FFI::XPathAxis::Parent:
        if (auto* parent = parent_of(context))
            callback(*parent);
        return;
    case FFI::XPathAxis::AncestorOrSelf:
        callback(context);
        [[fallthrough]];
    case FFI::XPathAxis::Ancestor:
        for (auto* node = parent_of(context); node; node = node->parent())
            callback(*node);
        return;
    case FFI::XPathAxis::FollowingSibling:
        for (auto* node = context.next_sibling(); node; node = node->next_sibling())
            callback(*node);
        return;
    case FFI::XPathAxis::PrecedingSibling:
        for (auto* node = context.previous_sibling(); node; node = node->previous_sibling())
            callback(*node);
        return;
    case FFI::XPathAxis::Following: {
        DOM::Node* node = nullptr;
        if (attribute) {
            // What follows an attribute starts with its element's children.
            if (auto* element = attribute->owner_element())
                node = element->next_in_pre_order();
        } else {
            // What follows a node starts after its last descendant.
            for (auto* ancestor = &context; ancestor && !node; ancestor = ancestor->parent())
                node = ancestor->next_sibling();
        }
        for (; node; node = node->next_in_pre_order())
            callback(*node);
        return;
    }
    case FFI::XPathAxis::Preceding: {
        // Everything before the node except its ancestors.
        auto* node = attribute ? attribute->owner_element() : &context;
        if (!node)
            return;
        while (auto* parent = node->parent()) {
            for (node = node->previous_in_pre_order(); node != parent; node = node->previous_in_pre_order())
                callback(*node);
            node = parent;
        }
        return;
    }
    case FFI::XPathAxis::Attribute: {
        auto* element = as_if<DOM::Element>(context);
        if (!element)
            return;
        for (size_t i = 0; i < element->attribute_list_size(); ++i) {
            // A namespace declaration is a namespace node in XPath's data model, not an attribute.
            auto attribute = element->attributes()->item(i);
            if (attribute->namespace_uri() != Namespace::XMLNS)
                callback(*attribute);
        }
        return;
    }
    case FFI::XPathAxis::Namespace:
        // The DOM has no namespace nodes.
        return;
    }
    VERIFY_NOT_REACHED();
}

// Unlike getElementById(), id() selects every element that has the id.
template<typename Callback>
static void for_each_element_with_id(DOM::Node& root, Utf16View id, Callback callback)
{
    if (auto* document = as_if<DOM::Document>(root)) {
        document->element_by_id().for_each_element_with_id(id, root, [&](DOM::Element& element) { callback(element); });
        return;
    }
    root.for_each_in_inclusive_subtree_of_type<DOM::Element>([&](DOM::Element& element) {
        if (element.id().has_value() && element.id()->view() == id)
            callback(element);
        return TraversalDecision::Continue;
    });
}

class Evaluator {
public:
    explicit Evaluator(Expression const& expression)
        : m_expression(expression)
        , m_program(XML::XPath::view(*expression.m_program))
    {
    }

    Value run(DOM::Node& context_node)
    {
        return evaluate(m_program.root, { .node = context_node });
    }

    // Whether a value that isn't a node-set was used where only a node-set makes sense.
    bool had_type_error() const { return m_had_type_error; }

private:
    ReadonlySpan<u32> list(u32 first, u32 count) const
    {
        return { m_program.lists + first, count };
    }

    NodeSet to_node_set(Value value)
    {
        if (auto* set = value.get_pointer<NodeSet>())
            return move(*set);
        m_had_type_error = true;
        return {};
    }

    bool node_matches(FFI::XPathStep const& step, Expression::Name const& name, DOM::Node& node) const
    {
        switch (static_cast<FFI::XPathNodeTest>(step.test)) {
        case FFI::XPathNodeTest::Node:
            return true;
        case FFI::XPathNodeTest::Text:
            return node.is_text();
        case FFI::XPathNodeTest::Comment:
            return node.is_comment();
        case FFI::XPathNodeTest::ProcessingInstruction:
            return is<DOM::ProcessingInstruction>(node);
        case FFI::XPathNodeTest::ProcessingInstructionWithTarget: {
            auto* processing_instruction = as_if<DOM::ProcessingInstruction>(node);
            return processing_instruction && processing_instruction->target() == name.local_name;
        }
        case FFI::XPathNodeTest::AnyName:
        case FFI::XPathNodeTest::AnyNameInNamespace:
        case FFI::XPathNodeTest::Name:
            break;
        }

        // A name test only matches nodes of its axis' principal type: attributes on the attribute axis and elements
        // on every other.
        bool any_name = static_cast<FFI::XPathNodeTest>(step.test) != FFI::XPathNodeTest::Name;
        if (static_cast<FFI::XPathAxis>(step.axis) == FFI::XPathAxis::Attribute) {
            auto& attribute = as<DOM::Attr>(node);
            if (any_name)
                return !name.namespace_.has_value() || attribute.namespace_uri() == name.namespace_;
            // https://html.spec.whatwg.org/multipage/infrastructure.html#interactions-with-xpath-and-xslt
            // An attribute of an HTML element in an HTML document is matched without regard to ASCII case.
            if (!name.namespace_.has_value() && !attribute.namespace_uri().has_value() && node.document().is_html_document()) {
                auto const* element = attribute.owner_element();
                if (element && element->namespace_uri() == Namespace::HTML)
                    return attribute.local_name().view().equals_ignoring_ascii_case(name.local_name.view());
            }
            return attribute.local_name() == name.local_name && attribute.namespace_uri() == name.namespace_;
        }

        auto* element = as_if<DOM::Element>(node);
        if (!element)
            return false;
        if (any_name)
            return !name.namespace_.has_value() || element->namespace_uri() == name.namespace_;
        // https://html.spec.whatwg.org/multipage/infrastructure.html#interactions-with-xpath-and-xslt
        // In an HTML document, a name without a prefix is one in the HTML namespace, and it is matched to an HTML
        // element's name without regard to ASCII case.
        if (node.document().is_html_document()) {
            if (element->namespace_uri() == Namespace::HTML) {
                return (!name.namespace_.has_value() || name.namespace_ == Namespace::HTML)
                    && element->local_name().view().equals_ignoring_ascii_case(name.local_name.view());
            }
            return name.namespace_.has_value() && element->local_name() == name.local_name && element->namespace_uri() == name.namespace_;
        }
        return element->local_name() == name.local_name && element->namespace_uri() == name.namespace_;
    }

    bool predicate_holds(u32 predicate, Context const& context)
    {
        auto value = evaluate(predicate, context);
        // A number is short for a test of the position: [3] is [position() = 3].
        if (auto* number = value.get_pointer<double>())
            return *number == static_cast<double>(context.position);
        return to_boolean(value);
    }

    // Narrows `nodes`, which are in the order their positions count in, to those that each of `predicates` holds for.
    void apply_predicates(ReadonlySpan<u32> predicates, GC::RootVector<GC::Ref<DOM::Node>>& nodes)
    {
        for (auto predicate : predicates) {
            size_t kept = 0;
            auto size = nodes.size();
            for (size_t i = 0; i < size; ++i) {
                if (predicate_holds(predicate, { .node = nodes[i], .position = i + 1, .size = size }))
                    nodes[kept++] = nodes[i];
            }
            nodes.shrink(kept, true);
        }
    }

    // https://www.w3.org/TR/1999/REC-xpath-19991116/#location-paths
    NodeSet apply_steps(NodeSet input, u32 first_step, u32 step_count)
    {
        for (u32 step_index = first_step; step_index < first_step + step_count; ++step_index) {
            auto const& step = m_program.steps[step_index];
            auto const& name = m_expression.m_names[step_index];
            auto axis = static_cast<FFI::XPathAxis>(step.axis);
            auto predicates = list(step.first_predicate, step.predicate_count);

            // Two context nodes can only reach the same node on an axis that goes past a context node's own children.
            bool several_contexts = input.nodes.size() > 1;
            bool can_repeat = several_contexts && !first_is_one_of(axis, FFI::XPathAxis::Self_, FFI::XPathAxis::Child, FFI::XPathAxis::Attribute);

            NodeSet output;
            GC::RootHashTable<DOM::Node*> seen;
            GC::RootVector<GC::Ref<DOM::Node>> matches;
            for (auto& context_node : input.nodes) {
                matches.clear_with_capacity();
                for_each_node_on_axis(axis, context_node, [&](DOM::Node& node) {
                    if (node_matches(step, name, node))
                        matches.append(node);
                });
                apply_predicates(predicates, matches);
                for (auto& match : matches) {
                    if (!can_repeat || seen.set(match.ptr()) == HashSetResult::InsertedNewEntry)
                        output.nodes.append(match);
                }
            }

            if (!several_contexts) {
                // OPTIMIZATION: What one context node selects on a reverse axis is in reverse document order, so
                //               turning it around saves sorting it later.
                if (is_reverse_axis(axis))
                    output.nodes.reverse();
            } else {
                output.is_in_document_order = output.nodes.size() < 2
                    || (input.is_in_document_order && first_is_one_of(axis, FFI::XPathAxis::Self_, FFI::XPathAxis::Attribute));
            }
            input = move(output);
        }
        return input;
    }

    Utf16String string_argument(ReadonlySpan<u32> arguments, size_t index, Context const& context)
    {
        if (index < arguments.size())
            return to_string(evaluate(arguments[index], context));
        return string_value(context.node);
    }

    // The node a function like name() is about: the first of its argument, or the context node if it has none.
    GC::Ptr<DOM::Node> node_argument(ReadonlySpan<u32> arguments, Context const& context)
    {
        if (arguments.is_empty())
            return context.node;
        auto value = evaluate(arguments[0], context);
        if (auto* set = value.get_pointer<NodeSet>())
            return first_in_document_order(*set);
        // Nothing converts a boolean, a number or a string to a node-set.
        m_had_type_error = true;
        return nullptr;
    }

    // https://www.w3.org/TR/1999/REC-xpath-19991116/#corelib
    Value call_function(FFI::XPathFunction function, ReadonlySpan<u32> arguments, Context const& context)
    {
        auto string = [&](size_t index) { return string_argument(arguments, index, context); };
        auto number = [&](size_t index) { return to_number(evaluate(arguments[index], context)); };

        switch (function) {
        case FFI::XPathFunction::Last:
            return static_cast<double>(context.size);
        case FFI::XPathFunction::Position:
            return static_cast<double>(context.position);
        case FFI::XPathFunction::Count:
            return static_cast<double>(to_node_set(evaluate(arguments[0], context)).nodes.size());
        case FFI::XPathFunction::Id:
            return function_id(evaluate(arguments[0], context), context);
        case FFI::XPathFunction::LocalName: {
            auto node = node_argument(arguments, context);
            if (auto* element = as_if<DOM::Element>(node.ptr()))
                return element->local_name().to_utf16_string();
            if (auto* attribute = as_if<DOM::Attr>(node.ptr()))
                return attribute->local_name().to_utf16_string();
            if (auto* processing_instruction = as_if<DOM::ProcessingInstruction>(node.ptr()))
                return processing_instruction->target().to_utf16_string();
            return Utf16String {};
        }
        case FFI::XPathFunction::NamespaceUri: {
            auto node = node_argument(arguments, context);
            Optional<Utf16FlyString> namespace_;
            if (auto* element = as_if<DOM::Element>(node.ptr()))
                namespace_ = element->namespace_uri();
            else if (auto* attribute = as_if<DOM::Attr>(node.ptr()))
                namespace_ = attribute->namespace_uri();
            return namespace_.has_value() ? namespace_->to_utf16_string() : Utf16String {};
        }
        case FFI::XPathFunction::Name: {
            auto node = node_argument(arguments, context);
            if (auto* element = as_if<DOM::Element>(node.ptr()))
                return element->qualified_name().to_utf16_string();
            if (auto* attribute = as_if<DOM::Attr>(node.ptr()))
                return attribute->name().to_utf16_string();
            if (auto* processing_instruction = as_if<DOM::ProcessingInstruction>(node.ptr()))
                return processing_instruction->target().to_utf16_string();
            return Utf16String {};
        }
        case FFI::XPathFunction::String:
            return string(0);
        case FFI::XPathFunction::Concat: {
            Utf16StringBuilder builder;
            for (size_t i = 0; i < arguments.size(); ++i)
                builder.append(string(i));
            return builder.to_string();
        }
        case FFI::XPathFunction::StartsWith: {
            auto haystack = string(0);
            auto needle = string(1);
            return needle.is_empty() || haystack.utf16_view().starts_with(needle.utf16_view());
        }
        case FFI::XPathFunction::Contains: {
            auto haystack = string(0);
            auto needle = string(1);
            return needle.is_empty() || haystack.utf16_view().contains(needle.utf16_view());
        }
        case FFI::XPathFunction::SubstringBefore: {
            auto haystack = string(0);
            auto needle = string(1);
            if (needle.is_empty())
                return Utf16String {};
            auto offset = haystack.utf16_view().find_code_unit_offset(needle.utf16_view());
            if (!offset.has_value())
                return Utf16String {};
            return Utf16String::from_utf16(haystack.utf16_view().substring_view(0, *offset));
        }
        case FFI::XPathFunction::SubstringAfter: {
            auto haystack = string(0);
            auto needle = string(1);
            if (needle.is_empty())
                return haystack;
            auto offset = haystack.utf16_view().find_code_unit_offset(needle.utf16_view());
            if (!offset.has_value())
                return Utf16String {};
            return Utf16String::from_utf16(haystack.utf16_view().substring_view(*offset + needle.length_in_code_units()));
        }
        case FFI::XPathFunction::Substring: {
            auto source = string(0);
            auto start = round_number(number(1));
            auto end = start + (arguments.size() == 3 ? round_number(number(2)) : static_cast<double>(INFINITY));
            if (isnan(start) || isnan(end))
                return Utf16String {};
            // Positions count from one, and the end is the position after the last character.
            auto length = static_cast<double>(source.length_in_code_units());
            auto first = clamp(start, 1.0, length + 1);
            auto last = clamp(end, first, length + 1);
            return Utf16String::from_utf16(source.utf16_view().substring_view(static_cast<size_t>(first) - 1, static_cast<size_t>(last - first)));
        }
        case FFI::XPathFunction::StringLength:
            return static_cast<double>(string(0).length_in_code_units());
        case FFI::XPathFunction::NormalizeSpace:
            return normalize_space(string(0));
        case FFI::XPathFunction::Translate: {
            auto source = string(0);
            auto from = string(1);
            auto to = string(2);
            Utf16StringBuilder builder;
            for (size_t i = 0; i < source.length_in_code_units(); ++i) {
                auto code_unit = source.code_unit_at(i);
                auto offset = from.utf16_view().find_code_unit_offset(code_unit);
                if (!offset.has_value())
                    builder.append_code_unit(code_unit);
                else if (*offset < to.length_in_code_units())
                    builder.append_code_unit(to.code_unit_at(*offset));
            }
            return builder.to_string();
        }
        case FFI::XPathFunction::Boolean:
            return to_boolean(evaluate(arguments[0], context));
        case FFI::XPathFunction::Not:
            return !to_boolean(evaluate(arguments[0], context));
        case FFI::XPathFunction::True:
            return true;
        case FFI::XPathFunction::False:
            return false;
        case FFI::XPathFunction::Lang:
            return function_lang(string(0), context);
        case FFI::XPathFunction::Number:
            if (arguments.is_empty())
                return string_to_number(string_value(context.node));
            return number(0);
        case FFI::XPathFunction::Sum: {
            double sum = 0;
            for (auto& node : to_node_set(evaluate(arguments[0], context)).nodes)
                sum += string_to_number(string_value(node));
            return sum;
        }
        case FFI::XPathFunction::Floor:
            return floor(number(0));
        case FFI::XPathFunction::Ceiling:
            return ceil(number(0));
        case FFI::XPathFunction::Round:
            return round_number(number(0));
        }
        VERIFY_NOT_REACHED();
    }

    // https://www.w3.org/TR/1999/REC-xpath-19991116/#function-id
    Value function_id(Value const& argument, Context const& context)
    {
        // The ids are the white space separated words of the argument, or of each node's string-value if it is a
        // node-set.
        Vector<Utf16String> lists;
        if (auto* set = argument.get_pointer<NodeSet>()) {
            for (auto& node : set->nodes)
                lists.append(string_value(node));
        } else {
            lists.append(to_string(argument));
        }

        NodeSet result;
        result.is_in_document_order = false;
        auto& root = root_of(context.node);
        GC::RootHashTable<DOM::Node*> seen;
        for (auto& ids : lists) {
            auto normalized = normalize_space(ids);
            normalized.utf16_view().for_each_split_view(' ', SplitBehavior::Nothing, [&](Utf16View id) {
                for_each_element_with_id(root, id, [&](DOM::Element& element) {
                    if (seen.set(&element) == HashSetResult::InsertedNewEntry)
                        result.nodes.append(element);
                });
                return IterationDecision::Continue;
            });
        }
        return result;
    }

    // https://www.w3.org/TR/1999/REC-xpath-19991116/#function-lang
    static bool function_lang(Utf16String const& language, Context const& context)
    {
        for (auto* node = context.node->is_attribute() ? parent_of(context.node) : context.node.ptr(); node; node = node->parent()) {
            auto* element = as_if<DOM::Element>(*node);
            if (!element)
                continue;
            auto xml_lang = element->get_attribute_ns(Namespace::XML, HTML::AttributeNames::lang);
            if (!xml_lang.has_value())
                continue;
            // The nearest xml:lang decides: it has to be the language, or the language and a suffix that starts with
            // a hyphen.
            auto const& value = *xml_lang;
            auto length = language.length_in_code_units();
            if (value.length_in_code_units() < length)
                return false;
            if (!value.utf16_view().substring_view(0, length).equals_ignoring_ascii_case(language.utf16_view()))
                return false;
            return value.length_in_code_units() == length || value.code_unit_at(length) == '-';
        }
        return false;
    }

    FFI::XPathKind kind_of(u32 index) const { return static_cast<FFI::XPathKind>(m_program.kinds[index]); }

    // The binary operators are the first kinds there are.
    static bool is_binary_operator(FFI::XPathKind kind) { return to_underlying(kind) <= to_underlying(FFI::XPathKind::Union); }

    Value evaluate(u32 index, Context const& context)
    {
        // The binary operators associate to the left, so "1 + 1 + 1" is a tree in which each operator is the left
        // operand of the next: as deep as the expression is long, which nothing limits. So the left operands are
        // followed in a loop, and only what the compiler limits the nesting of is evaluated by recursion.
        Vector<u32, 8> operators;
        while (is_binary_operator(kind_of(index))) {
            operators.append(index);
            index = m_program.a[index];
        }
        auto value = evaluate_operand(index, context);
        while (!operators.is_empty())
            value = apply_binary_operator(operators.take_last(), move(value), context);
        return value;
    }

    // Evaluates the binary operator at `index`, whose left operand has the value `left`.
    Value apply_binary_operator(u32 index, Value left, Context const& context)
    {
        auto kind = kind_of(index);
        auto b = m_program.b[index];

        switch (kind) {
        case FFI::XPathKind::Or:
            return to_boolean(left) || to_boolean(evaluate(b, context));
        case FFI::XPathKind::And:
            return to_boolean(left) && to_boolean(evaluate(b, context));
        case FFI::XPathKind::Equal:
        case FFI::XPathKind::NotEqual:
        case FFI::XPathKind::Less:
        case FFI::XPathKind::LessOrEqual:
        case FFI::XPathKind::Greater:
        case FFI::XPathKind::GreaterOrEqual: {
            auto right = evaluate(b, context);
            return compare_values(kind, left, right);
        }
        case FFI::XPathKind::Add:
        case FFI::XPathKind::Subtract:
        case FFI::XPathKind::Multiply:
        case FFI::XPathKind::Divide:
        case FFI::XPathKind::Modulo: {
            auto left_number = to_number(left);
            auto right_number = to_number(evaluate(b, context));
            if (kind == FFI::XPathKind::Add)
                return left_number + right_number;
            if (kind == FFI::XPathKind::Subtract)
                return left_number - right_number;
            if (kind == FFI::XPathKind::Multiply)
                return left_number * right_number;
            if (kind == FFI::XPathKind::Divide)
                return left_number / right_number;
            return fmod(left_number, right_number);
        }
        case FFI::XPathKind::Union: {
            auto result = to_node_set(move(left));
            auto right = to_node_set(evaluate(b, context));
            if (right.nodes.is_empty())
                return result;
            GC::RootHashTable<DOM::Node*> seen;
            for (auto& node : result.nodes)
                seen.set(node.ptr());
            for (auto& node : right.nodes) {
                if (seen.set(node.ptr()) == HashSetResult::InsertedNewEntry)
                    result.nodes.append(node);
            }
            result.is_in_document_order = result.nodes.size() < 2;
            return result;
        }
        default:
            VERIFY_NOT_REACHED();
        }
    }

    // Evaluates the node at `index`, which is anything but a binary operator.
    Value evaluate_operand(u32 index, Context const& context)
    {
        auto kind = kind_of(index);
        auto a = m_program.a[index];
        auto b = m_program.b[index];
        auto c = m_program.c[index];

        switch (kind) {
        case FFI::XPathKind::Or:
        case FFI::XPathKind::And:
        case FFI::XPathKind::Equal:
        case FFI::XPathKind::NotEqual:
        case FFI::XPathKind::Less:
        case FFI::XPathKind::LessOrEqual:
        case FFI::XPathKind::Greater:
        case FFI::XPathKind::GreaterOrEqual:
        case FFI::XPathKind::Add:
        case FFI::XPathKind::Subtract:
        case FFI::XPathKind::Multiply:
        case FFI::XPathKind::Divide:
        case FFI::XPathKind::Modulo:
        case FFI::XPathKind::Union:
            // evaluate() takes these apart.
            VERIFY_NOT_REACHED();
        case FFI::XPathKind::Negate:
            return -to_number(evaluate(a, context));
        case FFI::XPathKind::Number:
            return m_program.numbers[a];
        case FFI::XPathKind::String:
            return m_expression.m_strings.get(index).value();
        case FFI::XPathKind::Variable:
            // An expression with a variable reference doesn't compile, as there is no way to give it a value.
            VERIFY_NOT_REACHED();
        case FFI::XPathKind::Function:
            return call_function(static_cast<FFI::XPathFunction>(a), list(b, c), context);
        case FFI::XPathKind::Filter: {
            // The positions in a filter's predicates count in document order.
            auto set = to_node_set(evaluate(a, context));
            sort_in_document_order(set);
            apply_predicates(list(b, c), set.nodes);
            return set;
        }
        case FFI::XPathKind::RelativePath: {
            NodeSet start;
            start.nodes.append(context.node);
            return apply_steps(move(start), b, c);
        }
        case FFI::XPathKind::AbsolutePath: {
            // The root is that of the tree the context node is in, which needn't be a document.
            NodeSet start;
            start.nodes.append(root_of(context.node));
            return apply_steps(move(start), b, c);
        }
        case FFI::XPathKind::ExpressionPath:
            return apply_steps(to_node_set(evaluate(a, context)), b, c);
        }
        VERIFY_NOT_REACHED();
    }

    Expression const& m_expression;
    FFI::XPathProgramView m_program;
    bool m_had_type_error { false };
};

static WebIDL::ExceptionOr<Utf16FlyString> resolve_prefix(GC::Ptr<XPathNSResolver> resolver, Utf16String const& prefix)
{
    auto unknown_prefix = [&] {
        return WebIDL::NamespaceError::create(Utf16String::formatted("The prefix '{}' in the XPath expression has no namespace", prefix));
    };
    if (!resolver)
        return unknown_prefix();

    auto& callback = resolver->callback();
    auto& vm = callback.associated_realm().vm();
    auto completion = WebIDL::call_user_object_operation(callback, "lookupNamespaceURI"_utf16_fly_string, {}, { { JS::Value { JS::PrimitiveString::create(vm, prefix) } } });
    // https://webidl.spec.whatwg.org/#call-a-user-objects-operation
    // What the resolver throws is reported and not thrown on to the caller, who is told the prefix is unknown.
    if (completion.is_abrupt()) {
        HTML::report_an_exception(callback.associated_realm().global_object(), completion.value());
        return unknown_prefix();
    }
    auto result = completion.value();
    if (result.is_nullish())
        return unknown_prefix();
    auto namespace_ = WebIDL::to_utf16_string(vm, result);
    if (namespace_.is_error()) {
        HTML::report_an_exception(callback.associated_realm().global_object(), namespace_.error_value());
        return unknown_prefix();
    }
    return Utf16FlyString::from_utf16(namespace_.value().utf16_view());
}

WebIDL::ExceptionOr<NonnullOwnPtr<Expression>> Expression::compile(Utf16String const& source, GC::Ptr<XPathNSResolver> resolver)
{
    auto utf8 = source.to_utf8();
    auto bytes = utf8.bytes();
    auto expression = adopt_own(*new Expression);
    expression->m_program = XML::XPath::compile(bytes);
    if (!expression->m_program)
        return WebIDL::SyntaxError::create("Invalid XPath expression"_utf16);

    auto program = XML::XPath::view(*expression->m_program);
    auto text = [&](u32 offset, u32 length) {
        return Utf16String::from_utf8_without_validation(bytes.slice(offset, length));
    };

    for (size_t i = 0; i < program.node_count; ++i) {
        auto kind = static_cast<FFI::XPathKind>(program.kinds[i]);
        if (kind == FFI::XPathKind::Variable)
            return WebIDL::SyntaxError::create("XPath variable references are not supported"_utf16);
        if (kind == FFI::XPathKind::String)
            expression->m_strings.set(i, text(program.a[i], program.b[i]));
    }

    // The resolver is script and can tell in what order it is asked, so ask in the order the prefixes are written.
    Vector<size_t> steps_with_prefix;
    expression->m_names.resize(program.step_count);
    for (size_t i = 0; i < program.step_count; ++i) {
        auto const& step = program.steps[i];
        expression->m_names[i].local_name = Utf16FlyString::from_utf8_without_validation(StringView { bytes.slice(step.local_offset, step.local_length) });
        if (step.prefix_length != 0)
            steps_with_prefix.append(i);
    }
    quick_sort(steps_with_prefix, [&](auto a, auto b) { return program.steps[a].prefix_offset < program.steps[b].prefix_offset; });
    for (auto i : steps_with_prefix) {
        auto const& step = program.steps[i];
        expression->m_names[i].namespace_ = TRY(resolve_prefix(resolver, text(step.prefix_offset, step.prefix_length)));
    }
    return expression;
}

Expression::~Expression()
{
    XML::XPath::free(m_program);
}

WebIDL::ExceptionOr<GC::Ref<XPathResult>> Expression::evaluate(DOM::Node const& context_node, WebIDL::UnsignedShort type, GC::Ptr<XPathResult> result) const
{
    // A document fragment or a doctype is no node of XPath's data model.
    if (context_node.is_document_fragment() || context_node.is_document_type())
        return WebIDL::NotSupportedError::create("The node cannot be the context node of an XPath expression"_utf16);
    if (type > XPathResult::FIRST_ORDERED_NODE_TYPE)
        return WebIDL::NotSupportedError::create("Invalid XPath result type"_utf16);

    Evaluator evaluator { *this };
    auto value = evaluator.run(const_cast<DOM::Node&>(context_node));
    if (evaluator.had_type_error())
        return WebIDL::SyntaxError::create("The XPath expression uses a value that is not a node-set as one"_utf16);

    if (!result)
        result = XPathResult::create();
    switch (type) {
    case XPathResult::ANY_TYPE:
        break;
    case XPathResult::NUMBER_TYPE:
        value = to_number(value);
        break;
    case XPathResult::STRING_TYPE:
        value = to_string(value);
        break;
    case XPathResult::BOOLEAN_TYPE:
        value = to_boolean(value);
        break;
    default:
        if (!value.has<NodeSet>())
            return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "The XPath expression does not select nodes"_utf16 };
        break;
    }

    value.visit(
        [&](bool boolean) { result->set_boolean(boolean); },
        [&](double number) { result->set_number(number); },
        [&](Utf16String const& string) { result->set_string(string); },
        [&](NodeSet& set) {
            auto& root = root_of(const_cast<DOM::Node&>(context_node));
            // OPTIMIZATION: A result that only ever gives out one node holds just that node, and one that may give out
            //               any node of the set doesn't have the set sorted to pick it.
            if (type == XPathResult::ANY_UNORDERED_NODE_TYPE) {
                result->set_node_set(set.nodes.span().trim(1), type, root);
                return;
            }
            sort_in_document_order(set);
            if (type == XPathResult::FIRST_ORDERED_NODE_TYPE)
                result->set_node_set(set.nodes.span().trim(1), type, root);
            else
                result->set_node_set(set.nodes, type, root);
        });
    return GC::Ref<XPathResult> { *result };
}

WebIDL::ExceptionOr<GC::Ref<XPathExpression>> create_expression(Utf16String const& expression, GC::Ptr<XPathNSResolver> resolver)
{
    return XPathExpression::create(TRY(Expression::compile(expression, resolver)));
}

WebIDL::ExceptionOr<GC::Ref<XPathResult>> evaluate(Utf16String const& expression, DOM::Node const& context_node, GC::Ptr<XPathNSResolver> resolver, WebIDL::UnsignedShort type, GC::Ptr<XPathResult> result)
{
    auto compiled = TRY(Expression::compile(expression, resolver));
    return compiled->evaluate(context_node, type, result);
}

}
