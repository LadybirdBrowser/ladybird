#!/usr/bin/env python3

import json
import multiprocessing
import os
import sys

TESTS_LIBWEB_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
WPT_ROOT = os.path.join(TESTS_LIBWEB_ROOT, "WPT", "wpt")
WPT_TOOLS_ROOT = os.path.join(WPT_ROOT, "tools")


def main():
    sys.path[:0] = [
        os.path.join(WPT_TOOLS_ROOT, "serve"),
        WPT_TOOLS_ROOT,
        WPT_ROOT,
        os.path.join(WPT_TOOLS_ROOT, "third_party", "webencodings"),
        os.path.join(WPT_TOOLS_ROOT, "third_party", "six"),
        os.path.join(WPT_TOOLS_ROOT, "third_party", "html5lib"),
        # WPT/wpt/tools/serve/serve.py imports html5lib eagerly before WPT/wpt/tools/localpaths.py
        # gets a chance to set up the paths. If launch ever stops working, check serve.py
    ]

    import serve

    with open(os.path.join(os.path.dirname(__file__), "wpt-resource-overrides.json"), encoding="utf-8") as mapping_file:
        resource_mappings = json.load(mapping_file)["resources"]

    def route_builder(logger, aliases, config):
        builder = serve.get_route_builder(logger, aliases, config)
        for mapping in resource_mappings:
            file_path = os.path.join(TESTS_LIBWEB_ROOT, mapping["destination"])
            if os.path.exists(file_path):
                builder.add_static(file_path, {}, mapping["content_type"], mapping["route"])
        return builder

    mp_context = serve.MpContext()
    if os.name != "nt" and "fork" in multiprocessing.get_all_start_methods():
        mp_context = multiprocessing.get_context("fork")

    return serve.run(
        route_builder=route_builder,
        mp_context=mp_context,
        log_handlers=None,
        doc_root=WPT_ROOT,
        verbose=True,
    )


if __name__ == "__main__":
    raise SystemExit(main())
