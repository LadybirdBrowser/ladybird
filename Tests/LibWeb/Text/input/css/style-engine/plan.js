// Shared harness for the StyleEngine mutation-surface tests.
//
// Every way the engine's inputs can move should be provable twice over: that the mutation reaches
// the engine at all, and that its reaction batch is no wider than it has to be. The plan names
// exactly the elements whose semantic output changed before any consumer materializes the result.

// Settle everything before the mutation under test, so the batch describes that mutation and no
// other. Forcing layout is not enough: layout that is already clean does not flush style, and
// whether it happens to be clean depends on what else the page has been doing. Asking for the
// reactions afterwards takes the transaction the settle left behind, so the next one starts empty.
function planSetup() {
    internals.updateStyle();
    internals.styleEngineTransactionReactions();
}

// Run `mutate`, then report the elements whose semantic output changed.
function plan(name, mutate) {
    planSetup();
    mutate();
    const result = internals.styleEngineTransactionReactions();
    if (result.wholeDocument) {
        println(`${name}: WHOLE DOCUMENT`);
        return;
    }
    println(`${name}: [${result.elements.join(", ")}]`);
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
