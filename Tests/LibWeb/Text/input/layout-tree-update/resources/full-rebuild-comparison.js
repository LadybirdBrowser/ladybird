const layoutTreeDisplayValues = [
    "none",
    "contents",
    "block",
    "inline",
    "flow-root",
    "inline-block",
    "run-in",
    "list-item",
    "inline list-item",
    "flex",
    "inline-flex",
    "grid",
    "inline-grid",
    "table",
    "inline-table",
    "math",
    "-webkit-box",
    "-webkit-inline-box",
    "table-row-group",
    "table-header-group",
    "table-footer-group",
    "table-row",
    "table-cell",
    "table-column-group",
    "table-column",
    "table-caption",
    "block flow",
    "inline flow",
    "block flow-root",
    "inline flow-root",
    "block flex",
    "inline flex",
    "block grid",
    "inline grid",
    "block table",
    "inline table",
    "block flow list-item",
    "inline flow list-item",
];

function runLayoutTreeFullRebuildComparisonCases(cases) {
    const fixture = document.getElementById("fixture");
    const actualMismatches = [];
    const results = [];

    for (const case_ of cases) {
        fixture.replaceChildren();
        fixture.removeAttribute("style");

        const mutation = case_.setup(fixture);
        const comparisonDocuments = case_.comparisonDocuments ? case_.comparisonDocuments() : [document];
        for (const comparisonDocument of comparisonDocuments) comparisonDocument.body.offsetHeight;
        mutation();
        for (const comparisonDocument of comparisonDocuments) comparisonDocument.body.offsetHeight;

        const comparisons = comparisonDocuments.map(comparisonDocument =>
            comparisonDocument.defaultView.internals.compareLayoutTreeWithFullRebuild()
        );
        const matches = comparisons.every(comparison => comparison.matches);
        results.push({ name: case_.name, matches });
        if (!matches) actualMismatches.push(case_.name);

        if (case_.cleanup) case_.cleanup();
    }

    for (const result of results) {
        if (result.matches) println(`PASS: ${result.name}`);
        else println(`FAIL: ${result.name}`);
    }
    println(
        `${cases.length} cases; ${cases.length - actualMismatches.length} matched; ${actualMismatches.length} mismatched`
    );
}

async function runAsyncLayoutTreeFullRebuildComparisonCases(cases) {
    const fixture = document.getElementById("fixture");
    const actualMismatches = [];
    const results = [];

    for (const case_ of cases) {
        fixture.replaceChildren();
        fixture.removeAttribute("style");

        const mutation = await case_.setup(fixture);
        if (case_.settleBeforeMutation) await case_.settleBeforeMutation();
        const comparisonDocuments = case_.comparisonDocuments ? case_.comparisonDocuments() : [document];
        for (const comparisonDocument of comparisonDocuments) comparisonDocument.body.offsetHeight;

        await mutation();
        if (case_.settleAfterMutation) await case_.settleAfterMutation();
        for (const comparisonDocument of comparisonDocuments) comparisonDocument.body.offsetHeight;

        const comparisons = comparisonDocuments.map(comparisonDocument =>
            comparisonDocument.defaultView.internals.compareLayoutTreeWithFullRebuild()
        );
        const matches = comparisons.every(comparison => comparison.matches);
        results.push({ name: case_.name, matches });
        if (!matches) actualMismatches.push(case_.name);

        if (case_.cleanup) await case_.cleanup();
    }

    for (const result of results) println(`${result.matches ? "PASS" : "FAIL"}: ${result.name}`);
    println(
        `${cases.length} cases; ${cases.length - actualMismatches.length} matched; ${actualMismatches.length} mismatched`
    );
}

function layoutTreeIdentityTransition(before, after) {
    if (before === 0 && after === 0) return "ABSENT";
    if (before === 0) return "CREATED";
    if (after === 0) return "REMOVED";
    if (before === after) return "PRESERVED";
    return "RECREATED";
}

function runLayoutTreeInvalidationScopeCases(cases) {
    const fixture = document.getElementById("fixture");
    const results = [];
    let mismatchCount = 0;

    for (const case_ of cases) {
        fixture.replaceChildren();
        fixture.removeAttribute("style");

        const { mutate, trackedNodes } = case_.setup(fixture);
        document.body.offsetHeight;
        const identitiesBefore = trackedNodes.map(({ node }) => internals.layoutNodeIdentity(node));
        const buildsBefore = internals.layoutTreeBuildStats().builds;

        mutate();
        document.body.offsetHeight;
        const identitiesAfter = trackedNodes.map(({ node }) => internals.layoutNodeIdentity(node));
        const stats = internals.layoutTreeBuildStats();
        const didBuildLayoutTree = stats.builds !== buildsBefore;
        const comparison = internals.compareLayoutTreeWithFullRebuild();
        if (!comparison.matches) ++mismatchCount;

        results.push({
            name: case_.name,
            identities: trackedNodes.map(({ label }, index) => ({
                label,
                transition: layoutTreeIdentityTransition(identitiesBefore[index], identitiesAfter[index]),
            })),
            didBuildLayoutTree,
            rebuiltSubtreeRoots: stats.lastBuildRebuiltSubtreeRoots,
            escapedRebuildRoots: stats.lastBuildEscapedRebuildRoots,
            matches: comparison.matches,
        });
    }

    for (const result of results) {
        for (const identity of result.identities) println(`${identity.transition}: ${result.name} | ${identity.label}`);
        if (result.didBuildLayoutTree) {
            println(
                `STATS: ${result.name} | rebuilt subtree roots=${result.rebuiltSubtreeRoots}, escaped=${result.escapedRebuildRoots}`
            );
        } else {
            println(`STATS: ${result.name} | no layout tree build`);
        }
        println(`${result.matches ? "PASS" : "FAIL"}: ${result.name} | incremental tree matches full rebuild`);
    }
    println(`${cases.length} cases; ${cases.length - mismatchCount} matched; ${mismatchCount} mismatched`);
}

const layoutTreeInvalidationScopeContextDisplays = layoutTreeDisplayValues.filter(display => display !== "none");

function createLayoutTreeInvalidationScopeCase(name, contextDisplay, targetDisplay, configureInitial, mutateTarget) {
    return {
        name,
        setup(fixture) {
            const distant = document.createElement("aside");
            const distantChild = document.createElement("small");
            const context = document.createElement("section");
            const before = document.createElement("div");
            const beforeChild = document.createElement("span");
            const target = document.createElement("div");
            const targetChild = document.createElement("span");
            const after = document.createElement("div");
            const afterChild = document.createElement("span");

            distant.append("distant-before", distantChild, "distant-after");
            distantChild.textContent = "distant-child";
            context.style.display = contextDisplay;
            before.append("before", beforeChild);
            beforeChild.textContent = "before-child";
            target.style.display = targetDisplay;
            target.append("target-before", targetChild, "target-after");
            targetChild.textContent = "target-child";
            after.append(afterChild, "after");
            afterChild.textContent = "after-child";
            context.append(before, target, after);
            fixture.append(distant, context);
            configureInitial(target);

            return {
                mutate: () => mutateTarget(target),
                trackedNodes: [
                    { label: "fixture ancestor", node: fixture },
                    { label: "distant sibling", node: distant },
                    { label: "distant sibling descendant", node: distantChild },
                    { label: "DOM parent", node: context },
                    { label: "preceding sibling", node: before },
                    { label: "preceding sibling descendant", node: beforeChild },
                    { label: "target", node: target },
                    { label: "target descendant", node: targetChild },
                    { label: "following sibling", node: after },
                    { label: "following sibling descendant", node: afterChild },
                ],
            };
        },
    };
}

function createComparisonIframe(fixture) {
    const iframe = document.createElement("iframe");
    iframe.style.display = "block";
    fixture.appendChild(iframe);
    iframe.contentDocument.body.innerHTML = '<main id="frame-fixture"></main>';
    return iframe;
}

function displayTransitionCases(partition, partitionCount) {
    const cases = [];
    for (let fromIndex = 0; fromIndex < layoutTreeDisplayValues.length; ++fromIndex) {
        if (fromIndex % partitionCount !== partition) continue;

        for (let toIndex = 0; toIndex < layoutTreeDisplayValues.length; ++toIndex) {
            if (fromIndex === toIndex) continue;

            const from = layoutTreeDisplayValues[fromIndex];
            const to = layoutTreeDisplayValues[toIndex];
            cases.push({
                name: `display transition ${from} -> ${to}`,
                setup(fixture) {
                    const before = document.createTextNode("before");
                    const target = document.createElement("div");
                    const child = document.createElement("span");
                    const after = document.createTextNode("after");
                    target.style.display = from;
                    child.textContent = "child";
                    target.append("text", child);
                    fixture.append(before, target, after);
                    return () => {
                        target.style.display = to;
                    };
                },
            });
        }
    }
    return cases;
}

function displayInvalidationScopeCases(partition, partitionCount) {
    const cases = [];
    let caseIndex = 0;
    for (let fromIndex = 0; fromIndex < layoutTreeDisplayValues.length; ++fromIndex) {
        const from = layoutTreeDisplayValues[fromIndex];
        for (let toIndex = 0; toIndex < layoutTreeDisplayValues.length; ++toIndex) {
            const to = layoutTreeDisplayValues[toIndex];
            if (from === to) continue;
            const index = caseIndex++;
            if (index % partitionCount !== partition) continue;
            const contextDisplay =
                layoutTreeInvalidationScopeContextDisplays[
                    (fromIndex * layoutTreeDisplayValues.length + toIndex) %
                        layoutTreeInvalidationScopeContextDisplays.length
                ];
            cases.push(
                createLayoutTreeInvalidationScopeCase(
                    `display ${from} -> ${to} in ${contextDisplay}`,
                    contextDisplay,
                    from,
                    () => {},
                    target => {
                        target.style.display = to;
                    }
                )
            );
        }
    }
    return cases;
}

