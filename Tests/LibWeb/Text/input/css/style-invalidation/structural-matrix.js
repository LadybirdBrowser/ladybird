const BASE = "base";
const MATCH = "match";
const PROBE_PROPERTY = "--style-invalidation-probe";
const MATCH_DECLARATION = `${PROBE_PROPERTY}: ${MATCH}; content: "";`;

function assertEqual(testName, actual, expected) {
    if (actual !== expected) throw new Error(`${testName}: expected ${expected}, got ${actual}`);
}

function resetStyleCounters() {
    internals.resetStyleInvalidationCounters();
}

function styleCounterSummary() {
    const counters = internals.getStyleInvalidationCounters();
    return (
        `styleInvalidations=${counters.styleInvalidations}, fullStyleInvalidations=${counters.fullStyleInvalidations}, ` +
        `elementStyleRecomputations=${counters.elementStyleRecomputations}, ` +
        `elementStyleNoopRecomputations=${counters.elementStyleNoopRecomputations}, ` +
        `elementInheritedStyleRecomputations=${counters.elementInheritedStyleRecomputations}, ` +
        `elementInheritedStyleNoopRecomputations=${counters.elementInheritedStyleNoopRecomputations}, ` +
        `hasAncestorWalkInvocations=${counters.hasAncestorWalkInvocations}, ` +
        `hasInvalidationMetadataCandidates=${counters.hasInvalidationMetadataCandidates}, ` +
        `hasMatchInvocations=${counters.hasMatchInvocations}, ` +
        `hasResultCacheHits=${counters.hasResultCacheHits}, hasResultCacheMisses=${counters.hasResultCacheMisses}`
    );
}

function styleCounters() {
    return internals.getStyleInvalidationCounters();
}

function printPassWithCounters(testName) {
    println(`PASS: ${testName} | ${styleCounterSummary()}`);
}

function hasWalkCounterDetailSummary() {
    const counters = styleCounters();
    return (
        `hasAncestorWalkVisits=${counters.hasAncestorWalkVisits}, ` +
        `hasAncestorSiblingElementChecks=${counters.hasAncestorSiblingElementChecks}`
    );
}

function printPassWithHasWalkCounters(testName) {
    println(`PASS: ${testName} | ${styleCounterSummary()}, ${hasWalkCounterDetailSummary()}`);
}

function makeElement(tagName, options = {}) {
    const element = document.createElement(tagName);
    if (options.id) element.id = options.id;
    if (options.className) element.className = options.className;
    if (options.text !== undefined) element.textContent = options.text;
    if (options.attributes) {
        for (const [name, value] of Object.entries(options.attributes)) element.setAttribute(name, value);
    }
    return element;
}

function makeSvgElement(tagName, options = {}) {
    const element = document.createElementNS("http://www.w3.org/2000/svg", tagName);
    if (options.id) element.id = options.id;
    if (options.className) element.setAttribute("class", options.className);
    if (options.attributes) {
        for (const [name, value] of Object.entries(options.attributes)) element.setAttribute(name, value);
    }
    return element;
}

function addStyle(root, cssText) {
    const style = document.createElement("style");
    style.textContent = cssText;
    root.appendChild(style);
    return style;
}

function addProbeStyle(root, selectorText, duplicateRuleCount = 1) {
    const rules = [];
    for (let i = 0; i < duplicateRuleCount; ++i) rules.push(`${selectorText} { ${MATCH_DECLARATION} }`);
    return addStyle(root, rules.join("\n"));
}

function makeSubject(options = {}) {
    const tagName = options.svgTagName || options.tagName || (options.shadowHost ? "x-subject" : "div");
    const subject = options.svgTagName
        ? makeSvgElement(tagName, { className: "subject", attributes: options.attributes })
        : makeElement(tagName, { className: "subject", attributes: options.attributes });
    if (options.shadowHost) subject.attachShadow({ mode: "open" }).appendChild(document.createElement("slot"));
    return subject;
}

function appendShadowPartHost(fixture, options = {}) {
    const host = makeElement("x-host");
    fixture.appendChild(host);
    const shadowRoot = host.attachShadow({ mode: "open" });
    const subject = makeSubject({
        ...options.subjectOptions,
        shadowHost: options.subjectIsShadowHost,
        attributes: { ...(options.subjectOptions?.attributes || {}), part: "foo" },
    });
    shadowRoot.appendChild(subject);
    return { host, shadowRoot, subject };
}

function appendSvgUseDocumentStyledSubject(fixture) {
    const sourceId = `source-${appendSvgUseDocumentStyledSubject.nextId++}`;
    const useId = `use-${appendSvgUseDocumentStyledSubject.nextId++}`;
    fixture.innerHTML = `<svg><defs><g id="${sourceId}" class="subject"></g></defs><use id="${useId}" href="#${sourceId}"></use></svg>`;

    document.body.offsetWidth;
    const use = document.getElementById(useId);
    return internals.getShadowRoot(use).firstChild;
}
appendSvgUseDocumentStyledSubject.nextId = 0;

function buildFixture(scope, selector, pseudoElement, options = {}) {
    const fixture = makeElement("section");
    document.body.appendChild(fixture);
    const duplicateRuleCount = options.duplicateRuleCount ?? 1;

    let subject = makeSubject(options.subjectOptions);
    let target = subject;
    let ruleStyle = null;
    let cleanup = () => {};

    if (scope === "document") {
        fixture.appendChild(subject);
        ruleStyle = addProbeStyle(document.head, `.subject${selector}${pseudoElement}`, duplicateRuleCount);
    } else if (scope === "shadow-internal") {
        const host = makeElement("div");
        fixture.appendChild(host);
        const shadowRoot = host.attachShadow({ mode: "open" });
        shadowRoot.appendChild(subject);
        addProbeStyle(shadowRoot, `.subject${selector}${pseudoElement}`, duplicateRuleCount);
    } else if (scope === "shadow-host") {
        target = makeElement("div");
        fixture.appendChild(target);
        const shadowRoot = target.attachShadow({ mode: "open" });
        shadowRoot.appendChild(document.createElement("slot"));
        target.className = "subject";
        subject = target;
        addProbeStyle(shadowRoot, `:host(.subject${selector})${pseudoElement}`, duplicateRuleCount);
    } else if (scope === "slotted-default") {
        const host = makeElement("div");
        fixture.appendChild(host);
        host.appendChild(subject);
        const shadowRoot = host.attachShadow({ mode: "open" });
        shadowRoot.appendChild(document.createElement("slot"));
        addProbeStyle(shadowRoot, `::slotted(.subject${selector})`, duplicateRuleCount);
    } else if (scope === "slotted-named") {
        const host = makeElement("div");
        fixture.appendChild(host);
        subject.slot = "named";
        host.appendChild(subject);
        const shadowRoot = host.attachShadow({ mode: "open" });
        const slot = makeElement("slot", { attributes: { name: "named" } });
        shadowRoot.appendChild(slot);
        addProbeStyle(shadowRoot, `::slotted(.subject${selector})`, duplicateRuleCount);
    } else if (scope === "slotted-shadow-host-default") {
        subject = makeSubject({ shadowHost: true });
        target = subject;
        const host = makeElement("div");
        fixture.appendChild(host);
        host.appendChild(subject);
        const shadowRoot = host.attachShadow({ mode: "open" });
        shadowRoot.appendChild(document.createElement("slot"));
        addProbeStyle(shadowRoot, `::slotted(.subject${selector})`, duplicateRuleCount);
    } else if (scope === "slotted-shadow-host-named") {
        subject = makeSubject({ shadowHost: true });
        target = subject;
        const host = makeElement("div");
        fixture.appendChild(host);
        subject.slot = "named";
        host.appendChild(subject);
        const shadowRoot = host.attachShadow({ mode: "open" });
        const slot = makeElement("slot", { attributes: { name: "named" } });
        shadowRoot.appendChild(slot);
        addProbeStyle(shadowRoot, `::slotted(.subject${selector})`, duplicateRuleCount);
    } else if (scope === "part-document") {
        const partFixture = appendShadowPartHost(fixture, options);
        subject = partFixture.subject;
        target = subject;
        ruleStyle = addProbeStyle(document.head, `x-host::part(foo)${selector}${pseudoElement}`, duplicateRuleCount);
    } else if (scope === "part-shadow-host-document") {
        const partFixture = appendShadowPartHost(fixture, { subjectIsShadowHost: true });
        subject = partFixture.subject;
        target = subject;
        ruleStyle = addProbeStyle(document.head, `x-host::part(foo)${selector}${pseudoElement}`, duplicateRuleCount);
    } else if (scope === "part-shadow-internal") {
        const partFixture = appendShadowPartHost(fixture, options);
        subject = partFixture.subject;
        target = subject;
        addProbeStyle(partFixture.shadowRoot, `:host::part(foo)${selector}${pseudoElement}`, duplicateRuleCount);
    } else if (scope === "part-shadow-host-internal") {
        const partFixture = appendShadowPartHost(fixture, { subjectIsShadowHost: true });
        subject = partFixture.subject;
        target = subject;
        addProbeStyle(partFixture.shadowRoot, `:host::part(foo)${selector}${pseudoElement}`, duplicateRuleCount);
    } else if (scope === "part-shadow-ancestor") {
        const outerHost = makeElement("x-outer-host");
        fixture.appendChild(outerHost);
        const outerShadowRoot = outerHost.attachShadow({ mode: "open" });
        const innerHost = makeElement("x-host");
        outerShadowRoot.appendChild(innerHost);
        const innerShadowRoot = innerHost.attachShadow({ mode: "open" });
        subject = makeElement("div", { className: "subject", attributes: { part: "foo" } });
        target = subject;
        innerShadowRoot.appendChild(subject);
        addProbeStyle(outerShadowRoot, `x-host::part(foo)${selector}${pseudoElement}`, duplicateRuleCount);
    } else if (scope === "part-shadow-host-ancestor") {
        const outerHost = makeElement("x-outer-host");
        fixture.appendChild(outerHost);
        const outerShadowRoot = outerHost.attachShadow({ mode: "open" });
        const innerHost = makeElement("x-host");
        outerShadowRoot.appendChild(innerHost);
        const innerShadowRoot = innerHost.attachShadow({ mode: "open" });
        subject = makeSubject({ shadowHost: true, attributes: { part: "foo" } });
        target = subject;
        innerShadowRoot.appendChild(subject);
        addProbeStyle(outerShadowRoot, `x-host::part(foo)${selector}${pseudoElement}`, duplicateRuleCount);
    } else if (scope === "document-styled-use-shadow") {
        ruleStyle = addProbeStyle(document.head, `.subject${selector}${pseudoElement}`, duplicateRuleCount);
        subject = appendSvgUseDocumentStyledSubject(fixture);
        target = subject;
    } else {
        throw new Error(`Unknown scope ${scope}`);
    }

    cleanup = () => {
        ruleStyle?.remove();
        fixture.remove();
    };
    return { cleanup, fixture, subject, target };
}

