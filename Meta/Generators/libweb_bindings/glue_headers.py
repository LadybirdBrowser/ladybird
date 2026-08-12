# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

from typing import Optional

from Utils.webidl_parser import Interface


def bindings_glue_header_for_interface(interface: Interface, wrapper_interface_name: Optional[str] = None) -> str:
    name = wrapper_interface_name or interface.name
    module = interface.path.parent.name

    module_headers = {
        "Animations": "LibWeb/Animations/BindingsGlue.h",
        "CSS": "LibWeb/CSS/BindingsGlue.h",
        "CookieStore": "LibWeb/CookieStore/BindingsGlue.h",
        "IndexedDB": "LibWeb/IndexedDB/BindingsGlue.h",
        "Streams": "LibWeb/Streams/BindingsGlue.h",
        "TrustedTypes": "LibWeb/TrustedTypes/BindingsGlue.h",
        "WebAssembly": "LibWeb/WebAssembly/BindingsGlue.h",
        "WebAudio": "LibWeb/WebAudio/BindingsGlue.h",
        "WebGL": "LibWeb/WebGL/BindingsGlue.h",
    }
    if module in module_headers:
        return module_headers[module]

    if module == "CustomElements":
        return "LibWeb/HTML/CustomElements/BindingsGlue.h"

    if name in ("Document", "Event", "EventTarget", "NodeIterator", "ShadowRoot", "TreeWalker"):
        return "LibWeb/DOM/BindingsGlue.h"

    if name == "WindowOrWorkerGlobalScope":
        return "LibWeb/Fetch/BindingsGlue.h"

    if name == "OffscreenCanvas":
        return "LibWeb/HTML/BindingsGlue.h"

    direct_headers = {
        "AnimationFrameProvider": "LibWeb/HTML/Window.h",
        "Element": "LibWeb/DOM/Element.h",
        "SubtleCrypto": "LibWeb/Crypto/SubtleCrypto.h",
        "Window": "LibWeb/HTML/Window.h",
    }
    if name in direct_headers:
        return direct_headers[name]

    raise RuntimeError(f"No bindings glue header is registered for {name}")