function flatTreeInvalidationScopeCases(partition, partitionCount) {
    const modes = ["direct shadow child", "assigned slottable", "slot fallback child"];
    const cases = [];
    let caseIndex = 0;
    for (let fromIndex = 0; fromIndex < layoutTreeDisplayValues.length; ++fromIndex) {
        const from = layoutTreeDisplayValues[fromIndex];
        for (let toIndex = 0; toIndex < layoutTreeDisplayValues.length; ++toIndex) {
            const to = layoutTreeDisplayValues[toIndex];
            if (from === to) continue;
            const index = caseIndex++;
            if (index % partitionCount !== partition) continue;
            const mode = modes[(fromIndex + toIndex) % modes.length];
            cases.push({
                name: `${mode} display ${from} -> ${to}`,
                setup(fixture) {
                    const distant = document.createElement("aside");
                    const host = document.createElement("div");
                    const shadow = host.attachShadow({ mode: "open" });
                    const shadowContainer = document.createElement("section");
                    const before = document.createElement("div");
                    const beforeChild = document.createElement("span");
                    const target = document.createElement("div");
                    const targetChild = document.createElement("span");
                    const after = document.createElement("div");
                    const afterChild = document.createElement("span");
                    const slot = document.createElement("slot");

                    distant.textContent = "distant";
                    before.append("before", beforeChild);
                    beforeChild.textContent = "before-child";
                    target.style.display = from;
                    target.append("target-before", targetChild, "target-after");
                    targetChild.textContent = "target-child";
                    after.append(afterChild, "after");
                    afterChild.textContent = "after-child";

                    if (mode === "direct shadow child") shadowContainer.append(before, target, after);
                    else {
                        slot.name = "target";
                        shadowContainer.append(before, slot, after);
                        if (mode === "assigned slottable") {
                            target.slot = "target";
                            host.appendChild(target);
                        } else {
                            slot.appendChild(target);
                        }
                    }
                    shadow.appendChild(shadowContainer);
                    fixture.append(distant, host);

                    return {
                        mutate: () => {
                            target.style.display = to;
                        },
                        trackedNodes: [
                            { label: "fixture ancestor", node: fixture },
                            { label: "distant sibling", node: distant },
                            { label: "shadow host", node: host },
                            { label: "shadow container", node: shadowContainer },
                            { label: "preceding flat-tree sibling", node: before },
                            { label: "preceding sibling descendant", node: beforeChild },
                            { label: "target", node: target },
                            { label: "target descendant", node: targetChild },
                            { label: "following flat-tree sibling", node: after },
                            { label: "following sibling descendant", node: afterChild },
                        ],
                    };
                },
            });
        }
    }
    return cases;
}

function nestedDisplayInteractionCases(partition, partitionCount) {
    const cases = [];
    for (let parentIndex = 0; parentIndex < layoutTreeDisplayValues.length; ++parentIndex) {
        if (parentIndex % partitionCount !== partition) continue;

        for (const childDisplay of layoutTreeDisplayValues) {
            const parentDisplay = layoutTreeDisplayValues[parentIndex];
            cases.push({
                name: `nested displays ${parentDisplay} > ${childDisplay}`,
                setup(fixture) {
                    const parent = document.createElement("div");
                    const child = document.createElement("div");
                    parent.style.display = parentDisplay;
                    child.style.display = childDisplay;
                    child.textContent = "child";
                    parent.append("before", child, "after");
                    fixture.appendChild(parent);
                    return () => {
                        const inserted = document.createElement(parentIndex % 2 ? "span" : "div");
                        inserted.textContent = "inserted";
                        child.insertBefore(inserted, child.firstChild);
                    };
                },
            });
        }
    }
    return cases;
}

function structuralDomMutationCases(partition, partitionCount) {
    const mutations = [
        ["append element", c => c.appendChild(document.createElement("b"))],
        ["append text", c => c.appendChild(document.createTextNode("appended"))],
        ["prepend element", c => c.prepend(document.createElement("b"))],
        ["insertBefore first", c => c.insertBefore(document.createElement("b"), c.firstChild)],
        ["insertBefore middle", c => c.insertBefore(document.createElement("b"), c.querySelector(".middle"))],
        ["before element", c => c.querySelector(".middle").before(document.createElement("b"), "before-text")],
        ["after element", c => c.querySelector(".middle").after("after-text", document.createElement("b"))],
        ["append mixed nodes", c => c.append("append-text", document.createElement("b"), "tail")],
        ["prepend mixed nodes", c => c.prepend("prepend-text", document.createElement("b"), "lead")],
        ["remove first", c => c.firstChild.remove()],
        ["remove middle", c => c.querySelector(".middle").remove()],
        ["remove last", c => c.lastChild.remove()],
        ["removeChild", c => c.removeChild(c.querySelector(".middle"))],
        ["replaceChild", c => c.replaceChild(document.createElement("strong"), c.querySelector(".middle"))],
        ["replaceWith", c => c.querySelector(".middle").replaceWith("replacement", document.createElement("strong"))],
        ["replaceWith empty", c => c.querySelector(".middle").replaceWith()],
        ["replaceChildren mixed", c => c.replaceChildren("before", document.createElement("strong"), "after")],
        ["replaceChildren empty", c => c.replaceChildren()],
        [
            "innerHTML replace",
            c => {
                c.innerHTML = "<strong>replacement</strong><div>block</div>";
            },
        ],
        [
            "innerHTML clear",
            c => {
                c.innerHTML = "";
            },
        ],
        [
            "outerHTML replace",
            c => {
                c.querySelector(".middle").outerHTML = "<strong>outer replacement</strong>";
            },
        ],
        [
            "outerHTML remove",
            c => {
                c.querySelector(".middle").outerHTML = "";
            },
        ],
        [
            "textContent replace",
            c => {
                c.textContent = "replacement text";
            },
        ],
        [
            "textContent clear",
            c => {
                c.textContent = "";
            },
        ],
        ["move first to end", c => c.appendChild(c.firstChild)],
        ["move last to start", c => c.prepend(c.lastChild)],
        [
            "move middle across parents",
            c => c.parentElement.querySelector(".destination").appendChild(c.querySelector(".middle")),
        ],
        [
            "append DocumentFragment",
            c => {
                const fragment = document.createDocumentFragment();
                fragment.append("fragment text", document.createElement("strong"), document.createElement("div"));
                c.appendChild(fragment);
            },
        ],
        [
            "insertAdjacentHTML beforebegin",
            c => c.querySelector(".middle").insertAdjacentHTML("beforebegin", "<strong>adjacent</strong>"),
        ],
        [
            "insertAdjacentHTML afterbegin",
            c => c.querySelector(".middle").insertAdjacentHTML("afterbegin", "<strong>adjacent</strong>"),
        ],
        [
            "insertAdjacentHTML beforeend",
            c => c.querySelector(".middle").insertAdjacentHTML("beforeend", "<strong>adjacent</strong>"),
        ],
        [
            "insertAdjacentHTML afterend",
            c => c.querySelector(".middle").insertAdjacentHTML("afterend", "<strong>adjacent</strong>"),
        ],
        [
            "insertAdjacentElement",
            c => c.querySelector(".middle").insertAdjacentElement("afterend", document.createElement("strong")),
        ],
        ["insertAdjacentText", c => c.querySelector(".middle").insertAdjacentText("beforebegin", "adjacent text")],
        ["splitText", c => c.querySelector(".text-target").firstChild.splitText(3)],
        ["CharacterData replaceData", c => c.querySelector(".text-target").firstChild.replaceData(1, 2, "eplaced")],
        [
            "normalize adjacent text",
            c => {
                c.append(document.createTextNode("one"), document.createTextNode("two"));
                c.normalize();
            },
        ],
        [
            "Range insertNode",
            c => {
                const range = document.createRange();
                range.setStart(c.querySelector(".text-target").firstChild, 2);
                range.collapse(true);
                range.insertNode(document.createElement("strong"));
            },
        ],
        [
            "Range deleteContents",
            c => {
                const range = document.createRange();
                range.setStartBefore(c.querySelector(".middle"));
                range.setEndAfter(c.querySelector(".last"));
                range.deleteContents();
            },
        ],
        [
            "Range extractContents",
            c => {
                const range = document.createRange();
                range.selectNode(c.querySelector(".middle"));
                range.extractContents();
            },
        ],
        [
            "Range surroundContents",
            c => {
                const range = document.createRange();
                range.selectNode(c.querySelector(".middle"));
                range.surroundContents(document.createElement("strong"));
            },
        ],
        [
            "append imported subtree",
            c => {
                const source = document.implementation.createHTMLDocument();
                source.body.innerHTML = "<section><b>imported</b></section>";
                c.appendChild(document.importNode(source.body.firstChild, true));
            },
        ],
        ["append cloned subtree", c => c.appendChild(c.querySelector(".middle").cloneNode(true))],
        [
            "append template contents",
            c => {
                const template = document.createElement("template");
                template.innerHTML = "template text <strong>template child</strong>";
                c.appendChild(template.content.cloneNode(true));
            },
        ],
        [
            "append DOMParser subtree",
            c => {
                const source = new DOMParser().parseFromString("<section><b>parsed</b></section>", "text/html");
                c.appendChild(source.body.firstChild);
            },
        ],
    ];

    const cases = [];
    for (let displayIndex = 0; displayIndex < layoutTreeDisplayValues.length; ++displayIndex) {
        if (displayIndex % partitionCount !== partition) continue;
        const display = layoutTreeDisplayValues[displayIndex];
        for (const [mutationName, mutate] of mutations) {
            cases.push({
                name: `${mutationName} in ${display}`,
                setup(fixture) {
                    const container = document.createElement("div");
                    const destination = document.createElement("div");
                    container.style.display = display;
                    destination.className = "destination";
                    container.innerHTML =
                        '<i class="first">first</i> lead <span class="middle">middle</span> tail <span class="text-target">abcdef</span><b class="last">last</b>';
                    fixture.append(container, destination);
                    return () => mutate(container);
                },
            });
        }
    }
    return cases;
}