function readProbe(target, pseudoElement) {
    return (
        getComputedStyle(target, pseudoElement || null)
            .getPropertyValue(PROBE_PROPERTY)
            .trim() || BASE
    );
}

function isSlottedScope(scope) {
    return scope.startsWith("slotted-");
}

function caseSupportedInScope(scope, selectorCase, pseudoElement = "") {
    // Bare ::slotted() rules style the assigned element itself, not a pseudo-element of the assigned element.
    // These cases can be enabled once ::slotted(...)::before selector chaining is supported.
    if (pseudoElement && isSlottedScope(scope)) return false;

    if (selectorCase.requiresSvgSubject) {
        if (pseudoElement) return false;
        return ![
            "document-styled-use-shadow",
            "shadow-host",
            "slotted-shadow-host-default",
            "slotted-shadow-host-named",
            "part-shadow-host-document",
            "part-shadow-host-internal",
            "part-shadow-ancestor",
            "part-shadow-host-ancestor",
        ].includes(scope);
    }
    return true;
}

function runCase({
    suite,
    scope,
    selector,
    name,
    setup,
    mutate,
    initial,
    after,
    pseudoElement = "",
    skipInitialRead = false,
    fixtureOptions = {},
}) {
    if (
        !caseSupportedInScope(
            scope,
            { requiresSvgSubject: fixtureOptions.subjectOptions?.svgTagName !== undefined },
            pseudoElement
        )
    )
        return;

    const testName = `${suite}: ${scope}: ${name}`;
    const { cleanup, fixture, subject, target } = buildFixture(scope, selector, pseudoElement, fixtureOptions);
    try {
        setup?.(subject, fixture);
        if (!skipInitialRead)
            assertEqual(`${testName} initial`, readProbe(target, pseudoElement), initial ? MATCH : BASE);
        resetStyleCounters();
        mutate(subject, fixture);
        assertEqual(`${testName} after mutation`, readProbe(target, pseudoElement), after ? MATCH : BASE);
        printPassWithCounters(testName);
    } finally {
        cleanup();
    }
}

function appendScopedDuplicateRuleFixture(scope, ruleText) {
    const fixture = makeElement("section");
    document.body.appendChild(fixture);

    if (scope === "document") {
        const style = addStyle(document.head, ruleText);
        return {
            fixture,
            cleanup: () => {
                style.remove();
                fixture.remove();
            },
        };
    }

    if (scope === "shadow-internal") {
        const host = makeElement("div");
        fixture.appendChild(host);
        const shadowRoot = host.attachShadow({ mode: "open" });
        addStyle(shadowRoot, ruleText);
        return { fixture: shadowRoot, cleanup: () => fixture.remove() };
    }

    throw new Error(`Unsupported duplicate rule scope ${scope}`);
}

function runDuplicateDescendantInvalidationRuleCase(scope) {
    const ruleText = `
        .active .target { ${MATCH_DECLARATION} }
        .active .target { ${MATCH_DECLARATION} }
    `;
    const scoped = appendScopedDuplicateRuleFixture(scope, ruleText);
    const ancestor = makeElement("div");
    const target = makeElement("div", { className: "target" });
    ancestor.appendChild(target);
    scoped.fixture.appendChild(ancestor);

    try {
        assertEqual(`duplicate descendant invalidation rule ${scope} initial`, readProbe(target), BASE);
        resetStyleCounters();
        ancestor.classList.add("active");
        assertEqual(`duplicate descendant invalidation rule ${scope} after mutation`, readProbe(target), MATCH);
        printPassWithCounters(`duplicate descendant invalidation rules merge: ${scope}`);
    } finally {
        scoped.cleanup();
    }
}

function runDuplicateSiblingInvalidationRuleCase(scope) {
    const ruleText = `
        .trigger.active + .target { ${MATCH_DECLARATION} }
        .trigger.active + .target { ${MATCH_DECLARATION} }
    `;
    const scoped = appendScopedDuplicateRuleFixture(scope, ruleText);
    const trigger = makeElement("div", { className: "trigger" });
    const target = makeElement("div", { className: "target" });
    scoped.fixture.append(trigger, target);

    try {
        assertEqual(`duplicate sibling invalidation rule ${scope} initial`, readProbe(target), BASE);
        resetStyleCounters();
        trigger.classList.add("active");
        assertEqual(`duplicate sibling invalidation rule ${scope} after mutation`, readProbe(target), MATCH);
        printPassWithCounters(`duplicate sibling invalidation rules merge: ${scope}`);
    } finally {
        scoped.cleanup();
    }
}

function runBatchedHasMutationCase({
    name,
    selector,
    setup,
    unrelatedMutation,
    relatedMutation,
    extraRuleText = "",
    expectedAfter = true,
}) {
    const style = addStyle(
        document.head,
        `
        :root:has(.flag) .subgrid:has(${selector}) { ${MATCH_DECLARATION} }
        ${extraRuleText}
    `
    );
    const fixture = makeElement("section");
    const flag = makeElement("div", { className: "flag" });
    const subgrid = makeElement("div", { className: "subgrid" });
    fixture.append(flag, subgrid);
    document.body.appendChild(fixture);

    const parts = { fixture, subgrid, ...setup(subgrid, fixture) };
    assertEqual(`${name} initial`, readProbe(subgrid), BASE);
    document.body.offsetWidth;

    resetStyleCounters();
    unrelatedMutation(parts);
    relatedMutation(parts);
    assertEqual(`${name} after batched mutations`, readProbe(subgrid), expectedAfter ? MATCH : BASE);
    printPassWithCounters(`batched :has mutation invalidates after earlier unrelated mutation: ${name}`);

    style.remove();
    fixture.remove();
}

function appendHitDescendant(subject) {
    const outer = makeElement("span", { className: "outer" });
    const middle = makeElement("span", { className: "middle" });
    middle.appendChild(makeElement("span", { className: "hit" }));
    outer.appendChild(middle);
    subject.appendChild(outer);
}

function appendSvgHit(subject) {
    subject.appendChild(makeSvgElement("rect", { className: "hit" }));
}

function appendSiblingPair(subject, firstClass, secondClass) {
    subject.appendChild(makeElement("span", { className: firstClass }));
    subject.appendChild(makeElement("span", { className: secondClass }));
}

