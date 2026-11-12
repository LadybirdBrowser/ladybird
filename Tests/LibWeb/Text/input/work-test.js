// Work expectations for the stages of the rendering pipeline: exact traces with workTest() and its per-stage
// wrappers, or queries over the traced events with layoutWork(). A step's `expect` is the trace of the test's stage,
// or an object with one trace per stage it expects work from; the step settles the last of those stages in pipeline
// order. Keep measured steps synchronous and buffer output: writing test results into the document can invalidate
// later steps.

function normalizeTrace(text) {
    const lines = text.replace(/\r/g, "").split("\n");
    while (lines.length && !lines[0].trim()) lines.shift();
    while (lines.length && !lines.at(-1).trim()) lines.pop();
    const indent = Math.min(...lines.filter(line => line.trim()).map(line => line.match(/^ */)[0].length));
    return lines.map(line => line.slice(indent).trimEnd()).join("\n");
}

function traceDifference(expected, actual) {
    const left = normalizeTrace(expected).split("\n");
    const right = normalizeTrace(actual).split("\n");
    const index = left.findIndex((line, index) => line !== right[index]);
    if (index < 0 && left.length === right.length) return null;
    const line = index < 0 ? left.length : index;
    return `line ${line + 1}: expected ${JSON.stringify(left[line] ?? "<end>")}, got ${JSON.stringify(right[line] ?? "<end>")}`;
}

// Block fixtures can opt out of anonymous line containers created by source indentation.
function removeBodyWhitespace() {
    for (const node of [...document.body.childNodes]) {
        if (node.nodeType === Node.TEXT_NODE && !node.textContent.trim()) node.remove();
    }
}

// Replaces the content of the body with `html`, keeping the output element last, so that each case of a scenario file
// starts from a fixture of its own as a page of its own would.
function loadFixture(html) {
    const template = document.createElement("template");
    template.innerHTML = html;
    document.body.replaceChildren(template.content, __outputElement);
}

// The traced stages in pipeline order. Settling a stage brings every stage before it up to date as well.
const workStages = [
    {
        name: "style",
        begin: testInternals => testInternals.beginStyleTrace(),
        take: testInternals => testInternals.takeStyleTrace(),
        settle: testInternals => testInternals.updateStyle(),
    },
    {
        name: "layout",
        begin: testInternals => testInternals.beginLayoutTrace(),
        take: testInternals => testInternals.takeLayoutTrace(),
        settle: testInternals => testInternals.updateLayoutForTesting(),
    },
    {
        name: "visualContexts",
        begin: testInternals => testInternals.beginVisualContextTrace(),
        take: testInternals => testInternals.takeVisualContextTrace(),
        settle: testInternals => testInternals.updateVisualContextsForTesting(),
    },
    {
        name: "displayList",
        begin: testInternals => testInternals.beginDisplayListTrace(),
        take: testInternals => testInternals.takeDisplayListTrace(),
        settle: (testInternals, step) =>
            testInternals.recordDisplayListForTesting(step.paintOverlay ?? false, step.cold ?? false),
        expected: (step, trace) => `recording (overlay=${step.paintOverlay ?? false})\n${normalizeTrace(trace)}`,
    },
];

function workStage(name) {
    const stage = workStages.find(stage => stage.name === name);
    if (!stage) throw new Error(`unknown work stage ${JSON.stringify(name)}`);
    return stage;
}

function workExpectations(step, defaultStage) {
    const expect = typeof step.expect === "string" ? { [defaultStage]: step.expect } : step.expect;
    if (
        !expect ||
        typeof expect !== "object" ||
        !Object.keys(expect).length ||
        Object.values(expect).some(trace => typeof trace !== "string")
    )
        throw new Error(`${step.name}: missing exact ${defaultStage} work expectation`);
    for (const name of Object.keys(expect)) workStage(name);
    return expect;
}