function structuralDomRemovalCases(partition, partitionCount) {
    const removals = [
        ["remove leaf element", c => c.querySelector(".leaf").remove()],
        ["remove leaf text", c => c.querySelector(".text-leaf").firstChild.remove()],
        ["remove shallow subtree", c => c.querySelector(".shallow-subtree").remove()],
        ["remove deep subtree", c => c.querySelector(".deep-subtree").remove()],
        ["remove first sibling", c => c.querySelector(".sibling:first-child").remove()],
        ["remove middle sibling", c => c.querySelector(".sibling:nth-child(4)").remove()],
        ["remove last sibling", c => c.querySelector(".sibling:last-child").remove()],
        [
            "remove first sibling run",
            c => {
                for (let i = 0; i < 3; ++i) c.querySelector(".sibling:first-child").remove();
            },
        ],
        [
            "remove middle sibling run",
            c => {
                for (const node of Array.from(c.querySelectorAll(".sibling")).slice(2, 5)) node.remove();
            },
        ],
        [
            "remove last sibling run",
            c => {
                for (const node of Array.from(c.querySelectorAll(".sibling")).slice(-3)) node.remove();
            },
        ],
        [
            "remove alternating siblings",
            c => {
                for (const [index, node] of Array.from(c.querySelectorAll(".sibling")).entries()) {
                    if (index % 2 === 0) node.remove();
                }
            },
        ],
        [
            "remove all siblings forwards",
            c => {
                while (c.firstChild) c.firstChild.remove();
            },
        ],
        [
            "remove all siblings backwards",
            c => {
                while (c.lastChild) c.lastChild.remove();
            },
        ],
        [
            "Range delete first sibling run",
            c => {
                const siblings = c.querySelectorAll(".sibling");
                const range = document.createRange();
                range.setStartBefore(siblings[0]);
                range.setEndAfter(siblings[2]);
                range.deleteContents();
            },
        ],
        [
            "Range delete middle sibling run",
            c => {
                const siblings = c.querySelectorAll(".sibling");
                const range = document.createRange();
                range.setStartBefore(siblings[2]);
                range.setEndAfter(siblings[4]);
                range.deleteContents();
            },
        ],
        [
            "Range delete last sibling run",
            c => {
                const siblings = c.querySelectorAll(".sibling");
                const range = document.createRange();
                range.setStartBefore(siblings[5]);
                range.setEndAfter(siblings[7]);
                range.deleteContents();
            },
        ],
        ["replaceChildren removes subtree", c => c.replaceChildren()],
        [
            "innerHTML removes subtree",
            c => {
                c.innerHTML = "";
            },
        ],
        [
            "textContent removes subtree",
            c => {
                c.textContent = "";
            },
        ],
        ["remove generating parent", c => c.remove()],
        ["remove block from inline run", c => c.querySelector(".block-in-inline").remove()],
        ["remove display contents subtree", c => c.querySelector(".contents-subtree").remove()],
    ];

    const cases = [];
    for (let displayIndex = 0; displayIndex < layoutTreeDisplayValues.length; ++displayIndex) {
        if (displayIndex % partitionCount !== partition) continue;
        const display = layoutTreeDisplayValues[displayIndex];
        for (const [removalName, remove] of removals) {
            cases.push({
                name: `${removalName} from ${display}`,
                setup(fixture) {
                    const container = document.createElement("div");
                    container.style.display = display;
                    container.innerHTML = [
                        '<span class="sibling leaf">leaf</span>',
                        '<span class="sibling text-leaf">text</span>',
                        '<div class="sibling shallow-subtree"><b>child</b></div>',
                        '<div class="sibling deep-subtree"><div><div><strong>deep</strong></div></div></div>',
                        '<span class="sibling">middle</span>',
                        '<div class="sibling block-in-inline">block</div>',
                        '<div class="sibling contents-subtree" style="display:contents"><span>contents</span></div>',
                        '<span class="sibling ordinary-last">last</span>',
                    ].join("");
                    fixture.append("before", container, "after");
                    return () => remove(container);
                },
            });
        }
    }
    return cases;
}

function shadowSlotMutationCases() {
    const mutations = [
        ["append assigned node", host => host.appendChild(document.createElement("b"))],
        ["remove assigned node", host => host.querySelector(".assigned").remove()],
        [
            "replace assigned subtree",
            host => host.querySelector(".assigned").replaceWith(document.createElement("strong")),
        ],
        [
            "assign node to named slot",
            host => {
                host.querySelector(".assigned").slot = "named";
            },
        ],
        [
            "unassign node from named slot",
            host => {
                host.querySelector(".named-assigned").removeAttribute("slot");
            },
        ],
        [
            "rename slot",
            host => {
                host.shadowRoot.querySelector('slot[name="named"]').name = "renamed";
            },
        ],
        ["remove default slot", host => host.shadowRoot.querySelector("slot:not([name])").remove()],
        ["remove named slot", host => host.shadowRoot.querySelector('slot[name="named"]').remove()],
        ["insert additional slot", host => host.shadowRoot.appendChild(document.createElement("slot"))],
        [
            "replace shadow slot tree",
            host => {
                host.shadowRoot.innerHTML = "<div><slot name=named></slot><slot></slot></div>";
            },
        ],
        [
            "append fallback content",
            host => host.shadowRoot.querySelector("slot:not([name])").appendChild(document.createElement("i")),
        ],
        ["remove fallback content", host => host.shadowRoot.querySelector(".fallback").remove()],
    ];
    const cases = [];
    for (const hostDisplay of ["block", "inline", "contents", "flex", "grid", "table-cell", "none"]) {
        for (const [mutationName, mutate] of mutations) {
            cases.push({
                name: `${mutationName} with ${hostDisplay} shadow host`,
                setup(fixture) {
                    const host = document.createElement("div");
                    host.style.display = hostDisplay;
                    host.attachShadow({ mode: "open" }).innerHTML =
                        '<slot><em class="fallback">fallback</em></slot><slot name="named"></slot>';
                    host.innerHTML =
                        '<span class="assigned">default</span><span class="named-assigned" slot="named">named</span>';
                    fixture.appendChild(host);
                    return () => mutate(host);
                },
            });
        }
    }
    return cases;
}

function crossDocumentAdoptionCases() {
    const cases = [];
    const nodeConfigurations = [
        [
            "leaf element",
            doc => {
                const node = doc.createElement("span");
                node.textContent = "leaf";
                return node;
            },
        ],
        ["text node", doc => doc.createTextNode("adopted text")],
        [
            "deep subtree",
            doc => {
                const node = doc.createElement("section");
                node.innerHTML = "<div><span>deep</span></div>";
                return node;
            },
        ],
        [
            "display contents subtree",
            doc => {
                const node = doc.createElement("div");
                node.style.display = "contents";
                node.innerHTML = "before <span>contents child</span> after";
                return node;
            },
        ],
        [
            "table subtree",
            doc => {
                const node = doc.createElement("table");
                node.innerHTML = "<tbody><tr><td>cell</td></tr></tbody>";
                return node;
            },
        ],
        [
            "list item",
            doc => {
                const node = doc.createElement("li");
                node.textContent = "item";
                return node;
            },
        ],
        [
            "object fallback subtree",
            doc => {
                const node = doc.createElement("object");
                node.innerHTML = "<div>object fallback</div>";
                return node;
            },
        ],
        [
            "embed element",
            doc => {
                const node = doc.createElement("embed");
                node.setAttribute("type", "application/x-unknown");
                return node;
            },
        ],
    ];

    for (const [nodeName, createNode] of nodeConfigurations) {
        for (const direction of ["parent to iframe", "iframe to parent"]) {
            for (const adoption of ["implicit", "explicit"]) {
                let iframe;
                cases.push({
                    name: `${adoption} adoption of ${nodeName} ${direction}`,
                    setup(fixture) {
                        iframe = createComparisonIframe(fixture);
                        const frameFixture = iframe.contentDocument.getElementById("frame-fixture");
                        const sourceDocument = direction === "parent to iframe" ? document : iframe.contentDocument;
                        const sourceParent = direction === "parent to iframe" ? fixture : frameFixture;
                        const destinationDocument =
                            direction === "parent to iframe" ? iframe.contentDocument : document;
                        const destinationParent = direction === "parent to iframe" ? frameFixture : fixture;
                        const node = createNode(sourceDocument);
                        sourceParent.appendChild(node);
                        return () => {
                            if (adoption === "explicit") destinationDocument.adoptNode(node);
                            destinationParent.appendChild(node);
                        };
                    },
                    comparisonDocuments() {
                        return [document, iframe.contentDocument];
                    },
                });
            }
        }
    }

    for (const direction of ["parent to iframe", "iframe to parent"]) {
        let iframe;
        cases.push({
            name: `implicit adoption of sibling run ${direction}`,
            setup(fixture) {
                iframe = createComparisonIframe(fixture);
                const frameFixture = iframe.contentDocument.getElementById("frame-fixture");
                const sourceDocument = direction === "parent to iframe" ? document : iframe.contentDocument;
                const sourceParent = direction === "parent to iframe" ? fixture : frameFixture;
                const destinationParent = direction === "parent to iframe" ? frameFixture : fixture;
                const nodes = ["one", "two", "three"].map(text => {
                    const node = sourceDocument.createElement("span");
                    node.textContent = text;
                    sourceParent.appendChild(node);
                    return node;
                });
                return () => destinationParent.append(...nodes);
            },
            comparisonDocuments() {
                return [document, iframe.contentDocument];
            },
        });
    }

    for (const direction of ["parent to iframe", "iframe to parent"]) {
        let iframe;
        cases.push({
            name: `adopt DocumentFragment children ${direction}`,
            setup(fixture) {
                iframe = createComparisonIframe(fixture);
                const frameFixture = iframe.contentDocument.getElementById("frame-fixture");
                const sourceDocument = direction === "parent to iframe" ? document : iframe.contentDocument;
                const destinationParent = direction === "parent to iframe" ? frameFixture : fixture;
                const fragment = sourceDocument.createDocumentFragment();
                fragment.append(
                    "fragment text",
                    sourceDocument.createElement("div"),
                    sourceDocument.createElement("span")
                );
                return () => destinationParent.appendChild(fragment);
            },
            comparisonDocuments() {
                return [document, iframe.contentDocument];
            },
        });
    }

    return cases;
}

function embeddedContentMutationCases() {
    const cases = [];
    const elementTypes = ["iframe", "object", "embed"];
    const displays = ["block", "inline", "contents", "none", "flex", "table-cell"];
    const mutations = [
        ["insert", (fixture, element) => fixture.appendChild(element)],
        ["remove", (fixture, element) => element.remove(), true],
        ["replace", (fixture, element) => element.replaceWith(document.createElement("span")), true],
        [
            "move into display contents",
            (fixture, element) => fixture.querySelector(".contents-destination").appendChild(element),
            true,
        ],
        ["move out of display contents", (fixture, element) => fixture.appendChild(element), true, true],
        [
            "hide",
            (fixture, element) => {
                element.style.display = "none";
            },
            true,
        ],
        [
            "show",
            (fixture, element, display) => {
                element.style.display = display;
            },
            true,
            false,
            "none",
        ],
        ["remove containing subtree", fixture => fixture.querySelector(".containing-subtree").remove(), true],
    ];

    for (const elementType of elementTypes) {
        for (const display of displays) {
            for (const [
                mutationName,
                mutate,
                initiallyConnected = false,
                initiallyInContents = false,
                initialDisplay = display,
            ] of mutations) {
                cases.push({
                    name: `${mutationName} ${elementType} with display ${display}`,
                    setup(fixture) {
                        const containingSubtree = document.createElement("div");
                        containingSubtree.className = "containing-subtree";
                        const contentsDestination = document.createElement("div");
                        contentsDestination.className = "contents-destination";
                        contentsDestination.style.display = "contents";
                        const element = document.createElement(elementType);
                        element.style.display = initialDisplay;
                        element.setAttribute("title", `${elementType} target`);
                        if (elementType === "object") element.innerHTML = "<span>fallback</span>";
                        if (elementType === "embed") element.type = "application/x-unknown";
                        fixture.append(containingSubtree, contentsDestination);
                        if (initiallyConnected)
                            (initiallyInContents ? contentsDestination : containingSubtree).appendChild(element);
                        return () => mutate(fixture, element, display);
                    },
                });
            }
        }
    }

    return cases;
}

