let selectedTopTab = null;
let selectedTopTabButton = null;

let selectedBottomTab = null;
let selectedBottomTabButton = null;

let selectedDOMNode = null;
let pendingEditDOMNode = null;

let visibleDOMNodes = [];

let consoleGroupStack = [];
let consoleGroupNextID = 0;

let consoleHistory = [];
let consoleHistoryIndex = 0;

const decodeBase64 = encoded => {
    return new TextDecoder().decode(Uint8Array.from(atob(encoded), c => c.charCodeAt(0)));
};

const beginSplitViewDrag = () => {
    let inspectorTop = document.getElementById("inspector-top");
    let inspectorBottom = document.getElementById("inspector-bottom");
    let inspectorSeparator = document.getElementById("inspector-separator");

    const windowHeight = window.innerHeight;
    const separatorHeight = inspectorSeparator.clientHeight;

    const updateSplitView = event => {
        let position = Math.min(event.clientY, windowHeight - separatorHeight);
        position = Math.max(position, 0);

        inspectorTop.style.height = `${position}px`;
        inspectorBottom.style.height = `${windowHeight - position - separatorHeight}px`;

        event.preventDefault();
    };

    const endSplitViewDrag = () => {
        document.removeEventListener("mousemove", updateSplitView);
        document.removeEventListener("mouseup", endSplitViewDrag);
        document.body.style.cursor = "";
    };

    document.addEventListener("mousemove", updateSplitView);
    document.addEventListener("mouseup", endSplitViewDrag);
    document.body.style.cursor = "row-resize";
};

const selectTab = (tabButton, tabID, selectedTab, selectedTabButton) => {
    let tab = document.getElementById(tabID);

    if (selectedTab === tab) {
        return selectedTab;
    }
    if (selectedTab !== null) {
        selectedTab.style.display = "none";
        selectedTabButton.classList.remove("active");
    }

    tab.style.display = "block";
    tabButton.classList.add("active");

    return tab;
};

const selectTopTab = (tabButton, tabID) => {
    selectedTopTab = selectTab(tabButton, tabID, selectedTopTab, selectedTopTabButton);
    selectedTopTabButton = tabButton;
};

const selectBottomTab = (tabButton, tabID) => {
    selectedBottomTab = selectTab(tabButton, tabID, selectedBottomTab, selectedBottomTabButton);
    selectedBottomTabButton = tabButton;
};

let initialTopTabButton = document.getElementById("dom-tree-button");
selectTopTab(initialTopTabButton, "dom-tree");

let initialBottomTabButton = document.getElementById("console-button");
selectBottomTab(initialBottomTabButton, "console");

const scrollToElement = element => {
    // Include an offset to prevent the element being placed behind the fixed `tab-controls` header.
    const offset = 45;

    let position = element.getBoundingClientRect().top;
    position += window.pageYOffset - offset;

    window.scrollTo(0, position);
};

inspector.reset = () => {
    let domTree = document.getElementById("dom-tree");
    domTree.innerHTML = "";

    let accessibilityTree = document.getElementById("accessibility-tree");
    accessibilityTree.innerHTML = "";

    selectedDOMNode = null;
    pendingEditDOMNode = null;

    inspector.clearConsoleOutput();
};

inspector.loadDOMTree = tree => {
    let domTree = document.getElementById("dom-tree");
    domTree.innerHTML = decodeBase64(tree);

    let domNodes = domTree.querySelectorAll(".hoverable");

    for (let domNode of domNodes) {
        domNode.addEventListener("click", event => {
            inspectDOMNode(domNode);
            event.preventDefault();
        });
    }

    domNodes = domTree.querySelectorAll(".editable");

    for (let domNode of domNodes) {
        domNode.addEventListener("dblclick", event => {
            editDOMNode(domNode);
            event.preventDefault();
        });
    }

    domNodes = domTree.querySelectorAll("details");

    for (let domNode of domNodes) {
        domNode.addEventListener("toggle", event => {
            updateVisibleDOMNodes();
        });
    }

    updateVisibleDOMNodes();
};

inspector.loadAccessibilityTree = tree => {
    let accessibilityTree = document.getElementById("accessibility-tree");
    accessibilityTree.innerHTML = decodeBase64(tree);
};