// Runs one step: traces the stages it expects, mutates, settles the last of them, runs `check(equal)`, and throws
// with every difference and the actual traces where the work or a check is not as expected.
function runWorkStep(step, defaultStage, testInternals = internals) {
    const expect = workExpectations(step, defaultStage);
    const stages = workStages.filter(stage => stage.name in expect);
    const actual = {};
    let error;
    for (const stage of stages) stage.begin(testInternals);
    try {
        if (step.mutate?.()?.then) throw new Error("mutate must be synchronous");
        stages.at(-1).settle(testInternals, step);
        if (
            step.check?.((actual, expected, label) => {
                if (!Object.is(actual, expected)) {
                    throw new Error(`${label}: expected ${JSON.stringify(expected)}, got ${JSON.stringify(actual)}`);
                }
            })?.then
        )
            throw new Error("check must be synchronous");
    } catch (caught) {
        error = String(caught);
    } finally {
        for (const stage of stages) actual[stage.name] = stage.take(testInternals);
    }
    const differences = stages
        .map(stage => {
            const difference = traceDifference(
                stage.expected?.(step, expect[stage.name]) ?? expect[stage.name],
                actual[stage.name]
            );
            return difference && (stages.length > 1 ? `${stage.name} ${difference}` : difference);
        })
        .filter(Boolean);
    if (differences.length || error) {
        const traces = stages.map(stage => `Actual ${stage.name} trace:\n${actual[stage.name].trimEnd()}\nEnd trace`);
        throw new Error(`${step.name}: ${[...differences, error].filter(Boolean).join("\n")}\n${traces.join("\n")}`);
    }
}

// Runs `steps` after `setup`, each with exact work expectations, and prints one PASS or FAIL line per step at the end.
// The output element is hidden while the steps run, before `setup` or, with `hideOutputAfterSetup`, after it. The
// stage of the test is settled once before the first step, so that its work is exact.
function workTest({ setup, steps, cleanup }, { stage: defaultStage, hideOutputAfterSetup = false }) {
    promiseTest(async () => {
        const output = [];
        const originalDisplay = __outputElement.style.display;
        try {
            if (document.readyState !== "complete") {
                await new Promise(resolve => window.addEventListener("load", resolve, { once: true }));
            }
            if (!hideOutputAfterSetup) __outputElement.style.display = "none";
            await setup?.();
            await document.fonts.ready;
            __outputElement.style.display = "none";
            workStage(defaultStage).settle(internals, {});
            for (const step of steps) {
                try {
                    runWorkStep(step, defaultStage);
                    output.push(`PASS: ${step.name} (work${step.check ? " + checks" : ""})`);
                } catch (error) {
                    output.push(`FAIL: ${error}`);
                }
            }
        } catch (error) {
            output.push(`FAIL: ${error}`);
        } finally {
            workStage(defaultStage).take(internals);
            try {
                await cleanup?.();
            } catch (error) {
                output.push(`FAIL: cleanup: ${error}`);
            }
            __outputElement.style.display = originalDisplay;
            for (const line of output) println(line);
        }
    });
}

function layoutTest(spec) {
    workTest(spec, { stage: "layout" });
}

function styleTest(spec) {
    workTest(spec, { stage: "style" });
}

function visualContextTest(spec) {
    workTest(spec, { stage: "visualContexts" });
}

function displayListTest(spec) {
    workTest(spec, { stage: "displayList", hideOutputAfterSetup: true });
}

function runLayoutTestStep(step, testInternals = internals) {
    runWorkStep(step, "layout", testInternals);
}