function iframeDocumentMutationCases() {
    const mutations = [
        ["append element", c => c.appendChild(c.ownerDocument.createElement("div"))],
        ["append text", c => c.appendChild(c.ownerDocument.createTextNode("appended"))],
        ["remove leaf", c => c.querySelector(".leaf").remove()],
        ["remove subtree", c => c.querySelector(".subtree").remove()],
        [
            "remove sibling run",
            c =>
                Array.from(c.querySelectorAll(".sibling"))
                    .slice(1, 4)
                    .forEach(node => node.remove()),
        ],
        ["replace subtree", c => c.querySelector(".subtree").replaceWith(c.ownerDocument.createElement("strong"))],
        ["move sibling", c => c.appendChild(c.querySelector(".sibling"))],
        ["clear with replaceChildren", c => c.replaceChildren()],
        [
            "clear with innerHTML",
            c => {
                c.innerHTML = "";
            },
        ],
        [
            "insert table subtree",
            c => c.insertAdjacentHTML("beforeend", "<table><tbody><tr><td>cell</td></tr></tbody></table>"),
        ],
        [
            "insert display contents subtree",
            c => c.insertAdjacentHTML("beforeend", '<div style="display:contents"><span>contents</span></div>'),
        ],
    ];
    const cases = [];
    for (const [mutationName, mutate] of mutations) {
        let iframe;
        cases.push({
            name: `${mutationName} inside iframe document`,
            setup(fixture) {
                iframe = createComparisonIframe(fixture);
                const frameFixture = iframe.contentDocument.getElementById("frame-fixture");
                frameFixture.innerHTML =
                    '<span class="sibling leaf">one</span><div class="sibling subtree"><b>subtree</b></div><span class="sibling">three</span><span class="sibling">four</span><span class="sibling">five</span>';
                return () => mutate(frameFixture);
            },
            comparisonDocuments() {
                return [document, iframe.contentDocument];
            },
        });
    }
    return cases;
}

function objectFallbackMutationCases() {
    const mutations = [
        ["append fallback element", object => object.appendChild(document.createElement("strong"))],
        ["append fallback text", object => object.append("appended")],
        ["remove fallback leaf", object => object.querySelector(".leaf").remove()],
        ["remove fallback subtree", object => object.querySelector(".subtree").remove()],
        [
            "remove fallback sibling run",
            object =>
                Array.from(object.children)
                    .slice(1, 4)
                    .forEach(node => node.remove()),
        ],
        [
            "replace fallback subtree",
            object => object.querySelector(".subtree").replaceWith(document.createElement("strong")),
        ],
        ["clear fallback with replaceChildren", object => object.replaceChildren()],
        [
            "clear fallback with innerHTML",
            object => {
                object.innerHTML = "";
            },
        ],
        [
            "insert display contents fallback",
            object =>
                object.insertAdjacentHTML("beforeend", '<div style="display:contents"><span>contents</span></div>'),
        ],
        ["remove display contents fallback", object => object.querySelector(".contents").remove()],
        [
            "change fallback child to block",
            object => {
                object.querySelector(".leaf").style.display = "block";
            },
        ],
        [
            "change fallback child to contents",
            object => {
                object.querySelector(".leaf").style.display = "contents";
            },
        ],
    ];
    return mutations.map(([mutationName, mutate]) => ({
        name: `object ${mutationName}`,
        setup(fixture) {
            const object = document.createElement("object");
            object.innerHTML =
                '<span class="leaf">leaf</span><div class="subtree"><b>subtree</b></div><span>three</span><span>four</span><div class="contents" style="display:contents"><i>contents</i></div>';
            fixture.appendChild(object);
            return () => mutate(object);
        },
        async settleBeforeMutation() {
            await timeout(0);
        },
    }));
}

function uaShadowPseudoElementCases() {
    const cases = [];
    const placeholderHosts = ["input", "textarea"];
    for (const hostName of placeholderHosts) {
        for (const mutation of [
            "add placeholder",
            "remove placeholder",
            "hide placeholder with value",
            "show placeholder by clearing value",
            "remove host",
        ]) {
            cases.push({
                name: `::placeholder ${mutation} on ${hostName}`,
                setup(fixture) {
                    const style = document.createElement("style");
                    style.textContent = `${hostName}::placeholder { display: block; color: red; }`;
                    const host = document.createElement(hostName);
                    const creates = mutation === "add placeholder" || mutation === "show placeholder by clearing value";
                    if (!creates) host.placeholder = "placeholder";
                    if (mutation === "show placeholder by clearing value") host.value = "value";
                    fixture.append(style, host);
                    return () => {
                        if (mutation === "add placeholder") host.placeholder = "placeholder";
                        else if (mutation === "remove placeholder") host.removeAttribute("placeholder");
                        else if (mutation === "hide placeholder with value") host.value = "value";
                        else if (mutation === "show placeholder by clearing value") host.value = "";
                        else host.remove();
                    };
                },
            });
        }
    }

    for (const mutation of [
        "create by changing type",
        "remove by changing type",
        "remove file input",
        "hide file input",
        "show file input",
    ]) {
        cases.push({
            name: `::file-selector-button ${mutation}`,
            setup(fixture) {
                const style = document.createElement("style");
                style.textContent = "input::file-selector-button { display: inline-block; color: red; }";
                const input = document.createElement("input");
                const creates = mutation === "create by changing type" || mutation === "show file input";
                input.type = creates ? "text" : "file";
                if (mutation === "show file input") input.style.display = "none";
                fixture.append(style, input);
                return () => {
                    if (mutation === "create by changing type") input.type = "file";
                    else if (mutation === "remove by changing type") input.type = "text";
                    else if (mutation === "remove file input") input.remove();
                    else if (mutation === "hide file input") input.style.display = "none";
                    else input.style.display = "inline-block";
                };
            },
        });
    }

    for (const mutation of [
        "open details",
        "close details",
        "append slotted child",
        "remove slotted child",
        "remove details",
        "hide details",
        "show details",
    ]) {
        cases.push({
            name: `::details-content ${mutation}`,
            setup(fixture) {
                const style = document.createElement("style");
                style.textContent = "details::details-content { display: block; color: red; }";
                const details = document.createElement("details");
                details.innerHTML = '<summary>summary</summary><span class="content">content</span>';
                const startsOpen = mutation !== "open details";
                details.open = startsOpen;
                if (mutation === "show details") details.style.display = "none";
                fixture.append(style, details);
                return () => {
                    if (mutation === "open details") details.open = true;
                    else if (mutation === "close details") details.open = false;
                    else if (mutation === "append slotted child") details.appendChild(document.createElement("strong"));
                    else if (mutation === "remove slotted child") details.querySelector(".content").remove();
                    else if (mutation === "remove details") details.remove();
                    else if (mutation === "hide details") details.style.display = "none";
                    else details.style.display = "block";
                };
            },
        });
    }
    return cases;
}

function generatedPseudoElementCases() {
    const cases = [];

    for (const pseudo of ["before", "after"]) {
        for (const display of layoutTreeDisplayValues) {
            for (const direction of ["create", "remove"]) {
                cases.push({
                    name: `::${pseudo} ${direction} with display ${display}`,
                    setup(fixture) {
                        const style = document.createElement("style");
                        const target = document.createElement("div");
                        target.id = "pseudo-target";
                        target.textContent = "principal";
                        const generatedRule = `#pseudo-target::${pseudo} { content: "generated"; display: ${display}; }`;
                        style.textContent =
                            direction === "create" ? `#pseudo-target::${pseudo} { content: none; }` : generatedRule;
                        fixture.append(style, target);
                        return () => {
                            style.textContent =
                                direction === "create" ? generatedRule : `#pseudo-target::${pseudo} { content: none; }`;
                        };
                    },
                });
            }
        }
    }

    const contentValues = [
        '"text"',
        "attr(data-content)",
        "counter(item)",
        'open-quote "quoted" close-quote',
        "linear-gradient(red, blue)",
        '"a" / "alternative text"',
    ];
    for (const pseudo of ["before", "after"]) {
        for (const content of contentValues) {
            for (const hostDisplay of ["block", "inline", "list-item", "contents"]) {
                cases.push({
                    name: `::${pseudo} content ${content} on ${hostDisplay}`,
                    setup(fixture) {
                        const style = document.createElement("style");
                        style.textContent = `#pseudo-target::${pseudo} { content: none; }`;
                        const target = document.createElement("div");
                        target.id = "pseudo-target";
                        target.dataset.content = "attribute";
                        target.style.display = hostDisplay;
                        target.style.counterReset = "item 7";
                        target.textContent = "principal";
                        fixture.append(style, target);
                        return () => {
                            style.textContent = `#pseudo-target::${pseudo} { content: ${content}; display: inline; }`;
                        };
                    },
                });
            }
        }
    }

    for (const pseudo of ["before", "after"]) {
        for (const display of layoutTreeDisplayValues) {
            cases.push({
                name: `remove origin with ::${pseudo} display ${display}`,
                setup(fixture) {
                    const style = document.createElement("style");
                    style.textContent = `#pseudo-target::${pseudo} { content: "generated"; display: ${display}; }`;
                    const target = document.createElement("div");
                    target.id = "pseudo-target";
                    target.innerHTML = "principal <span>subtree</span>";
                    fixture.append(style, target);
                    return () => target.remove();
                },
            });
        }
    }

    return cases;
}

function markerPseudoElementCases() {
    const cases = [];
    const contents = [
        "normal",
        "none",
        '"marker"',
        'counter(list-item) "."',
        "open-quote",
        "linear-gradient(red, blue)",
    ];
    for (const hostDisplay of ["list-item", "inline list-item"]) {
        for (const position of ["inside", "outside"]) {
            for (const content of contents) {
                cases.push({
                    name: `::marker ${hostDisplay} ${position} ${content}`,
                    setup(fixture) {
                        const style = document.createElement("style");
                        style.textContent = "#marker-target::marker { content: normal; }";
                        const target = document.createElement("div");
                        target.id = "marker-target";
                        target.style.display = hostDisplay;
                        target.style.listStylePosition = position;
                        target.textContent = "item";
                        fixture.append(style, target);
                        return () => {
                            style.textContent = `#marker-target::marker { content: ${content}; }`;
                        };
                    },
                });
            }
        }
    }
    for (const position of ["inside", "outside"]) {
        for (const direction of [
            "create with content",
            "remove with content",
            "create with display",
            "remove with display",
        ]) {
            cases.push({
                name: `::marker ${direction} ${position}`,
                setup(fixture) {
                    const style = document.createElement("style");
                    const target = document.createElement("div");
                    target.id = "marker-target";
                    target.style.listStylePosition = position;
                    target.textContent = "item";
                    const usesContent = direction.endsWith("content");
                    const creates = direction.startsWith("create");
                    target.style.display = usesContent ? "list-item" : creates ? "block" : "list-item";
                    style.textContent = usesContent
                        ? `#marker-target::marker { content: ${creates ? "none" : '"marker"'}; }`
                        : "#marker-target::marker { content: normal; }";
                    fixture.append(style, target);
                    return () => {
                        if (usesContent)
                            style.textContent = `#marker-target::marker { content: ${creates ? '"marker"' : "none"}; }`;
                        else target.style.display = creates ? "list-item" : "block";
                    };
                },
            });
        }
    }
    return cases;
}