const childMutationCases = [
    {
        name: ":empty becomes false after element append",
        selector: ":empty",
        initial: true,
        after: false,
        mutate: subject => subject.appendChild(makeElement("span")),
    },
    {
        name: ":empty becomes false after textContent",
        selector: ":empty",
        initial: true,
        after: false,
        mutate: subject => (subject.textContent = "text"),
    },
    {
        name: "mixed-case SVG type selector with :empty becomes false after element append",
        selector: ":is(linearGradient.subject):empty",
        initial: true,
        after: false,
        requiresSvgSubject: true,
        fixtureOptions: { subjectOptions: { svgTagName: "linearGradient" } },
        mutate: subject => subject.appendChild(makeSvgElement("stop")),
    },
    {
        name: "mixed-case SVG type selector with :not(:empty) becomes true after element append",
        selector: ":is(linearGradient.subject):not(:empty)",
        initial: false,
        after: true,
        requiresSvgSubject: true,
        fixtureOptions: { subjectOptions: { svgTagName: "linearGradient" } },
        mutate: subject => subject.appendChild(makeSvgElement("stop")),
    },
    {
        name: "mixed-case SVG foreignObject type selector with :empty becomes false after element append",
        selector: ":is(foreignObject.subject):empty",
        initial: true,
        after: false,
        requiresSvgSubject: true,
        fixtureOptions: { subjectOptions: { svgTagName: "foreignObject" } },
        mutate: subject => subject.appendChild(makeSvgElement("g")),
    },
    {
        name: ":has(.hit) after direct child append",
        selector: ":has(.hit)",
        initial: false,
        after: true,
        mutate: subject => subject.appendChild(makeElement("span", { className: "hit" })),
    },
    {
        name: ":has(.hit) after DocumentFragment append",
        selector: ":has(.hit)",
        initial: false,
        after: true,
        mutate: subject => {
            const fragment = document.createDocumentFragment();
            fragment.appendChild(makeElement("span", { className: "hit" }));
            subject.appendChild(fragment);
        },
    },
    {
        name: ":has(.hit) after existing child move into parent",
        selector: ":has(.hit)",
        initial: false,
        after: true,
        mutate: (subject, fixture) => {
            const oldParent = makeElement("div");
            const child = makeElement("span", { className: "hit" });
            oldParent.appendChild(child);
            fixture.appendChild(oldParent);
            subject.appendChild(child);
        },
    },
    {
        name: ":has(.hit) after nested child append",
        selector: ":has(.hit)",
        initial: false,
        after: true,
        mutate: subject => {
            const wrapper = makeElement("span");
            wrapper.appendChild(makeElement("span", { className: "hit" }));
            subject.appendChild(wrapper);
        },
    },
    {
        name: ":has(> .hit) after direct child append",
        selector: ":has(> .hit)",
        initial: false,
        after: true,
        mutate: subject => subject.appendChild(makeElement("span", { className: "hit" })),
    },
    {
        name: ":has(> .hit:first-child) after prepend",
        selector: ":has(> .hit:first-child)",
        initial: false,
        after: true,
        setup: subject => subject.appendChild(makeElement("span")),
        mutate: subject => subject.prepend(makeElement("span", { className: "hit" })),
    },
    {
        name: ":has(> .hit:first-child) after insertBefore",
        selector: ":has(> .hit:first-child)",
        initial: false,
        after: true,
        setup: subject => subject.appendChild(makeElement("span")),
        mutate: subject => subject.insertBefore(makeElement("span", { className: "hit" }), subject.firstChild),
    },
    {
        name: ":has(> .hit) ignores nested child append",
        selector: ":has(> .hit)",
        initial: false,
        after: false,
        mutate: subject => {
            const wrapper = makeElement("span");
            wrapper.appendChild(makeElement("span", { className: "hit" }));
            subject.appendChild(wrapper);
        },
    },
    {
        name: ":has(span.hit) after typed child append",
        selector: ":has(span.hit)",
        initial: false,
        after: true,
        mutate: subject => subject.appendChild(makeElement("span", { className: "hit" })),
    },
    {
        name: ":has([data-hit]) after attribute-bearing child append",
        selector: ":has([data-hit])",
        initial: false,
        after: true,
        mutate: subject => subject.appendChild(makeElement("span", { attributes: { "data-hit": "" } })),
    },
    {
        name: ":has(#hit-id) after identified child append",
        selector: ":has(#hit-id)",
        initial: false,
        after: true,
        mutate: subject => subject.appendChild(makeElement("span", { id: "hit-id" })),
    },
    {
        name: ":has(:is(.hit)) after direct child append",
        selector: ":has(:is(.hit))",
        initial: false,
        after: true,
        mutate: subject => subject.appendChild(makeElement("span", { className: "hit" })),
    },
    {
        name: ":has(:where(.hit)) after direct child append",
        selector: ":has(:where(.hit))",
        initial: false,
        after: true,
        mutate: subject => subject.appendChild(makeElement("span", { className: "hit" })),
    },
    {
        name: ":has(.hit:not(.miss)) after direct child append",
        selector: ":has(.hit:not(.miss))",
        initial: false,
        after: true,
        mutate: subject => subject.appendChild(makeElement("span", { className: "hit" })),
    },
    {
        name: ":has(.hit:first-child) after first child append",
        selector: ":has(.hit:first-child)",
        initial: false,
        after: true,
        mutate: subject => subject.appendChild(makeElement("span", { className: "hit" })),
    },
    {
        name: ":has(.hit:last-child) after last child append",
        selector: ":has(.hit:last-child)",
        initial: false,
        after: true,
        setup: subject => subject.appendChild(makeElement("span")),
        mutate: subject => subject.appendChild(makeElement("span", { className: "hit" })),
    },
    {
        name: ":has(.hit:nth-child(2)) after second child append",
        selector: ":has(.hit:nth-child(2))",
        initial: false,
        after: true,
        setup: subject => subject.appendChild(makeElement("span")),
        mutate: subject => subject.appendChild(makeElement("span", { className: "hit" })),
    },
    {
        name: ":has(.hit + .after) after adjacent sibling append",
        selector: ":has(.hit + .after)",
        initial: false,
        after: true,
        setup: subject => subject.appendChild(makeElement("span", { className: "hit" })),
        mutate: subject => subject.appendChild(makeElement("span", { className: "after" })),
    },
    {
        name: ":has(.hit ~ .after) after indirect sibling append",
        selector: ":has(.hit ~ .after)",
        initial: false,
        after: true,
        setup: subject => subject.appendChild(makeElement("span", { className: "hit" })),
        mutate: subject => {
            subject.appendChild(makeElement("span"));
            subject.appendChild(makeElement("span", { className: "after" }));
        },
    },
    {
        name: ":has(> .wrapper > .hit) after nested structure append",
        selector: ":has(> .wrapper > .hit)",
        initial: false,
        after: true,
        mutate: subject => {
            const wrapper = makeElement("span", { className: "wrapper" });
            wrapper.appendChild(makeElement("span", { className: "hit" }));
            subject.appendChild(wrapper);
        },
    },
    {
        name: ":not(:has(.hit)) becomes false after child append",
        selector: ":not(:has(.hit))",
        initial: true,
        after: false,
        mutate: subject => subject.appendChild(makeElement("span", { className: "hit" })),
    },
    {
        name: ":has(.hit, .fallback) after first-list child append",
        selector: ":has(.hit, .fallback)",
        initial: false,
        after: true,
        mutate: subject => subject.appendChild(makeElement("span", { className: "hit" })),
    },
    {
        name: ":has(:empty) after empty child append",
        selector: ":has(:empty)",
        initial: false,
        after: true,
        mutate: subject => subject.appendChild(makeElement("span")),
    },
    {
        name: ":has(.hit) after replaceChild",
        selector: ":has(.hit)",
        initial: false,
        after: true,
        setup: subject => subject.appendChild(makeElement("span")),
        mutate: subject => subject.replaceChild(makeElement("span", { className: "hit" }), subject.firstChild),
    },
    {
        name: ":has(.hit) after replaceChildren",
        selector: ":has(.hit)",
        initial: false,
        after: true,
        setup: subject => subject.appendChild(makeElement("span")),
        mutate: subject => subject.replaceChildren(makeElement("span", { className: "hit" })),
    },
    {
        name: ":has(.hit) becomes false after child removal",
        selector: ":has(.hit)",
        initial: true,
        after: false,
        setup: subject => subject.appendChild(makeElement("span", { className: "hit" })),
        mutate: subject => subject.firstChild.remove(),
    },
    {
        name: ":has(.hit) becomes false after removeChild",
        selector: ":has(.hit)",
        initial: true,
        after: false,
        setup: subject => subject.appendChild(makeElement("span", { className: "hit" })),
        mutate: subject => subject.removeChild(subject.firstChild),
    },
    {
        name: ":has(.hit) becomes false after child moves out",
        selector: ":has(.hit)",
        initial: true,
        after: false,
        setup: subject => subject.appendChild(makeElement("span", { className: "hit" })),
        mutate: (subject, fixture) => makeElement("div").appendChild(subject.firstChild),
    },
    {
        name: ":empty becomes true after replaceChildren",
        selector: ":empty",
        initial: false,
        after: true,
        setup: subject => subject.appendChild(makeElement("span")),
        mutate: subject => subject.replaceChildren(),
    },
    {
        name: ":empty becomes true after textContent clear",
        selector: ":empty",
        initial: false,
        after: true,
        setup: subject => (subject.textContent = "text"),
        mutate: subject => (subject.textContent = ""),
    },
    {
        name: ":empty stays true after comment append",
        selector: ":empty",
        initial: true,
        after: true,
        mutate: subject => subject.appendChild(document.createComment("ignored")),
    },
    {
        name: ":empty becomes false after whitespace text append",
        selector: ":empty",
        initial: true,
        after: false,
        mutate: subject => subject.appendChild(document.createTextNode(" ")),
    },
    {
        name: ":dir(rtl) after dir=auto text mutation",
        selector: ":dir(rtl)",
        initial: false,
        after: true,
        setup: subject => {
            subject.dir = "auto";
            subject.textContent = "a";
        },
        mutate: subject => (subject.firstChild.data = "א"),
    },
];

