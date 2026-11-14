// Helpers that summarize layout subtrees and compare the incrementally updated layout tree with a full rebuild.

function repeatMarkup(count, makeMarkup) {
    let markup = "";
    for (let i = 0; i < count; ++i) markup += makeMarkup(i);
    return markup;
}

// Summarizes the layout subtree of a node as box counts per kind plus the text of all line box fragments in tree
// order, which shows generated text such as counter values and quotes.
function summarizeLayoutSubtree(node) {
    const kinds = new Map();
    const texts = [];
    let boxes = 0;
    for (const line of internals.dumpLayoutTree(node).split("\n")) {
        const trimmed = line.trim();
        if (trimmed.startsWith('"')) {
            texts.push(trimmed.slice(1, -1));
            continue;
        }
        const match = trimmed.match(/^(\w+) <([^>]*)>/);
        if (!match) continue;
        const kind = match[2] === "(anonymous)" ? `${match[1]}(anonymous)` : match[1];
        kinds.set(kind, (kinds.get(kind) ?? 0) + 1);
        ++boxes;
    }
    const kindList = [...kinds.entries()]
        .sort(([a], [b]) => (a < b ? -1 : a > b ? 1 : 0))
        .map(([kind, count]) => `${kind}=${count}`)
        .join(" ");
    return { boxes, kinds: kindList, text: texts.join(" ") };
}

// The text of all line box fragments below the box of each child of `parent`, in tree order, from one dump of the
// subtree of `parent`, whose box has one box per child of `parent` as its children, besides anonymous ones such as
// those of its pseudo-elements, whose text is left out.
function childLayoutTexts(parent) {
    const texts = [];
    let inChild = false;
    for (const line of internals.dumpLayoutTree(parent).split("\n")) {
        const trimmed = line.trim();
        if (/^  \w+ </.test(line)) {
            inChild = !line.includes("<(anonymous)>");
            if (inChild) texts.push([]);
        } else if (inChild && trimmed.startsWith('"')) texts[texts.length - 1].push(trimmed.slice(1, -1));
    }
    if (texts.length !== parent.children.length)
        throw new Error(`${texts.length} child boxes for ${parent.children.length} children`);
    return texts.map(childTexts => childTexts.join(" "));
}

function printLayoutSummaries(nodes) {
    let total = 0;
    for (const node of nodes) {
        const summary = summarizeLayoutSubtree(node);
        total += summary.boxes;
        println(`${node.id || node.nodeName}: ${summary.boxes} boxes: ${summary.kinds}`);
        if (summary.text) println(`  text: ${summary.text}`);
    }
    println(`total: ${total} boxes`);
}

function printFullRebuildComparison(name) {
    document.body.offsetHeight;
    const comparison = internals.compareLayoutTreeWithFullRebuild();
    println(`${comparison.matches ? "PASS" : "FAIL"}: ${name}`);
}

// Applies each mutation in turn to the same document and compares the incrementally updated tree with a full
// rebuild after each one.
function runSequentialFullRebuildComparisons(mutations) {
    printFullRebuildComparison("first build");
    for (const [name, mutate] of mutations) {
        mutate();
        printFullRebuildComparison(name);
    }
}