inspector.inspectDOMNodeID = nodeID => {
    let domNodes = document.querySelectorAll(`[data-id="${nodeID}"]`);
    if (domNodes.length !== 1) {
        return;
    }

    for (let domNode = domNodes[0]; domNode; domNode = domNode.parentNode) {
        if (domNode.tagName === "DETAILS") {
            domNode.setAttribute("open", "");
        }
    }

    inspectDOMNode(domNodes[0]);
    scrollToElement(selectedDOMNode);
};

inspector.clearInspectedDOMNode = () => {
    if (selectedDOMNode !== null) {
        selectedDOMNode.classList.remove("selected");
        selectedDOMNode = null;
    }
};

inspector.editDOMNodeID = nodeID => {
    if (pendingEditDOMNode === null) {
        return;
    }

    inspector.inspectDOMNodeID(nodeID);
    editDOMNode(pendingEditDOMNode);

    pendingEditDOMNode = null;
};

inspector.addAttributeToDOMNodeID = nodeID => {
    if (pendingEditDOMNode === null) {
        return;
    }

    inspector.inspectDOMNodeID(nodeID);
    addAttributeToDOMNode(pendingEditDOMNode);

    pendingEditDOMNode = null;
};

inspector.createPropertyTables = (computedStyle, resolvedStyle, customProperties) => {
    const createPropertyTable = (tableID, properties) => {
        let oldTable = document.getElementById(tableID);

        let newTable = document.createElement("tbody");
        newTable.setAttribute("id", tableID);

        Object.keys(properties)
            .sort()
            .forEach(name => {
                let row = newTable.insertRow();

                let nameColumn = row.insertCell();
                nameColumn.innerText = name;

                let valueColumn = row.insertCell();
                valueColumn.innerText = properties[name];
            });

        oldTable.parentNode.replaceChild(newTable, oldTable);
    };

    createPropertyTable("computed-style-table", JSON.parse(computedStyle));
    createPropertyTable("resolved-style-table", JSON.parse(resolvedStyle));
    createPropertyTable("custom-properties-table", JSON.parse(customProperties));
};

const inspectDOMNode = domNode => {
    if (selectedDOMNode === domNode) {
        return;
    }

    inspector.clearInspectedDOMNode();

    domNode.classList.add("selected");
    selectedDOMNode = domNode;

    inspector.inspectDOMNode(domNode.dataset.id, domNode.dataset.pseudoElement);
};

const createDOMEditor = (onHandleChange, onCancelChange) => {
    selectedDOMNode.classList.remove("selected");

    let input = document.createElement("input");
    input.classList.add("dom-editor");
    input.classList.add("selected");

    const handleChange = () => {
        input.removeEventListener("change", handleChange);
        input.removeEventListener("blur", cancelChange);
        input.removeEventListener("keydown", handleInput);

        try {
            onHandleChange(input.value);
        } catch {
            cancelChange();
        }
    };

    const cancelChange = () => {
        input.removeEventListener("change", handleChange);
        input.removeEventListener("blur", cancelChange);
        input.removeEventListener("keydown", handleInput);

        selectedDOMNode.classList.add("selected");
        onCancelChange(input);
    };

    const handleInput = event => {
        const ESCAPE_KEYCODE = 27;

        if (event.keyCode === ESCAPE_KEYCODE) {
            cancelChange();
            event.preventDefault();
        }
    };

    input.addEventListener("change", handleChange);
    input.addEventListener("blur", cancelChange);
    input.addEventListener("keydown", handleInput);

    setTimeout(() => {
        input.focus();

        // FIXME: Invoke `select` when it isn't just stubbed out.
        // input.select();
    });

    return input;
};

const parseDOMAttributes = value => {
    let element = document.createElement("div");
    element.innerHTML = `<div ${value}></div>`;

    return element.children[0].attributes;
};