function firstLetterPseudoElementCases() {
    const cases = [];
    const texts = ["Hello", "  Hello", '"(Hello', "! Hello", "123", "\nHello"];
    for (const display of ["block", "flow-root", "inline-block", "list-item", "table-cell", "contents"]) {
        for (const text of texts) {
            cases.push({
                name: `::first-letter ${display} ${JSON.stringify(text)}`,
                setup(fixture) {
                    const style = document.createElement("style");
                    style.textContent = "#first-letter-target::first-letter { color: red; float: none; }";
                    const target = document.createElement("div");
                    target.id = "first-letter-target";
                    target.style.display = display;
                    target.textContent = text;
                    fixture.append(style, target);
                    return () => {
                        style.textContent =
                            "#first-letter-target::first-letter { color: blue; float: left; font-size: 2em; }";
                    };
                },
            });
        }
    }
    for (const direction of ["create style", "remove style", "insert text", "remove text"]) {
        cases.push({
            name: `::first-letter ${direction}`,
            setup(fixture) {
                const style = document.createElement("style");
                const target = document.createElement("div");
                target.id = "first-letter-target";
                const creates = direction.startsWith("create") || direction.startsWith("insert");
                style.textContent =
                    direction.endsWith("style") && !creates
                        ? "#first-letter-target::first-letter { color: red; float: left; }"
                        : "";
                target.textContent = direction.endsWith("text") && creates ? "" : "Hello";
                fixture.append(style, target);
                return () => {
                    if (direction.endsWith("style"))
                        style.textContent = creates
                            ? "#first-letter-target::first-letter { color: red; float: left; }"
                            : "";
                    else target.textContent = creates ? "Hello" : "";
                };
            },
        });
    }
    return cases;
}

function backdropPseudoElementCases() {
    // Internal table/ruby roles are invalid top-layer configurations and several ruby roles are
    // not implemented yet. Exercise every box-generating family that can meaningfully occur here.
    const cases = [];
    for (const display of ["none", "contents", "block", "inline", "flex", "grid"]) {
        for (const direction of ["create", "remove"]) {
            let dialog;
            cases.push({
                name: `::backdrop ${direction} with display ${display}`,
                setup(fixture) {
                    const style = document.createElement("style");
                    style.textContent = `#backdrop-target::backdrop { display: ${display}; background: red; }`;
                    dialog = document.createElement("dialog");
                    dialog.id = "backdrop-target";
                    dialog.textContent = "dialog";
                    fixture.append(style, dialog);
                    if (direction === "remove") dialog.showModal();
                    return () => (direction === "create" ? dialog.showModal() : dialog.close());
                },
                cleanup() {
                    if (dialog.open) dialog.close();
                },
            });
        }
    }
    return cases;
}

const layoutTreeTableDisplayValues = [
    "table",
    "inline-table",
    "table-row-group",
    "table-header-group",
    "table-footer-group",
    "table-row",
    "table-cell",
    "table-column-group",
    "table-column",
    "table-caption",
    "block",
    "inline",
    "contents",
    "none",
];

function tableDisplayTransitionCases() {
    const cases = [];
    for (const from of layoutTreeTableDisplayValues) {
        for (const to of layoutTreeTableDisplayValues) {
            if (from === to) continue;
            cases.push({
                name: `table display transition ${from} -> ${to}`,
                setup(fixture) {
                    const target = document.createElement("div");
                    target.style.display = from;
                    target.append("before", document.createElement("div"), "after");
                    target.firstElementChild.style.display = "table-cell";
                    fixture.appendChild(target);
                    return () => {
                        target.style.display = to;
                    };
                },
            });
        }
    }
    return cases;
}

function tableDisplayTripleCases(partition, partitionCount) {
    const cases = [];
    for (let outerIndex = 0; outerIndex < layoutTreeTableDisplayValues.length; ++outerIndex) {
        if (outerIndex % partitionCount !== partition) continue;
        for (const middleDisplay of layoutTreeTableDisplayValues) {
            for (const innerDisplay of layoutTreeTableDisplayValues) {
                const outerDisplay = layoutTreeTableDisplayValues[outerIndex];
                cases.push({
                    name: `table triple ${outerDisplay} > ${middleDisplay} > ${innerDisplay}`,
                    setup(fixture) {
                        const outer = document.createElement("div");
                        const middle = document.createElement("div");
                        const inner = document.createElement("div");
                        outer.style.display = outerDisplay;
                        middle.style.display = middleDisplay;
                        inner.style.display = innerDisplay;
                        inner.textContent = "cellular";
                        middle.append("middle-before", inner, "middle-after");
                        outer.append("outer-before", middle, "outer-after");
                        fixture.appendChild(outer);
                        return () => {
                            const inserted = document.createElement("div");
                            inserted.style.display = outerIndex % 2 ? "table-cell" : "block";
                            inserted.textContent = "inserted";
                            middle.insertBefore(inserted, inner);
                        };
                    },
                });
            }
        }
    }
    return cases;
}

const layoutTreePositionFloatModes = [
    { name: "static", position: "static", float: "none", clear: "none" },
    { name: "relative", position: "relative", float: "none", clear: "none" },
    { name: "absolute", position: "absolute", float: "none", clear: "none" },
    { name: "fixed", position: "fixed", float: "none", clear: "none" },
    { name: "sticky", position: "sticky", float: "none", clear: "none" },
    { name: "float-left", position: "static", float: "left", clear: "none" },
    { name: "float-left-clear-both", position: "static", float: "left", clear: "both" },
    { name: "float-right", position: "static", float: "right", clear: "none" },
    { name: "float-right-clear-both", position: "static", float: "right", clear: "both" },
    { name: "float-inline-start", position: "static", float: "inline-start", clear: "none" },
    { name: "float-inline-end", position: "static", float: "inline-end", clear: "none" },
    { name: "relative-float-left", position: "relative", float: "left", clear: "none" },
];

const layoutTreePositioningContextDisplays = [
    "block",
    "inline",
    "contents",
    "flow-root",
    "flex",
    "grid",
    "table",
    "inline-table",
    "table-row",
    "table-cell",
];

function applyLayoutTreePositionFloatMode(element, mode) {
    element.style.position = mode.position;
    element.style.cssFloat = mode.float;
    element.style.clear = mode.clear;
    element.style.inset = mode.position === "static" ? "auto" : "1px auto auto 2px";
}

function createPositionFloatTarget(document, display, mode, className = "position-float-target") {
    const target = document.createElement("div");
    target.className = className;
    target.style.display = display;
    applyLayoutTreePositionFloatMode(target, mode);
    target.append("target-before", document.createElement("span"), "target-after");
    target.firstElementChild.textContent = "target-child";
    return target;
}

function positionFloatInsertionCases(partition, partitionCount) {
    const cases = [];
    let caseIndex = 0;
    const addCase = (contextDisplay, targetDisplay, mode, placement) => {
        const index = caseIndex++;
        if (index % partitionCount !== partition) return;
        cases.push({
            name: `insert ${mode.name} ${targetDisplay} at ${placement} of ${contextDisplay}`,
            setup(fixture) {
                const context = document.createElement("div");
                const middle = document.createElement("span");
                context.style.display = contextDisplay;
                middle.className = "middle";
                middle.textContent = "middle";
                context.append("before", middle, document.createElement("div"), "after");
                fixture.append("fixture-before", context, "fixture-after");
                return () => {
                    const target = createPositionFloatTarget(document, targetDisplay, mode);
                    if (placement === "start") context.prepend(target);
                    else if (placement === "middle") context.insertBefore(target, middle);
                    else context.append(target);
                };
            },
        });
    };

    for (const contextDisplay of layoutTreePositioningContextDisplays) {
        for (const targetDisplay of layoutTreeDisplayValues) {
            for (let modeIndex = 0; modeIndex < layoutTreePositionFloatModes.length; ++modeIndex) {
                const mode = layoutTreePositionFloatModes[modeIndex];
                addCase(contextDisplay, targetDisplay, mode, ["start", "middle", "end"][modeIndex % 3]);
            }
        }
    }

    for (let contextIndex = 0; contextIndex < layoutTreeDisplayValues.length; ++contextIndex) {
        const contextDisplay = layoutTreeDisplayValues[contextIndex];
        if (layoutTreePositioningContextDisplays.includes(contextDisplay)) continue;
        for (let modeIndex = 0; modeIndex < layoutTreePositionFloatModes.length; ++modeIndex) {
            const targetDisplay = layoutTreeDisplayValues[(contextIndex + modeIndex) % layoutTreeDisplayValues.length];
            addCase(
                contextDisplay,
                targetDisplay,
                layoutTreePositionFloatModes[modeIndex],
                ["start", "middle", "end"][(contextIndex + modeIndex) % 3]
            );
        }
    }
    return cases;
}

function layoutTreePositionFloatTransitions() {
    const transitions = [];
    for (const from of ["static", "relative", "absolute", "fixed", "sticky"]) {
        for (const to of ["static", "relative", "absolute", "fixed", "sticky"]) {
            if (from !== to)
                transitions.push({ name: `position ${from} -> ${to}`, from: { position: from }, to: { position: to } });
        }
    }
    for (const from of ["none", "left", "right", "inline-start", "inline-end"]) {
        for (const to of ["none", "left", "right", "inline-start", "inline-end"]) {
            if (from !== to)
                transitions.push({ name: `float ${from} -> ${to}`, from: { float: from }, to: { float: to } });
        }
    }
    for (const from of ["none", "left", "right", "both"]) {
        for (const to of ["none", "left", "right", "both"]) {
            if (from !== to)
                transitions.push({ name: `clear ${from} -> ${to}`, from: { clear: from }, to: { clear: to } });
        }
    }
    transitions.push(
        {
            name: "static none -> absolute left",
            from: { position: "static", float: "none" },
            to: { position: "absolute", float: "left" },
        },
        {
            name: "absolute left -> static none",
            from: { position: "absolute", float: "left" },
            to: { position: "static", float: "none" },
        },
        {
            name: "relative right -> fixed none",
            from: { position: "relative", float: "right" },
            to: { position: "fixed", float: "none" },
        },
        {
            name: "fixed none -> sticky right",
            from: { position: "fixed", float: "none" },
            to: { position: "sticky", float: "right" },
        }
    );
    return transitions;
}

