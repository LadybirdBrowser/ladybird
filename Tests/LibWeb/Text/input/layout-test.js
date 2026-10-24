// Layout work expectations: exact traces with layoutTest(), or queries over the traced events
// with layoutWork(). Keep measured steps synchronous and buffer output: writing test results
// into the document can invalidate later steps.

function normalizeLayoutTrace(text) {
    const lines = text.replace(/\r/g, "").split("\n");
    while (lines.length && !lines[0].trim()) lines.shift();
    while (lines.length && !lines.at(-1).trim()) lines.pop();
    const indent = Math.min(...lines.filter(line => line.trim()).map(line => line.match(/^ */)[0].length));
    return lines.map(line => line.slice(indent).trimEnd()).join("\n");
}

function layoutTraceDifference(expected, actual) {
    const left = normalizeLayoutTrace(expected).split("\n");
    const right = normalizeLayoutTrace(actual).split("\n");
    const index = left.findIndex((line, index) => line !== right[index]);
    if (index < 0 && left.length === right.length) return null;
    const line = index < 0 ? left.length : index;
    return `line ${line + 1}: expected ${JSON.stringify(left[line] ?? "<end>")}, got ${JSON.stringify(right[line] ?? "<end>")}`;
}

// The kinds of layout trace lines, each as a pattern and the event that its groups make.
const layoutTraceLines = (() => {
    const box = String.raw`(\S+)`;
    const cache = String.raw`\(cache=(\w+)\)`;
    const runFlags = String.raw`(?: \((measurement)?(?:, )?(intrinsic)?\))?`;
    return [
        ["layout FULL", () => ({ event: "layout", partial: false })],
        [`layout PARTIAL ${box}`, box => ({ event: "layout", partial: true, box })],
        [`${box} MEASURE CELL ${cache}`, (box, cache) => ({ event: "measure cell", box, cache })],
        [
            `${box} MEASURE ((?:min|max)-content (?:inline|block)) ${cache}(?: FROM (ITEMS|FLEX ITEMS|CHILDREN|COLUMNS|MAX-CONTENT))?`,
            (box, size, cache, from) => ({ event: "measure", box, size, cache, from: from?.toLowerCase() }),
        ],
        [`${box} INLINE SIZE FROM ITEMS`, box => ({ event: "inline size from items", box })],
        [`${box} REUSE IN-FLOW SUBTREE`, box => ({ event: "reuse in-flow subtree", box })],
        [
            `${box}/([a-z-]+)${runFlags} ([A-Z][A-Z -]*[A-Z])(?: ${cache})?`,
            (box, formattingContext, measurement, intrinsic, action, cache) => ({
                event: "run",
                box,
                formattingContext,
                measurement: !!measurement,
                intrinsic: !!intrinsic,
                action,
                cache,
            }),
        ],
    ].map(([pattern, makeEvent]) => [new RegExp(`^${pattern}$`), makeEvent]);
})();

// Parses a layout trace into one event per line. Each event has its depth, its kind in `event`, and the
// name of the box it is about in `box`, except for a full layout pass:
//   layout                  `partial`
//   run                     `formattingContext`, `measurement`, `intrinsic`, `action`, `cache`
//   reuse in-flow subtree
//   measure                 `size` ("min-content inline", ...), `cache`, `from` ("items", "flex items", "children", "columns", "max-content")
//   measure cell            `cache`
//   inline size from items
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
function layoutEventMatches(event, filter) {
    if (typeof filter === "function") return filter(event);
    return Object.entries(filter).every(([key, value]) => event[key] === value);
}

// Lays out what is pending, then runs `mutate` and lays out again, tracing only the layout work the
// mutation causes. Output printed before the call is laid out first, so it is never counted. Answers
// the trace and its events, with queries over the events that match a filter (see layoutEventMatches).
function layoutWork(mutate, testInternals = internals) {
    testInternals.updateLayoutForTesting();
    testInternals.beginLayoutTrace();
    let text;
    try {
        if (mutate()?.then) throw new Error("mutate must be synchronous");
        testInternals.updateLayoutForTesting();
    } finally {
        text = testInternals.takeLayoutTrace();
    }
    const events = parseLayoutTrace(text);
    const matching = filter => events.filter(event => layoutEventMatches(event, filter));
    return {
        text,
        events,
        count: filter => matching(filter).length,
        // The names of the boxes of the matching events, each once, in the order they first appear.
        boxes: filter => [...new Set(matching(filter).map(event => event.box))],
    };
}

// Match the display-list helper's optional block-fixture preparation.
function removeBodyWhitespace() {
    for (const node of [...document.body.childNodes]) {
        if (node.nodeType === Node.TEXT_NODE && !node.textContent.trim()) node.remove();
    }
}

// Also usable by scenario matrices that create and warm a fixture for each case.
function runLayoutTestStep(step, testInternals = internals) {
    if (typeof step.expect !== "string") throw new Error(`${step.name}: missing exact layout work expectation`);
    let error;
    let actual;
    testInternals.beginLayoutTrace();
    try {
        if (step.mutate?.()?.then) throw new Error("mutate must be synchronous");
        testInternals.updateLayoutForTesting();
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
        actual = testInternals.takeLayoutTrace();
    }
    const difference = layoutTraceDifference(step.expect, actual);
    if (difference || error) {
        throw new Error(
            `${step.name}: ${[difference, error].filter(Boolean).join("\n")}\nActual layout trace:\n${actual.trimEnd()}\nEnd trace`
        );
    }
}

function layoutTest({ setup, steps, cleanup }) {
    promiseTest(async () => {
        const output = [];
        const originalDisplay = __outputElement.style.display;
        try {
            if (document.readyState !== "complete") {
                await new Promise(resolve => window.addEventListener("load", resolve, { once: true }));
            }
            __outputElement.style.display = "none";
            await setup?.();
            await document.fonts.ready;
            internals.updateLayoutForTesting();
            for (const step of steps) {
                try {
                    runLayoutTestStep(step);
                    output.push(`PASS: ${step.name} (work${step.check ? " + checks" : ""})`);
                } catch (error) {
                    output.push(`FAIL: ${error}`);
                }
            }
        } catch (error) {
            output.push(`FAIL: ${error}`);
        } finally {
            internals.takeLayoutTrace();
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
