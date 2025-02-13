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

inspector.exportInspector = () => {
    const html = `<!DOCTYPE ${document.doctype.name}>\n${document.documentElement.outerHTML}`;
    inspector.exportInspectorHTML(html);
};

inspector.reset = () => {
    let domTree = document.getElementById("dom-tree");
    domTree.innerHTML = "";

    let accessibilityTree = document.getElementById("accessibility-tree");
    accessibilityTree.innerHTML = "";

    let cookieTable = document.getElementById("cookie-table");
    cookieTable.innerHTML = "";

    let styleSheetPicker = document.getElementById("style-sheet-picker");
    styleSheetPicker.replaceChildren();

    let styleSheetSource = document.getElementById("style-sheet-source");
    styleSheetSource.innerHTML = "";

    let fontsList = document.getElementById("fonts-list");
    fontsList.innerHTML = "";

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
            const type = domNode.dataset.nodeType;
            const text = event.target.innerText;

            if (type === "attribute" && event.target.classList.contains("attribute-value")) {
                text = text.substring(1, text.length - 1);
            }

            editDOMNode(domNode, text);
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

inspector.setCookies = cookies => {
    let oldTable = document.getElementById("cookie-table");

    let newTable = document.createElement("tbody");
    newTable.setAttribute("id", oldTable.id);

    const addColumn = (row, value) => {
        let column = row.insertCell();
        column.innerText = value;
        column.title = value;
    };

    cookies
        .sort((lhs, rhs) => lhs.name.localeCompare(rhs.name))
        .forEach(cookie => {
            let row = newTable.insertRow();

            addColumn(row, cookie.name);
            addColumn(row, cookie.value);
            addColumn(row, cookie.domain);
            addColumn(row, cookie.path);
            addColumn(row, new Date(cookie.creationTime).toLocaleString());
            addColumn(row, new Date(cookie.lastAccessTime).toLocaleString());
            addColumn(row, new Date(cookie.expiryTime).toLocaleString());

            row.addEventListener("contextmenu", event => {
                inspector.requestCookieContextMenu(cookie.index, event.clientX, event.clientY);
                event.preventDefault();
            });
        });

    oldTable.parentNode.replaceChild(newTable, oldTable);
};

inspector.setStyleSheets = styleSheets => {
    const styleSheetPicker = document.getElementById("style-sheet-picker");
    const styleSheetSource = document.getElementById("style-sheet-source");
    styleSheetPicker.replaceChildren();
    styleSheetSource.innerHTML = "";

    function addOption(styleSheet, text) {
        const option = document.createElement("option");
        option.innerText = text;
        if (styleSheet.type) {
            option.dataset["type"] = styleSheet.type;
        }
        if (styleSheet.domNodeId) {
            option.dataset["domNodeId"] = styleSheet.domNodeId;
        }
        if (styleSheet.url) {
            option.dataset["url"] = styleSheet.url;
        }
        styleSheetPicker.add(option);
    }

    if (styleSheets.length > 0) {
        let styleElementIndex = 1;
        for (const styleSheet of styleSheets) {
            switch (styleSheet.type) {
                case "StyleElement":
                    addOption(styleSheet, `Style element #${styleElementIndex++}`);
                    break;
                case "LinkElement":
                    addOption(styleSheet, styleSheet.url);
                    break;
                case "ImportRule":
                    addOption(styleSheet, styleSheet.url);
                    break;
                case "UserAgent":
                    addOption(styleSheet, `User agent: ${styleSheet.url}`);
                    break;
                case "UserStyle":
                    addOption(styleSheet, "User style");
                    break;
            }
        }
        styleSheetPicker.disabled = false;
    } else {
        addOption({}, "No style sheets found");
        styleSheetPicker.disabled = true;
    }

    styleSheetPicker.selectedIndex = 0;

    if (!styleSheetPicker.disabled) {
        loadStyleSheet();
    }
};

const loadStyleSheet = () => {
    const styleSheetPicker = document.getElementById("style-sheet-picker");
    const styleSheetSource = document.getElementById("style-sheet-source");
    const selectedOption = styleSheetPicker.selectedOptions[0];

    styleSheetSource.innerHTML = "Loading...";
    inspector.requestStyleSheetSource(
        selectedOption.dataset["type"],
        selectedOption.dataset["domNodeId"],
        selectedOption.dataset["url"]
    );
};

inspector.setStyleSheetSource = (identifier, sourceBase64) => {
    const styleSheetPicker = document.getElementById("style-sheet-picker");
    const styleSheetSource = document.getElementById("style-sheet-source");
    const selectedOption = styleSheetPicker.selectedOptions[0];

    // Make sure this is the source for the currently-selected style sheet.
    // NOTE: These are != not !== intentionally.
    if (
        identifier.type != selectedOption.dataset["type"] ||
        identifier.domNodeId != selectedOption.dataset["domNodeId"] ||
        identifier.url != selectedOption.dataset["url"]
    ) {
        console.log(
            JSON.stringify(identifier),
            "doesn't match",
            JSON.stringify(selectedOption.dataset)
        );
        return;
    }

    styleSheetSource.innerHTML = decodeBase64(sourceBase64);
};

const applyPropertyFilter = (row, searchText) => {
    let matches = false;
    if (searchText) {
        for (let cell of row.cells) {
            if (cell.textContent.toLowerCase().includes(searchText)) {
                matches = true;
                break;
            }
        }
    } else {
        // Empty searchText matches everything, so skip the checks.
        matches = true;
    }

    if (matches) {
        row.classList.remove("hidden-row");
    } else {
        row.classList.add("hidden-row");
    }
};

const setupPropertyFilter = inputId => {
    const filterInput = document.getElementById(`${inputId}-filter`);

    filterInput.addEventListener("input", event => {
        const searchText = event.target.value.toLowerCase();
        const tbody = document.getElementById(`${inputId}-table`);
        const rows = tbody.getElementsByTagName("tr");

        for (let row of rows) {
            applyPropertyFilter(row, searchText);
        }
    });
};

inspector.createPropertyTables = (computedStyle, resolvedStyle, customProperties) => {
    const createPropertyTable = (tableID, properties) => {
        let oldTable = document.getElementById(tableID);
        let newTable = document.createElement("tbody");
        newTable.setAttribute("id", tableID);

        let searchText = oldTable.parentNode.parentNode.querySelector(".property-filter").value;

        Object.keys(properties)
            .sort((a, b) => {
                let baseResult = a.localeCompare(b);
                // Manually move vendor-prefixed items after non-prefixed ones.
                if (a[0] === "-") {
                    if (b[0] === "-") {
                        return baseResult;
                    }
                    return 1;
                }
                if (b[0] === "-") {
                    return -1;
                }
                return baseResult;
            })
            .forEach(name => {
                let row = newTable.insertRow();

                let nameColumn = row.insertCell();
                nameColumn.innerText = name;

                let valueColumn = row.insertCell();
                valueColumn.innerText = properties[name];

                if (searchText) {
                    applyPropertyFilter(row, searchText);
                }
            });

        oldTable.parentNode.replaceChild(newTable, oldTable);
    };

    createPropertyTable("computed-style-table", JSON.parse(computedStyle));
    createPropertyTable("resolved-style-table", JSON.parse(resolvedStyle));
    createPropertyTable("custom-properties-table", JSON.parse(customProperties));
};

inspector.createFontList = fonts => {
    let fontsJSON = JSON.parse(fonts);
    if (!Array.isArray(fontsJSON)) return;

    const listId = "fonts-list";
    let oldList = document.getElementById(listId);

    let newList = document.createElement("div");
    newList.setAttribute("id", listId);
    const createFontEntry = (listContainer, font) => {
        let fontEntry = document.createElement("div");
        fontEntry.classList.add("font");

        let fontName = document.createElement("div");
        fontName.classList.add("name");
        fontName.innerText = font.name;
        fontEntry.appendChild(fontName);

        let fontSize = document.createElement("div");
        fontSize.classList.add("size");
        fontSize.innerText = font.size;
        fontEntry.appendChild(fontSize);

        let fontWeight = document.createElement("div");
        fontWeight.classList.add("Weight");
        fontWeight.innerText = font.weight;
        fontEntry.appendChild(fontWeight);

        let fontVariant = document.createElement("div");
        fontVariant.classList.add("Variant");
        fontVariant.innerText = font.variant;
        fontEntry.appendChild(fontVariant);

        listContainer.appendChild(fontEntry);
    };

    for (let font of fontsJSON) createFontEntry(newList, font);

    oldList.parentNode.replaceChild(newList, oldList);
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
    });

    return input;
};