function positionFloatTransitionCases(partition, partitionCount) {
    const transitions = layoutTreePositionFloatTransitions();
    const cases = [];
    let caseIndex = 0;
    for (let displayIndex = 0; displayIndex < layoutTreeDisplayValues.length; ++displayIndex) {
        const display = layoutTreeDisplayValues[displayIndex];
        for (let transitionIndex = 0; transitionIndex < transitions.length; ++transitionIndex) {
            const transition = transitions[transitionIndex];
            const index = caseIndex++;
            if (index % partitionCount !== partition) continue;
            const contextDisplay =
                layoutTreePositioningContextDisplays[
                    (displayIndex + transitionIndex) % layoutTreePositioningContextDisplays.length
                ];
            cases.push({
                name: `${transition.name} on ${display} in ${contextDisplay}`,
                setup(fixture) {
                    const context = document.createElement("div");
                    context.style.display = contextDisplay;
                    const target = document.createElement("div");
                    target.style.display = display;
                    target.style.position = transition.from.position || "static";
                    target.style.cssFloat = transition.from.float || "none";
                    target.style.clear = transition.from.clear || "none";
                    target.style.inset = "1px auto auto 2px";
                    target.append("before", document.createElement("span"), "after");
                    context.append("context-before", target, document.createElement("div"), "context-after");
                    fixture.appendChild(context);
                    return () => {
                        if (transition.to.position) target.style.position = transition.to.position;
                        if (transition.to.float) target.style.cssFloat = transition.to.float;
                        if (transition.to.clear) target.style.clear = transition.to.clear;
                    };
                },
            });
        }
    }
    return cases;
}

function positionFloatInvalidationScopeCases(partition, partitionCount) {
    const transitions = layoutTreePositionFloatTransitions();
    const cases = [];
    let caseIndex = 0;
    for (let displayIndex = 0; displayIndex < layoutTreeDisplayValues.length; ++displayIndex) {
        const display = layoutTreeDisplayValues[displayIndex];
        for (let transitionIndex = 0; transitionIndex < transitions.length; ++transitionIndex) {
            const transition = transitions[transitionIndex];
            const index = caseIndex++;
            if (index % partitionCount !== partition) continue;
            const contextDisplay =
                layoutTreeInvalidationScopeContextDisplays[
                    (displayIndex + transitionIndex) % layoutTreeInvalidationScopeContextDisplays.length
                ];
            cases.push(
                createLayoutTreeInvalidationScopeCase(
                    `${transition.name} on ${display} in ${contextDisplay}`,
                    contextDisplay,
                    display,
                    target => {
                        target.style.position = transition.from.position || "static";
                        target.style.cssFloat = transition.from.float || "none";
                        target.style.clear = transition.from.clear || "none";
                        target.style.inset = "1px auto auto 2px";
                    },
                    target => {
                        if (transition.to.position) target.style.position = transition.to.position;
                        if (transition.to.float) target.style.cssFloat = transition.to.float;
                        if (transition.to.clear) target.style.clear = transition.to.clear;
                    }
                )
            );
        }
    }
    return cases;
}

function otherStyleInvalidationScopeTransitions() {
    const transitions = [];
    const addCrossProduct = (property, values) => {
        for (const from of values) {
            for (const to of values) {
                if (from === to) continue;
                transitions.push({ name: `${property} ${from} -> ${to}`, property, from, to });
            }
        }
    };

    addCrossProduct("content-visibility", ["visible", "auto", "hidden"]);
    addCrossProduct("overflow-x", ["visible", "hidden", "clip", "scroll", "auto"]);
    addCrossProduct("overflow-y", ["visible", "hidden", "clip", "scroll", "auto"]);
    addCrossProduct("text-transform", ["none", "uppercase", "lowercase", "capitalize"]);
    addCrossProduct("content", ["normal", "none", '"replacement"', 'open-quote "replacement" close-quote']);
    return transitions;
}

function otherStyleInvalidationScopeCases(partition, partitionCount) {
    const transitions = otherStyleInvalidationScopeTransitions();
    const cases = [];
    let caseIndex = 0;
    for (let displayIndex = 0; displayIndex < layoutTreeDisplayValues.length; ++displayIndex) {
        const display = layoutTreeDisplayValues[displayIndex];
        for (let transitionIndex = 0; transitionIndex < transitions.length; ++transitionIndex) {
            const transition = transitions[transitionIndex];
            const index = caseIndex++;
            if (index % partitionCount !== partition) continue;
            const contextDisplay =
                layoutTreeInvalidationScopeContextDisplays[
                    (displayIndex * transitions.length + transitionIndex) %
                        layoutTreeInvalidationScopeContextDisplays.length
                ];
            cases.push(
                createLayoutTreeInvalidationScopeCase(
                    `${transition.name} on ${display} in ${contextDisplay}`,
                    contextDisplay,
                    display,
                    target => target.style.setProperty(transition.property, transition.from),
                    target => target.style.setProperty(transition.property, transition.to)
                )
            );
        }
    }
    return cases;
}

function pseudoElementInvalidationScopeCases(partition, partitionCount) {
    const actions = ["create", "remove", "change display", "change float", "change clear", "change position"];
    const cases = [];
    let caseIndex = 0;
    for (let originDisplayIndex = 0; originDisplayIndex < layoutTreeDisplayValues.length; ++originDisplayIndex) {
        const originDisplay = layoutTreeDisplayValues[originDisplayIndex];
        for (let pseudoIndex = 0; pseudoIndex < 2; ++pseudoIndex) {
            const pseudo = ["before", "after"][pseudoIndex];
            for (
                let pseudoDisplayIndex = 0;
                pseudoDisplayIndex < layoutTreeDisplayValues.length;
                ++pseudoDisplayIndex
            ) {
                const pseudoDisplay = layoutTreeDisplayValues[pseudoDisplayIndex];
                const action = actions[(originDisplayIndex + pseudoIndex + pseudoDisplayIndex) % actions.length];
                const index = caseIndex++;
                if (index % partitionCount !== partition) continue;
                const contextDisplay =
                    layoutTreeInvalidationScopeContextDisplays[
                        (originDisplayIndex + pseudoIndex + pseudoDisplayIndex) %
                            layoutTreeInvalidationScopeContextDisplays.length
                    ];
                const nextDisplay = layoutTreeDisplayValues[(pseudoDisplayIndex + 1) % layoutTreeDisplayValues.length];
                let style;
                const rule = state => {
                    const content = state === "without content" ? "none" : '"generated"';
                    const display = state === "next display" ? nextDisplay : pseudoDisplay;
                    const float = state === "next float" ? "left" : "none";
                    const clear = state === "next clear" ? "both" : "none";
                    const position = state === "next position" ? "absolute" : "static";
                    return `#scope-pseudo-target::${pseudo} { content: ${content}; display: ${display}; float: ${float}; clear: ${clear}; position: ${position}; }`;
                };
                cases.push(
                    createLayoutTreeInvalidationScopeCase(
                        `::${pseudo} ${action} ${pseudoDisplay} on ${originDisplay} in ${contextDisplay}`,
                        contextDisplay,
                        originDisplay,
                        target => {
                            target.id = "scope-pseudo-target";
                            target.previousElementSibling.style.display = "inline-block";
                            target.nextElementSibling.style.display = "inline-block";
                            style = document.createElement("style");
                            style.textContent = rule(action === "create" ? "without content" : "initial");
                            target.parentNode.parentNode.prepend(style);
                        },
                        () => {
                            if (action === "create") style.textContent = rule("initial");
                            else if (action === "remove") style.textContent = rule("without content");
                            else if (action === "change display") style.textContent = rule("next display");
                            else if (action === "change float") style.textContent = rule("next float");
                            else if (action === "change clear") style.textContent = rule("next clear");
                            else style.textContent = rule("next position");
                        }
                    )
                );
            }
        }
    }
    return cases;
}

function positionFloatStructuralMutationCases(partition, partitionCount) {
    const operations = [
        ["remove individual node", state => state.nodes[1].remove()],
        ["remove subtree", state => state.nodes[2].remove()],
        ["remove first sibling run", state => state.nodes.slice(0, 2).forEach(node => node.remove())],
        ["remove middle sibling run", state => state.nodes.slice(1, 4).forEach(node => node.remove())],
        ["remove last sibling run", state => state.nodes.slice(-2).forEach(node => node.remove())],
        ["remove all siblings forwards", state => state.container.replaceChildren()],
        ["replace individual node", state => state.nodes[1].replaceWith(document.createElement("strong"))],
        [
            "replace sibling run",
            state => state.container.replaceChildren("replacement", document.createElement("strong")),
        ],
        ["move node to start", state => state.container.prepend(state.nodes[2])],
        ["move node to end", state => state.container.append(state.nodes[1])],
        ["move subtree across parents", state => state.destination.append(state.nodes[2])],
        ["move sibling run across parents", state => state.destination.append(...state.nodes.slice(1, 4))],
        [
            "append positioned fragment",
            state => {
                const fragment = document.createDocumentFragment();
                fragment.append(state.nodes[1], state.nodes[2]);
                state.destination.append(fragment);
            },
        ],
        ["clear with innerHTML", state => (state.container.innerHTML = "")],
    ];
    const cases = [];
    let caseIndex = 0;
    for (let displayIndex = 0; displayIndex < layoutTreeDisplayValues.length; ++displayIndex) {
        const hostDisplay = layoutTreeDisplayValues[displayIndex];
        for (let modeIndex = 0; modeIndex < layoutTreePositionFloatModes.length; ++modeIndex) {
            const mode = layoutTreePositionFloatModes[modeIndex];
            for (let operationIndex = 0; operationIndex < operations.length; ++operationIndex) {
                const [operationName, operate] = operations[operationIndex];
                const index = caseIndex++;
                if (index % partitionCount !== partition) continue;
                const targetDisplay =
                    layoutTreeDisplayValues[
                        (displayIndex + modeIndex + operationIndex) % layoutTreeDisplayValues.length
                    ];
                cases.push({
                    name: `${operationName}, ${mode.name} ${targetDisplay} siblings in ${hostDisplay}`,
                    setup(fixture) {
                        const container = document.createElement("div");
                        const destination = document.createElement("div");
                        container.style.display = hostDisplay;
                        destination.style.display = operationIndex % 2 ? "contents" : "block";
                        destination.className = "destination";
                        const nodes = [];
                        for (let index = 0; index < 5; ++index) {
                            const node = createPositionFloatTarget(
                                document,
                                targetDisplay,
                                mode,
                                `position-float-target target-${index}`
                            );
                            node.appendChild(document.createElement("em")).textContent = `deep-${index}`;
                            nodes.push(node);
                        }
                        container.append("before", ...nodes, "after");
                        fixture.append(container, destination);
                        return () => operate({ container, destination, nodes });
                    },
                });
            }
        }
    }
    return cases;
}