childMutationCases.push(
    {
        name: ":has(*) after child append",
        selector: ":has(*)",
        initial: false,
        after: true,
        mutate: subject => subject.appendChild(makeElement("span")),
    },
    {
        name: ":has(> *) after child append",
        selector: ":has(> *)",
        initial: false,
        after: true,
        mutate: subject => subject.appendChild(makeElement("span")),
    },
    {
        name: ":has(:checked) after checked checkbox append",
        selector: ":has(:checked)",
        initial: false,
        after: true,
        mutate: subject => {
            const input = makeElement("input", { attributes: { type: "checkbox" } });
            input.checked = true;
            subject.appendChild(input);
        },
    },
    {
        name: ":has(:checked) becomes false after checked checkbox removal",
        selector: ":has(:checked)",
        initial: true,
        after: false,
        setup: subject => {
            const input = makeElement("input", { attributes: { type: "checkbox" } });
            input.checked = true;
            subject.appendChild(input);
        },
        mutate: subject => subject.firstChild.remove(),
    },
    {
        name: ":has(:not(:checked)) after unchecked checkbox append",
        selector: ":has(:not(:checked))",
        initial: false,
        after: true,
        mutate: subject => subject.appendChild(makeElement("input", { attributes: { type: "checkbox" } })),
    },
    {
        name: ":has(:enabled) after enabled input append",
        selector: ":has(:enabled)",
        initial: false,
        after: true,
        mutate: subject => subject.appendChild(makeElement("input")),
    },
    {
        name: ":has(:disabled) after disabled input append",
        selector: ":has(:disabled)",
        initial: false,
        after: true,
        mutate: subject => {
            const input = makeElement("input");
            input.disabled = true;
            subject.appendChild(input);
        },
    },
    {
        name: ":has(:required) after required input append",
        selector: ":has(:required)",
        initial: false,
        after: true,
        mutate: subject => {
            const input = makeElement("input");
            input.required = true;
            subject.appendChild(input);
        },
    },
    {
        name: ":has(:optional) after optional input append",
        selector: ":has(:optional)",
        initial: false,
        after: true,
        mutate: subject => subject.appendChild(makeElement("input")),
    },
    {
        name: ":has(:empty) becomes false after text inserted into only child",
        selector: ":has(:empty)",
        initial: true,
        after: false,
        setup: subject => subject.appendChild(makeElement("span")),
        mutate: subject => subject.firstChild.appendChild(document.createTextNode("x")),
    },
    {
        name: ":has(:not(:empty)) after non-empty child append",
        selector: ":has(:not(:empty))",
        initial: false,
        after: true,
        mutate: subject => subject.appendChild(makeElement("span", { text: "x" })),
    },
    {
        name: ":has(:dir(rtl)) after dir=auto child append",
        selector: ":has(:dir(rtl))",
        initial: false,
        after: true,
        mutate: subject => subject.appendChild(makeElement("span", { text: "א", attributes: { dir: "auto" } })),
    },
    {
        name: ":has(:lang(he)) after Hebrew language child append",
        selector: ":has(:lang(he))",
        initial: false,
        after: true,
        mutate: subject => subject.appendChild(makeElement("span", { attributes: { lang: "he" } })),
    },
    {
        name: ":has(:link) after linked anchor append",
        selector: ":has(:link)",
        initial: false,
        after: true,
        mutate: subject => subject.appendChild(makeElement("a", { attributes: { href: "https://example.com/" } })),
    },
    {
        name: ":has(:any-link) after linked anchor append",
        selector: ":has(:any-link)",
        initial: false,
        after: true,
        mutate: subject => subject.appendChild(makeElement("a", { attributes: { href: "https://example.com/" } })),
    },
    {
        name: ":has(:only-child) after only child append",
        selector: ":has(:only-child)",
        initial: false,
        after: true,
        mutate: subject => subject.appendChild(makeElement("span")),
    },
    {
        name: ":has(:only-child) becomes false after sibling append",
        selector: ":has(:only-child)",
        initial: true,
        after: false,
        setup: subject => subject.appendChild(makeElement("span")),
        mutate: subject => subject.appendChild(makeElement("span")),
    },
    {
        name: ":has(:nth-child(2)) after second child append",
        selector: ":has(:nth-child(2))",
        initial: false,
        after: true,
        setup: subject => subject.appendChild(makeElement("span")),
        mutate: subject => subject.appendChild(makeElement("span")),
    },
    {
        name: ":has(:nth-last-child(2)) after trailing child append",
        selector: ":has(:nth-last-child(2))",
        initial: false,
        after: true,
        setup: subject => subject.appendChild(makeElement("span")),
        mutate: subject => subject.appendChild(makeElement("span")),
    },
    {
        name: ":has(:first-of-type) after typed child append",
        selector: ":has(:first-of-type)",
        initial: false,
        after: true,
        mutate: subject => subject.appendChild(makeElement("span")),
    },
    {
        name: ":has(:is(:checked, :disabled)) after disabled input append",
        selector: ":has(:is(:checked, :disabled))",
        initial: false,
        after: true,
        mutate: subject => {
            const input = makeElement("input");
            input.disabled = true;
            subject.appendChild(input);
        },
    },
    {
        name: ":has(:where(:empty, :checked)) after empty child append",
        selector: ":has(:where(:empty, :checked))",
        initial: false,
        after: true,
        mutate: subject => subject.appendChild(makeElement("span")),
    },
    {
        name: ":has(rect.hit) after SVG child append",
        selector: ":has(rect.hit)",
        initial: false,
        after: true,
        mutate: appendSvgHit,
    },
    {
        name: ":has(> rect.hit) after direct SVG child append",
        selector: ":has(> rect.hit)",
        initial: false,
        after: true,
        mutate: appendSvgHit,
    },
    {
        name: ":has(.outer .middle .hit) after deep descendant append",
        selector: ":has(.outer .middle .hit)",
        initial: false,
        after: true,
        mutate: appendHitDescendant,
    },
    {
        name: ":has(> .outer .hit) after child subtree append",
        selector: ":has(> .outer .hit)",
        initial: false,
        after: true,
        mutate: appendHitDescendant,
    },
    {
        name: ":has(> .outer > .middle > .hit) after exact deep append",
        selector: ":has(> .outer > .middle > .hit)",
        initial: false,
        after: true,
        mutate: appendHitDescendant,
    },
    {
        name: ":has(.miss .hit) ignores hit without required ancestor",
        selector: ":has(.miss .hit)",
        initial: false,
        after: false,
        mutate: appendHitDescendant,
    },
    {
        name: ":has(.before + .hit) after adjacent pair append",
        selector: ":has(.before + .hit)",
        initial: false,
        after: true,
        mutate: subject => appendSiblingPair(subject, "before", "hit"),
    },
    {
        name: ":has(.before ~ .hit) after separated sibling append",
        selector: ":has(.before ~ .hit)",
        initial: false,
        after: true,
        mutate: subject => {
            subject.appendChild(makeElement("span", { className: "before" }));
            subject.appendChild(makeElement("span"));
            subject.appendChild(makeElement("span", { className: "hit" }));
        },
    },
    {
        name: ":has(.hit + .after + .tail) after sibling chain append",
        selector: ":has(.hit + .after + .tail)",
        initial: false,
        after: true,
        mutate: subject => {
            subject.appendChild(makeElement("span", { className: "hit" }));
            subject.appendChild(makeElement("span", { className: "after" }));
            subject.appendChild(makeElement("span", { className: "tail" }));
        },
    },
    {
        name: ":has(.hit ~ .after ~ .tail) after indirect chain append",
        selector: ":has(.hit ~ .after ~ .tail)",
        initial: false,
        after: true,
        mutate: subject => {
            subject.appendChild(makeElement("span", { className: "hit" }));
            subject.appendChild(makeElement("span"));
            subject.appendChild(makeElement("span", { className: "after" }));
            subject.appendChild(makeElement("span"));
            subject.appendChild(makeElement("span", { className: "tail" }));
        },
    },
    {
        name: ":has(.hit:only-child) after only child append",
        selector: ":has(.hit:only-child)",
        initial: false,
        after: true,
        mutate: subject => subject.appendChild(makeElement("span", { className: "hit" })),
    },
    {
        name: ":has(.hit:only-child) becomes false after sibling append",
        selector: ":has(.hit:only-child)",
        initial: true,
        after: false,
        setup: subject => subject.appendChild(makeElement("span", { className: "hit" })),
        mutate: subject => subject.appendChild(makeElement("span")),
    },
    {
        name: ":has(.hit:nth-last-child(1)) after last child append",
        selector: ":has(.hit:nth-last-child(1))",
        initial: false,
        after: true,
        setup: subject => subject.appendChild(makeElement("span")),
        mutate: subject => subject.appendChild(makeElement("span", { className: "hit" })),
    },
    {
        name: ":has(.hit:nth-last-child(2)) after trailing child append",
        selector: ":has(.hit:nth-last-child(2))",
        initial: false,
        after: true,
        setup: subject => subject.appendChild(makeElement("span")),
        mutate: subject => {
            subject.appendChild(makeElement("span", { className: "hit" }));
            subject.appendChild(makeElement("span"));
        },
    },
    {
        name: ":has(.hit:nth-of-type(2)) after second typed child append",
        selector: ":has(.hit:nth-of-type(2))",
        initial: false,
        after: true,
        setup: subject => subject.appendChild(makeElement("span")),
        mutate: subject => subject.appendChild(makeElement("span", { className: "hit" })),
    },
    {
        name: ":has(.hit:only-of-type) after only typed child append",
        selector: ":has(.hit:only-of-type)",
        initial: false,
        after: true,
        mutate: subject => subject.appendChild(makeElement("span", { className: "hit" })),
    },
    {
        name: ":has(.hit:only-of-type) becomes false after same type sibling append",
        selector: ":has(.hit:only-of-type)",
        initial: true,
        after: false,
        setup: subject => subject.appendChild(makeElement("span", { className: "hit" })),
        mutate: subject => subject.appendChild(makeElement("span")),
    },
    {
        name: ":has(:is(.hit, .fallback)) after second-list child append",
        selector: ":has(:is(.hit, .fallback))",
        initial: false,
        after: true,
        mutate: subject => subject.appendChild(makeElement("span", { className: "fallback" })),
    },
    {
        name: ":has(:where(.hit, .fallback)) after second-list child append",
        selector: ":has(:where(.hit, .fallback))",
        initial: false,
        after: true,
        mutate: subject => subject.appendChild(makeElement("span", { className: "fallback" })),
    },
    {
        name: ":has(.hit):has(.also) after independent children append",
        selector: ":has(.hit):has(.also)",
        initial: false,
        after: true,
        mutate: subject => {
            subject.appendChild(makeElement("span", { className: "hit" }));
            subject.appendChild(makeElement("span", { className: "also" }));
        },
    },
    {
        name: ":is(:has(.hit)) after direct child append",
        selector: ":is(:has(.hit))",
        initial: false,
        after: true,
        mutate: subject => subject.appendChild(makeElement("span", { className: "hit" })),
    },
    {
        name: ":where(:has(.hit)) after direct child append",
        selector: ":where(:has(.hit))",
        initial: false,
        after: true,
        mutate: subject => subject.appendChild(makeElement("span", { className: "hit" })),
    },
    {
        name: ":not(:has(.miss)):has(.hit) after direct child append",
        selector: ":not(:has(.miss)):has(.hit)",
        initial: false,
        after: true,
        mutate: subject => subject.appendChild(makeElement("span", { className: "hit" })),
    },
    {
        name: ":has(.hit:not(:empty)) after non-empty child append",
        selector: ":has(.hit:not(:empty))",
        initial: false,
        after: true,
        mutate: subject => subject.appendChild(makeElement("span", { className: "hit", text: "text" })),
    },
    {
        name: ":has(.hit:empty) becomes false after nested text append",
        selector: ":has(.hit:empty)",
        initial: true,
        after: false,
        setup: subject => subject.appendChild(makeElement("span", { className: "hit" })),
        mutate: subject => subject.firstChild.appendChild(document.createTextNode("text")),
    },
    {
        name: ":has(.hit) after append with multiple arguments",
        selector: ":has(.hit)",
        initial: false,
        after: true,
        mutate: subject => subject.append(makeElement("span"), makeElement("span", { className: "hit" })),
    },
    {
        name: ":has(.hit:first-child + .after) after prepend before existing child",
        selector: ":has(.hit:first-child + .after)",
        initial: false,
        after: true,
        setup: subject => subject.appendChild(makeElement("span", { className: "after" })),
        mutate: subject => subject.prepend(makeElement("span", { className: "hit" })),
    },
    {
        name: ":has(.hit:last-child) becomes false after trailing sibling append",
        selector: ":has(.hit:last-child)",
        initial: true,
        after: false,
        setup: subject => subject.appendChild(makeElement("span", { className: "hit" })),
        mutate: subject => subject.appendChild(makeElement("span")),
    },
    {
        name: ":has(.hit + .after) becomes false after middle child removal",
        selector: ":has(.hit + .after)",
        initial: true,
        after: false,
        setup: subject => {
            subject.appendChild(makeElement("span", { className: "hit" }));
            subject.appendChild(makeElement("span", { className: "after" }));
        },
        mutate: subject => subject.firstChild.remove(),
    },
    {
        name: ":has(.hit ~ .after) becomes false after following child removal",
        selector: ":has(.hit ~ .after)",
        initial: true,
        after: false,
        setup: subject => {
            subject.appendChild(makeElement("span", { className: "hit" }));
            subject.appendChild(makeElement("span"));
            subject.appendChild(makeElement("span", { className: "after" }));
        },
        mutate: subject => subject.lastChild.remove(),
    },
    {
        name: ":has(> .wrapper > .hit) becomes false after nested child removal",
        selector: ":has(> .wrapper > .hit)",
        initial: true,
        after: false,
        setup: subject => {
            const wrapper = makeElement("span", { className: "wrapper" });
            wrapper.appendChild(makeElement("span", { className: "hit" }));
            subject.appendChild(wrapper);
        },
        mutate: subject => subject.firstChild.firstChild.remove(),
    },
    {
        name: ":has(.hit) after replaceWith matching child",
        selector: ":has(.hit)",
        initial: false,
        after: true,
        setup: subject => subject.appendChild(makeElement("span")),
        mutate: subject => subject.firstChild.replaceWith(makeElement("span", { className: "hit" })),
    },
    {
        name: ":has(.hit) becomes false after replaceWith nonmatching child",
        selector: ":has(.hit)",
        initial: true,
        after: false,
        setup: subject => subject.appendChild(makeElement("span", { className: "hit" })),
        mutate: subject => subject.firstChild.replaceWith(makeElement("span")),
    },
    {
        name: ":has(.hit) after cloned child append",
        selector: ":has(.hit)",
        initial: false,
        after: true,
        mutate: subject => {
            const template = makeElement("span", { className: "hit" });
            subject.appendChild(template.cloneNode(true));
        },
    },
    {
        name: ":has(.hit) after fragment replaceChildren",
        selector: ":has(.hit)",
        initial: false,
        after: true,
        setup: subject => subject.appendChild(makeElement("span")),
        mutate: subject => {
            const fragment = document.createDocumentFragment();
            fragment.appendChild(makeElement("span", { className: "hit" }));
            subject.replaceChildren(fragment);
        },
    },
    {
        name: ":has(:not(.miss)) after non-miss child append",
        selector: ":has(:not(.miss))",
        initial: false,
        after: true,
        mutate: subject => subject.appendChild(makeElement("span", { className: "hit" })),
    },
    {
        name: ":has(:not(:empty)) after text child append",
        selector: ":has(:not(:empty))",
        initial: false,
        after: true,
        mutate: subject => subject.appendChild(makeElement("span", { text: "text" })),
    },
    {
        name: ":has(:nth-child(even).hit) after even child append",
        selector: ":has(:nth-child(even).hit)",
        initial: false,
        after: true,
        setup: subject => subject.appendChild(makeElement("span")),
        mutate: subject => subject.appendChild(makeElement("span", { className: "hit" })),
    },
    {
        name: ":has(:nth-child(odd).hit) after prepend shifts child index",
        selector: ":has(:nth-child(odd).hit)",
        initial: false,
        after: true,
        setup: subject => {
            subject.appendChild(makeElement("span"));
            subject.appendChild(makeElement("span", { className: "hit" }));
        },
        mutate: subject => subject.prepend(makeElement("span")),
    }
);

