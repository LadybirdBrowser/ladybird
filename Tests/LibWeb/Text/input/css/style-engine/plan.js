// Shared harness for the StyleEngine mutation-surface tests.
//
// Every way the engine's inputs can move should be provable twice over: that the mutation reaches
// the engine at all, and that the elements it changes are exactly the ones whose style it moves.
// The plan names the elements whose records the style pass after the mutation changed.

// Settle everything before the mutation under test, so the pass after it describes that mutation
// and no other. Forcing layout is not enough: layout that is already clean does not flush style,
// and whether it happens to be clean depends on what else the page has been doing. Resetting the
// counters forgets the last pass, so a mutation that restyles nothing reports nothing.
function planSetup() {
    internals.updateStyle();
    internals.resetStyleCounters();
}

// Run the style pass for the mutations since planSetup(), and report it as { fullPass, styled,
// changed }.
function planPass() {
    internals.updateStyle();
    return internals.styleLastPass();
}

// Run `mutate`, then report the elements whose records changed.
function plan(name, mutate) {
    planSetup();
    mutate();
    println(`${name}: [${planPass().changed.join(", ")}]`);
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