function positionedGeneratedPseudoElementCases(partition, partitionCount) {
    const actions = ["create", "remove", "change mode", "remove origin"];
    const cases = [];
    let caseIndex = 0;
    for (let hostDisplayIndex = 0; hostDisplayIndex < layoutTreeDisplayValues.length; ++hostDisplayIndex) {
        const hostDisplay = layoutTreeDisplayValues[hostDisplayIndex];
        for (const pseudo of ["before", "after"]) {
            for (let modeIndex = 0; modeIndex < layoutTreePositionFloatModes.length; ++modeIndex) {
                const mode = layoutTreePositionFloatModes[modeIndex];
                for (let actionIndex = 0; actionIndex < actions.length; ++actionIndex) {
                    const action = actions[actionIndex];
                    const nextMode =
                        layoutTreePositionFloatModes[(modeIndex + 1) % layoutTreePositionFloatModes.length];
                    const index = caseIndex++;
                    if (index % partitionCount !== partition) continue;
                    const pseudoDisplay =
                        layoutTreeDisplayValues[
                            (hostDisplayIndex + modeIndex + actionIndex) % layoutTreeDisplayValues.length
                        ];
                    cases.push({
                        name: `::${pseudo} ${
                            action === "change mode"
                                ? `${action} ${mode.name} -> ${nextMode.name}`
                                : `${action}, ${mode.name}`
                        } ${pseudoDisplay} on ${hostDisplay}`,
                        setup(fixture) {
                            const style = document.createElement("style");
                            const target = document.createElement("div");
                            target.id = "positioned-pseudo-target";
                            target.style.display = hostDisplay;
                            target.append("principal-before", document.createElement("span"), "principal-after");
                            const rule = (ruleMode, hasContent) =>
                                `#positioned-pseudo-target::${pseudo} { content: ${
                                    hasContent ? '"generated"' : "none"
                                }; display: ${pseudoDisplay}; position: ${ruleMode.position}; float: ${
                                    ruleMode.float
                                }; clear: ${ruleMode.clear}; inset: 1px auto auto 2px; }`;
                            style.textContent = rule(mode, action !== "create");
                            fixture.append(style, target);
                            return () => {
                                if (action === "create") style.textContent = rule(mode, true);
                                else if (action === "remove") style.textContent = rule(mode, false);
                                else if (action === "change mode") style.textContent = rule(nextMode, true);
                                else target.remove();
                            };
                        },
                    });
                }
            }
        }
    }
    return cases;
}

function positionedContainingBlockMutationCases(partition, partitionCount) {
    const actions = [
        "make ancestor relative",
        "make ancestor static",
        "move target to relative destination",
        "move ancestor across parents",
        "wrap target in relative block",
        "unwrap target from relative block",
    ];
    const cases = [];
    let caseIndex = 0;
    for (let ancestorDisplayIndex = 0; ancestorDisplayIndex < layoutTreeDisplayValues.length; ++ancestorDisplayIndex) {
        const ancestorDisplay = layoutTreeDisplayValues[ancestorDisplayIndex];
        for (let targetDisplayIndex = 0; targetDisplayIndex < layoutTreeDisplayValues.length; ++targetDisplayIndex) {
            const targetDisplay = layoutTreeDisplayValues[targetDisplayIndex];
            for (const position of ["absolute", "fixed"]) {
                const action =
                    actions[
                        (ancestorDisplayIndex + targetDisplayIndex + (position === "fixed" ? 1 : 0)) % actions.length
                    ];
                const index = caseIndex++;
                if (index % partitionCount !== partition) continue;
                cases.push({
                    name: `${action}, ${position} ${targetDisplay} under ${ancestorDisplay}`,
                    setup(fixture) {
                        const outer = document.createElement("div");
                        const ancestor = document.createElement("div");
                        const wrapper = document.createElement("div");
                        const destination = document.createElement("div");
                        const target = document.createElement("div");
                        outer.style.position = "relative";
                        ancestor.style.display = ancestorDisplay;
                        ancestor.style.position = action === "make ancestor static" ? "relative" : "static";
                        wrapper.style.position = "relative";
                        destination.style.position = "relative";
                        target.style.display = targetDisplay;
                        target.style.position = position;
                        target.style.inset = "1px auto auto 2px";
                        target.append("target-before", document.createElement("span"), "target-after");
                        if (action === "unwrap target from relative block") wrapper.appendChild(target);
                        else ancestor.append("ancestor-before", target, "ancestor-after");
                        outer.append(ancestor, destination);
                        if (action === "unwrap target from relative block")
                            ancestor.append("ancestor-before", wrapper, "ancestor-after");
                        fixture.append(outer, document.createElement("div"));
                        return () => {
                            if (action === "make ancestor relative") ancestor.style.position = "relative";
                            else if (action === "make ancestor static") ancestor.style.position = "static";
                            else if (action === "move target to relative destination") destination.appendChild(target);
                            else if (action === "move ancestor across parents") fixture.lastChild.appendChild(ancestor);
                            else if (action === "wrap target in relative block") {
                                target.replaceWith(wrapper);
                                wrapper.appendChild(target);
                            } else wrapper.replaceWith(target);
                        };
                    },
                });
            }
        }
    }
    return cases;
}

function semanticTableMutationCases() {
    const descriptions = [
        [
            "table",
            "<table id=t><tbody><tr><td>A</td></tr></tbody></table>",
            t => t.appendChild(document.createElement("tr")),
        ],
        ["direct cell under table", "<table id=t></table>", t => t.appendChild(document.createElement("td"))],
        [
            "direct cell under tbody",
            "<table><tbody id=t></tbody></table>",
            t => t.appendChild(document.createElement("td")),
        ],
        [
            "block under row",
            "<table><tbody><tr id=t><td>A</td></tr></tbody></table>",
            t => t.appendChild(document.createElement("div")),
        ],
        [
            "row under cell",
            "<table><tbody><tr><td id=t>A</td></tr></tbody></table>",
            t => t.appendChild(document.createElement("tr")),
        ],
        [
            "caption under row",
            "<table><tbody><tr id=t><td>A</td></tr></tbody></table>",
            t => t.appendChild(document.createElement("caption")),
        ],
        [
            "second caption",
            "<table id=t><caption>A</caption><tbody><tr><td>B</td></tr></tbody></table>",
            t => t.appendChild(document.createElement("caption")),
        ],
        [
            "colgroup after body",
            "<table id=t><tbody><tr><td>A</td></tr></tbody></table>",
            t => t.appendChild(document.createElement("colgroup")),
        ],
        [
            "column under body",
            "<table><tbody id=t><tr><td>A</td></tr></tbody></table>",
            t => t.appendChild(document.createElement("col")),
        ],
        [
            "non-whitespace table text",
            "<table id=t><tbody><tr><td>A</td></tr></tbody></table>",
            t => t.appendChild(document.createTextNode("text")),
        ],
        [
            "whitespace table text",
            "<table id=t><tbody><tr><td>A</td></tr></tbody></table>",
            t => t.appendChild(document.createTextNode("  \n  ")),
        ],
        [
            "remove only cell",
            "<table><tbody><tr id=t><td>A</td></tr></tbody></table>",
            t => t.firstElementChild.remove(),
        ],
        [
            "remove only row",
            "<table><tbody id=t><tr><td>A</td></tr></tbody></table>",
            t => t.firstElementChild.remove(),
        ],
        [
            "remove row group",
            "<table id=t><tbody><tr><td>A</td></tr></tbody></table>",
            t => t.firstElementChild.remove(),
        ],
        [
            "replace cell with block",
            "<table><tbody><tr id=t><td>A</td></tr></tbody></table>",
            t => t.replaceChildren(document.createElement("div")),
        ],
    ];

    return descriptions.map(([name, markup, mutate]) => ({
        name: `semantic table: ${name}`,
        setup(fixture) {
            fixture.innerHTML = markup;
            const target = fixture.querySelector("#t");
            return () => mutate(target);
        },
    }));
}