const attributeMutationCases = [
    {
        name: ":has(.hit) after descendant class change",
        selector: ":has(.hit)",
        mutate: child => child.classList.add("hit"),
    },
    {
        name: ":has(.HIT) after matching uppercase descendant class change",
        selector: ":has(.HIT)",
        mutate: child => child.classList.add("HIT"),
    },
    {
        name: ":has(.HIT) ignores lowercase descendant class change in standards mode",
        selector: ":has(.HIT)",
        after: false,
        mutate: child => child.classList.add("hit"),
    },
    {
        name: ":has(.hit) ignores uppercase descendant class change in standards mode",
        selector: ":has(.hit)",
        after: false,
        mutate: child => child.classList.add("HIT"),
    },
    {
        name: ":has([data-hit]) after descendant attribute change",
        selector: ":has([data-hit])",
        mutate: child => child.setAttribute("data-hit", ""),
    },
    {
        name: ":has(#hit-id) after descendant id change",
        selector: ":has(#hit-id)",
        mutate: child => (child.id = "hit-id"),
    },
    {
        name: ":has(span.hit:not(.miss)) after descendant compound change",
        selector: ":has(span.hit:not(.miss))",
        mutate: child => child.classList.add("hit"),
    },
    {
        name: ":not(:has(.hit)) becomes false after descendant class change",
        selector: ":not(:has(.hit))",
        initial: true,
        after: false,
        mutate: child => child.classList.add("hit"),
    },
    {
        name: ":has(:is(.hit)) after descendant class change",
        selector: ":has(:is(.hit))",
        mutate: child => child.classList.add("hit"),
    },
    {
        name: ":has(.hit) becomes false after descendant class removal",
        selector: ":has(.hit)",
        initial: true,
        after: false,
        setup: child => child.classList.add("hit"),
        mutate: child => child.classList.remove("hit"),
    },
    {
        name: ":has([data-hit]) becomes false after descendant attribute removal",
        selector: ":has([data-hit])",
        initial: true,
        after: false,
        setup: child => child.setAttribute("data-hit", ""),
        mutate: child => child.removeAttribute("data-hit"),
    },
    {
        name: ":not(:has(.hit)) becomes true after descendant class removal",
        selector: ":not(:has(.hit))",
        initial: false,
        after: true,
        setup: child => child.classList.add("hit"),
        mutate: child => child.classList.remove("hit"),
    },
];

attributeMutationCases.push(
    {
        name: ":has(.hit.extra) after descendant second class change",
        selector: ":has(.hit.extra)",
        setup: child => child.classList.add("hit"),
        mutate: child => child.classList.add("extra"),
    },
    {
        name: ":has(.hit) becomes false after descendant className clear",
        selector: ":has(.hit)",
        initial: true,
        after: false,
        setup: child => child.classList.add("hit"),
        mutate: child => (child.className = ""),
    },
    {
        name: ":has([data-hit='yes']) after descendant attribute value change",
        selector: ":has([data-hit='yes'])",
        mutate: child => child.setAttribute("data-hit", "yes"),
    },
    {
        name: ":has([data-hit^='ye']) after descendant prefix attribute change",
        selector: ":has([data-hit^='ye'])",
        mutate: child => child.setAttribute("data-hit", "yes"),
    },
    {
        name: ":has([data-hit$='es']) after descendant suffix attribute change",
        selector: ":has([data-hit$='es'])",
        mutate: child => child.setAttribute("data-hit", "yes"),
    },
    {
        name: ":has([data-hit*='e']) after descendant substring attribute change",
        selector: ":has([data-hit*='e'])",
        mutate: child => child.setAttribute("data-hit", "yes"),
    },
    {
        name: ":has([data-hit~='yes']) after descendant word attribute change",
        selector: ":has([data-hit~='yes'])",
        mutate: child => child.setAttribute("data-hit", "no yes maybe"),
    },
    {
        name: ":has([data-hit|='yes']) after descendant dash attribute change",
        selector: ":has([data-hit|='yes'])",
        mutate: child => child.setAttribute("data-hit", "yes-now"),
    },
    {
        name: ":has([data-hit='yes']) becomes false after descendant value change",
        selector: ":has([data-hit='yes'])",
        initial: true,
        after: false,
        setup: child => child.setAttribute("data-hit", "yes"),
        mutate: child => child.setAttribute("data-hit", "no"),
    },
    {
        name: ":has([hidden]) after descendant hidden property change",
        selector: ":has([hidden])",
        mutate: child => (child.hidden = true),
    },
    {
        name: ":has(:is(.hit, [data-hit])) after descendant attribute change",
        selector: ":has(:is(.hit, [data-hit]))",
        mutate: child => child.setAttribute("data-hit", ""),
    },
    {
        name: ":has(:where(.hit, [data-hit])) after descendant class change",
        selector: ":has(:where(.hit, [data-hit]))",
        mutate: child => child.classList.add("hit"),
    },
    {
        name: ":has(.hit:not([data-miss])) after descendant class change",
        selector: ":has(.hit:not([data-miss]))",
        mutate: child => child.classList.add("hit"),
    },
    {
        name: ":has(.hit:not([data-miss])) becomes false after descendant miss attribute change",
        selector: ":has(.hit:not([data-miss]))",
        initial: true,
        after: false,
        setup: child => child.classList.add("hit"),
        mutate: child => child.setAttribute("data-miss", ""),
    },
    {
        name: ":has(.hit + .after) after descendant sibling class change",
        selector: ":has(.hit + .after)",
        setup: child => {
            child.parentNode.appendChild(makeElement("span", { className: "after" }));
        },
        mutate: child => child.classList.add("hit"),
    },
    {
        name: ":has(.hit ~ .after) after indirect sibling class change",
        selector: ":has(.hit ~ .after)",
        setup: child => {
            child.parentNode.appendChild(makeElement("span"));
            child.parentNode.appendChild(makeElement("span", { className: "after" }));
        },
        mutate: child => child.classList.add("hit"),
    },
    {
        name: ":has(.hit:first-child) after first descendant class change",
        selector: ":has(.hit:first-child)",
        mutate: child => child.classList.add("hit"),
    },
    {
        name: ":has(.hit:last-child) after last descendant class change",
        selector: ":has(.hit:last-child)",
        mutate: child => child.classList.add("hit"),
    },
    {
        name: ":has(.hit:only-child) after only descendant class change",
        selector: ":has(.hit:only-child)",
        mutate: child => child.classList.add("hit"),
    },
    {
        name: ":has(.hit:nth-child(2)) after second descendant class change",
        selector: ":has(.hit:nth-child(2))",
        setup: child => {
            child.parentNode.appendChild(makeElement("span"));
        },
        mutate: child => child.nextSibling.classList.add("hit"),
    },
    {
        name: ":not(:has([data-hit])) becomes false after descendant attribute change",
        selector: ":not(:has([data-hit]))",
        initial: true,
        after: false,
        mutate: child => child.setAttribute("data-hit", ""),
    },
    {
        name: ":has([data-hit]) becomes true after descendant toggleAttribute",
        selector: ":has([data-hit])",
        mutate: child => child.toggleAttribute("data-hit", true),
    },
    {
        name: ":has([data-hit]) becomes false after descendant toggleAttribute",
        selector: ":has([data-hit])",
        initial: true,
        after: false,
        setup: child => child.setAttribute("data-hit", ""),
        mutate: child => child.toggleAttribute("data-hit", false),
    },
    {
        name: ":has(#hit-id.extra) after descendant id and class change",
        selector: ":has(#hit-id.extra)",
        mutate: child => {
            child.id = "hit-id";
            child.classList.add("extra");
        },
    }
);

const descendantTextMutationCases = [
    {
        name: ":has(:empty) becomes false after descendant text insert",
        selector: ":has(:empty)",
        initial: true,
        after: false,
        setup: child => {},
        mutate: child => (child.textContent = "text"),
    },
    {
        name: ":has(:not(:empty)) becomes true after descendant text insert",
        selector: ":has(:not(:empty))",
        initial: false,
        after: true,
        setup: child => {},
        mutate: child => (child.textContent = "text"),
    },
];

descendantTextMutationCases.push(
    {
        name: ":has(.hit:empty) becomes false after descendant textContent insert",
        selector: ":has(.hit:empty)",
        initial: true,
        after: false,
        setup: child => child.classList.add("hit"),
        mutate: child => (child.textContent = "text"),
    },
    {
        name: ":has(.hit:not(:empty)) becomes true after descendant textContent insert",
        selector: ":has(.hit:not(:empty))",
        initial: false,
        after: true,
        setup: child => child.classList.add("hit"),
        mutate: child => (child.textContent = "text"),
    },
    {
        name: ":has(:empty:first-child) becomes false after descendant text insert",
        selector: ":has(:empty:first-child)",
        initial: true,
        after: false,
        setup: child => {},
        mutate: child => (child.textContent = "text"),
    },
    {
        name: ":has(:not(:empty):first-child) becomes true after descendant text insert",
        selector: ":has(:not(:empty):first-child)",
        initial: false,
        after: true,
        setup: child => {},
        mutate: child => (child.textContent = "text"),
    },
    {
        name: ":not(:has(:not(:empty))) becomes false after descendant text insert",
        selector: ":not(:has(:not(:empty)))",
        initial: true,
        after: false,
        setup: child => {},
        mutate: child => (child.textContent = "text"),
    }
);

const descendantDirectionMutationCases = [
    {
        name: ":has(:dir(rtl)) after descendant dir=auto text mutation",
        selector: ":has(:dir(rtl))",
        initial: false,
        after: true,
        setup: child => {
            child.dir = "auto";
            child.textContent = "a";
        },
        mutate: child => (child.firstChild.data = "א"),
    },
    {
        name: ":has(:dir(ltr)) becomes false after descendant dir=auto text mutation",
        selector: ":has(:dir(ltr))",
        initial: true,
        after: false,
        setup: child => {
            child.dir = "auto";
            child.textContent = "a";
        },
        mutate: child => (child.firstChild.data = "א"),
    },
    {
        name: ":has(:dir(rtl):first-child) after first descendant dir=auto text mutation",
        selector: ":has(:dir(rtl):first-child)",
        initial: false,
        after: true,
        setup: child => {
            child.dir = "auto";
            child.textContent = "a";
        },
        mutate: child => (child.firstChild.data = "א"),
    },
    {
        name: ":not(:has(:dir(rtl))) becomes false after descendant dir=auto text mutation",
        selector: ":not(:has(:dir(rtl)))",
        initial: true,
        after: false,
        setup: child => {
            child.dir = "auto";
            child.textContent = "a";
        },
        mutate: child => (child.firstChild.data = "א"),
    },
];