// The kinds of layout trace lines, each as a pattern and the event that its groups make.
const layoutTraceLines = (() => {
    const box = String.raw`(\S+)`;
    const cache = String.raw`\(cache=(\w+)\)`;
    return [
        ["layout FULL", () => ({ event: "layout", partial: false })],
        [`layout PARTIAL ${box}`, box => ({ event: "layout", partial: true, box })],
        [
            `${box} MEASURE ((?:min|max)-content (?:inline|block)) ${cache}(?: FROM (ITEMS|FLEX ITEMS|GRID TRACKS|CHILDREN|COLUMNS|REPLACED CONTENT|ORTHOGONAL FLOW|MATH LAYOUT))?`,
            (box, size, cache, from) => ({ event: "measure", box, size, cache, from: from?.toLowerCase() }),
        ],
        [`${box} REUSE IN-FLOW SUBTREE`, box => ({ event: "reuse in-flow subtree", box })],
        [
            "build( WHOLE TREE| ESCAPED)?",
            kind => ({ event: "build", wholeTree: kind === " WHOLE TREE", escaped: kind === " ESCAPED" }),
        ],
        [`${box} REBUILD`, box => ({ event: "rebuild", box })],
        [`${box} CREATE`, box => ({ event: "create", box })],
        [
            `${box}/([a-z-]+) ([A-Z][A-Z -]*[A-Z])(?: ${cache})?`,
            (box, formattingContext, action, cache) => ({
                event: "run",
                box,
                formattingContext,
                action,
                cache,
            }),
        ],
    ].map(([pattern, makeEvent]) => [new RegExp(`^${pattern}$`), makeEvent]);
})();

// Parses a layout trace into one event per line. Each event has its depth, its kind in `event`, and the
// name of the box it is about in `box`, except for a full layout pass and a layout tree build:
//   build                   `wholeTree`, `escaped` (only traced with { treeBuilds: true })
//   rebuild, create         the box whose subtree a build rebuilt, and a box it created
//   layout                  `partial`
//   run                     `formattingContext`, `action`, `cache`
//   reuse in-flow subtree
//   measure                 `size` ("min-content inline", ...), `cache`, `from` ("items", "flex items",
//                           "grid tracks", "children", "columns", "replaced content", "orthogonal flow",
//                           "math layout")
function parseLayoutTrace(text) {
    const events = [];
    for (const line of text.replace(/\r/g, "").split("\n")) {
        if (!line.trim()) continue;
        const depth = line.match(/^ */)[0].length / 2;
        const content = line.trim();
        const kind = layoutTraceLines.find(([pattern]) => pattern.test(content));
        if (!kind) throw new Error(`unrecognized layout trace line: ${JSON.stringify(line)}`);
        const [pattern, makeEvent] = kind;
        events.push({ depth, ...makeEvent(...content.match(pattern).slice(1)) });
    }
    return events;
}

// Whether `event` has every property of `filter`, or passes it where it is a function.
function workEventMatches(event, filter) {
    if (typeof filter === "function") return filter(event);
    return Object.entries(filter).every(([key, value]) => event[key] === value);
}

// Brings `stage` up to date, then runs `mutate` and, unless `flush` is false, settles the stage again, tracing only
// the work the mutation causes. Output printed before the call is settled first, so it is never counted.
function traceWork(stageName, mutate, testInternals = internals, { flush = true } = {}) {
    const stage = workStage(stageName);
    stage.settle(testInternals, {});
    stage.begin(testInternals);
    let text;
    try {
        if (mutate()?.then) throw new Error("mutate must be synchronous");
        if (flush) stage.settle(testInternals, {});
    } finally {
        text = stage.take(testInternals);
    }
    return text;
}

// Lays out what is pending, then runs `mutate` and lays out again, tracing only the layout work the mutation causes.
// Answers the trace and its events, with queries over the events that match a filter (see workEventMatches).
function layoutWork(mutate, testInternals = internals, options = {}) {
    const text = traceWork("layout", mutate, testInternals, options);
    const events = parseLayoutTrace(text);
    const matching = filter => events.filter(event => workEventMatches(event, filter));
    return {
        text,
        events,
        count: filter => matching(filter).length,
        // The names of the boxes of the matching events, each once, in the order they first appear.
        boxes: filter => [...new Set(matching(filter).map(event => event.box))],
        // The passes the work ran, in order: "FULL", or "PARTIAL" and the box it laid out.
        passes: () => matching({ event: "layout" }).map(event => (event.partial ? `PARTIAL ${event.box}` : "FULL")),
    };
}