function tableFixupIncrementalCases() {
    const cases = [];
    const add = (name, markup, mutate) => {
        cases.push({
            name: `table fixup: ${name}`,
            setup(fixture) {
                fixture.innerHTML = markup;
                return () => mutate(fixture);
            },
        });
    };
    const cell = text => {
        const element = document.createElement("td");
        element.textContent = text;
        return element;
    };
    const row = (...texts) => {
        const element = document.createElement("tr");
        element.append(...texts.map(cell));
        return element;
    };
    const displayed = (display, text = "inserted") => {
        const element = document.createElement("div");
        element.style.display = display;
        element.textContent = text;
        return element;
    };

    const twoRowTable =
        '<table><tbody id="target"><tr><td>A</td><td>B</td></tr><tr><td>C</td><td>D</td></tr></tbody></table>';
    add("prepend row to tbody", twoRowTable, fixture => fixture.querySelector("#target").prepend(row("E", "F")));
    add("insert row in tbody", twoRowTable, fixture => {
        const target = fixture.querySelector("#target");
        target.insertBefore(row("E", "F"), target.lastElementChild);
    });
    add("append row to tbody", twoRowTable, fixture => fixture.querySelector("#target").append(row("E", "F")));
    add("append rows with fragment", twoRowTable, fixture => {
        const fragment = new DocumentFragment();
        fragment.append(row("E", "F"), row("G", "H"));
        fixture.querySelector("#target").append(fragment);
    });
    add("append row to thead", '<table><thead id="target"><tr><td>A</td></tr></thead></table>', fixture =>
        fixture.querySelector("#target").append(row("B"))
    );
    add("append row to tfoot", '<table><tfoot id="target"><tr><td>A</td></tr></tfoot></table>', fixture =>
        fixture.querySelector("#target").append(row("B"))
    );
    add("append direct row to table", '<table id="target"><tr><td>A</td></tr></table>', fixture =>
        fixture.querySelector("#target").append(row("B"))
    );
    add("append CSS row to tbody", twoRowTable, fixture => {
        const inserted = displayed("table-row", "");
        inserted.append(displayed("table-cell", "E"), displayed("table-cell", "F"));
        fixture.querySelector("#target").append(inserted);
    });
    add("append narrower row", twoRowTable, fixture => fixture.querySelector("#target").append(row("E")));
    add("append wider row", twoRowTable, fixture => fixture.querySelector("#target").append(row("E", "F", "G")));
    add("append spanning row", twoRowTable, fixture => {
        const inserted = row("spanning");
        inserted.firstElementChild.colSpan = 3;
        fixture.querySelector("#target").append(inserted);
    });

    const twoCellRow = '<table><tbody><tr id="target"><td>A</td><td>B</td></tr></tbody></table>';
    add("prepend cell to row", twoCellRow, fixture => fixture.querySelector("#target").prepend(cell("C")));
    add("insert cell in row", twoCellRow, fixture => {
        const target = fixture.querySelector("#target");
        target.insertBefore(cell("C"), target.lastElementChild);
    });
    add("append cell to row", twoCellRow, fixture => fixture.querySelector("#target").append(cell("C")));
    add("append CSS cell to row", twoCellRow, fixture =>
        fixture.querySelector("#target").append(displayed("table-cell", "C"))
    );
    add("remove first cell", twoCellRow, fixture => fixture.querySelector("#target").firstElementChild.remove());
    add("remove last cell", twoCellRow, fixture => fixture.querySelector("#target").lastElementChild.remove());
    add("replace cells with one cell", twoCellRow, fixture =>
        fixture.querySelector("#target").replaceChildren(cell("C"))
    );
    add("change cell colspan", twoCellRow, fixture => {
        fixture.querySelector("#target").firstElementChild.colSpan = 3;
    });
    add("change cell rowspan", twoCellRow, fixture => {
        fixture.querySelector("#target").firstElementChild.rowSpan = 3;
    });

    add("remove first row", twoRowTable, fixture => fixture.querySelector("#target").firstElementChild.remove());
    add("remove last row", twoRowTable, fixture => fixture.querySelector("#target").lastElementChild.remove());
    add("replace rows", twoRowTable, fixture => fixture.querySelector("#target").replaceChildren(row("E", "F")));
    add("move row within group", twoRowTable, fixture => {
        const target = fixture.querySelector("#target");
        target.prepend(target.lastElementChild);
    });
    add(
        "move row between groups",
        '<table><tbody id="source"><tr id="moving"><td>A</td></tr></tbody><tbody id="target"><tr><td>B</td></tr></tbody></table>',
        fixture => fixture.querySelector("#target").append(fixture.querySelector("#moving"))
    );
    add(
        "move row between tables",
        '<table><tbody><tr id="moving"><td>A</td></tr></tbody></table><table><tbody id="target"><tr><td>B</td></tr></tbody></table>',
        fixture => fixture.querySelector("#target").append(fixture.querySelector("#moving"))
    );
    add(
        "move cell between rows",
        '<table><tbody><tr><td id="moving">A</td></tr><tr id="target"><td>B</td></tr></tbody></table>',
        fixture => fixture.querySelector("#target").append(fixture.querySelector("#moving"))
    );

    add("append tbody", '<table id="target"><tbody><tr><td>A</td></tr></tbody></table>', fixture => {
        const body = document.createElement("tbody");
        body.append(row("B"));
        fixture.querySelector("#target").append(body);
    });
    add("prepend thead", '<table id="target"><tbody><tr><td>A</td></tr></tbody></table>', fixture => {
        const head = document.createElement("thead");
        head.append(row("B"));
        fixture.querySelector("#target").prepend(head);
    });
    add("append tfoot", '<table id="target"><tbody><tr><td>A</td></tr></tbody></table>', fixture => {
        const foot = document.createElement("tfoot");
        foot.append(row("B"));
        fixture.querySelector("#target").append(foot);
    });
    add("remove tbody", '<table><tbody id="target"><tr><td>A</td></tr></tbody></table>', fixture =>
        fixture.querySelector("#target").remove()
    );
    add(
        "move tbody between tables",
        '<table><tbody id="moving"><tr><td>A</td></tr></tbody></table><table id="target"></table>',
        fixture => fixture.querySelector("#target").append(fixture.querySelector("#moving"))
    );

    add("prepend caption", '<table id="target"><tbody><tr><td>A</td></tr></tbody></table>', fixture => {
        const caption = document.createElement("caption");
        caption.textContent = "caption";
        fixture.querySelector("#target").prepend(caption);
    });
    add(
        "append second caption",
        '<table id="target"><caption>A</caption><tbody><tr><td>B</td></tr></tbody></table>',
        fixture => {
            const caption = document.createElement("caption");
            caption.textContent = "second";
            fixture.querySelector("#target").append(caption);
        }
    );
    add(
        "remove caption",
        '<table><caption id="target">A</caption><tbody><tr><td>B</td></tr></tbody></table>',
        fixture => fixture.querySelector("#target").remove()
    );
    add("append colgroup", '<table id="target"><tbody><tr><td>A</td></tr></tbody></table>', fixture => {
        const group = document.createElement("colgroup");
        group.append(document.createElement("col"));
        fixture.querySelector("#target").append(group);
    });
    add(
        "append column to colgroup",
        '<table><colgroup id="target"><col></colgroup><tbody><tr><td>A</td></tr></tbody></table>',
        fixture => fixture.querySelector("#target").append(document.createElement("col"))
    );
    add(
        "remove column",
        '<table><colgroup><col id="target"><col></colgroup><tbody><tr><td>A</td></tr></tbody></table>',
        fixture => fixture.querySelector("#target").remove()
    );

    add("insert cell under table", '<table id="target"><tbody><tr><td>A</td></tr></tbody></table>', fixture =>
        fixture.querySelector("#target").append(cell("B"))
    );
    add("insert cell under row group", '<table><tbody id="target"><tr><td>A</td></tr></tbody></table>', fixture =>
        fixture.querySelector("#target").append(cell("B"))
    );
    add("insert cell under block", '<div id="target"></div>', fixture =>
        fixture.querySelector("#target").append(displayed("table-cell"))
    );
    add("insert row under cell", '<table><tbody><tr><td id="target">A</td></tr></tbody></table>', fixture =>
        fixture.querySelector("#target").append(row("B"))
    );
    add("insert row under block", '<div id="target"></div>', fixture =>
        fixture.querySelector("#target").append(displayed("table-row"))
    );
    add("insert column under row group", '<table><tbody id="target"><tr><td>A</td></tr></tbody></table>', fixture =>
        fixture.querySelector("#target").append(document.createElement("col"))
    );
    add("insert caption under row", twoCellRow, fixture => {
        const caption = document.createElement("caption");
        caption.textContent = "caption";
        fixture.querySelector("#target").append(caption);
    });
    add("insert block under table", '<table id="target"><tbody><tr><td>A</td></tr></tbody></table>', fixture =>
        fixture.querySelector("#target").append(document.createElement("div"))
    );
    add("insert block under row group", '<table><tbody id="target"><tr><td>A</td></tr></tbody></table>', fixture =>
        fixture.querySelector("#target").append(document.createElement("div"))
    );
    add("insert block under row", twoCellRow, fixture =>
        fixture.querySelector("#target").append(document.createElement("div"))
    );
    add("insert text under table", '<table id="target"><tbody><tr><td>A</td></tr></tbody></table>', fixture =>
        fixture.querySelector("#target").append("text")
    );
    add("insert whitespace under table", '<table id="target"><tbody><tr><td>A</td></tr></tbody></table>', fixture =>
        fixture.querySelector("#target").append("  \n  ")
    );
    add("insert text under row group", '<table><tbody id="target"><tr><td>A</td></tr></tbody></table>', fixture =>
        fixture.querySelector("#target").append("text")
    );
    add("insert text under row", twoCellRow, fixture => fixture.querySelector("#target").append("text"));

    add(
        "append row to collapsed table",
        '<table style="border-collapse:collapse"><tbody id="target"><tr><td>A</td><td>B</td></tr></tbody></table>',
        fixture => fixture.querySelector("#target").append(row("C", "D"))
    );
    add("append nested table", '<table><tbody><tr><td id="target">outer</td></tr></tbody></table>', fixture => {
        const table = document.createElement("table");
        const body = document.createElement("tbody");
        body.append(row("inner"));
        table.append(body);
        fixture.querySelector("#target").append(table);
    });
    add(
        "append row to nested table",
        '<table><tbody><tr><td>outer<table><tbody id="target"><tr><td>inner</td></tr></tbody></table></td></tr></tbody></table>',
        fixture => fixture.querySelector("#target").append(row("second"))
    );
    add(
        "move nested table between cells",
        '<table><tbody><tr><td><table id="moving"><tbody><tr><td>inner</td></tr></tbody></table></td><td id="target"></td></tr></tbody></table>',
        fixture => fixture.querySelector("#target").append(fixture.querySelector("#moving"))
    );
    add(
        "append outer row preserves nested missing cells",
        '<table><tbody id="target"><tr><td>outer<table><tbody><tr><td>A</td><td>B</td></tr><tr><td>C</td></tr></tbody></table></td></tr></tbody></table>',
        fixture => fixture.querySelector("#target").append(row("outer second"))
    );
    add(
        "remove outer column preserves nested missing cells",
        '<table><colgroup><col id="target"><col></colgroup><tbody><tr><td>outer<table><tbody><tr><td>A</td><td>B</td></tr><tr><td>C</td></tr></tbody></table></td></tr></tbody></table>',
        fixture => fixture.querySelector("#target").remove()
    );
    add(
        "append outer row preserves deeply nested missing cells",
        '<table><tbody id="target"><tr><td>outer<table><tbody><tr><td>middle<table><tbody><tr><td>A</td><td>B</td></tr><tr><td>C</td></tr></tbody></table></td></tr></tbody></table></td></tr></tbody></table>',
        fixture => fixture.querySelector("#target").append(row("outer second"))
    );

    return cases;
}

function anonymousWrapperCases() {
    const cases = [];
    for (const inlineDisplay of ["inline", "inline flow", "contents"]) {
        for (const insertedDisplay of ["block", "flow-root", "flex", "grid", "table", "list-item"]) {
            for (const depth of [1, 2, 3, 4]) {
                for (const position of ["start", "middle", "end"]) {
                    for (const mutation of [
                        "insert block",
                        "remove block",
                        "insert second block",
                        "remove first block",
                        "remove second block",
                        "block to inline",
                        "inline to block",
                    ]) {
                        cases.push({
                            name: `anonymous wrapper ${mutation}, ${inlineDisplay}, ${insertedDisplay}, depth ${depth}, ${position}`,
                            setup(fixture) {
                                let outer = document.createElement("span");
                                outer.style.display = inlineDisplay;
                                fixture.append("fixture-before", outer, "fixture-after");
                                let insertionParent = outer;
                                for (let i = 1; i < depth; ++i) {
                                    const nested = document.createElement("span");
                                    nested.style.display = inlineDisplay;
                                    insertionParent.append("level-before", nested, "level-after");
                                    insertionParent = nested;
                                }
                                insertionParent.append("before", document.createElement("i"), "after");
                                const makeInserted = () => {
                                    const inserted = document.createElement("div");
                                    inserted.style.display =
                                        mutation === "inline to block" ? "inline" : insertedDisplay;
                                    inserted.textContent = "inserted";
                                    return inserted;
                                };
                                const insertAtPosition = inserted => {
                                    if (position === "start") insertionParent.prepend(inserted);
                                    else if (position === "middle")
                                        insertionParent.insertBefore(inserted, insertionParent.lastChild);
                                    else insertionParent.append(inserted);
                                };
                                const needsInitialBlock = mutation !== "insert block" && mutation !== "inline to block";
                                let firstInserted;
                                let secondInserted;
                                if (needsInitialBlock || mutation === "inline to block") {
                                    firstInserted = makeInserted();
                                    insertAtPosition(firstInserted);
                                }
                                if (mutation === "remove first block" || mutation === "remove second block") {
                                    secondInserted = makeInserted();
                                    firstInserted.after(secondInserted);
                                }
                                return () => {
                                    if (mutation === "insert block") insertAtPosition(makeInserted());
                                    else if (mutation === "remove block") firstInserted.remove();
                                    else if (mutation === "insert second block") firstInserted.after(makeInserted());
                                    else if (mutation === "remove first block") firstInserted.remove();
                                    else if (mutation === "remove second block") secondInserted.remove();
                                    else if (mutation === "block to inline") firstInserted.style.display = "inline";
                                    else firstInserted.style.display = insertedDisplay;
                                };
                            },
                        });
                    }
                }
            }
        }
    }
    return cases;
}