const descendantLanguageMutationCases = [
    {
        name: ":has(:lang(he)) after descendant lang attribute change",
        selector: ":has(:lang(he))",
        initial: false,
        after: true,
        setup: child => {},
        mutate: child => (child.lang = "he"),
    },
    {
        name: ":has(:lang(en)) becomes false after descendant lang attribute change",
        selector: ":has(:lang(en))",
        initial: true,
        after: false,
        setup: child => (child.lang = "en"),
        mutate: child => (child.lang = "he"),
    },
    {
        name: ":has(:lang(he):first-child) after first descendant lang attribute change",
        selector: ":has(:lang(he):first-child)",
        initial: false,
        after: true,
        setup: child => {},
        mutate: child => (child.lang = "he"),
    },
    {
        name: ":not(:has(:lang(he))) becomes false after descendant lang attribute change",
        selector: ":not(:has(:lang(he)))",
        initial: true,
        after: false,
        setup: child => {},
        mutate: child => (child.lang = "he"),
    },
];

const partChildMutationCases = [
    {
        name: "::part():empty becomes false after element append",
        selector: ":empty",
        initial: true,
        after: false,
        mutate: subject => subject.appendChild(makeElement("span")),
    },
    {
        name: "::part():empty becomes false after textContent",
        selector: ":empty",
        initial: true,
        after: false,
        mutate: subject => (subject.textContent = "text"),
    },
    {
        name: "::part():is(:empty) becomes false after element append",
        selector: ":is(:empty)",
        initial: true,
        after: false,
        mutate: subject => subject.appendChild(makeElement("span")),
    },
    {
        name: "::part():where(:empty) becomes false after element append",
        selector: ":where(:empty)",
        initial: true,
        after: false,
        mutate: subject => subject.appendChild(makeElement("span")),
    },
    {
        name: "::part():not(:empty) becomes true after textContent",
        selector: ":not(:empty)",
        initial: false,
        after: true,
        mutate: subject => (subject.textContent = "text"),
    },
    {
        name: "::part():empty becomes true after child removal",
        selector: ":empty",
        initial: false,
        after: true,
        setup: subject => subject.appendChild(makeElement("span")),
        mutate: subject => subject.firstChild.remove(),
    },
    {
        name: "::part():empty becomes true after replaceChildren",
        selector: ":empty",
        initial: false,
        after: true,
        setup: subject => subject.appendChild(makeElement("span")),
        mutate: subject => subject.replaceChildren(),
    },
    {
        name: "::part():dir(rtl) after dir=auto text mutation",
        selector: ":dir(rtl)",
        initial: false,
        after: true,
        setup: subject => {
            subject.dir = "auto";
            subject.textContent = "a";
        },
        mutate: subject => (subject.firstChild.data = "א"),
    },
];

partChildMutationCases.push(
    {
        name: "::part():has(.hit) after direct child append",
        selector: ":has(.hit)",
        initial: false,
        after: true,
        mutate: subject => subject.appendChild(makeElement("span", { className: "hit" })),
    },
    {
        name: "::part():has(> .hit) after direct child append",
        selector: ":has(> .hit)",
        initial: false,
        after: true,
        mutate: subject => subject.appendChild(makeElement("span", { className: "hit" })),
    },
    {
        name: "::part():has(.outer .middle .hit) after deep child append",
        selector: ":has(.outer .middle .hit)",
        initial: false,
        after: true,
        mutate: appendHitDescendant,
    },
    {
        name: "::part():has(.hit + .after) after adjacent sibling append",
        selector: ":has(.hit + .after)",
        initial: false,
        after: true,
        setup: subject => subject.appendChild(makeElement("span", { className: "hit" })),
        mutate: subject => subject.appendChild(makeElement("span", { className: "after" })),
    },
    {
        name: "::part():has(.hit ~ .after) after indirect sibling append",
        selector: ":has(.hit ~ .after)",
        initial: false,
        after: true,
        setup: subject => subject.appendChild(makeElement("span", { className: "hit" })),
        mutate: subject => {
            subject.appendChild(makeElement("span"));
            subject.appendChild(makeElement("span", { className: "after" }));
        },
    },
    {
        name: "::part():has(rect.hit) after SVG child append",
        selector: ":has(rect.hit)",
        initial: false,
        after: true,
        mutate: appendSvgHit,
    },
    {
        name: "::part():has(.hit:first-child) after prepend",
        selector: ":has(.hit:first-child)",
        initial: false,
        after: true,
        setup: subject => subject.appendChild(makeElement("span")),
        mutate: subject => subject.prepend(makeElement("span", { className: "hit" })),
    },
    {
        name: "::part():has(.hit:last-child) becomes false after trailing sibling append",
        selector: ":has(.hit:last-child)",
        initial: true,
        after: false,
        setup: subject => subject.appendChild(makeElement("span", { className: "hit" })),
        mutate: subject => subject.appendChild(makeElement("span")),
    },
    {
        name: "::part():not(:has(.hit)) becomes false after child append",
        selector: ":not(:has(.hit))",
        initial: true,
        after: false,
        mutate: subject => subject.appendChild(makeElement("span", { className: "hit" })),
    },
    {
        name: "::part():has(:is(.hit, [data-hit])) after child append",
        selector: ":has(:is(.hit, [data-hit]))",
        initial: false,
        after: true,
        mutate: subject => subject.appendChild(makeElement("span", { attributes: { "data-hit": "" } })),
    },
    {
        name: "::part():has(.hit) becomes false after child removal",
        selector: ":has(.hit)",
        initial: true,
        after: false,
        setup: subject => subject.appendChild(makeElement("span", { className: "hit" })),
        mutate: subject => subject.firstChild.remove(),
    },
    {
        name: "::part():has(.hit) after replaceChildren",
        selector: ":has(.hit)",
        initial: false,
        after: true,
        setup: subject => subject.appendChild(makeElement("span")),
        mutate: subject => subject.replaceChildren(makeElement("span", { className: "hit" })),
    }
);

const formChildMutationCases = [
    {
        name: ":invalid after required input append",
        selector: ":invalid",
        initial: false,
        after: true,
        fixtureOptions: { subjectOptions: { tagName: "form" } },
        mutate: subject => {
            const input = makeElement("input");
            input.required = true;
            subject.appendChild(input);
        },
    },
    {
        name: ":valid after required input removal",
        selector: ":valid",
        initial: false,
        after: true,
        fixtureOptions: { subjectOptions: { tagName: "form" } },
        setup: subject => {
            const input = makeElement("input");
            input.required = true;
            subject.appendChild(input);
        },
        mutate: subject => subject.firstChild.remove(),
    },
];

formChildMutationCases.push(
    {
        name: ":invalid after required fieldset input append",
        selector: ":invalid",
        initial: false,
        after: true,
        fixtureOptions: { subjectOptions: { tagName: "form" } },
        mutate: subject => {
            const fieldset = makeElement("fieldset");
            const input = makeElement("input");
            input.required = true;
            fieldset.appendChild(input);
            subject.appendChild(fieldset);
        },
    },
    {
        name: ":has(:invalid) after required input append",
        selector: ":has(:invalid)",
        initial: false,
        after: true,
        fixtureOptions: { subjectOptions: { tagName: "form" } },
        mutate: subject => {
            const input = makeElement("input");
            input.required = true;
            subject.appendChild(input);
        },
    },
    {
        name: ":has(:valid) after optional input append",
        selector: ":has(:valid)",
        initial: false,
        after: true,
        fixtureOptions: { subjectOptions: { tagName: "form" } },
        mutate: subject => subject.appendChild(makeElement("input")),
    },
    {
        name: ":has(:invalid) becomes false after required input removal",
        selector: ":has(:invalid)",
        initial: true,
        after: false,
        fixtureOptions: { subjectOptions: { tagName: "form" } },
        setup: subject => {
            const input = makeElement("input");
            input.required = true;
            subject.appendChild(input);
        },
        mutate: subject => subject.firstChild.remove(),
    }
);

const placeholderChildMutationCases = [
    {
        name: ":placeholder-shown becomes false after text insert",
        selector: ":placeholder-shown",
        initial: true,
        after: false,
        fixtureOptions: { subjectOptions: { tagName: "textarea", attributes: { placeholder: "placeholder" } } },
        mutate: subject => subject.appendChild(document.createTextNode("value")),
    },
    {
        name: ":not(:placeholder-shown) becomes true after text insert",
        selector: ":not(:placeholder-shown)",
        initial: false,
        after: true,
        fixtureOptions: { subjectOptions: { tagName: "textarea", attributes: { placeholder: "placeholder" } } },
        mutate: subject => subject.appendChild(document.createTextNode("value")),
    },
];

placeholderChildMutationCases.push(
    {
        name: ":placeholder-shown becomes true after text removal",
        selector: ":placeholder-shown",
        initial: false,
        after: true,
        fixtureOptions: { subjectOptions: { tagName: "textarea", attributes: { placeholder: "placeholder" } } },
        setup: subject => subject.appendChild(document.createTextNode("value")),
        mutate: subject => subject.firstChild.remove(),
    },
    {
        name: ":has(:placeholder-shown) after textarea child append",
        selector: ":has(:placeholder-shown)",
        initial: false,
        after: true,
        mutate: subject => subject.appendChild(makeElement("textarea", { attributes: { placeholder: "placeholder" } })),
    },
    {
        name: ":has(:not(:placeholder-shown)) after filled textarea child append",
        selector: ":has(:not(:placeholder-shown))",
        initial: false,
        after: true,
        mutate: subject => {
            const textarea = makeElement("textarea", { attributes: { placeholder: "placeholder" } });
            textarea.appendChild(document.createTextNode("value"));
            subject.appendChild(textarea);
        },
    }
);