// Parses a style trace into one event per line:
//   pass      `kind` ("FULL", "PARTIAL", "COLD", "ANIMATION" for the animation samples the host composed, or
//             "PROGRAM" for what the engine did to the rules it styles from)
//   program   `name` of what the engine did to its rules, `times` it did so in a row
//   restyle   `element`, `outcome` ("computed", "shared", "reused", "declined", "kept" for an element that only took
//             the custom properties its parent hands down now, "sampled" or "sampled every group" in an ANIMATION
//             block), `changed`
// With `stylePassesOnly`, the PROGRAM and ANIMATION blocks are left out.
function parseStyleTrace(text, { stylePassesOnly = false } = {}) {
    const events = [];
    for (const line of text.replace(/\r/g, "").split("\n")) {
        if (!line.trim()) continue;
        let match = line.match(/^style (FULL|PARTIAL|COLD|ANIMATION|PROGRAM)$/);
        if (match) {
            events.push({ event: "pass", kind: match[1] });
            continue;
        }
        match = line.match(
            /^  ((?:rule index|user agent rule index|selectors|style rules|sheet declarations) [a-z ]+?)(?: x(\d+))?$/
        );
        if ((match && events.at(-1)?.kind === "PROGRAM") || (match && events.at(-1)?.event === "program")) {
            events.push({ event: "program", name: match[1], times: match[2] ? Number(match[2]) : 1 });
            continue;
        }
        match = line.match(/^  (\S+)(?: \(([^)]*)\))?$/);
        if (!match) throw new Error(`unrecognized style trace line: ${JSON.stringify(line)}`);
        const notes = match[2] ? match[2].split(", ") : [];
        events.push({
            event: "restyle",
            element: match[1],
            outcome: notes.find(note => note !== "changed"),
            changed: notes.includes("changed"),
        });
    }
    if (!stylePassesOnly) return events;
    let inStylePass = false;
    return events.filter(event => {
        if (event.event === "pass") inStylePass = ["FULL", "PARTIAL", "COLD"].includes(event.kind);
        return inStylePass;
    });
}