const editDOMNode = domNode => {
    if (selectedDOMNode === null) {
        return;
    }

    const domNodeID = selectedDOMNode.dataset.id;
    const type = domNode.dataset.nodeType;

    const handleChange = value => {
        if (type === "text" || type === "comment") {
            inspector.setDOMNodeText(domNodeID, value);
        } else if (type === "tag") {
            const element = document.createElement(value);
            inspector.setDOMNodeTag(domNodeID, value);
        } else if (type === "attribute") {
            const attributeIndex = domNode.dataset.attributeIndex;
            const attributes = parseDOMAttributes(value);

            inspector.replaceDOMNodeAttribute(domNodeID, attributeIndex, attributes);
        }
    };

    const cancelChange = editor => {
        editor.parentNode.replaceChild(domNode, editor);
    };

    let editor = createDOMEditor(handleChange, cancelChange);

    if (type === "text") {
        let emptyTextSpan = domNode.querySelector(".internal");

        if (emptyTextSpan === null) {
            editor.value = domNode.innerText;
        }
    } else {
        editor.value = domNode.innerText;
    }

    domNode.parentNode.replaceChild(editor, domNode);
};

const addAttributeToDOMNode = domNode => {
    if (selectedDOMNode === null) {
        return;
    }

    const domNodeID = selectedDOMNode.dataset.id;

    const handleChange = value => {
        const attributes = parseDOMAttributes(value);
        inspector.addDOMNodeAttributes(domNodeID, attributes);
    };

    const cancelChange = () => {
        container.remove();
    };

    let editor = createDOMEditor(handleChange, cancelChange);
    editor.placeholder = 'name="value"';

    let nbsp = document.createElement("span");
    nbsp.innerHTML = "&nbsp;";

    let container = document.createElement("span");
    container.appendChild(nbsp);
    container.appendChild(editor);

    domNode.parentNode.insertBefore(container, domNode.parentNode.lastChild);
};

const updateVisibleDOMNodes = () => {
    let domTree = document.getElementById("dom-tree");

    visibleDOMNodes = [];

    function recurseDOMNodes(node) {
        for (let child of node.children) {
            if (child.classList.contains("hoverable")) {
                visibleDOMNodes.push(child);
            }

            if (child.tagName === "DIV") {
                if (node.open) {
                    recurseDOMNodes(child);
                }
            } else {
                recurseDOMNodes(child);
            }
        }
    }

    recurseDOMNodes(domTree);
};

const requestContextMenu = (clientX, clientY, domNode) => {
    pendingEditDOMNode = null;

    if (typeof domNode.dataset.nodeType === "undefined") {
        if (domNode.parentNode !== null) {
            domNode = domNode.parentNode;
        }
    }

    const domNodeID = domNode.closest(".hoverable")?.dataset.id;
    const type = domNode.dataset.nodeType;

    if (typeof domNodeID === "undefined" || typeof type === "undefined") {
        return;
    }

    let tag = null;
    let attributeIndex = null;

    if (type === "tag") {
        tag = domNode.dataset.tag;
    } else if (type === "attribute") {
        tag = domNode.dataset.tag;
        attributeIndex = domNode.dataset.attributeIndex;
    }

    pendingEditDOMNode = domNode;
    inspector.requestDOMTreeContextMenu(domNodeID, clientX, clientY, type, tag, attributeIndex);
};

const executeConsoleScript = consoleInput => {
    const script = consoleInput.value;

    if (!/\S/.test(script)) {
        return;
    }

    if (consoleHistory.length === 0 || consoleHistory[consoleHistory.length - 1] !== script) {
        consoleHistory.push(script);
    }

    consoleHistoryIndex = consoleHistory.length;

    inspector.executeConsoleScript(script);
    consoleInput.value = "";
};

const setConsoleInputToPreviousHistoryItem = consoleInput => {
    if (consoleHistoryIndex === 0) {
        return;
    }

    --consoleHistoryIndex;

    const script = consoleHistory[consoleHistoryIndex];
    consoleInput.value = script;
};

const setConsoleInputToNextHistoryItem = consoleInput => {
    if (consoleHistory.length === 0) {
        return;
    }

    const lastIndex = consoleHistory.length - 1;

    if (consoleHistoryIndex < lastIndex) {
        ++consoleHistoryIndex;

        consoleInput.value = consoleHistory[consoleHistoryIndex];
        return;
    }

    if (consoleHistoryIndex === lastIndex) {
        ++consoleHistoryIndex;

        consoleInput.value = "";
        return;
    }
};