const descendantStateMutationCases = [
    {
        name: ":has(:checked) after checkbox checked",
        selector: ":has(:checked)",
        setup: subject => subject.appendChild(makeElement("input", { attributes: { type: "checkbox" } })),
        mutate: subject => (subject.firstChild.checked = true),
    },
    {
        name: ":has(:checked) becomes false after checkbox unchecked",
        selector: ":has(:checked)",
        initial: true,
        after: false,
        setup: subject => {
            const input = makeElement("input", { attributes: { type: "checkbox" } });
            input.checked = true;
            subject.appendChild(input);
        },
        mutate: subject => (subject.firstChild.checked = false),
    },
    {
        name: ":has(:required) after input required",
        selector: ":has(:required)",
        setup: subject => subject.appendChild(makeElement("input")),
        mutate: subject => (subject.firstChild.required = true),
    },
    {
        name: ":has(:optional) becomes false after input required",
        selector: ":has(:optional)",
        initial: true,
        after: false,
        setup: subject => subject.appendChild(makeElement("input")),
        mutate: subject => (subject.firstChild.required = true),
    },
    {
        name: ":has(:disabled) after input disabled",
        selector: ":has(:disabled)",
        setup: subject => subject.appendChild(makeElement("input")),
        mutate: subject => (subject.firstChild.disabled = true),
    },
    {
        name: ":has(:enabled) becomes false after input disabled",
        selector: ":has(:enabled)",
        initial: true,
        after: false,
        setup: subject => subject.appendChild(makeElement("input")),
        mutate: subject => (subject.firstChild.disabled = true),
    },
    {
        name: ":has(:placeholder-shown) after textarea placeholder added",
        selector: ":has(:placeholder-shown)",
        setup: subject => subject.appendChild(makeElement("textarea")),
        mutate: subject => (subject.firstChild.placeholder = "placeholder"),
    },
    {
        name: ":has(:placeholder-shown) becomes false after textarea value append",
        selector: ":has(:placeholder-shown)",
        initial: true,
        after: false,
        setup: subject => subject.appendChild(makeElement("textarea", { attributes: { placeholder: "placeholder" } })),
        mutate: subject => subject.firstChild.appendChild(document.createTextNode("value")),
    },
    {
        name: ":has(:checked:first-child) after first checkbox checked",
        selector: ":has(:checked:first-child)",
        setup: subject => subject.appendChild(makeElement("input", { attributes: { type: "checkbox" } })),
        mutate: subject => (subject.firstChild.checked = true),
    },
    {
        name: ":has(:checked:nth-child(2)) after second checkbox checked",
        selector: ":has(:checked:nth-child(2))",
        setup: subject => {
            subject.appendChild(makeElement("input", { attributes: { type: "checkbox" } }));
            subject.appendChild(makeElement("input", { attributes: { type: "checkbox" } }));
        },
        mutate: subject => (subject.children[1].checked = true),
    },
    {
        name: ":has(:checked + .after) after checkbox checked",
        selector: ":has(:checked + .after)",
        setup: subject => {
            subject.appendChild(makeElement("input", { attributes: { type: "checkbox" } }));
            subject.appendChild(makeElement("span", { className: "after" }));
        },
        mutate: subject => (subject.firstChild.checked = true),
    },
    {
        name: ":has(:checked ~ .after) after checkbox checked",
        selector: ":has(:checked ~ .after)",
        setup: subject => {
            subject.appendChild(makeElement("input", { attributes: { type: "checkbox" } }));
            subject.appendChild(makeElement("span"));
            subject.appendChild(makeElement("span", { className: "after" }));
        },
        mutate: subject => (subject.firstChild.checked = true),
    },
    {
        name: ":not(:has(:checked)) becomes false after checkbox checked",
        selector: ":not(:has(:checked))",
        initial: true,
        after: false,
        setup: subject => subject.appendChild(makeElement("input", { attributes: { type: "checkbox" } })),
        mutate: subject => (subject.firstChild.checked = true),
    },
    {
        name: ":has(:is(:checked, .fallback)) after checkbox checked",
        selector: ":has(:is(:checked, .fallback))",
        setup: subject => subject.appendChild(makeElement("input", { attributes: { type: "checkbox" } })),
        mutate: subject => (subject.firstChild.checked = true),
    },
    {
        name: ":has(:checked):has(:disabled) after independent state changes",
        selector: ":has(:checked):has(:disabled)",
        setup: subject => {
            subject.appendChild(makeElement("input", { attributes: { type: "checkbox" } }));
            subject.appendChild(makeElement("input"));
        },
        mutate: subject => {
            subject.firstChild.checked = true;
            subject.children[1].disabled = true;
        },
    },
];

function runDetachedReconnectCase(scope, selectorCase, pseudoElement = "") {
    runCase({
        suite: pseudoElement ? "detached pseudo-element child mutation" : "detached child mutation",
        scope,
        initial: selectorCase.initial,
        after: selectorCase.after,
        pseudoElement,
        ...selectorCase,
        mutate: (subject, fixture) => {
            fixture.remove();
            selectorCase.mutate(subject, fixture);
            document.body.appendChild(fixture);
        },
    });
}

function runInheritedLanguageCase(scope, selectorCase, pseudoElement = "") {
    if (!caseSupportedInScope(scope, selectorCase, pseudoElement)) return;

    const suite = pseudoElement ? "inherited language pseudo-element mutation" : "inherited language mutation";
    const testName = `${suite}: ${scope}: ${selectorCase.name}`;
    const { cleanup, fixture, subject, target } = buildFixture(scope, selectorCase.selector, pseudoElement);
    try {
        subject.appendChild(makeElement("span"));
        selectorCase.setup?.(subject, fixture);
        assertEqual(`${testName} initial`, readProbe(target, pseudoElement), selectorCase.initial ? MATCH : BASE);
        resetStyleCounters();
        selectorCase.mutate(subject, fixture);
        assertEqual(`${testName} after mutation`, readProbe(target, pseudoElement), selectorCase.after ? MATCH : BASE);
        printPassWithCounters(testName);
    } finally {
        cleanup();
    }
}

const inheritedLanguageCases = [
    {
        name: ":has(:lang(he)) after ancestor lang change",
        selector: ":has(:lang(he))",
        initial: false,
        after: true,
        mutate: (subject, fixture) => (fixture.lang = "he"),
    },
    {
        name: ":has(:lang(en)) becomes false after ancestor lang change",
        selector: ":has(:lang(en))",
        initial: true,
        after: false,
        setup: (subject, fixture) => (fixture.lang = "en"),
        mutate: (subject, fixture) => (fixture.lang = "he"),
    },
    {
        name: ":not(:has(:lang(he))) becomes false after ancestor lang change",
        selector: ":not(:has(:lang(he)))",
        initial: true,
        after: false,
        mutate: (subject, fixture) => (fixture.lang = "he"),
    },
    {
        name: ":has(:lang(he):first-child) after ancestor lang change",
        selector: ":has(:lang(he):first-child)",
        initial: false,
        after: true,
        mutate: (subject, fixture) => (fixture.lang = "he"),
    },
    {
        name: ":has(:is(:lang(he), .fallback)) after ancestor lang change",
        selector: ":has(:is(:lang(he), .fallback))",
        initial: false,
        after: true,
        mutate: (subject, fixture) => (fixture.lang = "he"),
    },
    {
        name: ":has(:where(:lang(he), .fallback)) after ancestor lang change",
        selector: ":has(:where(:lang(he), .fallback))",
        initial: false,
        after: true,
        mutate: (subject, fixture) => (fixture.lang = "he"),
    },
];

function runPrecomputedDetachedInsertionCase(selectorCase) {
    const fixture = makeElement("section");
    document.body.appendChild(fixture);
    const style = addStyle(document.head, selectorCase.cssText);
    try {
        const holdingParent = makeElement("section");
        const container = makeElement("section", { className: "container" });
        fixture.append(holdingParent, container);
        const subtree = selectorCase.makeSubtree();
        const target = selectorCase.target(subtree);
        holdingParent.appendChild(subtree);

        assertEqual(`${selectorCase.name} precomputed initial`, readProbe(target), BASE);
        subtree.remove();
        resetStyleCounters();
        container.appendChild(subtree);
        assertEqual(`${selectorCase.name} after insertion`, readProbe(target), selectorCase.after ?? MATCH);
        printPassWithCounters(`precomputed detached insertion: ${selectorCase.name}`);
    } finally {
        style.remove();
        fixture.remove();
    }
}

const precomputedDetachedInsertionCases = [
    {
        name: "descendant class selector",
        cssText: `.container .leaf { ${MATCH_DECLARATION} }`,
        makeSubtree: () => {
            const root = makeElement("div");
            root.appendChild(makeElement("span", { className: "leaf" }));
            return root;
        },
        target: subtree => subtree.firstChild,
    },
    {
        name: "direct child chain selector",
        cssText: `.container > .detached-root > .leaf { ${MATCH_DECLARATION} }`,
        makeSubtree: () => {
            const root = makeElement("div", { className: "detached-root" });
            root.appendChild(makeElement("span", { className: "leaf" }));
            return root;
        },
        target: subtree => subtree.firstChild,
    },
    {
        name: "id selector",
        cssText: `.container #detached-leaf { ${MATCH_DECLARATION} }`,
        makeSubtree: () => {
            const root = makeElement("div");
            root.appendChild(makeElement("span", { id: "detached-leaf" }));
            return root;
        },
        target: subtree => subtree.firstChild,
    },
    {
        name: "mixed-case ancestor id selector",
        cssText: `.container #Foo .leaf { ${MATCH_DECLARATION} }`,
        makeSubtree: () => {
            const root = makeElement("div", { id: "Foo" });
            root.appendChild(makeElement("span", { className: "leaf" }));
            return root;
        },
        target: subtree => subtree.firstChild,
    },
    {
        name: "mixed-case ancestor id selector rejects lowercase id",
        cssText: `.container #Foo .leaf { ${MATCH_DECLARATION} }`,
        after: BASE,
        makeSubtree: () => {
            const root = makeElement("div", { id: "foo" });
            root.appendChild(makeElement("span", { className: "leaf" }));
            return root;
        },
        target: subtree => subtree.firstChild,
    },
    {
        name: "mixed-case ancestor id selector inside selector list",
        cssText: `.container :is(#Foo) .leaf { ${MATCH_DECLARATION} }`,
        makeSubtree: () => {
            const root = makeElement("div", { id: "Foo" });
            root.appendChild(makeElement("span", { className: "leaf" }));
            return root;
        },
        target: subtree => subtree.firstChild,
    },
    {
        name: "attribute selector",
        cssText: `.container [data-detached-leaf] { ${MATCH_DECLARATION} }`,
        makeSubtree: () => {
            const root = makeElement("div");
            root.appendChild(makeElement("span", { attributes: { "data-detached-leaf": "" } }));
            return root;
        },
        target: subtree => subtree.firstChild,
    },
    {
        name: "adjacent sibling selector",
        cssText: `.container .before + .leaf { ${MATCH_DECLARATION} }`,
        makeSubtree: () => {
            const root = makeElement("div");
            root.appendChild(makeElement("span", { className: "before" }));
            root.appendChild(makeElement("span", { className: "leaf" }));
            return root;
        },
        target: subtree => subtree.lastChild,
    },
    {
        name: "subsequent sibling selector",
        cssText: `.container .before ~ .leaf { ${MATCH_DECLARATION} }`,
        makeSubtree: () => {
            const root = makeElement("div");
            root.appendChild(makeElement("span", { className: "before" }));
            root.appendChild(makeElement("span"));
            root.appendChild(makeElement("span", { className: "leaf" }));
            return root;
        },
        target: subtree => subtree.lastChild,
    },
    {
        name: "positional selector",
        cssText: `.container .leaf:nth-child(2) { ${MATCH_DECLARATION} }`,
        makeSubtree: () => {
            const root = makeElement("div");
            root.appendChild(makeElement("span"));
            root.appendChild(makeElement("span", { className: "leaf" }));
            return root;
        },
        target: subtree => subtree.lastChild,
    },
    {
        name: "inherited custom-property probe",
        cssText: `.container { ${MATCH_DECLARATION} } .leaf { --style-invalidation-probe: inherit; }`,
        makeSubtree: () => {
            const root = makeElement("div");
            root.appendChild(makeElement("span", { className: "leaf" }));
            return root;
        },
        target: subtree => subtree.firstChild,
    },
    {
        name: "custom property inheritance",
        cssText: `.container { --detached-probe: ${MATCH}; } .leaf { --style-invalidation-probe: var(--detached-probe, ${BASE}); }`,
        makeSubtree: () => {
            const root = makeElement("div");
            root.appendChild(makeElement("span", { className: "leaf" }));
            return root;
        },
        target: subtree => subtree.firstChild,
    },
    {
        name: "selector list",
        cssText: `.container .miss, .container .leaf { ${MATCH_DECLARATION} }`,
        makeSubtree: () => {
            const root = makeElement("div");
            root.appendChild(makeElement("span", { className: "leaf" }));
            return root;
        },
        target: subtree => subtree.firstChild,
    },
];

