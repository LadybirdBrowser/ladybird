/*
 * Copyright (c) 2022, Jonah Shafran <jonahshafran@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/StringView.h>

namespace Web::ARIA {

#define ENUMERATE_ARIA_ROLES                \
    __ENUMERATE_ARIA_ROLE(alert)            \
    __ENUMERATE_ARIA_ROLE(alertdialog)      \
    __ENUMERATE_ARIA_ROLE(application)      \
    __ENUMERATE_ARIA_ROLE(article)          \
    __ENUMERATE_ARIA_ROLE(banner)           \
    __ENUMERATE_ARIA_ROLE(blockquote)       \
    __ENUMERATE_ARIA_ROLE(button)           \
    __ENUMERATE_ARIA_ROLE(caption)          \
    __ENUMERATE_ARIA_ROLE(cell)             \
    __ENUMERATE_ARIA_ROLE(checkbox)         \
    __ENUMERATE_ARIA_ROLE(code)             \
    __ENUMERATE_ARIA_ROLE(columnheader)     \
    __ENUMERATE_ARIA_ROLE(combobox)         \
    __ENUMERATE_ARIA_ROLE(command)          \
    __ENUMERATE_ARIA_ROLE(complementary)    \
    __ENUMERATE_ARIA_ROLE(composite)        \
    __ENUMERATE_ARIA_ROLE(contentinfo)      \
    __ENUMERATE_ARIA_ROLE(definition)       \
    __ENUMERATE_ARIA_ROLE(deletion)         \
    __ENUMERATE_ARIA_ROLE(dialog)           \
    __ENUMERATE_ARIA_ROLE(directory)        \
    __ENUMERATE_ARIA_ROLE(document)         \
    __ENUMERATE_ARIA_ROLE(emphasis)         \
    __ENUMERATE_ARIA_ROLE(feed)             \
    __ENUMERATE_ARIA_ROLE(figure)           \
    __ENUMERATE_ARIA_ROLE(form)             \
    __ENUMERATE_ARIA_ROLE(generic)          \
    __ENUMERATE_ARIA_ROLE(grid)             \
    __ENUMERATE_ARIA_ROLE(gridcell)         \
    __ENUMERATE_ARIA_ROLE(group)            \
    __ENUMERATE_ARIA_ROLE(heading)          \
    __ENUMERATE_ARIA_ROLE(img)              \
    __ENUMERATE_ARIA_ROLE(input)            \
    __ENUMERATE_ARIA_ROLE(insertion)        \
    __ENUMERATE_ARIA_ROLE(landmark)         \
    __ENUMERATE_ARIA_ROLE(link)             \
    __ENUMERATE_ARIA_ROLE(list)             \
    __ENUMERATE_ARIA_ROLE(listbox)          \
    __ENUMERATE_ARIA_ROLE(listitem)         \
    __ENUMERATE_ARIA_ROLE(log)              \
    __ENUMERATE_ARIA_ROLE(main)             \
    __ENUMERATE_ARIA_ROLE(marquee)          \
    __ENUMERATE_ARIA_ROLE(math)             \
    __ENUMERATE_ARIA_ROLE(meter)            \
    __ENUMERATE_ARIA_ROLE(menu)             \
    __ENUMERATE_ARIA_ROLE(menubar)          \
    __ENUMERATE_ARIA_ROLE(menuitem)         \
    __ENUMERATE_ARIA_ROLE(menuitemcheckbox) \
    __ENUMERATE_ARIA_ROLE(menuitemradio)    \
    __ENUMERATE_ARIA_ROLE(navigation)       \
    __ENUMERATE_ARIA_ROLE(none)             \
    __ENUMERATE_ARIA_ROLE(note)             \
    __ENUMERATE_ARIA_ROLE(option)           \
    __ENUMERATE_ARIA_ROLE(paragraph)        \
    __ENUMERATE_ARIA_ROLE(presentation)     \
    __ENUMERATE_ARIA_ROLE(progressbar)      \
    __ENUMERATE_ARIA_ROLE(radio)            \
    __ENUMERATE_ARIA_ROLE(radiogroup)       \
    __ENUMERATE_ARIA_ROLE(range)            \
    __ENUMERATE_ARIA_ROLE(region)           \
    __ENUMERATE_ARIA_ROLE(roletype)         \
    __ENUMERATE_ARIA_ROLE(row)              \
    __ENUMERATE_ARIA_ROLE(rowgroup)         \
    __ENUMERATE_ARIA_ROLE(rowheader)        \
    __ENUMERATE_ARIA_ROLE(scrollbar)        \
    __ENUMERATE_ARIA_ROLE(search)           \
    __ENUMERATE_ARIA_ROLE(searchbox)        \
    __ENUMERATE_ARIA_ROLE(section)          \
    __ENUMERATE_ARIA_ROLE(sectionhead)      \
    __ENUMERATE_ARIA_ROLE(select)           \
    __ENUMERATE_ARIA_ROLE(separator)        \
    __ENUMERATE_ARIA_ROLE(slider)           \
    __ENUMERATE_ARIA_ROLE(spinbutton)       \
    __ENUMERATE_ARIA_ROLE(status)           \
    __ENUMERATE_ARIA_ROLE(strong)           \
    __ENUMERATE_ARIA_ROLE(structure)        \
    __ENUMERATE_ARIA_ROLE(subscript)        \
    __ENUMERATE_ARIA_ROLE(superscript)      \
    __ENUMERATE_ARIA_ROLE(switch_)          \
    __ENUMERATE_ARIA_ROLE(tab)              \
    __ENUMERATE_ARIA_ROLE(table)            \
    __ENUMERATE_ARIA_ROLE(tablist)          \
    __ENUMERATE_ARIA_ROLE(tabpanel)         \
    __ENUMERATE_ARIA_ROLE(term)             \
    __ENUMERATE_ARIA_ROLE(textbox)          \
    __ENUMERATE_ARIA_ROLE(time)             \
    __ENUMERATE_ARIA_ROLE(timer)            \
    __ENUMERATE_ARIA_ROLE(toolbar)          \
    __ENUMERATE_ARIA_ROLE(tooltip)          \
    __ENUMERATE_ARIA_ROLE(tree)             \
    __ENUMERATE_ARIA_ROLE(treegrid)         \
    __ENUMERATE_ARIA_ROLE(treeitem)         \
    __ENUMERATE_ARIA_ROLE(widget)           \
    __ENUMERATE_ARIA_ROLE(window)

enum class Role {
#define __ENUMERATE_ARIA_ROLE(name) name,
    ENUMERATE_ARIA_ROLES
#undef __ENUMERATE_ARIA_ROLE
};

StringView role_name(Role);
Optional<Role> role_from_string(StringView role_name);

bool is_abstract_role(Role);
bool is_widget_role(Role);
bool is_document_structure_role(Role);
bool is_landmark_role(Role);
bool is_live_region_role(Role);
bool is_windows_role(Role);

bool is_non_abstract_role(Role);
bool allows_name_from_content(Role);

}
