// Exact work expectations for incremental display list recordings. Output is buffered: println()
// changes the document and would otherwise invalidate the next step's caches.

function normalizeDisplayListTrace(text) {
    const lines = text.replace(/\r/g, "").split("\n");
    while (lines.length && !lines[0].trim()) lines.shift();
    while (lines.length && !lines.at(-1).trim()) lines.pop();
    const indent = Math.min(...lines.filter(line => line.trim()).map(line => line.match(/^ */)[0].length));
    return lines.map(line => line.slice(indent).trimEnd()).join("\n");
}

function displayListTraceDifference(expected, actual) {
    const left = normalizeDisplayListTrace(expected).split("\n");
    const right = normalizeDisplayListTrace(actual).split("\n");
    const index = left.findIndex((line, index) => line !== right[index]);
    if (index < 0 && left.length === right.length) return null;
    const line = index < 0 ? left.length : index;
    return `line ${line + 1}: expected ${JSON.stringify(left[line] ?? "<end>")}, got ${JSON.stringify(right[line] ?? "<end>")}`;
}

// Block fixtures can opt out of anonymous line containers created by source indentation.
// This is fixture preparation, before either the warm recording or the measured steps.
function removeBodyWhitespace() {
    for (const node of [...document.body.childNodes]) {
        if (node.nodeType === Node.TEXT_NODE && !node.textContent.trim()) node.remove();
    }
}

function displayListTest({ setup, steps, cleanup, cold = false }) {
    promiseTest(async () => {
        const output = [];
        const fail = message => output.push(`FAIL: ${message}`);
        try {
            // Loading resources is outside the measured interval. Steps themselves are synchronous,
            // so scheduled rendering cannot silently warm or replace the caches between them.
            if (document.readyState !== "complete") {
                await new Promise(resolve => window.addEventListener("load", resolve, { once: true }));
            }
            await setup?.();
            await document.fonts.ready;
            __outputElement.style.display = "none";
            if (!cold) internals.recordDisplayListForTesting(false, false);
            for (const step of steps) {
                let error = null;
                internals.beginDisplayListTrace();
                try {
                    if (step.mutate?.()?.then) throw new Error("mutate must be synchronous");
                    internals.recordDisplayListForTesting(step.paintOverlay ?? false, step.cold ?? false);
                    const result = step.check?.((actual, expected, label) => {
                        if (!Object.is(actual, expected)) {
                            throw new Error(
                                `${label}: expected ${JSON.stringify(expected)}, got ${JSON.stringify(actual)}`
                            );
                        }
                    });
                    if (result?.then) throw new Error("check must be synchronous");
                } catch (caught) {
                    error = String(caught);
                }
                const actual = internals.takeDisplayListTrace();
                if (typeof step.expect !== "string") throw new Error(`${step.name}: missing exact work expectation`);
                const expected = `recording (overlay=${step.paintOverlay ?? false})\n${normalizeDisplayListTrace(step.expect)}`;
                const difference = displayListTraceDifference(expected, actual);
                if (difference || error) {
                    fail(`${step.name}${difference ? `: ${difference}` : ""}`);
                    if (error) output.push(error);
                    output.push(`Actual trace for ${JSON.stringify(step.name)}:\n${actual.trimEnd()}\nEnd trace`);
                } else {
                    output.push(`PASS: ${step.name} (work${step.check ? " + checks" : ""})`);
                }
            }
        } catch (error) {
            fail(String(error));
        } finally {
            internals.takeDisplayListTrace();
            try {
                await cleanup?.();
            } catch (error) {
                fail(`cleanup: ${error}`);
            }
            __outputElement.style.display = "";
            for (const line of output) println(line);
        }
    });
}