function stressCaseSupportedInScope(scope, selectorCase) {
    if (!caseSupportedInScope(scope, selectorCase)) return false;
    return !(scope === "document-styled-use-shadow" && selectorCase.selector.startsWith(":dir("));
}

function runStressCase(scope, selectorCase, mode) {
    if (!stressCaseSupportedInScope(scope, selectorCase)) return;

    if (mode === "warm") {
        runCase({
            suite: "stress warm child mutation",
            scope,
            initial: selectorCase.initial,
            after: selectorCase.after,
            ...selectorCase,
        });
    } else if (mode === "cold") {
        runCase({
            suite: "stress cold child mutation",
            scope,
            initial: selectorCase.initial,
            after: selectorCase.after,
            skipInitialRead: true,
            ...selectorCase,
        });
    } else if (mode === "pseudo-warm") {
        runCase({
            suite: "stress warm pseudo-element child mutation",
            scope,
            initial: selectorCase.initial,
            after: selectorCase.after,
            pseudoElement: "::before",
            ...selectorCase,
        });
    } else if (mode === "pseudo-cold") {
        runCase({
            suite: "stress cold pseudo-element child mutation",
            scope,
            initial: selectorCase.initial,
            after: selectorCase.after,
            pseudoElement: "::before",
            skipInitialRead: true,
            ...selectorCase,
        });
    } else if (mode === "detached") {
        runDetachedReconnectCase(scope, selectorCase);
    } else if (mode === "detached-pseudo") {
        runDetachedReconnectCase(scope, selectorCase, "::before");
    } else {
        throw new Error(`Unknown stress mode ${mode}`);
    }
}

function runDescendantStressCase(scope, selectorCase, mode, options = {}) {
    if (options.skipDocumentStyledUseShadow && scope === "document-styled-use-shadow") return;

    const pseudoElement = mode.includes("pseudo") ? "::before" : "";
    const skipInitialRead = mode.includes("cold");
    const isDetached = mode.includes("detached");
    runCase({
        suite: `${options.suite} stress ${mode}`,
        scope,
        ...selectorCase,
        initial: selectorCase.initial ?? false,
        after: selectorCase.after ?? true,
        pseudoElement,
        skipInitialRead,
        setup: subject => {
            const child = makeElement(options.childTagName || "span", options.childOptions || {});
            subject.appendChild(child);
            selectorCase.setup?.(child);
        },
        mutate: (subject, fixture) => {
            if (isDetached) fixture.remove();
            selectorCase.mutate(subject.firstChild);
            if (isDetached) document.body.appendChild(fixture);
        },
    });
}

function runStateStressCase(scope, selectorCase, mode) {
    const pseudoElement = mode.includes("pseudo") ? "::before" : "";
    const skipInitialRead = mode.includes("cold");
    const isDetached = mode.includes("detached");
    runCase({
        suite: `descendant state stress ${mode}`,
        scope,
        initial: selectorCase.initial ?? false,
        after: selectorCase.after ?? true,
        pseudoElement,
        skipInitialRead,
        ...selectorCase,
        mutate: (subject, fixture) => {
            if (isDetached) fixture.remove();
            selectorCase.mutate(subject, fixture);
            if (isDetached) document.body.appendChild(fixture);
        },
    });
}

function runInheritedLanguageStressCase(scope, selectorCase, mode) {
    const pseudoElement = mode.includes("pseudo") ? "::before" : "";
    if (!caseSupportedInScope(scope, selectorCase, pseudoElement)) return;

    const suite = `inherited language stress ${mode}`;
    const testName = `${suite}: ${scope}: ${selectorCase.name}`;
    const { cleanup, fixture, subject, target } = buildFixture(scope, selectorCase.selector, pseudoElement);
    try {
        subject.appendChild(makeElement("span"));
        selectorCase.setup?.(subject, fixture);
        if (!mode.includes("cold"))
            assertEqual(`${testName} initial`, readProbe(target, pseudoElement), selectorCase.initial ? MATCH : BASE);
        resetStyleCounters();
        if (mode.includes("detached")) fixture.remove();
        selectorCase.mutate(subject, fixture);
        if (mode.includes("detached")) document.body.appendChild(fixture);
        assertEqual(`${testName} after mutation`, readProbe(target, pseudoElement), selectorCase.after ? MATCH : BASE);
        printPassWithCounters(testName);
    } finally {
        cleanup();
    }
}

function cloneSubtreeForPrecomputedStress(fixture, selectorCase) {
    const holdingParent = makeElement("section");
    fixture.appendChild(holdingParent);
    const subtree = selectorCase.makeSubtree();
    const target = selectorCase.target(subtree);
    holdingParent.appendChild(subtree);
    assertEqual(`${selectorCase.name} precomputed stress initial`, readProbe(target), BASE);
    subtree.remove();
    holdingParent.remove();
    return { subtree, target };
}

function runPrecomputedDetachedInsertionStressCase(selectorCase, mode) {
    const fixture = makeElement("section");
    document.body.appendChild(fixture);
    const style = addStyle(document.head, selectorCase.cssText);
    try {
        const container = makeElement("section", { className: "container" });
        fixture.appendChild(container);
        if (mode === "append") {
            const { subtree, target } = cloneSubtreeForPrecomputedStress(fixture, selectorCase);
            resetStyleCounters();
            container.appendChild(subtree);
            assertEqual(
                `${selectorCase.name} append stress after insertion`,
                readProbe(target),
                selectorCase.after ?? MATCH
            );
        } else if (mode === "prepend") {
            container.appendChild(makeElement("span"));
            const { subtree, target } = cloneSubtreeForPrecomputedStress(fixture, selectorCase);
            resetStyleCounters();
            container.prepend(subtree);
            assertEqual(
                `${selectorCase.name} prepend stress after insertion`,
                readProbe(target),
                selectorCase.after ?? MATCH
            );
        } else if (mode === "replaceChildren") {
            container.appendChild(makeElement("span"));
            const { subtree, target } = cloneSubtreeForPrecomputedStress(fixture, selectorCase);
            resetStyleCounters();
            container.replaceChildren(subtree);
            assertEqual(
                `${selectorCase.name} replaceChildren stress after insertion`,
                readProbe(target),
                selectorCase.after ?? MATCH
            );
        } else if (mode === "fragment") {
            const { subtree, target } = cloneSubtreeForPrecomputedStress(fixture, selectorCase);
            const fragment = document.createDocumentFragment();
            fragment.appendChild(subtree);
            resetStyleCounters();
            container.appendChild(fragment);
            assertEqual(
                `${selectorCase.name} fragment stress after insertion`,
                readProbe(target),
                selectorCase.after ?? MATCH
            );
        } else {
            throw new Error(`Unknown precomputed detached insertion stress mode ${mode}`);
        }
        printPassWithCounters(`precomputed detached insertion stress ${mode}: ${selectorCase.name}`);
    } finally {
        style.remove();
        fixture.remove();
    }
}

function runSlotMoveCase() {
    const fixture = makeElement("section");
    document.body.appendChild(fixture);
    try {
        const styledHost = makeElement("x-styled-host");
        const plainHost = makeElement("x-plain-host");
        const subject = makeSubject({ shadowHost: true });
        subject.appendChild(makeElement("span"));
        fixture.append(styledHost, plainHost);
        styledHost.appendChild(subject);

        const styledShadowRoot = styledHost.attachShadow({ mode: "open" });
        styledShadowRoot.appendChild(document.createElement("slot"));
        addStyle(styledShadowRoot, `::slotted(.subject:not(:empty)) { ${MATCH_DECLARATION} }`);

        const plainShadowRoot = plainHost.attachShadow({ mode: "open" });
        plainShadowRoot.appendChild(document.createElement("slot"));

        assertEqual("slot move out initial", readProbe(subject), MATCH);
        resetStyleCounters();
        plainHost.appendChild(subject);
        assertEqual("slot move out after mutation", readProbe(subject), BASE);
        styledHost.appendChild(subject);
        assertEqual("slot move in after mutation", readProbe(subject), MATCH);
        printPassWithCounters("scope move: slotted shadow-host target moves between slot scopes");
    } finally {
        fixture.remove();
    }
}

function runNestedSlotPartCase() {
    const fixture = makeElement("section");
    document.body.appendChild(fixture);
    try {
        const outerHost = makeElement("x-outer-host");
        const innerHost = makeElement("x-host");
        fixture.appendChild(outerHost);

        const outerShadowRoot = outerHost.attachShadow({ mode: "open" });
        outerShadowRoot.appendChild(innerHost);
        addStyle(outerShadowRoot, `x-host::part(foo):not(:empty)::before { ${MATCH_DECLARATION} }`);

        const innerShadowRoot = innerHost.attachShadow({ mode: "open" });
        const partSlot = makeElement("slot", { className: "subject", attributes: { part: "foo" } });
        innerShadowRoot.appendChild(partSlot);

        assertEqual("nested slot part initial", readProbe(partSlot, "::before"), BASE);
        resetStyleCounters();
        partSlot.appendChild(makeElement("span"));
        assertEqual("nested slot part after mutation", readProbe(partSlot, "::before"), MATCH);
        printPassWithCounters("combined topology: ancestor shadow exposes part slot with generated pseudo-element");
    } finally {
        fixture.remove();
    }
}