// The id of a traced element, or its whole name where it has none.
function shortStyleName(element) {
    return element.match(/#([^.@]+)/)?.[1] ?? element;
}

// Settles style, then runs `mutate` and settles style again, tracing only the passes the mutation causes. Answers the
// trace and its events, with queries over them.
function styleWork(mutate, testInternals = internals, options = {}) {
    const text = traceWork("style", mutate, testInternals, options);
    const events = parseStyleTrace(text);
    const matching = filter => events.filter(event => workEventMatches(event, filter));
    const elements = filter => [...new Set(matching(filter).map(event => event.element))];
    return {
        text,
        events,
        count: filter => matching(filter).length,
        // The kinds of the style passes, in order, leaving out the PROGRAM and ANIMATION blocks.
        passes: () =>
            matching(event => event.event === "pass" && ["FULL", "PARTIAL", "COLD"].includes(event.kind)).map(
                event => event.kind
            ),
        // What the engine did to the rules it styles from, in order, each with how many times it did so in a row.
        program: () =>
            matching({ event: "program" }).map(event =>
                event.times > 1 ? `${event.name} x${event.times}` : event.name
            ),
        // The elements whose animations the host composed over their records, each once.
        sampled: () => elements(event => event.event === "restyle" && event.outcome?.startsWith("sampled")),
        // The elements the passes styled, each once, in the order they first appear.
        restyled: () =>
            elements(
                event =>
                    event.event === "restyle" && !["kept", "sampled", "sampled every group"].includes(event.outcome)
            ),
        // The elements whose records the passes changed, each once.
        changed: () => elements({ event: "restyle", changed: true }),
    };
}

// Like layoutWork(), with the layout tree builds traced too. Answers, for each node of `tracked`, how its box fared:
// "PRESERVED", "RECREATED", "CREATED", "REMOVED" or "ABSENT". A box counts as recreated where a build created a box
// named for its element, so tracked elements need ids of their own.
function layoutTreeBuildWork(mutate, { tracked = [] } = {}, testInternals = internals) {
    const hasBox = node => testInternals.dumpLayoutTree(node) !== "(no layout node)";
    testInternals.updateLayoutForTesting();
    const hadBox = tracked.map(hasBox);
    testInternals.beginLayoutTrace({ treeBuilds: true });
    let text;
    try {
        if (mutate()?.then) throw new Error("mutate must be synchronous");
        testInternals.updateLayoutForTesting();
    } finally {
        text = testInternals.takeLayoutTrace();
    }
    const events = parseLayoutTrace(text);
    const builds = events.filter(event => event.event === "build");
    const created = events.filter(event => event.event === "create").map(event => event.box);
    const namesElement = (box, element) =>
        element.id !== "" &&
        box.includes(`<${element.nodeName}>#${element.id}`) &&
        /^(\.|$)/.test(box.split(`<${element.nodeName}>#${element.id}`)[1]);
    return {
        text,
        events,
        builds: () => builds.length,
        rebuiltWholeTree: () => builds.some(build => build.wholeTree),
        escaped: () => builds.some(build => build.escaped),
        rebuiltRoots: () => events.filter(event => event.event === "rebuild").map(event => event.box),
        transitions: () =>
            tracked.map((node, index) => {
                const has = hasBox(node);
                if (!hadBox[index] && !has) return "ABSENT";
                if (!hadBox[index]) return "CREATED";
                if (!has) return "REMOVED";
                if (builds.some(build => build.wholeTree) || created.some(box => namesElement(box, node)))
                    return "RECREATED";
                return "PRESERVED";
            }),
    };
}

// Parses a visual context trace into one event per line:
//   update     `scope` the update ran ("DIRTY-PATH", "EVERY-BOX", "FRESH-TREE"), and `asked` where it widened
//   node       `box`, `role` ("transform", "clip", "effects", ...), `action` ("ALLOCATED", "REPURPOSED", "UPDATED",
//              "RELEASED"), `freedSlot`, `removedBox`
//   epoch, recording
function parseVisualContextTrace(text) {
    const events = [];
    for (const line of text.replace(/\r/g, "").split("\n")) {
        if (!line.trim()) continue;
        let match = line.match(/^update (DIRTY-PATH|EVERY-BOX|FRESH-TREE)(?: -> (DIRTY-PATH|EVERY-BOX|FRESH-TREE))?$/);
        if (match) {
            events.push(
                match[2] ? { event: "update", asked: match[1], scope: match[2] } : { event: "update", scope: match[1] }
            );
            continue;
        }
        if (line === "  epoch NEW") {
            events.push({ event: "epoch" });
            continue;
        }
        if (line === "  recording NEEDED") {
            events.push({ event: "recording" });
            continue;
        }
        match = line.match(
            /^  (\S+) (.+?) (ALLOCATED|REPURPOSED|UPDATED|RELEASED)( \(freed slot\))?( \(removed box\))?$/
        );
        if (!match) throw new Error(`unrecognized visual context trace line: ${JSON.stringify(line)}`);
        events.push({
            event: "node",
            box: match[1],
            role: match[2],
            action: match[3],
            freedSlot: !!match[4],
            removedBox: !!match[5],
        });
    }
    return events;
}

// Brings the visual context tree up to date, then runs `mutate` and does so again, tracing only the updates the
// mutation causes. Answers the trace and its events, with queries over them.
function visualContextWork(mutate, testInternals = internals, options = {}) {
    const text = traceWork("visualContexts", mutate, testInternals, options);
    const events = parseVisualContextTrace(text);
    const matching = filter => events.filter(event => workEventMatches(event, filter));
    return {
        text,
        events,
        count: filter => matching(filter).length,
        // The scopes of the updates, in order.
        updates: () => matching({ event: "update" }).map(event => event.scope),
        // The boxes of the matching node events, each once, in the order they first appear.
        boxes: filter => [...new Set(matching({ event: "node", ...filter }).map(event => event.box))],
    };
}
