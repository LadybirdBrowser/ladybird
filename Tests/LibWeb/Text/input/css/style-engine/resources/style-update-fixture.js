// Shared by the small correctness test and Meta/style-update-capture.py.
function createStyleUpdateFixture(elementCount, scenario, seed, measure = false) {
    const snapshot = () => ({
        rust: internals.styleEngineCounters(),
        cpp: internals.getStyleInvalidationCounters(),
        ffi: internals.styleFfiCounters(),
    });
    const now = () => (measure ? performance.now() * 1000 : null);
    const elapsed = start => (measure ? now() - start : null);
    const check = (node, property, expected) => {
        const actual = getComputedStyle(node)[property];
        if (actual !== expected)
            throw new Error(`${scenario}: ${node.id}.${property}: expected ${expected}, got ${actual}`);
        return { node: node.id, property, expected, actual };
    };
    const observe = checks => checks.map(([node, property, expected]) => check(node, property, expected));
    const interval = (name, kind, mutate, checks) => {
        const before = snapshot();
        const start = now();
        mutate();
        const mutationMicroseconds = elapsed(start);
        const observationStart = now();
        const observations = observe(checks());
        const observationMicroseconds = elapsed(observationStart);
        const after = snapshot();
        return { name, kind, before, after, mutationMicroseconds, observationMicroseconds, observations };
    };
    if (!Number.isInteger(elementCount) || elementCount < 100 || !["connected", "detached"].includes(scenario))
        throw new Error("Invalid fixture configuration");
    const style = document.createElement("style");
    style.textContent = `
        .fixture { color: rgb(10, 20, 30) }
        .fixture.theme { color: rgb(40, 50, 60) }
        .fixture .item { display: block; opacity: 1; margin: 0; padding: 0; border: 0 solid black }
        .fixture .active > .item { opacity: 0.5 }
        .fixture .signal + .item { margin-left: 11px }
        .fixture .signal ~ .item { margin-right: 13px }
        .fixture .lane > .item:nth-child(2) { padding-top: 17px }
        .fixture .lane > span:nth-of-type(2) { padding-bottom: 19px }
        .fixture .lane > :nth-child(2 of [data-picked="yes"]) { border-top-width: 7px }
        .fixture [data-note="hot"] { padding-left: 23px }
        .fixture .destination > .item { opacity: 0.75 }
    `;
    const root = document.createElement("div");
    root.id = "fixture-root";
    root.className = "fixture";
    const lane = document.createElement("div");
    lane.id = "fixture-lane";
    lane.className = "lane";
    const destination = document.createElement("div");
    destination.id = "fixture-destination";
    destination.className = "destination";
    const items = [];
    let count = 3;
    let random = seed >>> 0;
    const next = () => (random = (Math.imul(random, 1664525) + 1013904223) >>> 0);
    const setup = interval(
        "construction",
        "setup",
        () => {
            document.head.appendChild(style);
            if (scenario === "connected") document.body.appendChild(root);
            root.append(lane, destination);
            const width = Math.min(1000, Math.floor(elementCount / 3));
            for (let i = 0; i < width; ++i) {
                const item = document.createElement(i % 4 === 2 ? "div" : "span");
                item.id = `fixture-item-${i}`;
                item.className = "item";
                item.setAttribute("data-picked", "yes");
                lane.appendChild(item);
                items.push(item);
                ++count;
            }
            // Bounded depth, many inheritance consumers, and a wide positional sequence.
            // The seed changes only the remaining tree's parent choices, not the named assertions.
            const parents = [destination];
            while (count < elementCount) {
                const item = document.createElement("div");
                item.id = `fixture-tree-${count}`;
                item.className = "item";
                parents[next() % parents.length].appendChild(item);
                if (parents.length < 64) parents.push(item);
                ++count;
            }
            if (scenario === "detached") document.body.appendChild(root);
        },
        () => [
            [items[1], "color", "rgb(10, 20, 30)"],
            [items[1], "paddingTop", "17px"],
            [items[1], "paddingBottom", "19px"],
            [items[1], "borderTopWidth", "7px"],
        ]
    );
    if (root.querySelectorAll("*").length + 1 !== elementCount) throw new Error("Fixture element count is wrong");
    const [first, second, third, fourth] = items;
    const inserted = document.createElement("span");
    inserted.id = "fixture-inserted";
    inserted.className = "item";
    const steps = [
        ["ancestor-class-add", () => root.classList.add("theme"), [[second, "color", "rgb(40, 50, 60)"]]],
        ["ancestor-class-remove", () => root.classList.remove("theme"), [[second, "color", "rgb(10, 20, 30)"]]],
        ["child-class-add", () => lane.classList.add("active"), [[second, "opacity", "0.5"]]],
        ["child-class-remove", () => lane.classList.remove("active"), [[second, "opacity", "1"]]],
        [
            "sibling-class-add",
            () => first.classList.add("signal"),
            [
                [second, "marginLeft", "11px"],
                [fourth, "marginRight", "13px"],
            ],
        ],
        [
            "sibling-class-remove",
            () => first.classList.remove("signal"),
            [
                [second, "marginLeft", "0px"],
                [fourth, "marginRight", "0px"],
            ],
        ],
        [
            "filtered-attribute-set",
            () => first.setAttribute("data-picked", "no"),
            [
                [second, "borderTopWidth", "0px"],
                [third, "borderTopWidth", "7px"],
            ],
        ],
        ["local-attribute-set", () => second.setAttribute("data-note", "hot"), [[second, "paddingLeft", "23px"]]],
        [
            "insert",
            () => lane.insertBefore(inserted, first),
            [
                [first, "paddingTop", "17px"],
                [first, "paddingBottom", "19px"],
                [second, "paddingTop", "0px"],
                [second, "paddingBottom", "0px"],
            ],
        ],
        [
            "remove",
            () => inserted.remove(),
            [
                [first, "paddingTop", "0px"],
                [second, "paddingTop", "17px"],
                [second, "paddingBottom", "19px"],
            ],
        ],
        [
            "move-out",
            () => destination.appendChild(second),
            [
                [second, "opacity", "0.75"],
                [third, "paddingTop", "17px"],
                [fourth, "paddingBottom", "19px"],
                [fourth, "borderTopWidth", "7px"],
            ],
        ],
        [
            "move-back",
            () => lane.insertBefore(second, third),
            [
                [second, "opacity", "1"],
                [second, "paddingTop", "17px"],
                [second, "paddingBottom", "19px"],
                [third, "borderTopWidth", "7px"],
                [fourth, "borderTopWidth", "0px"],
            ],
        ],
    ];
    let nextStep = 0;
    return {
        setup,
        snapshot,
        step(index) {
            if (index !== nextStep++) throw new Error("Fixture mutations must run exactly once in order");
            const [name, mutate, checks] = steps[index];
            return interval(name, "mutation", mutate, () => checks);
        },
        dispose() {
            root.remove();
            style.remove();
        },
    };
}
