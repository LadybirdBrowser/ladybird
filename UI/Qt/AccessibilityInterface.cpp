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
    return (role == "generic"sv && name.is_empty())
        || (role == "paragraph"sv && name.is_empty());
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
        // Text leaves always have Text.
        if (role == "text leaf"sv)
            return static_cast<QAccessibleTextInterface*>(this);
        // Leaf-like roles that Orca reads as whole units — expose Text so Orca can read their name/content.
        if (role == "link"sv || role == "button"sv || role == "heading"sv
            || role == "img"sv || role == "image"sv || role == "menuitem"sv
            || role == "tab"sv || role == "checkbox"sv || role == "radio"sv
            || role == "listitem"sv || role == "list"sv)
            return static_cast<QAccessibleTextInterface*>(this);
        // Other named elements with no children.
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
    collect_unignored_children(m_node_id, children);
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
    collect_unignored_children(m_node_id, children);
    return children.size();
}

int AccessibilityInterface::indexOfChild(QAccessibleInterface const* iface) const
{
    auto const* child_iface = static_cast<AccessibilityInterface const*>(iface);
    if (!child_iface)
        return -1;

    QList<i64> children;
    collect_unignored_children(m_node_id, children);
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

    accessible_state.focusable = true;
    if (data->is_focused)
        accessible_state.focused = true;
    if (data->is_disabled)
        accessible_state.disabled = true;

    auto role = data->role.bytes_as_string_view();

    if (role == "document"sv && m_view->hasFocus())
        accessible_state.focused = true;

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

    if (role == "textbox"sv || role == "searchbox"sv)
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
    // Return a minimal valid attribute string so Qt's bridge produces a non-empty a{ss} dict. Orca crashes when the
    // attributes dict is None (empty response from AT-SPI2).
    return QStringLiteral("direction:ltr");
}

int AccessibilityInterface::cursorPosition() const { return 0; }

QRect AccessibilityInterface::characterRect(int) const
{
    return rect();
}

int AccessibilityInterface::selectionCount() const { return 0; }

int AccessibilityInterface::offsetAtPoint(QPoint const&) const { return 0; }

void AccessibilityInterface::selection(int, int* startOffset, int* endOffset) const
{
    *startOffset = 0;
    *endOffset = 0;
}

// Build hypertext-aware text for this node. Text leaf children contribute their text content; non-text children
// contribute U+FFFC (object replacement character) as embedded objects.
QString AccessibilityInterface::build_hypertext() const
{
    auto const* data = node_data();
    if (!data)
        return {};

    // Text leaves just return their own name.
    if (data->role.bytes_as_string_view() == "text leaf"sv)
        return QString::fromUtf8(reinterpret_cast<char const*>(data->name.bytes().data()), data->name.bytes().size());

    // Containers: walk unignored children.
    QList<i64> children;
    collect_unignored_children(m_node_id, children);
    if (children.isEmpty()) {
        // No children — return the node's own name (e.g. for <img alt="...">).
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
            result += QChar(0xFFFC); // Embedded object replacement character.
        }
    }
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

void AccessibilityInterface::scrollToSubstring(int, int) { }

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

    // "tag" attribute — Orca's web script uses this to identify web elements (is_web_element checks for it). Synthesize
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

    // "xml-roles" — Orca uses this to distinguish landmark types.
    if (!role.is_empty() && role != "text leaf"sv) {
        attrs[QStringLiteral("xml-roles")] = QString::fromUtf8(
            reinterpret_cast<char const*>(data->role.bytes().data()),
            data->role.bytes().size());
    }

    // "level" — heading level
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

    // Find the table ancestor.
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
