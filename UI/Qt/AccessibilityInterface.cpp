/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "AccessibilityInterface.h"
#include "WebContentView.h"

#include <LibWebView/AccessibilityNodeData.h>
#include <LibWebView/AccessibilityTreeManager.h>

#include <QAccessible>
#include <QWidget>

namespace Ladybird {

// Factory function — Qt calls this to get the accessible interface for any QObject.
QAccessibleInterface* accessibility_factory(QString const& class_name, QObject* object)
{
    if (class_name == QStringLiteral("Ladybird::WebContentView")) {
        if (auto* view = qobject_cast<WebContentView*>(object))
            return new WebContentViewAccessible(view);
    }
    return nullptr;
}

// WebContentViewAccessible bridges WebContentView into the Qt accessibility tree.

WebContentViewAccessible::WebContentViewAccessible(WebContentView* view)
    : QAccessibleWidget(view, QAccessible::Grouping)
{
}

int WebContentViewAccessible::childCount() const
{
    auto* view = static_cast<WebContentView*>(widget());
    if (!view || !view->m_accessibility_manager || view->m_accessibility_manager->is_empty())
        return 0;
    return 1; // The document root.
}

QAccessibleInterface* WebContentViewAccessible::child(int index) const
{
    if (index != 0)
        return nullptr;
    auto* view = static_cast<WebContentView*>(widget());
    if (!view || !view->m_accessibility_manager)
        return nullptr;
    auto const* root = view->m_accessibility_manager->root();
    if (!root)
        return nullptr;
    return view->accessibility_interface_for_node(root->id);
}

QAccessibleInterface* WebContentViewAccessible::focusChild() const
{
    auto* root_child = child(0);
    if (!root_child)
        return nullptr;
    return root_child->focusChild();
}

int WebContentViewAccessible::indexOfChild(QAccessibleInterface const* iface) const
{
    auto* root_child = child(0);
    if (root_child && root_child == iface)
        return 0;
    return -1;
}

QAccessible::Role WebContentViewAccessible::role() const
{
    return QAccessible::Grouping;
}

static bool is_ignored_role(StringView role, String const& name)
{
    // Generic containers without a name (div, span, etc.) add no meaning — and cluttering Orca's tree with them hurts
    // navigation. Paragraphs must *not* be ignored: Orca relies on the PARAGRAPH role being present in the AT-SPI2 tree
    // for is_text_block_element() and for sentence-extension logic during Say All. If we ignore paragraphs, their
    // text-leaf children bubble up and Orca sees them as Role.LABEL — which isn’t a text_block_element — snd sentence
    // extension then walks across paragraph boundaries undesirably pulling in everything up to the nearest heading.
    return role == "generic"sv && name.is_empty();
}

// This gets roles that Orca (and other AT-SPI2 clients) read as whole units via Atspi.Text. When the container has
// embedded children (e.g. a link inside a list item), Atspi.Text by itself only carries U+FFFC object-replacement
// markers; expanding those requires the Atspi.Hypertext interface, which Qt's built-in AT-SPI2 bridge doesn't
// implement. So, to keep screen-reader speech complete for those container-as-unit reads, we flatten descendant text
// directly into the returned string. Finer-grained hypertext navigation (arrow-key caret movement across a paragraph
// with a link, etc.) still runs on the general code path that preserves the U+FFFCs.
static bool is_leaf_like_container_role(StringView role)
{
    return role == "link"sv
        || role == "button"sv
        || role == "heading"sv
        || role == "img"sv
        || role == "image"sv
        || role == "menuitem"sv
        || role == "tab"sv
        || role == "checkbox"sv
        || role == "radio"sv
        || role == "listitem"sv;
}

AccessibilityInterface::AccessibilityInterface(i64 node_id, WebView::AccessibilityTreeManager const* manager, WebContentView* view)
    : m_node_id(node_id)
    , m_manager(manager)
    , m_view(view)
{
    // Qt's bridge requires a non-null QObject* from object().
    m_object = new QObject(view);

    m_id = QAccessible::registerAccessibleInterface(this);
}

AccessibilityInterface::~AccessibilityInterface()
{
}

WebView::AccessibilityNodeData const* AccessibilityInterface::node_data() const
{
    return m_manager->node(m_node_id);
}

bool AccessibilityInterface::is_ignored(WebView::AccessibilityNodeData const* data) const
{
    if (!data)
        return true;
    return is_ignored_role(data->role.bytes_as_string_view(), data->name);
}

void AccessibilityInterface::collect_unignored_children(i64 node_id, QList<i64>& out) const
{
    auto const* data = m_manager->node(node_id);
    if (!data)
        return;

    for (auto child_id : data->child_ids) {
        auto const* child_data = m_manager->node(child_id);
        if (!child_data)
            continue;

        if (is_ignored(child_data)) {
            collect_unignored_children(child_id, out);
        } else {
            out.append(child_id);
        }
    }
}

// Hide text-leaf children from AT-SPI2 — matching Firefox's EmbeddedObjCollector pattern.
//
// We expose two kinds of containers:
//
//   - Leaf-like containers (listitem, link, heading, button, etc.) flatten descendant text into their own Atspi.Text
//     via flatten_descendant_text() - so text-leaf children are already reachable through the parent's text content.
//
//   - Hypertext-exposing containers (paragraph, section, etc.) expose the descendant text interleaved with U+FFFC
//     markers for embedded objects via build_hypertext() — so text-leaf children are likewise reachable through the
//     parent's own Atspi.Text.
//
// In both cases, re-exposing text-leaves as AT-SPI2 children makes AT clients (Orca's flat review in particular) walk
// in and produce duplicate zones covering text already covered by the parent's own zones. So we hide text-leaf children
// and keep embedded objects (links, images, etc.). Those remain reachable for structural navigation and DOM focus.
void AccessibilityInterface::collect_exposed_children(i64 node_id, QList<i64>& out) const
{
    QList<i64> raw;
    collect_unignored_children(node_id, raw);

    for (auto child_id : raw) {
        auto const* child_data = m_manager->node(child_id);
        if (!child_data)
            continue;
        if (child_data->role.bytes_as_string_view() == "text leaf"sv)
            continue;
        out.append(child_id);
    }
}

i64 AccessibilityInterface::find_unignored_parent(i64 node_id) const
{
    auto const* data = m_manager->node(node_id);
    if (!data)
        return -1;

    i64 parent_id = data->parent_id;
    while (parent_id != -1) {
        auto const* parent_data = m_manager->node(parent_id);
        if (!parent_data)
            return -1;
        if (!is_ignored(parent_data))
            return parent_id;
        parent_id = parent_data->parent_id;
    }
    return -1;
}

// QAccessibleInterface

bool AccessibilityInterface::isValid() const
{
    return m_manager && node_data() != nullptr;
}

QObject* AccessibilityInterface::object() const
{
    return m_object;
}

QAccessibleInterface* AccessibilityInterface::childAt(int x, int y) const
{
    for (int i = 0; i < childCount(); ++i) {
        auto* iface = child(i);
        if (iface && iface->rect().contains(x, y))
            return iface;
    }
    return nullptr;
}

void* AccessibilityInterface::interface_cast(QAccessible::InterfaceType type)
{
    switch (type) {
    case QAccessible::ActionInterface: {
        auto const* data = node_data();
        if (!data)
            break;
        auto role = data->role.bytes_as_string_view();
        if (role == "button"sv || role == "link"sv || role == "checkbox"sv
            || role == "radio"sv || role == "menuitem"sv || role == "tab"sv)
            return static_cast<QAccessibleActionInterface*>(this);
        break;
    }
    case QAccessible::TextInterface: {
        auto const* data = node_data();
        if (!data)
            break;
        auto role = data->role.bytes_as_string_view();
        // Important: We deliberately exclude the list role (the container, not the list items) from any of the
        // conditions we check below — because it actually must *not* have a Text interface. Otherwise, Orca will skip
        // the list entirely during Say All iteration. That's because (1) the Text would be just U+FFFC per listitem,
        // and (2) Orca can't expand that without a Hypertext interface — *which Qt’s AT-SPI2 bridge doesn't give us*.
        // And so when Orca sees that, it'll treat find_next_caret_in_order on the list as "text object with
        // unexpandable markers" — and that's what leads to Orca skipping the list entirely during Say All iteration.
        if (role == "text leaf"sv)
            return static_cast<QAccessibleTextInterface*>(this); // Text leaves always have Text.
        if (role == "link"sv || role == "button"sv || role == "heading"sv
            || role == "img"sv || role == "image"sv || role == "menuitem"sv
            || role == "tab"sv || role == "checkbox"sv || role == "radio"sv
            || role == "listitem"sv)
            return static_cast<QAccessibleTextInterface*>(this);
        // Text block containers need a Text interface so that Orca's is_text_block_element heuristic matches;
        // otherwise, supports_text() returns False — and its sentence-contents extension walks across paragraph
        // boundaries looking for a text-block-element to stop at. Unlike leaf-like containers, these are *not* in
        // is_leaf_like_container_role — so build_hypertext uses the general code path (text-leaf children inline;
        // embedded objects as U+FFFC).
        if (role == "paragraph"sv)
            return static_cast<QAccessibleTextInterface*>(this);
        // Any other named elements with no children.
        if (!data->name.is_empty() && childCount() == 0)
            return static_cast<QAccessibleTextInterface*>(this);
        break;
    }
    case QAccessible::TableCellInterface: {
        auto mapped_role = role();
        if (mapped_role == QAccessible::Cell || mapped_role == QAccessible::ColumnHeader || mapped_role == QAccessible::RowHeader)
            return static_cast<QAccessibleTableCellInterface*>(this);
        break;
    }
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    case QAccessible::AttributesInterface:
        return static_cast<QAccessibleAttributesInterface*>(this);
#endif
    default:
        break;
    }
    return nullptr;
}

QAccessibleInterface* AccessibilityInterface::parent() const
{
    auto const* data = node_data();
    if (!data || data->parent_id == -1)
        return QAccessible::queryAccessibleInterface(m_view);

    i64 parent_id = find_unignored_parent(m_node_id);
    if (parent_id == -1)
        return QAccessible::queryAccessibleInterface(m_view);

    return m_view->accessibility_interface_for_node(parent_id);
}

QAccessibleInterface* AccessibilityInterface::child(int index) const
{
    QList<i64> children;
    collect_exposed_children(m_node_id, children);
    if (index < 0 || index >= children.size())
        return nullptr;
    return m_view->accessibility_interface_for_node(children[index]);
}

QAccessibleInterface* AccessibilityInterface::focusChild() const
{
    if (state().focused)
        return const_cast<AccessibilityInterface*>(this);

    for (int i = 0; i < childCount(); ++i) {
        if (auto* child_iface = child(i)) {
            if (auto* focused = child_iface->focusChild())
                return focused;
        }
    }
    return nullptr;
}

int AccessibilityInterface::childCount() const
{
    QList<i64> children;
    collect_exposed_children(m_node_id, children);
    return children.size();
}

int AccessibilityInterface::indexOfChild(QAccessibleInterface const* iface) const
{
    auto const* child_iface = static_cast<AccessibilityInterface const*>(iface);
    if (!child_iface)
        return -1;

    QList<i64> children;
    collect_exposed_children(m_node_id, children);
    return children.indexOf(child_iface->node_id());
}

QString AccessibilityInterface::text(QAccessible::Text t) const
{
    auto const* data = node_data();
    if (!data)
        return {};

    auto to_qstring = [](String const& s) -> QString {
        if (s.is_empty())
            return {};
        return QString::fromUtf8(reinterpret_cast<char const*>(s.bytes().data()), s.bytes().size());
    };

    switch (t) {
    case QAccessible::Name:
        return to_qstring(data->name);
    case QAccessible::Description:
        return to_qstring(data->description);
    case QAccessible::Value:
        if (!data->value.is_empty())
            return to_qstring(data->value);
        if (data->heading_level > 0)
            return QString::number(data->heading_level);
        return {};
    default:
        return {};
    }
}

void AccessibilityInterface::setText(QAccessible::Text, QString const&)
{
}

QRect AccessibilityInterface::rect() const
{
    auto const* data = node_data();
    if (!data)
        return {};

    auto local = QRect(data->bounds.x(), data->bounds.y(),
        data->bounds.width(), data->bounds.height());
    auto global_pos = m_view->mapToGlobal(QPoint(local.x(), local.y()));
    return QRect(global_pos.x(), global_pos.y(), local.width(), local.height());
}

QAccessible::Role AccessibilityInterface::map_role() const
{
    auto const* data = node_data();
    if (!data)
        return QAccessible::NoRole;

    auto role = data->role.bytes_as_string_view();

    if (role == "document"sv)
        return QAccessible::WebDocument;
    if (role == "button"sv)
        return QAccessible::Button;
    if (role == "link"sv)
        return QAccessible::Link;
    if (role == "heading"sv)
        return QAccessible::Heading;
    if (role == "textbox"sv)
        return QAccessible::EditableText;
    if (role == "checkbox"sv)
        return QAccessible::CheckBox;
    if (role == "radio"sv)
        return QAccessible::RadioButton;
    if (role == "combobox"sv)
        return QAccessible::ComboBox;
    if (role == "list"sv)
        return QAccessible::List;
    if (role == "listitem"sv)
        return QAccessible::ListItem;
    if (role == "table"sv)
        return QAccessible::Table;
    if (role == "row"sv)
        return QAccessible::Row;
    if (role == "cell"sv || role == "gridcell"sv)
        return QAccessible::Cell;
    if (role == "columnheader"sv)
        return QAccessible::ColumnHeader;
    if (role == "rowheader"sv)
        return QAccessible::RowHeader;
    if (role == "img"sv || role == "image"sv)
        return QAccessible::Graphic;
    if (role == "navigation"sv)
        return QAccessible::Section;
    if (role == "main"sv)
        return QAccessible::Section;
    if (role == "banner"sv)
        return QAccessible::Section;
    if (role == "complementary"sv)
        return QAccessible::Section;
    if (role == "contentinfo"sv)
        return QAccessible::Footer;
    if (role == "search"sv)
        return QAccessible::Section;
    if (role == "form"sv)
        return QAccessible::Form;
    if (role == "region"sv)
        return QAccessible::Section;
    if (role == "dialog"sv || role == "alertdialog"sv)
        return QAccessible::Dialog;
    if (role == "progressbar"sv)
        return QAccessible::ProgressBar;
    if (role == "slider"sv)
        return QAccessible::Slider;
    if (role == "tab"sv)
        return QAccessible::PageTab;
    if (role == "tablist"sv)
        return QAccessible::PageTabList;
    if (role == "tabpanel"sv)
        return QAccessible::Pane;
    if (role == "menu"sv)
        return QAccessible::PopupMenu;
    if (role == "menuitem"sv)
        return QAccessible::MenuItem;
    if (role == "menubar"sv)
        return QAccessible::MenuBar;
    if (role == "separator"sv)
        return QAccessible::Separator;
    if (role == "alert"sv)
        return QAccessible::AlertMessage;
    if (role == "status"sv)
        return QAccessible::StatusBar;
    if (role == "log"sv)
        return QAccessible::Section;
    if (role == "text leaf"sv)
        return QAccessible::StaticText;
    if (role == "generic"sv)
        return QAccessible::Section;
    if (role == "article"sv)
        return QAccessible::Section;
    if (role == "figure"sv)
        return QAccessible::Section;
    if (role == "group"sv)
        return QAccessible::Grouping;
    if (role == "searchbox"sv)
        return QAccessible::EditableText;
    if (role == "paragraph"sv)
        return QAccessible::Paragraph;

    return QAccessible::Section;
}

QAccessible::Role AccessibilityInterface::role() const
{
    return map_role();
}

QAccessible::State AccessibilityInterface::state() const
{
    QAccessible::State accessible_state;
    auto const* data = node_data();
    if (!data)
        return accessible_state;

    auto role = data->role.bytes_as_string_view();

    // Only report focusable for roles that are actually keyboard-focusable. Reporting focusable on non-interactive
    // elements (headings, paragraphs, lists, list items, generic containers) breaks Orca heuristics;
    // is_text_block_element returns False for any focusable object — which causes sentence-contents extension to run
    // past paragraph boundaries and pull in the *entire* document.
    bool role_is_focusable = role == "button"sv
        || role == "link"sv
        || role == "textbox"sv
        || role == "searchbox"sv
        || role == "checkbox"sv
        || role == "radio"sv
        || role == "combobox"sv
        || role == "slider"sv
        || role == "spinbutton"sv
        || role == "menuitem"sv
        || role == "menuitemcheckbox"sv
        || role == "menuitemradio"sv
        || role == "tab"sv
        || role == "treeitem"sv
        || role == "option"sv
        || role == "switch"sv;
    if (role_is_focusable || data->is_editable || data->is_focused)
        accessible_state.focusable = true;
    if (data->is_focused)
        accessible_state.focused = true;
    if (data->is_disabled)
        accessible_state.disabled = true;

    // Only report visible/showing for the *active* tab's elements. Elements from *inactive* tabs *must* be hidden from
    // AT-SPI2 — so that Orca doesn't try to navigate to them during Say All.
    if (m_view && m_view->isVisible()) {
        accessible_state.invisible = false;
    } else {
        accessible_state.invisible = true;
        accessible_state.offscreen = true;
    }

    if (role == "link"sv)
        accessible_state.linked = true;
    if (role == "text leaf"sv)
        accessible_state.readOnly = true;

    return accessible_state;
}

// QAccessibleActionInterface

QStringList AccessibilityInterface::actionNames() const
{
    auto const* data = node_data();
    if (!data)
        return {};

    QStringList actions;
    auto role = data->role.bytes_as_string_view();
    if (role == "button"sv || role == "link"sv || role == "checkbox"sv
        || role == "radio"sv || role == "menuitem"sv || role == "tab"sv)
        actions << QAccessibleActionInterface::pressAction();

    // All elements support setFocus so that Orca's GrabFocus (AT-SPI2 Component interface) can move DOM focus there —
    // which triggers the CSS :focus-visible focus ring during screen-reader navigation.
    actions << QAccessibleActionInterface::setFocusAction();

    return actions;
}

void AccessibilityInterface::doAction(QString const& actionName)
{
    if (actionName == QAccessibleActionInterface::pressAction())
        m_view->perform_accessibility_action(m_node_id, "press"_string);
    else if (actionName == QAccessibleActionInterface::setFocusAction())
        m_view->perform_accessibility_action(m_node_id, "focus"_string);
}

QStringList AccessibilityInterface::keyBindingsForAction(QString const&) const
{
    return {};
}

// QAccessibleTextInterface

void AccessibilityInterface::addSelection(int, int) { }

QString AccessibilityInterface::attributes(int offset, int* startOffset, int* endOffset) const
{
    *startOffset = 0;
    *endOffset = characterCount();
    if (*endOffset <= 0)
        *endOffset = offset + 1;
    // Return a minimal valid attribute string — so Qt's bridge produces a non-empty a{ss} dict. Orca otherwise crashes
    // when the attributes dict is None (empty response from AT-SPI2).
    return QStringLiteral("direction:ltr");
}

int AccessibilityInterface::cursorPosition() const { return 0; }

// For a text leaf, return the rect of the character at 'offset' using per-character layout offsets. Per-character data
// is populated by LibWeb from PaintableFragments and correctly places each character on its visual line — even when the
// text wraps. Coordinates are widget-local.
static QRect leaf_character_rect_local(WebView::AccessibilityNodeData const* leaf_data, int offset)
{
    if (!leaf_data)
        return {};
    auto local_full = QRect(leaf_data->bounds.x(), leaf_data->bounds.y(),
        leaf_data->bounds.width(), leaf_data->bounds.height());
    if (leaf_data->character_offsets.is_empty() || offset < 0)
        return local_full;
    // Return empty rect for out-of-range offsets. Qt's AT-SPI2 bridge implementation of GetRangeExtents iterates
    // characterRect over [startOffset..endOffset] *inclusive* — while callers pass endOffset as *exclusive*; returning
    // an empty rect for the extra past-end offset keeps the union confined to the requested range.
    if (offset >= static_cast<int>(leaf_data->character_offsets.size()))
        return {};

    auto const& char_pt = leaf_data->character_offsets[offset];
    int line_index = 0;
    for (size_t i = 0; i < leaf_data->line_break_character_offsets.size(); ++i) {
        if (leaf_data->line_break_character_offsets[i] <= offset)
            line_index = static_cast<int>(i);
    }
    int line_height = (line_index < static_cast<int>(leaf_data->line_heights.size()))
        ? leaf_data->line_heights[line_index]
        : leaf_data->bounds.height();

    int char_width = line_height / 2;
    int next_offset = offset + 1;
    if (next_offset < static_cast<int>(leaf_data->character_offsets.size())) {
        auto const& next_pt = leaf_data->character_offsets[next_offset];
        if (next_pt.y() == char_pt.y())
            char_width = next_pt.x() - char_pt.x();
    }

    return QRect(leaf_data->bounds.x() + char_pt.x(),
        leaf_data->bounds.y() + char_pt.y(),
        char_width,
        line_height);
}

// Return the rect of the character at offset in this interface's Atspi.Text. Text leaves: use per-character layout
// data. General containers exposing hypertext (paragraphs, sections, etc.): walk the unignored children concatenated
// by build_hypertext(). Text-leaf children contribute per-character rects from their own layout data; embedded-object
// children contribute their bounding rect at their U+FFFC position. That all makes per-character rects match the visual
// line each character is actually on — which is what Orca's flat review depends on to cluster zones by line.
QRect AccessibilityInterface::characterRect(int offset) const
{
    auto const* data = node_data();
    if (!data)
        return {};

    auto to_global = [this](QRect local) {
        auto global_pos = m_view->mapToGlobal(QPoint(local.x(), local.y()));
        return QRect(global_pos.x(), global_pos.y(), local.width(), local.height());
    };

    auto role = data->role.bytes_as_string_view();
    if (role == "text leaf"sv)
        return to_global(leaf_character_rect_local(data, offset));

    if (is_leaf_like_container_role(role))
        return rect();

    // Qt's AT-SPI2 bridge GetRangeExtents iterates characterRect over [startOffset..endOffset] *inclusive* — while
    // callers (including Orca) pass endOffset as exclusive. For a range that ends at the start of the next visual line,
    // the inclusive iteration picks up a character from the next line, and the union rect spans two lines — which
    // breaks Orca's flat-review line clustering (5px middle-y tolerance). Compensate by returning an empty rect when
    // that offset begins a new visual line. Qt's operator| treats isNull() rects as no-ops — so the union stays
    // confined to the intended line.
    if (offset > 0) {
        // Peek at offset-1 and offset by walking children; if y jumps, treat as line start and return empty.
        QList<i64> children_peek;
        collect_unignored_children(m_node_id, children_peek);
        auto resolve_y = [&](int off) -> Optional<i32> {
            int cum = 0;
            for (auto child_id : children_peek) {
                auto const* cd = m_manager->node(child_id);
                if (!cd)
                    continue;
                auto crole = cd->role.bytes_as_string_view();
                if (crole == "text leaf"sv) {
                    auto qs = QString::fromUtf8(reinterpret_cast<char const*>(cd->name.bytes().data()),
                        cd->name.bytes().size());
                    int len = qs.size();
                    if (off < cum + len) {
                        int leaf_off = off - cum;
                        if (cd->character_offsets.is_empty() || leaf_off < 0)
                            return cd->bounds.y();
                        if (leaf_off >= static_cast<int>(cd->character_offsets.size()))
                            leaf_off = static_cast<int>(cd->character_offsets.size()) - 1;
                        return cd->bounds.y() + cd->character_offsets[leaf_off].y();
                    }
                    cum += len;
                } else {
                    if (off == cum)
                        return cd->bounds.y();
                    cum += 1;
                }
            }
            return {};
        };
        auto y_prev = resolve_y(offset - 1);
        auto y_here = resolve_y(offset);
        if (y_prev.has_value() && y_here.has_value() && y_prev.value() != y_here.value())
            return {};
    }

    QList<i64> children;
    collect_unignored_children(m_node_id, children);
    int cumulative = 0;
    for (auto child_id : children) {
        auto const* child_data = m_manager->node(child_id);
        if (!child_data)
            continue;
        auto child_role = child_data->role.bytes_as_string_view();
        if (child_role == "text leaf"sv) {
            auto leaf_string = QString::fromUtf8(
                reinterpret_cast<char const*>(child_data->name.bytes().data()),
                child_data->name.bytes().size());
            int leaf_len = leaf_string.size();
            if (offset < cumulative + leaf_len)
                return to_global(leaf_character_rect_local(child_data, offset - cumulative));
            cumulative += leaf_len;
        } else {
            if (offset == cumulative) {
                auto local = QRect(child_data->bounds.x(), child_data->bounds.y(),
                    child_data->bounds.width(), child_data->bounds.height());
                return to_global(local);
            }
            cumulative += 1;
        }
    }

    // Past-end offset: return empty rect (see comment in leaf_character_rect_local).
    return {};
}

int AccessibilityInterface::selectionCount() const { return 0; }

int AccessibilityInterface::offsetAtPoint(QPoint const&) const { return 0; }

void AccessibilityInterface::selection(int, int* startOffset, int* endOffset) const
{
    *startOffset = 0;
    *endOffset = 0;
}

// This builds hypertext-aware text for this node. Text-leaf children contribute their text content; non-text children
// contribute U+FFFC (object replacement character) as embedded objects — except for leaf-like container roles
// (listitem, heading, link, button, etc.), where we flatten the full descendant text directly. That’s because Qt's
// bridge doesn't expose AtkHypertext — and AT clients therefore can't expand the U+FFFC markers into child content.
QString AccessibilityInterface::build_hypertext() const
{
    auto const* data = node_data();
    if (!data)
        return {};

    auto role = data->role.bytes_as_string_view();

    // Text leaves just return their own name.
    if (role == "text leaf"sv)
        return QString::fromUtf8(reinterpret_cast<char const*>(data->name.bytes().data()), data->name.bytes().size());

    // Leaf-like containers: flatten all descendant text so the whole unit speaks as one string.
    if (is_leaf_like_container_role(role))
        return flatten_descendant_text(m_node_id);

    // General containers: text-leaf children inline, other children become U+FFFC.
    QList<i64> children;
    collect_unignored_children(m_node_id, children);
    if (children.isEmpty()) {
        // No children — return the node's own name (e.g. for img[alt]).
        return QString::fromUtf8(reinterpret_cast<char const*>(data->name.bytes().data()), data->name.bytes().size());
    }

    QString result;
    for (auto child_id : children) {
        auto const* child_data = m_manager->node(child_id);
        if (!child_data)
            continue;
        if (child_data->role.bytes_as_string_view() == "text leaf"sv) {
            result += QString::fromUtf8(
                reinterpret_cast<char const*>(child_data->name.bytes().data()),
                child_data->name.bytes().size());
        } else {
            result += QChar(0xFFFC); // Embedded-object marker.
        }
    }
    return result;
}

// This is used for leaf-like containers whose children's text must be inlined (see build_hypertext). Text leaves
// contribute their name; every other node recurses into its unignored children – and, if that yields nothing, falls
// back to the node's own name.
QString AccessibilityInterface::flatten_descendant_text(i64 node_id) const
{
    auto const* data = m_manager->node(node_id);
    if (!data)
        return {};

    auto role = data->role.bytes_as_string_view();
    if (role == "text leaf"sv)
        return QString::fromUtf8(reinterpret_cast<char const*>(data->name.bytes().data()), data->name.bytes().size());

    QString result;
    QList<i64> children;
    collect_unignored_children(node_id, children);
    for (auto child_id : children)
        result += flatten_descendant_text(child_id);

    if (result.isEmpty() && !data->name.is_empty())
        result = QString::fromUtf8(reinterpret_cast<char const*>(data->name.bytes().data()), data->name.bytes().size());

    return result;
}

QString AccessibilityInterface::text(int startOffset, int endOffset) const
{
    auto full_text = build_hypertext();
    return full_text.mid(startOffset, endOffset - startOffset);
}

void AccessibilityInterface::removeSelection(int) { }
void AccessibilityInterface::setCursorPosition(int) { }
void AccessibilityInterface::setSelection(int, int, int) { }

int AccessibilityInterface::characterCount() const
{
    return build_hypertext().size();
}

void AccessibilityInterface::scrollToSubstring(int, int)
{
    // Orca calls scrollToSubstring when its browse-mode cursor moves to an element. To get :focus-visible to paint an
    // outline, we use the "scroll_into_view" action to scroll the element into view and mark it as the document's
    // accessibility focus target — all without moving DOM focus. But Orca may hold stale references to inactive tab
    // objects and send scrollToSubstring to them — so, we only do this if this view is visible.
    if (m_view && m_view->isVisible())
        m_view->perform_accessibility_action(m_node_id, "scroll_into_view"_string);
}

// Compute the character offsets where each visual line starts — using per-character y from characterRect. The last
// entry is char_count as an end sentinel. Robust against small y-jitter by only treating a change in y as a new line
// when the jump is larger than half the current line height.
static Optional<i32> offset_to_y_in_container(WebView::AccessibilityTreeManager const* manager, i64 node_id, int offset)
{
    auto const* data = manager->node(node_id);
    if (!data)
        return {};
    auto role = data->role.bytes_as_string_view();

    if (role == "text leaf"sv) {
        if (data->character_offsets.is_empty())
            return data->bounds.y();
        int idx = offset < 0 ? 0
                             : (offset >= static_cast<int>(data->character_offsets.size())
                                       ? static_cast<int>(data->character_offsets.size()) - 1
                                       : offset);
        return data->bounds.y() + data->character_offsets[idx].y();
    }

    int cum = 0;
    for (auto child_id : data->child_ids) {
        auto const* cd = manager->node(child_id);
        if (!cd)
            continue;
        // Skip ignored children (they don't contribute to hypertext offsets at this level; their children are promoted).
        auto is_ignored_child = cd->role.bytes_as_string_view() == "generic"sv && cd->name.is_empty();
        if (is_ignored_child) {
            // Walk grandchildren as if they were direct children here.
            for (auto grand_id : cd->child_ids) {
                auto const* gd = manager->node(grand_id);
                if (!gd)
                    continue;
                auto grole = gd->role.bytes_as_string_view();
                if (grole == "text leaf"sv) {
                    auto qs = QString::fromUtf8(reinterpret_cast<char const*>(gd->name.bytes().data()),
                        gd->name.bytes().size());
                    int len = qs.size();
                    if (offset < cum + len)
                        return offset_to_y_in_container(manager, grand_id, offset - cum);
                    cum += len;
                } else {
                    if (offset == cum)
                        return gd->bounds.y();
                    cum += 1;
                }
            }
            continue;
        }
        auto crole = cd->role.bytes_as_string_view();
        if (crole == "text leaf"sv) {
            auto qs = QString::fromUtf8(reinterpret_cast<char const*>(cd->name.bytes().data()),
                cd->name.bytes().size());
            int len = qs.size();
            if (offset < cum + len)
                return offset_to_y_in_container(manager, child_id, offset - cum);
            cum += len;
        } else {
            if (offset == cum)
                return cd->bounds.y();
            cum += 1;
        }
    }
    return {};
}

static QList<int> compute_visual_line_starts(AccessibilityInterface const& iface, int char_count)
{
    QList<int> starts;
    starts.append(0);
    if (char_count <= 0) {
        starts.append(0);
        return starts;
    }
    auto* manager = iface.accessibility_manager();
    auto node_id = iface.node_id();
    auto y0 = offset_to_y_in_container(manager, node_id, 0);
    i32 prev_y = y0.value_or(0);
    for (int i = 1; i < char_count; ++i) {
        auto y = offset_to_y_in_container(manager, node_id, i);
        if (!y.has_value())
            continue;
        if (y.value() != prev_y) {
            starts.append(i);
            prev_y = y.value();
        }
    }
    starts.append(char_count);
    return starts;
}

static QString text_at_visual_line_containing(AccessibilityInterface const& iface, QString const& full_text, int offset,
    int* start_offset, int* end_offset)
{
    int count = full_text.size();
    if (offset < 0 || offset > count) {
        *start_offset = *end_offset = -1;
        return {};
    }
    auto line_starts = compute_visual_line_starts(iface, count);
    int clamped = qBound(0, offset, count);
    int line_index = -1;
    for (int i = 0; i + 1 < line_starts.size(); ++i) {
        if (line_starts[i] <= clamped && clamped < line_starts[i + 1]) {
            line_index = i;
            break;
        }
    }
    if (line_index < 0) {
        line_index = line_starts.size() - 2;
        if (line_index < 0) {
            *start_offset = *end_offset = 0;
            return {};
        }
    }
    *start_offset = line_starts[line_index];
    *end_offset = line_starts[line_index + 1];
    return full_text.mid(*start_offset, *end_offset - *start_offset);
}

// We want visual-line boundary iteration for *text-block* containers (paragraphs, sections), whose inline content can
// wrap across multiple visual lines — so Orca's flat review clusters zones by line, and line-by-line navigation walks
// the wrapped lines. But we do *not* want it for *leaf-like* containers (listitem, link, heading, button, image,
// menuitem, tab, checkbox, radio) — because those are *atomic units* whose full flattened text is one presentation
// unit. Orca's _generate_text_line (reached via structural navigation's I key for list items, H for headings, etc.)
// calls get_string_at_offset(obj, 0, LINE). And for an atomic unit, that's supposed to be the *whole* text — not the
// first visual sub-line. Falling through to Qt's default textAtOffset here matches what the base does (split on \n) —
// which gives the full flattened text for leaf-like containers whose content has no embedded \n.
static bool should_use_visual_line_boundary(AccessibilityInterface const& iface)
{
    auto const* data = iface.node_data();
    if (!data)
        return false;
    return !is_leaf_like_container_role(data->role.bytes_as_string_view());
}

QString AccessibilityInterface::textAtOffset(int offset, QAccessible::TextBoundaryType boundaryType,
    int* startOffset, int* endOffset) const
{
    if (boundaryType != QAccessible::LineBoundary || !should_use_visual_line_boundary(*this))
        return QAccessibleTextInterface::textAtOffset(offset, boundaryType, startOffset, endOffset);
    auto full_text = build_hypertext();
    return text_at_visual_line_containing(*this, full_text, offset, startOffset, endOffset);
}

QString AccessibilityInterface::textBeforeOffset(int offset, QAccessible::TextBoundaryType boundaryType,
    int* startOffset, int* endOffset) const
{
    if (boundaryType != QAccessible::LineBoundary || !should_use_visual_line_boundary(*this))
        return QAccessibleTextInterface::textBeforeOffset(offset, boundaryType, startOffset, endOffset);
    auto full_text = build_hypertext();
    int this_start = -1, this_end = -1;
    text_at_visual_line_containing(*this, full_text, offset, &this_start, &this_end);
    if (this_start <= 0) {
        *startOffset = *endOffset = -1;
        return {};
    }
    return text_at_visual_line_containing(*this, full_text, this_start - 1, startOffset, endOffset);
}

QString AccessibilityInterface::textAfterOffset(int offset, QAccessible::TextBoundaryType boundaryType,
    int* startOffset, int* endOffset) const
{
    if (boundaryType != QAccessible::LineBoundary || !should_use_visual_line_boundary(*this))
        return QAccessibleTextInterface::textAfterOffset(offset, boundaryType, startOffset, endOffset);
    auto full_text = build_hypertext();
    int this_start = -1, this_end = -1;
    text_at_visual_line_containing(*this, full_text, offset, &this_start, &this_end);
    if (this_end >= full_text.size()) {
        *startOffset = *endOffset = -1;
        return {};
    }
    return text_at_visual_line_containing(*this, full_text, this_end, startOffset, endOffset);
}

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
// QAccessibleAttributesInterface

QList<QAccessible::Attribute> AccessibilityInterface::attributeKeys() const
{
    return { QAccessible::Attribute::Custom };
}

QVariant AccessibilityInterface::attributeValue(QAccessible::Attribute key) const
{
    if (key != QAccessible::Attribute::Custom)
        return {};

    auto const* data = node_data();
    if (!data)
        return {};

    QHash<QString, QString> attrs;

    auto role = data->role.bytes_as_string_view();

    // "tag" attribute: Orca's web script uses this to identify web elements (is_web_element checks for it). Synthesize
    // from role.
    if (role == "document"sv)
        attrs[QStringLiteral("tag")] = QStringLiteral("body");
    else if (role == "heading"sv)
        attrs[QStringLiteral("tag")] = QStringLiteral("h%1").arg(data->heading_level > 0 ? data->heading_level : 1);
    else if (role == "link"sv)
        attrs[QStringLiteral("tag")] = QStringLiteral("a");
    else if (role == "img"sv || role == "image"sv)
        attrs[QStringLiteral("tag")] = QStringLiteral("img");
    else if (role == "list"sv)
        attrs[QStringLiteral("tag")] = QStringLiteral("ul");
    else if (role == "listitem"sv)
        attrs[QStringLiteral("tag")] = QStringLiteral("li");
    else if (role == "table"sv)
        attrs[QStringLiteral("tag")] = QStringLiteral("table");
    else if (role == "row"sv)
        attrs[QStringLiteral("tag")] = QStringLiteral("tr");
    else if (role == "cell"sv || role == "gridcell"sv)
        attrs[QStringLiteral("tag")] = QStringLiteral("td");
    else if (role == "columnheader"sv)
        attrs[QStringLiteral("tag")] = QStringLiteral("th");
    else if (role == "button"sv)
        attrs[QStringLiteral("tag")] = QStringLiteral("button");
    else if (role == "textbox"sv || role == "searchbox"sv)
        attrs[QStringLiteral("tag")] = QStringLiteral("input");
    else if (role == "form"sv)
        attrs[QStringLiteral("tag")] = QStringLiteral("form");
    else if (role == "navigation"sv)
        attrs[QStringLiteral("tag")] = QStringLiteral("nav");
    else if (role == "main"sv)
        attrs[QStringLiteral("tag")] = QStringLiteral("main");
    else if (role == "banner"sv)
        attrs[QStringLiteral("tag")] = QStringLiteral("header");
    else if (role == "contentinfo"sv)
        attrs[QStringLiteral("tag")] = QStringLiteral("footer");
    else if (role == "complementary"sv)
        attrs[QStringLiteral("tag")] = QStringLiteral("aside");
    else if (role == "search"sv)
        attrs[QStringLiteral("tag")] = QStringLiteral("search");
    else if (role == "region"sv)
        attrs[QStringLiteral("tag")] = QStringLiteral("section");
    else if (role == "paragraph"sv)
        attrs[QStringLiteral("tag")] = QStringLiteral("p");
    else if (role == "separator"sv)
        attrs[QStringLiteral("tag")] = QStringLiteral("hr");
    else if (role == "article"sv)
        attrs[QStringLiteral("tag")] = QStringLiteral("article");
    else if (role == "blockquote"sv)
        attrs[QStringLiteral("tag")] = QStringLiteral("blockquote");
    else if (role == "text leaf"sv)
        attrs[QStringLiteral("tag")] = QStringLiteral("#text");
    else if (role == "generic"sv)
        attrs[QStringLiteral("tag")] = QStringLiteral("div");
    else if (!role.is_empty())
        attrs[QStringLiteral("tag")] = QStringLiteral("div");

    // "xml-roles": Orca uses this to distinguish landmark types.
    if (!role.is_empty() && role != "text leaf"sv) {
        attrs[QStringLiteral("xml-roles")] = QString::fromUtf8(
            reinterpret_cast<char const*>(data->role.bytes().data()),
            data->role.bytes().size());
    }

    // "level": heading level
    if (data->heading_level > 0)
        attrs[QStringLiteral("level")] = QString::number(data->heading_level);

    return QVariant::fromValue(attrs);
}

#endif

// QAccessibleTableCellInterface

int AccessibilityInterface::columnExtent() const
{
    auto const* data = node_data();
    return data ? data->column_span : 1;
}

QList<QAccessibleInterface*> AccessibilityInterface::columnHeaderCells() const
{
    return {};
}

int AccessibilityInterface::columnIndex() const
{
    auto const* data = node_data();
    if (!data)
        return 0;

    auto const* row = m_manager->node(data->parent_id);
    if (!row)
        return 0;

    int column = 0;
    for (auto sibling_id : row->child_ids) {
        if (sibling_id == m_node_id)
            return column;
        column++;
    }
    return 0;
}

int AccessibilityInterface::rowExtent() const
{
    auto const* data = node_data();
    return data ? data->row_span : 1;
}

QList<QAccessibleInterface*> AccessibilityInterface::rowHeaderCells() const
{
    return {};
}

int AccessibilityInterface::rowIndex() const
{
    auto const* data = node_data();
    if (!data)
        return 0;

    i64 table_id = -1;
    i64 ancestor_id = data->parent_id;
    while (ancestor_id != -1) {
        auto const* ancestor = m_manager->node(ancestor_id);
        if (!ancestor)
            break;
        if (ancestor->role.bytes_as_string_view() == "table"sv) {
            table_id = ancestor_id;
            break;
        }
        ancestor_id = ancestor->parent_id;
    }

    if (table_id == -1)
        return 0;

    auto const* table = m_manager->node(table_id);
    if (!table)
        return 0;

    int index = 0;
    for (auto child_id : table->child_ids) {
        auto const* child = m_manager->node(child_id);
        if (!child)
            continue;
        if (child->role.bytes_as_string_view() == "row"sv) {
            if (child->id == data->parent_id)
                return index;
            index++;
        } else if (child->role.bytes_as_string_view() == "rowgroup"sv) {
            for (auto gc_id : child->child_ids) {
                auto const* gc = m_manager->node(gc_id);
                if (gc && gc->role.bytes_as_string_view() == "row"sv) {
                    if (gc->id == data->parent_id)
                        return index;
                    index++;
                }
            }
        }
    }
    return 0;
}

bool AccessibilityInterface::isSelected() const
{
    return false;
}

QAccessibleInterface* AccessibilityInterface::table() const
{
    auto const* data = node_data();
    if (!data)
        return nullptr;

    i64 ancestor_id = data->parent_id;
    while (ancestor_id != -1) {
        auto const* ancestor = m_manager->node(ancestor_id);
        if (!ancestor)
            return nullptr;
        if (ancestor->role.bytes_as_string_view() == "table"sv)
            return m_view->accessibility_interface_for_node(ancestor_id);
        ancestor_id = ancestor->parent_id;
    }
    return nullptr;
}

}
