// Shared harness for the StyleEngine mutation-surface tests.
//
// Every way the engine's inputs can move should be provable twice over: that the mutation reaches
// the engine at all, and that the elements it changes are exactly the ones whose style it moves.
// The plan names the elements whose records the style passes after the mutation changed. Tests load
// work-test.js before this file.

// Run `mutate`, then report the elements whose records the style passes after it changed, the elements they restyled
// without changing them, and whether one of the passes restyled every element. Everything before the mutation is
// settled first, so the passes describe that mutation and no other.
function plan(name, mutate) {
    const work = styleWork(mutate);
    const changed = work.changed();
    const unchanged = work.restyled().filter(element => !changed.includes(element));
    let line = `${name}: [${changed.map(shortStyleName).join(", ")}]`;
    if (unchanged.length) line += ` + unchanged [${unchanged.map(shortStyleName).join(", ")}]`;
    if (work.passes().includes("FULL")) line += " (full pass)";
    println(line);
}

function make(tagName, id, options = {}) {
    const element = document.createElement(tagName);
    element.id = id;
    if (options.className) element.className = options.className;
    for (const [name, value] of Object.entries(options.attributes || {})) element.setAttribute(name, value);
    return element;
}

function addStyleSheet(cssText) {
    const style = document.createElement("style");
    style.textContent = cssText;
    document.head.appendChild(style);
    return style;
}
