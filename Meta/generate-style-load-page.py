#!/usr/bin/env python3
"""Generate a local page for style-arrival measurements.

A real page load is network-dominated and cannot see a per-element style regression. This one can:
it is one document, no network, and its cost is almost entirely deciding what matches what.

The seed is fixed so two builds see the same document, and every knob is separate so a measurement
can vary exactly one property of the page - which is the method that found what profiling did not.

    ./Meta/generate-style-load-page.py > /tmp/style-load.html
    ./Meta/generate-style-load-page.py --rules 200 --shapes '.c' > /tmp/one-shape.html

Then, from the build directory:

    ./bin/Ladybird.app/Contents/MacOS/WebDriver --headless -p 4540 &
    ./Meta/measure-style-load.py 4540 file:///tmp/style-load.html
"""

import argparse
import random

# The mix a page of hand-written CSS has, in rough proportion. `--shapes` replaces it wholesale so
# one shape can be measured on its own.
DEFAULT_SHAPES = [
    ".c{i}",
    ".c{i}",
    "div.c{i}",
    ".c{i} .d{i}",
    ".c{i} > .d{i}",
    '[data-k="{i}"]',
    ".c{i}:hover, .d{i}:first-child",
]

DECLARATIONS = [
    "color: rgb({r}, {g}, {b})",
    "border-top-width: {i}px",
    "margin-left: {i}px",
    "letter-spacing: {i}px",
]


def rules(count, shapes, rng):
    for i in range(count):
        shape = shapes[i % len(shapes)]
        declaration = DECLARATIONS[i % len(DECLARATIONS)]
        yield "%s { %s }" % (
            shape.format(i=i),
            declaration.format(i=i % 20, r=rng.randrange(256), g=rng.randrange(256), b=rng.randrange(256)),
        )


def elements(count, pool, depth, rng):
    open_tags = []
    for i in range(count):
        # A tree rather than a list, so descendant and child combinators have something to walk.
        if len(open_tags) >= depth or (open_tags and rng.random() < 0.35):
            yield "</%s>" % open_tags.pop()
        tag = "div" if i % 3 else "span"
        classes = "c%d d%d" % (rng.randrange(pool), rng.randrange(pool))
        yield '<%s class="%s" data-k="%d">' % (tag, classes, rng.randrange(pool))
        open_tags.append(tag)
    while open_tags:
        yield "</%s>" % open_tags.pop()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--elements", type=int, default=20000)
    parser.add_argument("--rules", type=int, default=1200)
    parser.add_argument("--depth", type=int, default=12)
    parser.add_argument("--seed", type=int, default=20260727)
    parser.add_argument("--shapes", help="comma-separated selector templates, `{i}` for the rule index")
    parser.add_argument(
        "--unshared",
        action="store_true",
        help="give every element a class of its own, declaring a value nothing else declares, so "
        "that style sharing cannot answer for any of them. This is the page that measures what one "
        "computed style costs: on a page whose elements share, four in five are answered from "
        "another element and the cost of building one is invisible.",
    )
    parser.add_argument(
        "--name-pool",
        type=int,
        help="how many distinct class and attribute names the elements draw from, when that has to "
        "stay fixed while the rule count varies (defaults to the rule count)",
    )
    arguments = parser.parse_args()

    shapes = arguments.shapes.split(",") if arguments.shapes else DEFAULT_SHAPES
    rng = random.Random(arguments.seed)

    print("<!DOCTYPE html>")
    print("<meta charset=utf-8>")
    print("<title>style load</title>")
    print("<style>")
    for rule in rules(arguments.rules, shapes, rng):
        print(rule)
    if arguments.unshared:
        for index in range(arguments.elements):
            print(
                ".u%d { color: rgb(%d, %d, %d); margin-left: %dpx }"
                % (index, index % 256, (index * 7) % 256, (index * 13) % 256, index % 40)
            )
    print("</style>")
    pool = arguments.name_pool or arguments.rules
    unique = 0
    for element in elements(arguments.elements, pool, arguments.depth, rng):
        if arguments.unshared and element.startswith("<") and not element.startswith("</"):
            element = element.replace('class="', 'class="u%d ' % unique, 1)
            unique += 1
        print(element, end="")
    print()


if __name__ == "__main__":
    main()