const consoleParentGroup = () => {
    if (consoleGroupStack.length === 0) {
        return document.getElementById("console-output");
    }

    const lastConsoleGroup = consoleGroupStack[consoleGroupStack.length - 1];
    return document.getElementById(`console-group-${lastConsoleGroup.id}`);
};

const scrollConsoleToBottom = () => {
    let consoleOutput = document.getElementById("console-output");

    // FIXME: It should be sufficient to scrollTo a y value of document.documentElement.offsetHeight,
    //        but due to an unknown bug offsetHeight seems to not be properly updated after spamming
    //        a lot of document changes.
    //
    // The setTimeout makes the scrollTo async and allows the DOM to be updated.
    setTimeout(function () {
        consoleOutput.scrollTo(0, 1_000_000_000);
    }, 0);
};

inspector.appendConsoleOutput = output => {
    let parent = consoleParentGroup();

    let element = document.createElement("p");
    element.innerHTML = decodeBase64(output);

    parent.appendChild(element);
    scrollConsoleToBottom();
};

inspector.clearConsoleOutput = () => {
    let consoleOutput = document.getElementById("console-output");
    consoleOutput.innerHTML = "";

    consoleGroupStack = [];
};

inspector.beginConsoleGroup = (label, startExpanded) => {
    let parent = consoleParentGroup();

    const group = {
        id: ++consoleGroupNextID,
        label: label,
    };
    consoleGroupStack.push(group);

    let details = document.createElement("details");
    details.id = `console-group-${group.id}`;
    details.open = startExpanded;

    let summary = document.createElement("summary");
    summary.innerHTML = decodeBase64(label);

    details.appendChild(summary);
    parent.appendChild(details);
    scrollConsoleToBottom();
};

inspector.endConsoleGroup = () => {
    consoleGroupStack.pop();
};

document.addEventListener("DOMContentLoaded", () => {
    let inspectorSeparator = document.getElementById("inspector-separator");
    inspectorSeparator.addEventListener("mousedown", beginSplitViewDrag);

    let consoleInput = document.getElementById("console-input");
    consoleInput.focus();

    consoleInput.addEventListener("keydown", event => {
        const UP_ARROW_KEYCODE = 38;
        const DOWN_ARROW_KEYCODE = 40;
        const RETURN_KEYCODE = 13;

        if (event.keyCode === UP_ARROW_KEYCODE) {
            setConsoleInputToPreviousHistoryItem(consoleInput);
            event.preventDefault();
        } else if (event.keyCode === DOWN_ARROW_KEYCODE) {
            setConsoleInputToNextHistoryItem(consoleInput);
            event.preventDefault();
        } else if (event.keyCode === RETURN_KEYCODE) {
            executeConsoleScript(consoleInput);
            event.preventDefault();
        }
    });

    document.addEventListener("contextmenu", event => {
        requestContextMenu(event.clientX, event.clientY, event.target);
        event.preventDefault();
    });

    document.addEventListener("keydown", event => {
        const UP_ARROW_KEYCODE = 38;
        const DOWN_ARROW_KEYCODE = 40;
        const RETURN_KEYCODE = 13;
        const SPACE_KEYCODE = 32;

        if (document.activeElement.tagName !== "INPUT") {
            if (event.keyCode == UP_ARROW_KEYCODE || event.keyCode == DOWN_ARROW_KEYCODE) {
                let selectedIndex = visibleDOMNodes.indexOf(selectedDOMNode);
                if (selectedIndex < 0) {
                    return;
                }

                let newIndex;

                if (event.keyCode == UP_ARROW_KEYCODE) {
                    newIndex = selectedIndex - 1;
                } else if (event.keyCode == DOWN_ARROW_KEYCODE) {
                    newIndex = selectedIndex + 1;
                }

                if (visibleDOMNodes[newIndex]) {
                    inspectDOMNode(visibleDOMNodes[newIndex]);
                }
            } else if (event.keyCode == RETURN_KEYCODE || event.keyCode == SPACE_KEYCODE) {
                if (selectedDOMNode.parentNode.tagName === "SUMMARY") {
                    selectedDOMNode.parentNode.click();
                }
            }
        }
    });

    inspector.inspectorLoaded();
});