const parseDOMAttributes = value => {
    let element = document.createElement("div");
    element.innerHTML = `<div ${value}></div>`;

    return element.children[0].attributes;
};

const editDOMNode = (domNode, textToSelect) => {
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

    setTimeout(() => {
        if (typeof textToSelect !== "undefined") {
            const index = editor.value.indexOf(textToSelect);
            if (index !== -1) {
                editor.setSelectionRange(index, index + textToSelect.length);
                return;
            }
        }

        editor.select();
    });

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
        const RIGHT_ARROW_KEYCODE = 39;
        const LEFT_ARROW_KEYCODE = 37;
        const RETURN_KEYCODE = 13;
        const SPACE_KEYCODE = 32;

        const move = delta => {
            let selectedIndex = visibleDOMNodes.indexOf(selectedDOMNode);
            if (selectedIndex < 0) {
                return;
            }

            let newIndex = selectedIndex + delta;

            if (visibleDOMNodes[newIndex]) {
                inspectDOMNode(visibleDOMNodes[newIndex]);
            }
        };

        if (document.activeElement.tagName !== "INPUT") {
            const isSummary = selectedDOMNode.parentNode.tagName === "SUMMARY";
            const isDiv = selectedDOMNode.parentNode.tagName === "DIV";

            if (event.keyCode == UP_ARROW_KEYCODE) {
                move(-1);
            } else if (event.keyCode == DOWN_ARROW_KEYCODE) {
                move(1);
            } else if (event.keyCode == RETURN_KEYCODE || event.keyCode == SPACE_KEYCODE) {
                if (isSummary) {
                    selectedDOMNode.parentNode.click();
                }
            } else if (event.keyCode == RIGHT_ARROW_KEYCODE) {
                if (isSummary && selectedDOMNode.parentNode.parentNode.open === false) {
                    selectedDOMNode.parentNode.click();
                } else if (selectedDOMNode.parentNode.parentNode.open === true && !isDiv) {
                    move(1);
                }
            } else if (event.keyCode == LEFT_ARROW_KEYCODE) {
                if (isSummary && selectedDOMNode.parentNode.parentNode.open === true) {
                    selectedDOMNode.parentNode.click();
                } else if (selectedDOMNode.parentNode.parentNode.open === false || isDiv) {
                    move(-1);
                }
            }
        }
    });

    // Setup filters for property tables
    ["computed-style", "resolved-style", "custom-properties"].forEach(setupPropertyFilter);

    inspector.inspectorLoaded();
});
