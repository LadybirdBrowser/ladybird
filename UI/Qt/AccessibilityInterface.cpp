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

// WebContentViewAccessible — bridges WebContentView into the Qt accessibility tree

WebContentViewAccessible::WebContentViewAccessible(WebContentView* view)
    : QAccessibleWidget(view, QAccessible::Grouping)
{
}

int WebContentViewAccessible::childCount() const
{
    auto* view = static_cast<WebContentView*>(widget());
    if (!view || !view->m_accessibility_manager || view->m_accessibility_manager->is_empty())
        return 0;
    return 1; // The document root
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
    auto* c = child(0);
    if (!c)
        return nullptr;
    return c->focusChild();
}

int WebContentViewAccessible::indexOfChild(QAccessibleInterface const* iface) const
{
    auto* c = child(0);
    if (c && c == iface)
        return 0;
    return -1;
}

QAccessible::Role WebContentViewAccessible::role() const
{
    return QAccessible::Grouping;
}

static bool is_ignored_role(StringView role, AK::String const& name)
{
    return (role == "generic"sv && name.is_empty())
        || (role == "paragraph"sv && name.is_empty());
}

AccessibilityInterface::AccessibilityInterface(i64 node_id, WebView::AccessibilityTreeManager const* manager, WebContentView* view)
    : m_node_id(node_id)
    , m_manager(manager)
    , m_view(view)
{
    // Qt's macOS bridge requires a non-null QObject* from object()
    // to properly track elements for navigation. Create a dummy
    // QObject parented to the view widget.
    m_object = new QObject(view);

    // Set macOS-specific properties for landmark subroles and role
    // descriptions. Qt's cocoa bridge reads these via the
    // _qt_mac_subrole and _qt_mac_roleDescription dynamic properties.
    auto const* data = node_data();
    if (data) {
        auto r = data->role.bytes_as_string_view();
        struct LandmarkInfo {
            StringView role;
            char const* subrole;
            char const* description;
        };
        static constexpr LandmarkInfo landmarks[] = {
            { "navigation"sv, "AXLandmarkNavigation", "navigation" },
            { "main"sv, "AXLandmarkMain", "main" },
            { "banner"sv, "AXLandmarkBanner", "banner" },
            { "complementary"sv, "AXLandmarkComplementary", "complementary" },
            { "contentinfo"sv, "AXLandmarkContentInfo", "content information" },
            { "search"sv, "AXLandmarkSearch", "search" },
            { "form"sv, "AXLandmarkForm", "form" },
            { "region"sv, "AXLandmarkRegion", "region" },
        };
        for (auto const& info : landmarks) {
            if (r == info.role) {
                m_object->setProperty("_qt_mac_subrole", QString::fromLatin1(info.subrole));
                m_object->setProperty("_qt_mac_roleDescription", QString::fromLatin1(info.description));
                break;
            }
        }
        if (r == "document"sv)
            m_object->setProperty("_qt_mac_roleDescription", QStringLiteral("HTML content"));
    }

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
        if (data && data->role.bytes_as_string_view() == "text leaf"sv)
            return static_cast<QAccessibleTextInterface*>(this);
        break;
    }
    case QAccessible::TableCellInterface: {
        auto r = role();
        if (r == QAccessible::Cell || r == QAccessible::ColumnHeader || r == QAccessible::RowHeader)
            return static_cast<QAccessibleTableCellInterface*>(this);
        break;
    }
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
        if (auto* c = child(i)) {
            if (auto* fc = c->focusChild())
                return fc;
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

    auto to_qstring = [](AK::String const& s) -> QString {
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
    QAccessible::State s;
    auto const* data = node_data();
    if (!data)
        return s;

    s.focusable = true;
    if (data->is_focused)
        s.focused = true;
    if (data->is_disabled)
        s.disabled = true;

    auto role = data->role.bytes_as_string_view();
    if (role == "link"sv)
        s.linked = true;
    if (role == "text leaf"sv)
        s.readOnly = true;

    return s;
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

QString AccessibilityInterface::attributes(int, int* startOffset, int* endOffset) const
{
    *startOffset = 0;
    *endOffset = 0;
    return {};
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

QString AccessibilityInterface::text(int startOffset, int endOffset) const
{
    auto const* data = node_data();
    if (!data)
        return {};

    auto full_text = QString::fromUtf8(reinterpret_cast<char const*>(data->name.bytes().data()), data->name.bytes().size());
    return full_text.mid(startOffset, endOffset - startOffset);
}

void AccessibilityInterface::removeSelection(int) { }
void AccessibilityInterface::setCursorPosition(int) { }
void AccessibilityInterface::setSelection(int, int, int) { }

int AccessibilityInterface::characterCount() const
{
    auto const* data = node_data();
    if (!data)
        return 0;
    auto full_text = QString::fromUtf8(reinterpret_cast<char const*>(data->name.bytes().data()), data->name.bytes().size());
    return full_text.size();
}

void AccessibilityInterface::scrollToSubstring(int, int) { }

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

    int col = 0;
    for (auto sibling_id : row->child_ids) {
        if (sibling_id == m_node_id)
            return col;
        col++;
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

    // Find the table ancestor
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
