/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <LibWeb/Export.h>

namespace Web::HTML::TagNames {

#define ENUMERATE_HTML_TAGS                                  \
    __ENUMERATE_HTML_TAG(a, "a")                             \
    __ENUMERATE_HTML_TAG(abbr, "abbr")                       \
    __ENUMERATE_HTML_TAG(acronym, "acronym")                 \
    __ENUMERATE_HTML_TAG(address, "address")                 \
    __ENUMERATE_HTML_TAG(applet, "applet")                   \
    __ENUMERATE_HTML_TAG(area, "area")                       \
    __ENUMERATE_HTML_TAG(article, "article")                 \
    __ENUMERATE_HTML_TAG(aside, "aside")                     \
    __ENUMERATE_HTML_TAG(audio, "audio")                     \
    __ENUMERATE_HTML_TAG(b, "b")                             \
    __ENUMERATE_HTML_TAG(base, "base")                       \
    __ENUMERATE_HTML_TAG(basefont, "basefont")               \
    __ENUMERATE_HTML_TAG(bdi, "bdi")                         \
    __ENUMERATE_HTML_TAG(bdo, "bdo")                         \
    __ENUMERATE_HTML_TAG(bgsound, "bgsound")                 \
    __ENUMERATE_HTML_TAG(big, "big")                         \
    __ENUMERATE_HTML_TAG(blink, "blink")                     \
    __ENUMERATE_HTML_TAG(blockquote, "blockquote")           \
    __ENUMERATE_HTML_TAG(body, "body")                       \
    __ENUMERATE_HTML_TAG(br, "br")                           \
    __ENUMERATE_HTML_TAG(button, "button")                   \
    __ENUMERATE_HTML_TAG(canvas, "canvas")                   \
    __ENUMERATE_HTML_TAG(caption, "caption")                 \
    __ENUMERATE_HTML_TAG(center, "center")                   \
    __ENUMERATE_HTML_TAG(cite, "cite")                       \
    __ENUMERATE_HTML_TAG(code, "code")                       \
    __ENUMERATE_HTML_TAG(col, "col")                         \
    __ENUMERATE_HTML_TAG(colgroup, "colgroup")               \
    __ENUMERATE_HTML_TAG(data, "data")                       \
    __ENUMERATE_HTML_TAG(datalist, "datalist")               \
    __ENUMERATE_HTML_TAG(dd, "dd")                           \
    __ENUMERATE_HTML_TAG(del, "del")                         \
    __ENUMERATE_HTML_TAG(details, "details")                 \
    __ENUMERATE_HTML_TAG(dfn, "dfn")                         \
    __ENUMERATE_HTML_TAG(dialog, "dialog")                   \
    __ENUMERATE_HTML_TAG(dir, "dir")                         \
    __ENUMERATE_HTML_TAG(div, "div")                         \
    __ENUMERATE_HTML_TAG(dl, "dl")                           \
    __ENUMERATE_HTML_TAG(dt, "dt")                           \
    __ENUMERATE_HTML_TAG(em, "em")                           \
    __ENUMERATE_HTML_TAG(embed, "embed")                     \
    __ENUMERATE_HTML_TAG(fieldset, "fieldset")               \
    __ENUMERATE_HTML_TAG(figcaption, "figcaption")           \
    __ENUMERATE_HTML_TAG(figure, "figure")                   \
    __ENUMERATE_HTML_TAG(font, "font")                       \
    __ENUMERATE_HTML_TAG(footer, "footer")                   \
    __ENUMERATE_HTML_TAG(form, "form")                       \
    __ENUMERATE_HTML_TAG(frame, "frame")                     \
    __ENUMERATE_HTML_TAG(frameset, "frameset")               \
    __ENUMERATE_HTML_TAG(h1, "h1")                           \
    __ENUMERATE_HTML_TAG(h2, "h2")                           \
    __ENUMERATE_HTML_TAG(h3, "h3")                           \
    __ENUMERATE_HTML_TAG(h4, "h4")                           \
    __ENUMERATE_HTML_TAG(h5, "h5")                           \
    __ENUMERATE_HTML_TAG(h6, "h6")                           \
    __ENUMERATE_HTML_TAG(head, "head")                       \
    __ENUMERATE_HTML_TAG(header, "header")                   \
    __ENUMERATE_HTML_TAG(hgroup, "hgroup")                   \
    __ENUMERATE_HTML_TAG(hr, "hr")                           \
    __ENUMERATE_HTML_TAG(html, "html")                       \
    __ENUMERATE_HTML_TAG(i, "i")                             \
    __ENUMERATE_HTML_TAG(iframe, "iframe")                   \
    __ENUMERATE_HTML_TAG(image, "image")                     \
    __ENUMERATE_HTML_TAG(img, "img")                         \
    __ENUMERATE_HTML_TAG(input, "input")                     \
    __ENUMERATE_HTML_TAG(ins, "ins")                         \
    __ENUMERATE_HTML_TAG(isindex, "isindex")                 \
    __ENUMERATE_HTML_TAG(kbd, "kbd")                         \
    __ENUMERATE_HTML_TAG(keygen, "keygen")                   \
    __ENUMERATE_HTML_TAG(label, "label")                     \
    __ENUMERATE_HTML_TAG(legend, "legend")                   \
    __ENUMERATE_HTML_TAG(li, "li")                           \
    __ENUMERATE_HTML_TAG(link, "link")                       \
    __ENUMERATE_HTML_TAG(listing, "listing")                 \
    __ENUMERATE_HTML_TAG(main, "main")                       \
    __ENUMERATE_HTML_TAG(map, "map")                         \
    __ENUMERATE_HTML_TAG(mark, "mark")                       \
    __ENUMERATE_HTML_TAG(marquee, "marquee")                 \
    __ENUMERATE_HTML_TAG(math, "math")                       \
    __ENUMERATE_HTML_TAG(menu, "menu")                       \
    __ENUMERATE_HTML_TAG(menuitem, "menuitem")               \
    __ENUMERATE_HTML_TAG(meta, "meta")                       \
    __ENUMERATE_HTML_TAG(meter, "meter")                     \
    __ENUMERATE_HTML_TAG(multicol, "multicol")               \
    __ENUMERATE_HTML_TAG(nav, "nav")                         \
    __ENUMERATE_HTML_TAG(nextid, "nextid")                   \
    __ENUMERATE_HTML_TAG(nobr, "nobr")                       \
    __ENUMERATE_HTML_TAG(noembed, "noembed")                 \
    __ENUMERATE_HTML_TAG(noframes, "noframes")               \
    __ENUMERATE_HTML_TAG(noscript, "noscript")               \
    __ENUMERATE_HTML_TAG(object, "object")                   \
    __ENUMERATE_HTML_TAG(ol, "ol")                           \
    __ENUMERATE_HTML_TAG(optgroup, "optgroup")               \
    __ENUMERATE_HTML_TAG(option, "option")                   \
    __ENUMERATE_HTML_TAG(output, "output")                   \
    __ENUMERATE_HTML_TAG(p, "p")                             \
    __ENUMERATE_HTML_TAG(param, "param")                     \
    __ENUMERATE_HTML_TAG(path, "path")                       \
    __ENUMERATE_HTML_TAG(picture, "picture")                 \
    __ENUMERATE_HTML_TAG(plaintext, "plaintext")             \
    __ENUMERATE_HTML_TAG(pre, "pre")                         \
    __ENUMERATE_HTML_TAG(progress, "progress")               \
    __ENUMERATE_HTML_TAG(q, "q")                             \
    __ENUMERATE_HTML_TAG(rb, "rb")                           \
    __ENUMERATE_HTML_TAG(rp, "rp")                           \
    __ENUMERATE_HTML_TAG(rt, "rt")                           \
    __ENUMERATE_HTML_TAG(rtc, "rtc")                         \
    __ENUMERATE_HTML_TAG(ruby, "ruby")                       \
    __ENUMERATE_HTML_TAG(s, "s")                             \
    __ENUMERATE_HTML_TAG(samp, "samp")                       \
    __ENUMERATE_HTML_TAG(script, "script")                   \
    __ENUMERATE_HTML_TAG(search, "search")                   \
    __ENUMERATE_HTML_TAG(section, "section")                 \
    __ENUMERATE_HTML_TAG(selectedcontent, "selectedcontent") \
    __ENUMERATE_HTML_TAG(select, "select")                   \
    __ENUMERATE_HTML_TAG(slot, "slot")                       \
    __ENUMERATE_HTML_TAG(small, "small")                     \
    __ENUMERATE_HTML_TAG(source, "source")                   \
    __ENUMERATE_HTML_TAG(spacer, "spacer")                   \
    __ENUMERATE_HTML_TAG(span, "span")                       \
    __ENUMERATE_HTML_TAG(strike, "strike")                   \
    __ENUMERATE_HTML_TAG(strong, "strong")                   \
    __ENUMERATE_HTML_TAG(style, "style")                     \
    __ENUMERATE_HTML_TAG(sub, "sub")                         \
    __ENUMERATE_HTML_TAG(summary, "summary")                 \
    __ENUMERATE_HTML_TAG(sup, "sup")                         \
    __ENUMERATE_HTML_TAG(svg, "svg")                         \
    __ENUMERATE_HTML_TAG(table, "table")                     \
    __ENUMERATE_HTML_TAG(tbody, "tbody")                     \
    __ENUMERATE_HTML_TAG(td, "td")                           \
    __ENUMERATE_HTML_TAG(template_, "template")              \
    __ENUMERATE_HTML_TAG(textarea, "textarea")               \
    __ENUMERATE_HTML_TAG(tfoot, "tfoot")                     \
    __ENUMERATE_HTML_TAG(th, "th")                           \
    __ENUMERATE_HTML_TAG(thead, "thead")                     \
    __ENUMERATE_HTML_TAG(time, "time")                       \
    __ENUMERATE_HTML_TAG(title, "title")                     \
    __ENUMERATE_HTML_TAG(tr, "tr")                           \
    __ENUMERATE_HTML_TAG(track, "track")                     \
    __ENUMERATE_HTML_TAG(tt, "tt")                           \
    __ENUMERATE_HTML_TAG(u, "u")                             \
    __ENUMERATE_HTML_TAG(ul, "ul")                           \
    __ENUMERATE_HTML_TAG(var, "var")                         \
    __ENUMERATE_HTML_TAG(video, "video")                     \
    __ENUMERATE_HTML_TAG(wbr, "wbr")                         \
    __ENUMERATE_HTML_TAG(xmp, "xmp")

#define __ENUMERATE_HTML_TAG(name, tag) extern WEB_API FlyString name;
ENUMERATE_HTML_TAGS
#undef __ENUMERATE_HTML_TAG

}
