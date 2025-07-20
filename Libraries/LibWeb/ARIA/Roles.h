/*
 * Copyright (c) 2022, Jonah Shafran <jonahshafran@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/StringView.h>
#include <LibWeb/Export.h>

namespace Web::ARIA {

#define ENUMERATE_ARIA_ROLES                                     \
    __ENUMERATE_ARIA_ROLE(alert, "alert")                        \
    __ENUMERATE_ARIA_ROLE(alertdialog, "alertdialog")            \
    __ENUMERATE_ARIA_ROLE(application, "application")            \
    __ENUMERATE_ARIA_ROLE(article, "article")                    \
    __ENUMERATE_ARIA_ROLE(banner, "banner")                      \
    __ENUMERATE_ARIA_ROLE(blockquote, "blockquote")              \
    __ENUMERATE_ARIA_ROLE(button, "button")                      \
    __ENUMERATE_ARIA_ROLE(caption, "caption")                    \
    __ENUMERATE_ARIA_ROLE(cell, "cell")                          \
    __ENUMERATE_ARIA_ROLE(checkbox, "checkbox")                  \
    __ENUMERATE_ARIA_ROLE(code, "code")                          \
    __ENUMERATE_ARIA_ROLE(columnheader, "columnheader")          \
    __ENUMERATE_ARIA_ROLE(combobox, "combobox")                  \
    __ENUMERATE_ARIA_ROLE(command, "command")                    \
    __ENUMERATE_ARIA_ROLE(complementary, "complementary")        \
    __ENUMERATE_ARIA_ROLE(composite, "composite")                \
    __ENUMERATE_ARIA_ROLE(contentinfo, "contentinfo")            \
    __ENUMERATE_ARIA_ROLE(definition, "definition")              \
    __ENUMERATE_ARIA_ROLE(deletion, "deletion")                  \
    __ENUMERATE_ARIA_ROLE(dialog, "dialog")                      \
    __ENUMERATE_ARIA_ROLE(directory, "directory")                \
    __ENUMERATE_ARIA_ROLE(document, "document")                  \
    __ENUMERATE_ARIA_ROLE(emphasis, "emphasis")                  \
    __ENUMERATE_ARIA_ROLE(feed, "feed")                          \
    __ENUMERATE_ARIA_ROLE(figure, "figure")                      \
    __ENUMERATE_ARIA_ROLE(form, "form")                          \
    __ENUMERATE_ARIA_ROLE(generic, "generic")                    \
    __ENUMERATE_ARIA_ROLE(graphicsdocument, "graphics-document") \
    __ENUMERATE_ARIA_ROLE(graphicsobject, "graphics-object")     \
    __ENUMERATE_ARIA_ROLE(graphicssymbol, "graphics-symbol")     \
    __ENUMERATE_ARIA_ROLE(grid, "grid")                          \
    __ENUMERATE_ARIA_ROLE(gridcell, "gridcell")                  \
    __ENUMERATE_ARIA_ROLE(group, "group")                        \
    __ENUMERATE_ARIA_ROLE(heading, "heading")                    \
    __ENUMERATE_ARIA_ROLE(image, "image")                        \
    __ENUMERATE_ARIA_ROLE(img, "img")                            \
    __ENUMERATE_ARIA_ROLE(input, "input")                        \
    __ENUMERATE_ARIA_ROLE(insertion, "insertion")                \
    __ENUMERATE_ARIA_ROLE(landmark, "landmark")                  \
    __ENUMERATE_ARIA_ROLE(link, "link")                          \
    __ENUMERATE_ARIA_ROLE(list, "list")                          \
    __ENUMERATE_ARIA_ROLE(listbox, "listbox")                    \
    __ENUMERATE_ARIA_ROLE(listitem, "listitem")                  \
    __ENUMERATE_ARIA_ROLE(log, "log")                            \
    __ENUMERATE_ARIA_ROLE(main, "main")                          \
    __ENUMERATE_ARIA_ROLE(mark, "mark")                          \
    __ENUMERATE_ARIA_ROLE(marquee, "marquee")                    \
    __ENUMERATE_ARIA_ROLE(math, "math")                          \
    __ENUMERATE_ARIA_ROLE(meter, "meter")                        \
    __ENUMERATE_ARIA_ROLE(menu, "menu")                          \
    __ENUMERATE_ARIA_ROLE(menubar, "menubar")                    \
    __ENUMERATE_ARIA_ROLE(menuitem, "menuitem")                  \
    __ENUMERATE_ARIA_ROLE(menuitemcheckbox, "menuitemcheckbox")  \
    __ENUMERATE_ARIA_ROLE(menuitemradio, "menuitemradio")        \
    __ENUMERATE_ARIA_ROLE(navigation, "navigation")              \
    __ENUMERATE_ARIA_ROLE(none, "none")                          \
    __ENUMERATE_ARIA_ROLE(note, "note")                          \
    __ENUMERATE_ARIA_ROLE(option, "option")                      \
    __ENUMERATE_ARIA_ROLE(paragraph, "paragraph")                \
    __ENUMERATE_ARIA_ROLE(presentation, "presentation")          \
    __ENUMERATE_ARIA_ROLE(progressbar, "progressbar")            \
    __ENUMERATE_ARIA_ROLE(radio, "radio")                        \
    __ENUMERATE_ARIA_ROLE(radiogroup, "radiogroup")              \
    __ENUMERATE_ARIA_ROLE(range, "range")                        \
    __ENUMERATE_ARIA_ROLE(region, "region")                      \
    __ENUMERATE_ARIA_ROLE(roletype, "roletype")                  \
    __ENUMERATE_ARIA_ROLE(row, "row")                            \
    __ENUMERATE_ARIA_ROLE(rowgroup, "rowgroup")                  \
    __ENUMERATE_ARIA_ROLE(rowheader, "rowheader")                \
    __ENUMERATE_ARIA_ROLE(scrollbar, "scrollbar")                \
    __ENUMERATE_ARIA_ROLE(search, "search")                      \
    __ENUMERATE_ARIA_ROLE(searchbox, "searchbox")                \
    __ENUMERATE_ARIA_ROLE(section, "section")                    \
    __ENUMERATE_ARIA_ROLE(sectionfooter, "sectionfooter")        \
    __ENUMERATE_ARIA_ROLE(sectionhead, "sectionhead")            \
    __ENUMERATE_ARIA_ROLE(sectionheader, "sectionheader")        \
    __ENUMERATE_ARIA_ROLE(select, "select")                      \
    __ENUMERATE_ARIA_ROLE(separator, "separator")                \
    __ENUMERATE_ARIA_ROLE(slider, "slider")                      \
    __ENUMERATE_ARIA_ROLE(spinbutton, "spinbutton")              \
    __ENUMERATE_ARIA_ROLE(status, "status")                      \
    __ENUMERATE_ARIA_ROLE(strong, "strong")                      \
    __ENUMERATE_ARIA_ROLE(structure, "structure")                \
    __ENUMERATE_ARIA_ROLE(subscript, "subscript")                \
    __ENUMERATE_ARIA_ROLE(suggestion, "suggestion")              \
    __ENUMERATE_ARIA_ROLE(superscript, "superscript")            \
    __ENUMERATE_ARIA_ROLE(switch_, "switch")                     \
    __ENUMERATE_ARIA_ROLE(tab, "tab")                            \
    __ENUMERATE_ARIA_ROLE(table, "table")                        \
    __ENUMERATE_ARIA_ROLE(tablist, "tablist")                    \
    __ENUMERATE_ARIA_ROLE(tabpanel, "tabpanel")                  \
    __ENUMERATE_ARIA_ROLE(term, "term")                          \
    __ENUMERATE_ARIA_ROLE(textbox, "textbox")                    \
    __ENUMERATE_ARIA_ROLE(time, "time")                          \
    __ENUMERATE_ARIA_ROLE(timer, "timer")                        \
    __ENUMERATE_ARIA_ROLE(toolbar, "toolbar")                    \
    __ENUMERATE_ARIA_ROLE(tooltip, "tooltip")                    \
    __ENUMERATE_ARIA_ROLE(tree, "tree")                          \
    __ENUMERATE_ARIA_ROLE(treegrid, "treegrid")                  \
    __ENUMERATE_ARIA_ROLE(treeitem, "treeitem")                  \
    __ENUMERATE_ARIA_ROLE(widget, "widget")                      \
    __ENUMERATE_ARIA_ROLE(window, "window")

enum class Role {
#define __ENUMERATE_ARIA_ROLE(name, attribute) name,
    ENUMERATE_ARIA_ROLES
#undef __ENUMERATE_ARIA_ROLE
};

WEB_API StringView role_name(Role);
Optional<Role> role_from_string(StringView role_name);

bool is_abstract_role(Role);
bool is_widget_role(Role);
bool is_document_structure_role(Role);
bool is_landmark_role(Role);
bool is_live_region_role(Role);
bool is_windows_role(Role);

bool allows_name_from_content(Role);

}
