"use strict";

var htmlNamespace = "http://www.w3.org/1999/xhtml";

var cssStylingFlag = false;

var defaultSingleLineContainerName = "div";

// This is bad :(
var globalRange = null;

// Commands are stored in a dictionary where we call their actions and such
var commands = {};

///////////////////////////////////////////////////////////////////////////////
////////////////////////////// Utility functions //////////////////////////////
///////////////////////////////////////////////////////////////////////////////
//@{

function nextNode(node) {
    if (node.hasChildNodes()) {
        return node.firstChild;
    }
    return nextNodeDescendants(node);
}

function previousNode(node) {
    if (node.previousSibling) {
        node = node.previousSibling;
        while (node.hasChildNodes()) {
            node = node.lastChild;
        }
        return node;
    }
    if (node.parentNode
    && node.parentNode.nodeType == Node.ELEMENT_NODE) {
        return node.parentNode;
    }
    return null;
}

function nextNodeDescendants(node) {
    while (node && !node.nextSibling) {
        node = node.parentNode;
    }
    if (!node) {
        return null;
    }
    return node.nextSibling;
}

/**
 * Returns true if ancestor is an ancestor of descendant, false otherwise.
 */
function isAncestor(ancestor, descendant) {
    return ancestor
        && descendant
        && Boolean(ancestor.compareDocumentPosition(descendant) & Node.DOCUMENT_POSITION_CONTAINED_BY);
}

/**
 * Returns true if ancestor is an ancestor of or equal to descendant, false
 * otherwise.
 */
function isAncestorContainer(ancestor, descendant) {
    return (ancestor || descendant)
        && (ancestor == descendant || isAncestor(ancestor, descendant));
}

/**
 * Returns true if descendant is a descendant of ancestor, false otherwise.
 */
function isDescendant(descendant, ancestor) {
    return ancestor
        && descendant
        && Boolean(ancestor.compareDocumentPosition(descendant) & Node.DOCUMENT_POSITION_CONTAINED_BY);
}

/**
 * Returns true if node1 is before node2 in tree order, false otherwise.
 */
function isBefore(node1, node2) {
    return Boolean(node1.compareDocumentPosition(node2) & Node.DOCUMENT_POSITION_FOLLOWING);
}

/**
 * Returns true if node1 is after node2 in tree order, false otherwise.
 */
function isAfter(node1, node2) {
    return Boolean(node1.compareDocumentPosition(node2) & Node.DOCUMENT_POSITION_PRECEDING);
}

function getAncestors(node) {
    var ancestors = [];
    while (node.parentNode) {
        ancestors.unshift(node.parentNode);
        node = node.parentNode;
    }
    return ancestors;
}

function getInclusiveAncestors(node) {
    return getAncestors(node).concat(node);
}

function getDescendants(node) {
    var descendants = [];
    var stop = nextNodeDescendants(node);
    while ((node = nextNode(node))
    && node != stop) {
        descendants.push(node);
    }
    return descendants;
}

function getInclusiveDescendants(node) {
    return [node].concat(getDescendants(node));
}

function convertProperty(property) {
    // Special-case for now
    var map = {
        "fontFamily": "font-family",
        "fontSize": "font-size",
        "fontStyle": "font-style",
        "fontWeight": "font-weight",
        "textDecoration": "text-decoration",
    };
    if (typeof map[property] != "undefined") {
        return map[property];
    }

    return property;
}

// Return the <font size=X> value for the given CSS size, or undefined if there
// is none.
function cssSizeToLegacy(cssVal) {
    return {
        "x-small": 1,
        "small": 2,
        "medium": 3,
        "large": 4,
        "x-large": 5,
        "xx-large": 6,
        "xxx-large": 7
    }[cssVal];
}

// Return the CSS size given a legacy size.
function legacySizeToCss(legacyVal) {
    return {
        1: "x-small",
        2: "small",
        3: "medium",
        4: "large",
        5: "x-large",
        6: "xx-large",
        7: "xxx-large",
    }[legacyVal];
}

// Opera 11 puts HTML elements in the null namespace, it seems.
function isHtmlNamespace(ns) {
    return ns === null
        || ns === htmlNamespace;
}

// "the directionality" from HTML.  I don't bother caring about non-HTML
// elements.
//
// "The directionality of an element is either 'ltr' or 'rtl', and is
// determined as per the first appropriate set of steps from the following
// list:"
function getDirectionality(element) {
    // "If the element's dir attribute is in the ltr state
    //     The directionality of the element is 'ltr'."
    if (element.dir == "ltr") {
        return "ltr";
    }

    // "If the element's dir attribute is in the rtl state
    //     The directionality of the element is 'rtl'."
    if (element.dir == "rtl") {
        return "rtl";
    }

    // "If the element's dir attribute is in the auto state
    // "If the element is a bdi element and the dir attribute is not in a
    // defined state (i.e. it is not present or has an invalid value)
    //     [lots of complicated stuff]
    //
    // Skip this, since no browser implements it anyway.

    // "If the element is a root element and the dir attribute is not in a
    // defined state (i.e. it is not present or has an invalid value)
    //     The directionality of the element is 'ltr'."
    if (!isHtmlElement(element.parentNode)) {
        return "ltr";
    }

    // "If the element has a parent element and the dir attribute is not in a
    // defined state (i.e. it is not present or has an invalid value)
    //     The directionality of the element is the same as the element's
    //     parent element's directionality."
    return getDirectionality(element.parentNode);
}

//@}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////// DOM Range functions /////////////////////////////
///////////////////////////////////////////////////////////////////////////////
//@{

function getNodeIndex(node) {
    var ret = 0;
    while (node.previousSibling) {
        ret++;
        node = node.previousSibling;
    }
    return ret;
}

// "The length of a Node node is the following, depending on node:
//
// ProcessingInstruction
// DocumentType
//   Always 0.
// Text
// Comment
//   node's length.
// Any other node
//   node's childNodes's length."
function getNodeLength(node) {
    switch (node.nodeType) {
        case Node.PROCESSING_INSTRUCTION_NODE:
        case Node.DOCUMENT_TYPE_NODE:
            return 0;

        case Node.TEXT_NODE:
        case Node.COMMENT_NODE:
            return node.length;

        default:
            return node.childNodes.length;
    }
}

/**
 * The position of two boundary points relative to one another, as defined by
 * DOM Range.
 */
function getPosition(nodeA, offsetA, nodeB, offsetB) {
    // "If node A is the same as node B, return equal if offset A equals offset
    // B, before if offset A is less than offset B, and after if offset A is
    // greater than offset B."
    if (nodeA == nodeB) {
        if (offsetA == offsetB) {
            return "equal";
        }
        if (offsetA < offsetB) {
            return "before";
        }
        if (offsetA > offsetB) {
            return "after";
        }
    }

    // "If node A is after node B in tree order, compute the position of (node
    // B, offset B) relative to (node A, offset A). If it is before, return
    // after. If it is after, return before."
    if (nodeB.compareDocumentPosition(nodeA) & Node.DOCUMENT_POSITION_FOLLOWING) {
        var pos = getPosition(nodeB, offsetB, nodeA, offsetA);
        if (pos == "before") {
            return "after";
        }
        if (pos == "after") {
            return "before";
        }
    }

    // "If node A is an ancestor of node B:"
    if (nodeB.compareDocumentPosition(nodeA) & Node.DOCUMENT_POSITION_CONTAINS) {
        // "Let child equal node B."
        var child = nodeB;

        // "While child is not a child of node A, set child to its parent."
        while (child.parentNode != nodeA) {
            child = child.parentNode;
        }

        // "If the index of child is less than offset A, return after."
        if (getNodeIndex(child) < offsetA) {
            return "after";
        }
    }

    // "Return before."
    return "before";
}

/**
 * Returns the furthest ancestor of a Node as defined by DOM Range.
 */
function getFurthestAncestor(node) {
    var root = node;
    while (root.parentNode != null) {
        root = root.parentNode;
    }
    return root;
}

/**
 * "contained" as defined by DOM Range: "A Node node is contained in a range
 * range if node's furthest ancestor is the same as range's root, and (node, 0)
 * is after range's start, and (node, length of node) is before range's end."
 */
function isContained(node, range) {
    var pos1 = getPosition(node, 0, range.startContainer, range.startOffset);
    var pos2 = getPosition(node, getNodeLength(node), range.endContainer, range.endOffset);

    return getFurthestAncestor(node) == getFurthestAncestor(range.startContainer)
        && pos1 == "after"
        && pos2 == "before";
}

/**
 * Return all nodes contained in range that the provided function returns true
 * for, omitting any with an ancestor already being returned.
 */
function getContainedNodes(range, condition) {
    if (typeof condition == "undefined") {
        condition = function() { return true };
    }
    var node = range.startContainer;
    if (node.hasChildNodes()
    && range.startOffset < node.childNodes.length) {
        // A child is contained
        node = node.childNodes[range.startOffset];
    } else if (range.startOffset == getNodeLength(node)) {
        // No descendant can be contained
        node = nextNodeDescendants(node);
    } else {
        // No children; this node at least can't be contained
        node = nextNode(node);
    }

    var stop = range.endContainer;
    if (stop.hasChildNodes()
    && range.endOffset < stop.childNodes.length) {
        // The node after the last contained node is a child
        stop = stop.childNodes[range.endOffset];
    } else {
        // This node and/or some of its children might be contained
        stop = nextNodeDescendants(stop);
    }

    var nodeList = [];
    while (isBefore(node, stop)) {
        if (isContained(node, range)
        && condition(node)) {
            nodeList.push(node);
            node = nextNodeDescendants(node);
            continue;
        }
        node = nextNode(node);
    }
    return nodeList;
}

/**
 * As above, but includes nodes with an ancestor that's already been returned.
 */
function getAllContainedNodes(range, condition) {
    if (typeof condition == "undefined") {
        condition = function() { return true };
    }
    var node = range.startContainer;
    if (node.hasChildNodes()
    && range.startOffset < node.childNodes.length) {
        // A child is contained
        node = node.childNodes[range.startOffset];
    } else if (range.startOffset == getNodeLength(node)) {
        // No descendant can be contained
        node = nextNodeDescendants(node);
    } else {
        // No children; this node at least can't be contained
        node = nextNode(node);
    }

    var stop = range.endContainer;
    if (stop.hasChildNodes()
    && range.endOffset < stop.childNodes.length) {
        // The node after the last contained node is a child
        stop = stop.childNodes[range.endOffset];
    } else {
        // This node and/or some of its children might be contained
        stop = nextNodeDescendants(stop);
    }

    var nodeList = [];
    while (isBefore(node, stop)) {
        if (isContained(node, range)
        && condition(node)) {
            nodeList.push(node);
        }
        node = nextNode(node);
    }
    return nodeList;
}

// Returns either null, or something of the form rgb(x, y, z), or something of
// the form rgb(x, y, z, w) with w != 0.
function normalizeColor(color) {
    if (color.toLowerCase() == "currentcolor") {
        return null;
    }

    if (normalizeColor.resultCache === undefined) {
        normalizeColor.resultCache = {};
    }

    if (normalizeColor.resultCache[color] !== undefined) {
        return normalizeColor.resultCache[color];
    }

    var originalColor = color;

    var outerSpan = document.createElement("span");
    document.body.appendChild(outerSpan);
    outerSpan.style.color = "black";

    var innerSpan = document.createElement("span");
    outerSpan.appendChild(innerSpan);
    innerSpan.style.color = color;
    color = getComputedStyle(innerSpan).color;

    if (color == "rgb(0, 0, 0)") {
        // Maybe it's really black, maybe it's invalid.
        outerSpan.color = "white";
        color = getComputedStyle(innerSpan).color;
        if (color != "rgb(0, 0, 0)") {
            return normalizeColor.resultCache[originalColor] = null;
        }
    }

    document.body.removeChild(outerSpan);

    // I rely on the fact that browsers generally provide consistent syntax for
    // getComputedStyle(), although it's not standardized.  There are only
    // three exceptions I found:
    if (/^rgba\([0-9]+, [0-9]+, [0-9]+, 1\)$/.test(color)) {
        // IE10PP2 seems to do this sometimes.
        return normalizeColor.resultCache[originalColor] =
            color.replace("rgba", "rgb").replace(", 1)", ")");
    }
    if (color == "transparent") {
        // IE10PP2, Firefox 7.0a2, and Opera 11.50 all return "transparent" if
        // the specified value is "transparent".
        return normalizeColor.resultCache[originalColor] =
            "rgba(0, 0, 0, 0)";
    }
    // Chrome 15 dev adds way too many significant figures.  This isn't a full
    // fix, it just fixes one case that comes up in tests.
    color = color.replace(/, 0.496094\)$/, ", 0.5)");
    return normalizeColor.resultCache[originalColor] = color;
}

// Returns either null, or something of the form #xxxxxx.
function parseSimpleColor(color) {
    color = normalizeColor(color);
    var matches = /^rgb\(([0-9]+), ([0-9]+), ([0-9]+)\)$/.exec(color);
    if (matches) {
        return "#"
            + parseInt(matches[1]).toString(16).replace(/^.$/, "0$&")
            + parseInt(matches[2]).toString(16).replace(/^.$/, "0$&")
            + parseInt(matches[3]).toString(16).replace(/^.$/, "0$&");
    }
    return null;
}

//@}

//////////////////////////////////////////////////////////////////////////////
/////////////////////////// Edit command functions ///////////////////////////
//////////////////////////////////////////////////////////////////////////////

/////////////////////////////////////////////////
///// Methods of the HTMLDocument interface /////
/////////////////////////////////////////////////
//@{

var executionStackDepth = 0;

// Helper function for common behavior.
function editCommandMethod(command, range, callback) {
    // Set up our global range magic, but only if we're the outermost function
    if (executionStackDepth == 0 && typeof range != "undefined") {
        globalRange = range;
    } else if (executionStackDepth == 0) {
        globalRange = null;
        globalRange = getActiveRange();
    }

    executionStackDepth++;
    try {
        var ret = callback();
    } catch(e) {
        executionStackDepth--;
        throw e;
    }
    executionStackDepth--;
    return ret;
}

function myExecCommand(command, showUi, value, range) {
    // "All of these methods must treat their command argument ASCII
    // case-insensitively."
    command = command.toLowerCase();

    // "If only one argument was provided, let show UI be false."
    //
    // If range was passed, I can't actually detect how many args were passed
    // . . .
    if (arguments.length == 1
    || (arguments.length >=4 && typeof showUi == "undefined")) {
        showUi = false;
    }

    // "If only one or two arguments were provided, let value be the empty
    // string."
    if (arguments.length <= 2
    || (arguments.length >=4 && typeof value == "undefined")) {
        value = "";
    }

    return editCommandMethod(command, range, (function(command, showUi, value) { return function() {
        // "If command is not supported or not enabled, return false."
        if (!(command in commands) || !myQueryCommandEnabled(command)) {
            return false;
        }

        // "Take the action for command, passing value to the instructions as an
        // argument."
        var ret = commands[command].action(value);

        // Check for bugs
        if (ret !== true && ret !== false) {
            throw "execCommand() didn't return true or false: " + ret;
        }

        // "If the previous step returned false, return false."
        if (ret === false) {
            return false;
        }

        // "Return true."
        return true;
    }})(command, showUi, value));
}

function myQueryCommandEnabled(command, range) {
    // "All of these methods must treat their command argument ASCII
    // case-insensitively."
    command = command.toLowerCase();

    return editCommandMethod(command, range, (function(command) { return function() {
        // "Return true if command is both supported and enabled, false
        // otherwise."
        if (!(command in commands)) {
            return false;
        }

        // "Among commands defined in this specification, those listed in
        // Miscellaneous commands are always enabled, except for the cut
        // command and the paste command. The other commands defined here are
        // enabled if the active range is not null, its start node is either
        // editable or an editing host, its end node is either editable or an
        // editing host, and there is some editing host that is an inclusive
        // ancestor of both its start node and its end node."
        return ["copy", "defaultparagraphseparator", "selectall", "stylewithcss",
        "usecss"].indexOf(command) != -1
            || (
                getActiveRange() !== null
                && (isEditable(getActiveRange().startContainer) || isEditingHost(getActiveRange().startContainer))
                && (isEditable(getActiveRange().endContainer) || isEditingHost(getActiveRange().endContainer))
                && (getInclusiveAncestors(getActiveRange().commonAncestorContainer).some(isEditingHost))
            );
    }})(command));
}

function myQueryCommandIndeterm(command, range) {
    // "All of these methods must treat their command argument ASCII
    // case-insensitively."
    command = command.toLowerCase();

    return editCommandMethod(command, range, (function(command) { return function() {
        // "If command is not supported or has no indeterminacy, return false."
        if (!(command in commands) || !("indeterm" in commands[command])) {
            return false;
        }

        // "Return true if command is indeterminate, otherwise false."
        return commands[command].indeterm();
    }})(command));
}

function myQueryCommandState(command, range) {
    // "All of these methods must treat their command argument ASCII
    // case-insensitively."
    command = command.toLowerCase();

    return editCommandMethod(command, range, (function(command) { return function() {
        // "If command is not supported or has no state, return false."
        if (!(command in commands) || !("state" in commands[command])) {
            return false;
        }

        // "If the state override for command is set, return it."
        if (typeof getStateOverride(command) != "undefined") {
            return getStateOverride(command);
        }

        // "Return true if command's state is true, otherwise false."
        return commands[command].state();
    }})(command));
}

// "When the queryCommandSupported(command) method on the HTMLDocument
// interface is invoked, the user agent must return true if command is
// supported, and false otherwise."
function myQueryCommandSupported(command) {
    // "All of these methods must treat their command argument ASCII
    // case-insensitively."
    command = command.toLowerCase();

    return command in commands;
}

function myQueryCommandValue(command, range) {
    // "All of these methods must treat their command argument ASCII
    // case-insensitively."
    command = command.toLowerCase();

    return editCommandMethod(command, range, function() {
        // "If command is not supported or has no value, return the empty string."
        if (!(command in commands) || !("value" in commands[command])) {
            return "";
        }

        // "If command is "fontSize" and its value override is set, convert the
        // value override to an integer number of pixels and return the legacy
        // font size for the result."
        if (command == "fontsize"
        && getValueOverride("fontsize") !== undefined) {
            return getLegacyFontSize(getValueOverride("fontsize"));
        }

        // "If the value override for command is set, return it."
        if (typeof getValueOverride(command) != "undefined") {
            return getValueOverride(command);
        }

        // "Return command's value."
        return commands[command].value();
    });
}
//@}

//////////////////////////////
///// Common definitions /////
//////////////////////////////
//@{

// "An HTML element is an Element whose namespace is the HTML namespace."
//
// I allow an extra argument to more easily check whether something is a
// particular HTML element, like isHtmlElement(node, "OL").  It accepts arrays
// too, like isHtmlElement(node, ["OL", "UL"]) to check if it's an ol or ul.
function isHtmlElement(node, tags) {
    if (typeof tags == "string") {
        tags = [tags];
    }
    if (typeof tags == "object") {
        tags = tags.map(function(tag) { return tag.toUpperCase() });
    }
    return node
        && node.nodeType == Node.ELEMENT_NODE
        && isHtmlNamespace(node.namespaceURI)
        && (typeof tags == "undefined" || tags.indexOf(node.tagName) != -1);
}

// "A prohibited paragraph child name is "address", "article", "aside",
// "blockquote", "caption", "center", "col", "colgroup", "dd", "details",
// "dir", "div", "dl", "dt", "fieldset", "figcaption", "figure", "footer",
// "form", "h1", "h2", "h3", "h4", "h5", "h6", "header", "hgroup", "hr", "li",
// "listing", "menu", "nav", "ol", "p", "plaintext", "pre", "section",
// "summary", "table", "tbody", "td", "tfoot", "th", "thead", "tr", "ul", or
// "xmp"."
var prohibitedParagraphChildNames = ["address", "article", "aside",
    "blockquote", "caption", "center", "col", "colgroup", "dd", "details",
    "dir", "div", "dl", "dt", "fieldset", "figcaption", "figure", "footer",
    "form", "h1", "h2", "h3", "h4", "h5", "h6", "header", "hgroup", "hr", "li",
    "listing", "menu", "nav", "ol", "p", "plaintext", "pre", "section",
    "summary", "table", "tbody", "td", "tfoot", "th", "thead", "tr", "ul",
    "xmp"];

// "A prohibited paragraph child is an HTML element whose local name is a
// prohibited paragraph child name."
function isProhibitedParagraphChild(node) {
    return isHtmlElement(node, prohibitedParagraphChildNames);
}

// "A block node is either an Element whose "display" property does not have
// resolved value "inline" or "inline-block" or "inline-table" or "none", or a
// Document, or a DocumentFragment."
function isBlockNode(node) {
    return node
        && ((node.nodeType == Node.ELEMENT_NODE && ["inline", "inline-block", "inline-table", "none"].indexOf(getComputedStyle(node).display) == -1)
        || node.nodeType == Node.DOCUMENT_NODE
        || node.nodeType == Node.DOCUMENT_FRAGMENT_NODE);
}

// "An inline node is a node that is not a block node."
function isInlineNode(node) {
    return node && !isBlockNode(node);
}

// "An editing host is a node that is either an HTML element with a
// contenteditable attribute set to the true state, or the HTML element child
// of a Document whose designMode is enabled."
function isEditingHost(node) {
    return node
        && isHtmlElement(node)
        && (node.contentEditable == "true"
        || (node.parentNode
        && node.parentNode.nodeType == Node.DOCUMENT_NODE
        && node.parentNode.designMode == "on"));
}

// "Something is editable if it is a node; it is not an editing host; it does
// not have a contenteditable attribute set to the false state; its parent is
// an editing host or editable; and either it is an HTML element, or it is an
// svg or math element, or it is not an Element and its parent is an HTML
// element."
function isEditable(node) {
    return node
        && !isEditingHost(node)
        && (node.nodeType != Node.ELEMENT_NODE || node.contentEditable != "false")
        && (isEditingHost(node.parentNode) || isEditable(node.parentNode))
        && (isHtmlElement(node)
        || (node.nodeType == Node.ELEMENT_NODE && node.namespaceURI == "http://www.w3.org/2000/svg" && node.localName == "svg")
        || (node.nodeType == Node.ELEMENT_NODE && node.namespaceURI == "http://www.w3.org/1998/Math/MathML" && node.localName == "math")
        || (node.nodeType != Node.ELEMENT_NODE && isHtmlElement(node.parentNode)));
}

// Helper function, not defined in the spec
function hasEditableDescendants(node) {
    for (var i = 0; i < node.childNodes.length; i++) {
        if (isEditable(node.childNodes[i])
        || hasEditableDescendants(node.childNodes[i])) {
            return true;
        }
    }
    return false;
}

// "The editing host of node is null if node is neither editable nor an editing
// host; node itself, if node is an editing host; or the nearest ancestor of
// node that is an editing host, if node is editable."
function getEditingHostOf(node) {
    if (isEditingHost(node)) {
        return node;
    } else if (isEditable(node)) {
        var ancestor = node.parentNode;
        while (!isEditingHost(ancestor)) {
            ancestor = ancestor.parentNode;
        }
        return ancestor;
    } else {
        return null;
    }
}

// "Two nodes are in the same editing host if the editing host of the first is
// non-null and the same as the editing host of the second."
function inSameEditingHost(node1, node2) {
    return getEditingHostOf(node1)
        && getEditingHostOf(node1) == getEditingHostOf(node2);
}

// "A collapsed line break is a br that begins a line box which has nothing
// else in it, and therefore has zero height."
function isCollapsedLineBreak(br) {
    if (!isHtmlElement(br, "br")) {
        return false;
    }

    // Add a zwsp after it and see if that changes the height of the nearest
    // non-inline parent.  Note: this is not actually reliable, because the
    // parent might have a fixed height or something.
    var ref = br.parentNode;
    while (getComputedStyle(ref).display == "inline") {
        ref = ref.parentNode;
    }
    var refStyle = ref.hasAttribute("style") ? ref.getAttribute("style") : null;
    ref.style.height = "auto";
    ref.style.maxHeight = "none";
    ref.style.minHeight = "0";
    var space = document.createTextNode("\u200b");
    var origHeight = ref.offsetHeight;
    if (origHeight == 0) {
        throw "isCollapsedLineBreak: original height is zero, bug?";
    }
    br.parentNode.insertBefore(space, br.nextSibling);
    var finalHeight = ref.offsetHeight;
    space.parentNode.removeChild(space);
    if (refStyle === null) {
        // Without the setAttribute() line, removeAttribute() doesn't work in
        // Chrome 14 dev.  I have no idea why.
        ref.setAttribute("style", "");
        ref.removeAttribute("style");
    } else {
        ref.setAttribute("style", refStyle);
    }

    // Allow some leeway in case the zwsp didn't create a whole new line, but
    // only made an existing line slightly higher.  Firefox 6.0a2 shows this
    // behavior when the first line is bold.
    return origHeight < finalHeight - 5;
}

// "An extraneous line break is a br that has no visual effect, in that
// removing it from the DOM would not change layout, except that a br that is
// the sole child of an li is not extraneous."
//
// FIXME: This doesn't work in IE, since IE ignores display: none in
// contenteditable.
function isExtraneousLineBreak(br) {
    if (!isHtmlElement(br, "br")) {
        return false;
    }

    if (isHtmlElement(br.parentNode, "li")
    && br.parentNode.childNodes.length == 1) {
        return false;
    }

    // Make the line break disappear and see if that changes the block's
    // height.  Yes, this is an absurd hack.  We have to reset height etc. on
    // the reference node because otherwise its height won't change if it's not
    // auto.
    var ref = br.parentNode;
    while (getComputedStyle(ref).display == "inline") {
        ref = ref.parentNode;
    }
    var refStyle = ref.hasAttribute("style") ? ref.getAttribute("style") : null;
    ref.style.height = "auto";
    ref.style.maxHeight = "none";
    ref.style.minHeight = "0";
    var brStyle = br.hasAttribute("style") ? br.getAttribute("style") : null;
    var origHeight = ref.offsetHeight;
    if (origHeight == 0) {
        throw "isExtraneousLineBreak: original height is zero, bug?";
    }
    br.setAttribute("style", "display:none");
    var finalHeight = ref.offsetHeight;
    if (refStyle === null) {
        // Without the setAttribute() line, removeAttribute() doesn't work in
        // Chrome 14 dev.  I have no idea why.
        ref.setAttribute("style", "");
        ref.removeAttribute("style");
    } else {
        ref.setAttribute("style", refStyle);
    }
    if (brStyle === null) {
        br.removeAttribute("style");
    } else {
        br.setAttribute("style", brStyle);
    }

    return origHeight == finalHeight;
}

// "A whitespace node is either a Text node whose data is the empty string; or
// a Text node whose data consists only of one or more tabs (0x0009), line
// feeds (0x000A), carriage returns (0x000D), and/or spaces (0x0020), and whose
// parent is an Element whose resolved value for "white-space" is "normal" or
// "nowrap"; or a Text node whose data consists only of one or more tabs
// (0x0009), carriage returns (0x000D), and/or spaces (0x0020), and whose
// parent is an Element whose resolved value for "white-space" is "pre-line"."
function isWhitespaceNode(node) {
    return node
        && node.nodeType == Node.TEXT_NODE
        && (node.data == ""
        || (
            /^[\t\n\r ]+$/.test(node.data)
            && node.parentNode
            && node.parentNode.nodeType == Node.ELEMENT_NODE
            && ["normal", "nowrap"].indexOf(getComputedStyle(node.parentNode).whiteSpace) != -1
        ) || (
            /^[\t\r ]+$/.test(node.data)
            && node.parentNode
            && node.parentNode.nodeType == Node.ELEMENT_NODE
            && getComputedStyle(node.parentNode).whiteSpace == "pre-line"
        ));
}

// "node is a collapsed whitespace node if the following algorithm returns
// true:"
function isCollapsedWhitespaceNode(node) {
    // "If node is not a whitespace node, return false."
    if (!isWhitespaceNode(node)) {
        return false;
    }

    // "If node's data is the empty string, return true."
    if (node.data == "") {
        return true;
    }

    // "Let ancestor be node's parent."
    var ancestor = node.parentNode;

    // "If ancestor is null, return true."
    if (!ancestor) {
        return true;
    }

    // "If the "display" property of some ancestor of node has resolved value
    // "none", return true."
    if (getAncestors(node).some(function(ancestor) {
        return ancestor.nodeType == Node.ELEMENT_NODE
            && getComputedStyle(ancestor).display == "none";
    })) {
        return true;
    }

    // "While ancestor is not a block node and its parent is not null, set
    // ancestor to its parent."
    while (!isBlockNode(ancestor)
    && ancestor.parentNode) {
        ancestor = ancestor.parentNode;
    }

    // "Let reference be node."
    var reference = node;

    // "While reference is a descendant of ancestor:"
    while (reference != ancestor) {
        // "Let reference be the node before it in tree order."
        reference = previousNode(reference);

        // "If reference is a block node or a br, return true."
        if (isBlockNode(reference)
        || isHtmlElement(reference, "br")) {
            return true;
        }

        // "If reference is a Text node that is not a whitespace node, or is an
        // img, break from this loop."
        if ((reference.nodeType == Node.TEXT_NODE && !isWhitespaceNode(reference))
        || isHtmlElement(reference, "img")) {
            break;
        }
    }

    // "Let reference be node."
    reference = node;

    // "While reference is a descendant of ancestor:"
    var stop = nextNodeDescendants(ancestor);
    while (reference != stop) {
        // "Let reference be the node after it in tree order, or null if there
        // is no such node."
        reference = nextNode(reference);

        // "If reference is a block node or a br, return true."
        if (isBlockNode(reference)
        || isHtmlElement(reference, "br")) {
            return true;
        }

        // "If reference is a Text node that is not a whitespace node, or is an
        // img, break from this loop."
        if ((reference && reference.nodeType == Node.TEXT_NODE && !isWhitespaceNode(reference))
        || isHtmlElement(reference, "img")) {
            break;
        }
    }

    // "Return false."
    return false;
}

// "Something is visible if it is a node that either is a block node, or a Text
// node that is not a collapsed whitespace node, or an img, or a br that is not
// an extraneous line break, or any node with a visible descendant; excluding
// any node with an ancestor container Element whose "display" property has
// resolved value "none"."
function isVisible(node) {
    if (!node) {
        return false;
    }

    if (getAncestors(node).concat(node)
    .filter(function(node) { return node.nodeType == Node.ELEMENT_NODE })
    .some(function(node) { return getComputedStyle(node).display == "none" })) {
        return false;
    }

    if (isBlockNode(node)
    || (node.nodeType == Node.TEXT_NODE && !isCollapsedWhitespaceNode(node))
    || isHtmlElement(node, "img")
    || (isHtmlElement(node, "br") && !isExtraneousLineBreak(node))) {
        return true;
    }

    for (var i = 0; i < node.childNodes.length; i++) {
        if (isVisible(node.childNodes[i])) {
            return true;
        }
    }

    return false;
}

// "Something is invisible if it is a node that is not visible."
function isInvisible(node) {
    return node && !isVisible(node);
}

// "A collapsed block prop is either a collapsed line break that is not an
// extraneous line break, or an Element that is an inline node and whose
// children are all either invisible or collapsed block props and that has at
// least one child that is a collapsed block prop."
function isCollapsedBlockProp(node) {
    if (isCollapsedLineBreak(node)
    && !isExtraneousLineBreak(node)) {
        return true;
    }

    if (!isInlineNode(node)
    || node.nodeType != Node.ELEMENT_NODE) {
        return false;
    }

    var hasCollapsedBlockPropChild = false;
    for (var i = 0; i < node.childNodes.length; i++) {
        if (!isInvisible(node.childNodes[i])
        && !isCollapsedBlockProp(node.childNodes[i])) {
            return false;
        }
        if (isCollapsedBlockProp(node.childNodes[i])) {
            hasCollapsedBlockPropChild = true;
        }
    }

    return hasCollapsedBlockPropChild;
}

// "The active range is the range of the selection given by calling
// getSelection() on the context object. (Thus the active range may be null.)"
//
// We cheat and return globalRange if that's defined.  We also ensure that the
// active range meets the requirements that selection boundary points are
// supposed to meet, i.e., that the nodes are both Text or Element nodes that
// descend from a Document.
function getActiveRange() {
    var ret;
    if (globalRange) {
        ret = globalRange;
    } else if (getSelection().rangeCount) {
        ret = getSelection().getRangeAt(0);
    } else {
        return null;
    }
    if ([Node.TEXT_NODE, Node.ELEMENT_NODE].indexOf(ret.startContainer.nodeType) == -1
    || [Node.TEXT_NODE, Node.ELEMENT_NODE].indexOf(ret.endContainer.nodeType) == -1
    || !ret.startContainer.ownerDocument
    || !ret.endContainer.ownerDocument
    || !isDescendant(ret.startContainer, ret.startContainer.ownerDocument)
    || !isDescendant(ret.endContainer, ret.endContainer.ownerDocument)) {
        throw "Invalid active range; test bug?";
    }
    return ret;
}

// "For some commands, each HTMLDocument must have a boolean state override
// and/or a string value override. These do not change the command's state or
// value, but change the way some algorithms behave, as specified in those
// algorithms' definitions. Initially, both must be unset for every command.
// Whenever the number of ranges in the Selection changes to something
// different, and whenever a boundary point of the range at a given index in
// the Selection changes to something different, the state override and value
// override must be unset for every command."
//
// We implement this crudely by using setters and getters.  To verify that the
// selection hasn't changed, we copy the active range and just check the
// endpoints match.  This isn't really correct, but it's good enough for us.
// Unset state/value overrides are undefined.  We put everything in a function
// so no one can access anything except via the provided functions, since
// otherwise callers might mistakenly use outdated overrides (if the selection
// has changed).
var getStateOverride, setStateOverride, unsetStateOverride,
    getValueOverride, setValueOverride, unsetValueOverride;
(function() {
    var stateOverrides = {};
    var valueOverrides = {};
    var storedRange = null;

    function resetOverrides() {
        if (!storedRange
        || storedRange.startContainer != getActiveRange().startContainer
        || storedRange.endContainer != getActiveRange().endContainer
        || storedRange.startOffset != getActiveRange().startOffset
        || storedRange.endOffset != getActiveRange().endOffset) {
            stateOverrides = {};
            valueOverrides = {};
            storedRange = getActiveRange().cloneRange();
        }
    }

    getStateOverride = function(command) {
        resetOverrides();
        return stateOverrides[command];
    };

    setStateOverride = function(command, newState) {
        resetOverrides();
        stateOverrides[command] = newState;
    };

    unsetStateOverride = function(command) {
        resetOverrides();
        delete stateOverrides[command];
    }

    getValueOverride = function(command) {
        resetOverrides();
        return valueOverrides[command];
    }

    // "The value override for the backColor command must be the same as the
    // value override for the hiliteColor command, such that setting one sets
    // the other to the same thing and unsetting one unsets the other."
    setValueOverride = function(command, newValue) {
        resetOverrides();
        valueOverrides[command] = newValue;
        if (command == "backcolor") {
            valueOverrides.hilitecolor = newValue;
        } else if (command == "hilitecolor") {
            valueOverrides.backcolor = newValue;
        }
    }

    unsetValueOverride = function(command) {
        resetOverrides();
        delete valueOverrides[command];
        if (command == "backcolor") {
            delete valueOverrides.hilitecolor;
        } else if (command == "hilitecolor") {
            delete valueOverrides.backcolor;
        }
    }
})();

//@}

/////////////////////////////
///// Common algorithms /////
/////////////////////////////

///// Assorted common algorithms /////
//@{

// Magic array of extra ranges whose endpoints we want to preserve.
var extraRanges = [];

function movePreservingRanges(node, newParent, newIndex) {
    // For convenience, I allow newIndex to be -1 to mean "insert at the end".
    if (newIndex == -1) {
        newIndex = newParent.childNodes.length;
    }

    // "When the user agent is to move a Node to a new location, preserving
    // ranges, it must remove the Node from its original parent (if any), then
    // insert it in the new location. In doing so, however, it must ignore the
    // regular range mutation rules, and instead follow these rules:"

    // "Let node be the moved Node, old parent and old index be the old parent
    // (which may be null) and index, and new parent and new index be the new
    // parent and index."
    var oldParent = node.parentNode;
    var oldIndex = getNodeIndex(node);

    // We preserve the global range object, the ranges in the selection, and
    // any range that's in the extraRanges array.  Any other ranges won't get
    // updated, because we have no references to them.
    var ranges = [globalRange].concat(extraRanges);
    for (var i = 0; i < getSelection().rangeCount; i++) {
        ranges.push(getSelection().getRangeAt(i));
    }
    var boundaryPoints = [];
    ranges.forEach(function(range) {
        boundaryPoints.push([range.startContainer, range.startOffset]);
        boundaryPoints.push([range.endContainer, range.endOffset]);
    });

    boundaryPoints.forEach(function(boundaryPoint) {
        // "If a boundary point's node is the same as or a descendant of node,
        // leave it unchanged, so it moves to the new location."
        //
        // No modifications necessary.

        // "If a boundary point's node is new parent and its offset is greater
        // than new index, add one to its offset."
        if (boundaryPoint[0] == newParent
        && boundaryPoint[1] > newIndex) {
            boundaryPoint[1]++;
        }

        // "If a boundary point's node is old parent and its offset is old index or
        // old index + 1, set its node to new parent and add new index − old index
        // to its offset."
        if (boundaryPoint[0] == oldParent
        && (boundaryPoint[1] == oldIndex
        || boundaryPoint[1] == oldIndex + 1)) {
            boundaryPoint[0] = newParent;
            boundaryPoint[1] += newIndex - oldIndex;
        }

        // "If a boundary point's node is old parent and its offset is greater than
        // old index + 1, subtract one from its offset."
        if (boundaryPoint[0] == oldParent
        && boundaryPoint[1] > oldIndex + 1) {
            boundaryPoint[1]--;
        }
    });

    // Now actually move it and preserve the ranges.
    if (newParent.childNodes.length == newIndex) {
        newParent.appendChild(node);
    } else {
        newParent.insertBefore(node, newParent.childNodes[newIndex]);
    }

    globalRange.setStart(boundaryPoints[0][0], boundaryPoints[0][1]);
    globalRange.setEnd(boundaryPoints[1][0], boundaryPoints[1][1]);

    for (var i = 0; i < extraRanges.length; i++) {
        extraRanges[i].setStart(boundaryPoints[2*i + 2][0], boundaryPoints[2*i + 2][1]);
        extraRanges[i].setEnd(boundaryPoints[2*i + 3][0], boundaryPoints[2*i + 3][1]);
    }

    getSelection().removeAllRanges();
    for (var i = 1 + extraRanges.length; i < ranges.length; i++) {
        var newRange = document.createRange();
        newRange.setStart(boundaryPoints[2*i][0], boundaryPoints[2*i][1]);
        newRange.setEnd(boundaryPoints[2*i + 1][0], boundaryPoints[2*i + 1][1]);
        getSelection().addRange(newRange);
    }
}

function setTagName(element, newName) {
    // "If element is an HTML element with local name equal to new name, return
    // element."
    if (isHtmlElement(element, newName.toUpperCase())) {
        return element;
    }

    // "If element's parent is null, return element."
    if (!element.parentNode) {
        return element;
    }

    // "Let replacement element be the result of calling createElement(new
    // name) on the ownerDocument of element."
    var replacementElement = element.ownerDocument.createElement(newName);

    // "Insert replacement element into element's parent immediately before
    // element."
    element.parentNode.insertBefore(replacementElement, element);

    // "Copy all attributes of element to replacement element, in order."
    for (var i = 0; i < element.attributes.length; i++) {
        replacementElement.setAttributeNS(element.attributes[i].namespaceURI, element.attributes[i].name, element.attributes[i].value);
    }

    // "While element has children, append the first child of element as the
    // last child of replacement element, preserving ranges."
    while (element.childNodes.length) {
        movePreservingRanges(element.firstChild, replacementElement, replacementElement.childNodes.length);
    }

    // "Remove element from its parent."
    element.parentNode.removeChild(element);

    // "Return replacement element."
    return replacementElement;
}

function removeExtraneousLineBreaksBefore(node) {
    // "Let ref be the previousSibling of node."
    var ref = node.previousSibling;

    // "If ref is null, abort these steps."
    if (!ref) {
        return;
    }

    // "While ref has children, set ref to its lastChild."
    while (ref.hasChildNodes()) {
        ref = ref.lastChild;
    }

    // "While ref is invisible but not an extraneous line break, and ref does
    // not equal node's parent, set ref to the node before it in tree order."
    while (isInvisible(ref)
    && !isExtraneousLineBreak(ref)
    && ref != node.parentNode) {
        ref = previousNode(ref);
    }

    // "If ref is an editable extraneous line break, remove it from its
    // parent."
    if (isEditable(ref)
    && isExtraneousLineBreak(ref)) {
        ref.parentNode.removeChild(ref);
    }
}

function removeExtraneousLineBreaksAtTheEndOf(node) {
    // "Let ref be node."
    var ref = node;

    // "While ref has children, set ref to its lastChild."
    while (ref.hasChildNodes()) {
        ref = ref.lastChild;
    }

    // "While ref is invisible but not an extraneous line break, and ref does
    // not equal node, set ref to the node before it in tree order."
    while (isInvisible(ref)
    && !isExtraneousLineBreak(ref)
    && ref != node) {
        ref = previousNode(ref);
    }

    // "If ref is an editable extraneous line break:"
    if (isEditable(ref)
    && isExtraneousLineBreak(ref)) {
        // "While ref's parent is editable and invisible, set ref to its
        // parent."
        while (isEditable(ref.parentNode)
        && isInvisible(ref.parentNode)) {
            ref = ref.parentNode;
        }

        // "Remove ref from its parent."
        ref.parentNode.removeChild(ref);
    }
}

// "To remove extraneous line breaks from a node, first remove extraneous line
// breaks before it, then remove extraneous line breaks at the end of it."
function removeExtraneousLineBreaksFrom(node) {
    removeExtraneousLineBreaksBefore(node);
    removeExtraneousLineBreaksAtTheEndOf(node);
}

//@}
///// Wrapping a list of nodes /////
//@{

function wrap(nodeList, siblingCriteria, newParentInstructions) {
    // "If not provided, sibling criteria returns false and new parent
    // instructions returns null."
    if (typeof siblingCriteria == "undefined") {
        siblingCriteria = function() { return false };
    }
    if (typeof newParentInstructions == "undefined") {
        newParentInstructions = function() { return null };
    }

    // "If every member of node list is invisible, and none is a br, return
    // null and abort these steps."
    if (nodeList.every(isInvisible)
    && !nodeList.some(function(node) { return isHtmlElement(node, "br") })) {
        return null;
    }

    // "If node list's first member's parent is null, return null and abort
    // these steps."
    if (!nodeList[0].parentNode) {
        return null;
    }

    // "If node list's last member is an inline node that's not a br, and node
    // list's last member's nextSibling is a br, append that br to node list."
    if (isInlineNode(nodeList[nodeList.length - 1])
    && !isHtmlElement(nodeList[nodeList.length - 1], "br")
    && isHtmlElement(nodeList[nodeList.length - 1].nextSibling, "br")) {
        nodeList.push(nodeList[nodeList.length - 1].nextSibling);
    }

    // "While node list's first member's previousSibling is invisible, prepend
    // it to node list."
    while (isInvisible(nodeList[0].previousSibling)) {
        nodeList.unshift(nodeList[0].previousSibling);
    }

    // "While node list's last member's nextSibling is invisible, append it to
    // node list."
    while (isInvisible(nodeList[nodeList.length - 1].nextSibling)) {
        nodeList.push(nodeList[nodeList.length - 1].nextSibling);
    }

    // "If the previousSibling of the first member of node list is editable and
    // running sibling criteria on it returns true, let new parent be the
    // previousSibling of the first member of node list."
    var newParent;
    if (isEditable(nodeList[0].previousSibling)
    && siblingCriteria(nodeList[0].previousSibling)) {
        newParent = nodeList[0].previousSibling;

    // "Otherwise, if the nextSibling of the last member of node list is
    // editable and running sibling criteria on it returns true, let new parent
    // be the nextSibling of the last member of node list."
    } else if (isEditable(nodeList[nodeList.length - 1].nextSibling)
    && siblingCriteria(nodeList[nodeList.length - 1].nextSibling)) {
        newParent = nodeList[nodeList.length - 1].nextSibling;

    // "Otherwise, run new parent instructions, and let new parent be the
    // result."
    } else {
        newParent = newParentInstructions();
    }

    // "If new parent is null, abort these steps and return null."
    if (!newParent) {
        return null;
    }

    // "If new parent's parent is null:"
    if (!newParent.parentNode) {
        // "Insert new parent into the parent of the first member of node list
        // immediately before the first member of node list."
        nodeList[0].parentNode.insertBefore(newParent, nodeList[0]);

        // "If any range has a boundary point with node equal to the parent of
        // new parent and offset equal to the index of new parent, add one to
        // that boundary point's offset."
        //
        // Only try to fix the global range.
        if (globalRange.startContainer == newParent.parentNode
        && globalRange.startOffset == getNodeIndex(newParent)) {
            globalRange.setStart(globalRange.startContainer, globalRange.startOffset + 1);
        }
        if (globalRange.endContainer == newParent.parentNode
        && globalRange.endOffset == getNodeIndex(newParent)) {
            globalRange.setEnd(globalRange.endContainer, globalRange.endOffset + 1);
        }
    }

    // "Let original parent be the parent of the first member of node list."
    var originalParent = nodeList[0].parentNode;

    // "If new parent is before the first member of node list in tree order:"
    if (isBefore(newParent, nodeList[0])) {
        // "If new parent is not an inline node, but the last visible child of
        // new parent and the first visible member of node list are both inline
        // nodes, and the last child of new parent is not a br, call
        // createElement("br") on the ownerDocument of new parent and append
        // the result as the last child of new parent."
        if (!isInlineNode(newParent)
        && isInlineNode([].filter.call(newParent.childNodes, isVisible).slice(-1)[0])
        && isInlineNode(nodeList.filter(isVisible)[0])
        && !isHtmlElement(newParent.lastChild, "BR")) {
            newParent.appendChild(newParent.ownerDocument.createElement("br"));
        }

        // "For each node in node list, append node as the last child of new
        // parent, preserving ranges."
        for (var i = 0; i < nodeList.length; i++) {
            movePreservingRanges(nodeList[i], newParent, -1);
        }

    // "Otherwise:"
    } else {
        // "If new parent is not an inline node, but the first visible child of
        // new parent and the last visible member of node list are both inline
        // nodes, and the last member of node list is not a br, call
        // createElement("br") on the ownerDocument of new parent and insert
        // the result as the first child of new parent."
        if (!isInlineNode(newParent)
        && isInlineNode([].filter.call(newParent.childNodes, isVisible)[0])
        && isInlineNode(nodeList.filter(isVisible).slice(-1)[0])
        && !isHtmlElement(nodeList[nodeList.length - 1], "BR")) {
            newParent.insertBefore(newParent.ownerDocument.createElement("br"), newParent.firstChild);
        }

        // "For each node in node list, in reverse order, insert node as the
        // first child of new parent, preserving ranges."
        for (var i = nodeList.length - 1; i >= 0; i--) {
            movePreservingRanges(nodeList[i], newParent, 0);
        }
    }

    // "If original parent is editable and has no children, remove it from its
    // parent."
    if (isEditable(originalParent) && !originalParent.hasChildNodes()) {
        originalParent.parentNode.removeChild(originalParent);
    }

    // "If new parent's nextSibling is editable and running sibling criteria on
    // it returns true:"
    if (isEditable(newParent.nextSibling)
    && siblingCriteria(newParent.nextSibling)) {
        // "If new parent is not an inline node, but new parent's last child
        // and new parent's nextSibling's first child are both inline nodes,
        // and new parent's last child is not a br, call createElement("br") on
        // the ownerDocument of new parent and append the result as the last
        // child of new parent."
        if (!isInlineNode(newParent)
        && isInlineNode(newParent.lastChild)
        && isInlineNode(newParent.nextSibling.firstChild)
        && !isHtmlElement(newParent.lastChild, "BR")) {
            newParent.appendChild(newParent.ownerDocument.createElement("br"));
        }

        // "While new parent's nextSibling has children, append its first child
        // as the last child of new parent, preserving ranges."
        while (newParent.nextSibling.hasChildNodes()) {
            movePreservingRanges(newParent.nextSibling.firstChild, newParent, -1);
        }

        // "Remove new parent's nextSibling from its parent."
        newParent.parentNode.removeChild(newParent.nextSibling);
    }

    // "Remove extraneous line breaks from new parent."
    removeExtraneousLineBreaksFrom(newParent);

    // "Return new parent."
    return newParent;
}


//@}
///// Allowed children /////
//@{

// "A name of an element with inline contents is "a", "abbr", "b", "bdi",
// "bdo", "cite", "code", "dfn", "em", "h1", "h2", "h3", "h4", "h5", "h6", "i",
// "kbd", "mark", "p", "pre", "q", "rp", "rt", "ruby", "s", "samp", "small",
// "span", "strong", "sub", "sup", "u", "var", "acronym", "listing", "strike",
// "xmp", "big", "blink", "font", "marquee", "nobr", or "tt"."
var namesOfElementsWithInlineContents = ["a", "abbr", "b", "bdi", "bdo",
    "cite", "code", "dfn", "em", "h1", "h2", "h3", "h4", "h5", "h6", "i",
    "kbd", "mark", "p", "pre", "q", "rp", "rt", "ruby", "s", "samp", "small",
    "span", "strong", "sub", "sup", "u", "var", "acronym", "listing", "strike",
    "xmp", "big", "blink", "font", "marquee", "nobr", "tt"];

// "An element with inline contents is an HTML element whose local name is a
// name of an element with inline contents."
function isElementWithInlineContents(node) {
    return isHtmlElement(node, namesOfElementsWithInlineContents);
}

function isAllowedChild(child, parent_) {
    // "If parent is "colgroup", "table", "tbody", "tfoot", "thead", "tr", or
    // an HTML element with local name equal to one of those, and child is a
    // Text node whose data does not consist solely of space characters, return
    // false."
    if ((["colgroup", "table", "tbody", "tfoot", "thead", "tr"].indexOf(parent_) != -1
    || isHtmlElement(parent_, ["colgroup", "table", "tbody", "tfoot", "thead", "tr"]))
    && typeof child == "object"
    && child.nodeType == Node.TEXT_NODE
    && !/^[ \t\n\f\r]*$/.test(child.data)) {
        return false;
    }

    // "If parent is "script", "style", "plaintext", or "xmp", or an HTML
    // element with local name equal to one of those, and child is not a Text
    // node, return false."
    if ((["script", "style", "plaintext", "xmp"].indexOf(parent_) != -1
    || isHtmlElement(parent_, ["script", "style", "plaintext", "xmp"]))
    && (typeof child != "object" || child.nodeType != Node.TEXT_NODE)) {
        return false;
    }

    // "If child is a Document, DocumentFragment, or DocumentType, return
    // false."
    if (typeof child == "object"
    && (child.nodeType == Node.DOCUMENT_NODE
    || child.nodeType == Node.DOCUMENT_FRAGMENT_NODE
    || child.nodeType == Node.DOCUMENT_TYPE_NODE)) {
        return false;
    }

    // "If child is an HTML element, set child to the local name of child."
    if (isHtmlElement(child)) {
        child = child.tagName.toLowerCase();
    }

    // "If child is not a string, return true."
    if (typeof child != "string") {
        return true;
    }

    // "If parent is an HTML element:"
    if (isHtmlElement(parent_)) {
        // "If child is "a", and parent or some ancestor of parent is an a,
        // return false."
        //
        // "If child is a prohibited paragraph child name and parent or some
        // ancestor of parent is an element with inline contents, return
        // false."
        //
        // "If child is "h1", "h2", "h3", "h4", "h5", or "h6", and parent or
        // some ancestor of parent is an HTML element with local name "h1",
        // "h2", "h3", "h4", "h5", or "h6", return false."
        var ancestor = parent_;
        while (ancestor) {
            if (child == "a" && isHtmlElement(ancestor, "a")) {
                return false;
            }
            if (prohibitedParagraphChildNames.indexOf(child) != -1
            && isElementWithInlineContents(ancestor)) {
                return false;
            }
            if (/^h[1-6]$/.test(child)
            && isHtmlElement(ancestor)
            && /^H[1-6]$/.test(ancestor.tagName)) {
                return false;
            }
            ancestor = ancestor.parentNode;
        }

        // "Let parent be the local name of parent."
        parent_ = parent_.tagName.toLowerCase();
    }

    // "If parent is an Element or DocumentFragment, return true."
    if (typeof parent_ == "object"
    && (parent_.nodeType == Node.ELEMENT_NODE
    || parent_.nodeType == Node.DOCUMENT_FRAGMENT_NODE)) {
        return true;
    }

    // "If parent is not a string, return false."
    if (typeof parent_ != "string") {
        return false;
    }

    // "If parent is on the left-hand side of an entry on the following list,
    // then return true if child is listed on the right-hand side of that
    // entry, and false otherwise."
    switch (parent_) {
        case "colgroup":
            return child == "col";
        case "table":
            return ["caption", "col", "colgroup", "tbody", "td", "tfoot", "th", "thead", "tr"].indexOf(child) != -1;
        case "tbody":
        case "thead":
        case "tfoot":
            return ["td", "th", "tr"].indexOf(child) != -1;
        case "tr":
            return ["td", "th"].indexOf(child) != -1;
        case "dl":
            return ["dt", "dd"].indexOf(child) != -1;
        case "dir":
        case "ol":
        case "ul":
            return ["dir", "li", "ol", "ul"].indexOf(child) != -1;
        case "hgroup":
            return /^h[1-6]$/.test(child);
    }

    // "If child is "body", "caption", "col", "colgroup", "frame", "frameset",
    // "head", "html", "tbody", "td", "tfoot", "th", "thead", or "tr", return
    // false."
    if (["body", "caption", "col", "colgroup", "frame", "frameset", "head",
    "html", "tbody", "td", "tfoot", "th", "thead", "tr"].indexOf(child) != -1) {
        return false;
    }

    // "If child is "dd" or "dt" and parent is not "dl", return false."
    if (["dd", "dt"].indexOf(child) != -1
    && parent_ != "dl") {
        return false;
    }

    // "If child is "li" and parent is not "ol" or "ul", return false."
    if (child == "li"
    && parent_ != "ol"
    && parent_ != "ul") {
        return false;
    }

    // "If parent is on the left-hand side of an entry on the following list
    // and child is listed on the right-hand side of that entry, return false."
    var table = [
        [["a"], ["a"]],
        [["dd", "dt"], ["dd", "dt"]],
        [["h1", "h2", "h3", "h4", "h5", "h6"], ["h1", "h2", "h3", "h4", "h5", "h6"]],
        [["li"], ["li"]],
        [["nobr"], ["nobr"]],
        [namesOfElementsWithInlineContents, prohibitedParagraphChildNames],
        [["td", "th"], ["caption", "col", "colgroup", "tbody", "td", "tfoot", "th", "thead", "tr"]],
    ];
    for (var i = 0; i < table.length; i++) {
        if (table[i][0].indexOf(parent_) != -1
        && table[i][1].indexOf(child) != -1) {
            return false;
        }
    }

    // "Return true."
    return true;
}


//@}

//////////////////////////////////////
///// Inline formatting commands /////
//////////////////////////////////////

///// Inline formatting command definitions /////
//@{

// "A node node is effectively contained in a range range if range is not
// collapsed, and at least one of the following holds:"
function isEffectivelyContained(node, range) {
    if (range.collapsed) {
        return false;
    }

    // "node is contained in range."
    if (isContained(node, range)) {
        return true;
    }

    // "node is range's start node, it is a Text node, and its length is
    // different from range's start offset."
    if (node == range.startContainer
    && node.nodeType == Node.TEXT_NODE
    && getNodeLength(node) != range.startOffset) {
        return true;
    }

    // "node is range's end node, it is a Text node, and range's end offset is
    // not 0."
    if (node == range.endContainer
    && node.nodeType == Node.TEXT_NODE
    && range.endOffset != 0) {
        return true;
    }

    // "node has at least one child; and all its children are effectively
    // contained in range; and either range's start node is not a descendant of
    // node or is not a Text node or range's start offset is zero; and either
    // range's end node is not a descendant of node or is not a Text node or
    // range's end offset is its end node's length."
    if (node.hasChildNodes()
    && [].every.call(node.childNodes, function(child) { return isEffectivelyContained(child, range) })
    && (!isDescendant(range.startContainer, node)
    || range.startContainer.nodeType != Node.TEXT_NODE
    || range.startOffset == 0)
    && (!isDescendant(range.endContainer, node)
    || range.endContainer.nodeType != Node.TEXT_NODE
    || range.endOffset == getNodeLength(range.endContainer))) {
        return true;
    }

    return false;
}

// Like get(All)ContainedNodes(), but for effectively contained nodes.
function getEffectivelyContainedNodes(range, condition) {
    if (typeof condition == "undefined") {
        condition = function() { return true };
    }
    var node = range.startContainer;
    while (isEffectivelyContained(node.parentNode, range)) {
        node = node.parentNode;
    }

    var stop = nextNodeDescendants(range.endContainer);

    var nodeList = [];
    while (isBefore(node, stop)) {
        if (isEffectivelyContained(node, range)
        && condition(node)) {
            nodeList.push(node);
            node = nextNodeDescendants(node);
            continue;
        }
        node = nextNode(node);
    }
    return nodeList;
}

function getAllEffectivelyContainedNodes(range, condition) {
    if (typeof condition == "undefined") {
        condition = function() { return true };
    }
    var node = range.startContainer;
    while (isEffectivelyContained(node.parentNode, range)) {
        node = node.parentNode;
    }

    var stop = nextNodeDescendants(range.endContainer);

    var nodeList = [];
    while (isBefore(node, stop)) {
        if (isEffectivelyContained(node, range)
        && condition(node)) {
            nodeList.push(node);
        }
        node = nextNode(node);
    }
    return nodeList;
}

// "A modifiable element is a b, em, i, s, span, strong, sub, sup, or u element
// with no attributes except possibly style; or a font element with no
// attributes except possibly style, color, face, and/or size; or an a element
// with no attributes except possibly style and/or href."
function isModifiableElement(node) {
    if (!isHtmlElement(node)) {
        return false;
    }

    if (["B", "EM", "I", "S", "SPAN", "STRIKE", "STRONG", "SUB", "SUP", "U"].indexOf(node.tagName) != -1) {
        if (node.attributes.length == 0) {
            return true;
        }

        if (node.attributes.length == 1
        && node.hasAttribute("style")) {
            return true;
        }
    }

    if (node.tagName == "FONT" || node.tagName == "A") {
        var numAttrs = node.attributes.length;

        if (node.hasAttribute("style")) {
            numAttrs--;
        }

        if (node.tagName == "FONT") {
            if (node.hasAttribute("color")) {
                numAttrs--;
            }

            if (node.hasAttribute("face")) {
                numAttrs--;
            }

            if (node.hasAttribute("size")) {
                numAttrs--;
            }
        }

        if (node.tagName == "A"
        && node.hasAttribute("href")) {
            numAttrs--;
        }

        if (numAttrs == 0) {
            return true;
        }
    }

    return false;
}

function isSimpleModifiableElement(node) {
    // "A simple modifiable element is an HTML element for which at least one
    // of the following holds:"
    if (!isHtmlElement(node)) {
        return false;
    }

    // Only these elements can possibly be a simple modifiable element.
    if (["A", "B", "EM", "FONT", "I", "S", "SPAN", "STRIKE", "STRONG", "SUB", "SUP", "U"].indexOf(node.tagName) == -1) {
        return false;
    }

    // "It is an a, b, em, font, i, s, span, strike, strong, sub, sup, or u
    // element with no attributes."
    if (node.attributes.length == 0) {
        return true;
    }

    // If it's got more than one attribute, everything after this fails.
    if (node.attributes.length > 1) {
        return false;
    }

    // "It is an a, b, em, font, i, s, span, strike, strong, sub, sup, or u
    // element with exactly one attribute, which is style, which sets no CSS
    // properties (including invalid or unrecognized properties)."
    //
    // Not gonna try for invalid or unrecognized.
    if (node.hasAttribute("style")
    && node.style.length == 0) {
        return true;
    }

    // "It is an a element with exactly one attribute, which is href."
    if (node.tagName == "A"
    && node.hasAttribute("href")) {
        return true;
    }

    // "It is a font element with exactly one attribute, which is either color,
    // face, or size."
    if (node.tagName == "FONT"
    && (node.hasAttribute("color")
    || node.hasAttribute("face")
    || node.hasAttribute("size")
    )) {
        return true;
    }

    // "It is a b or strong element with exactly one attribute, which is style,
    // and the style attribute sets exactly one CSS property (including invalid
    // or unrecognized properties), which is "font-weight"."
    if ((node.tagName == "B" || node.tagName == "STRONG")
    && node.hasAttribute("style")
    && node.style.length == 1
    && node.style.fontWeight != "") {
        return true;
    }

    // "It is an i or em element with exactly one attribute, which is style,
    // and the style attribute sets exactly one CSS property (including invalid
    // or unrecognized properties), which is "font-style"."
    if ((node.tagName == "I" || node.tagName == "EM")
    && node.hasAttribute("style")
    && node.style.length == 1
    && node.style.fontStyle != "") {
        return true;
    }

    // "It is an a, font, or span element with exactly one attribute, which is
    // style, and the style attribute sets exactly one CSS property (including
    // invalid or unrecognized properties), and that property is not
    // "text-decoration"."
    if ((node.tagName == "A" || node.tagName == "FONT" || node.tagName == "SPAN")
    && node.hasAttribute("style")
    && node.style.length == 1
    && node.style.textDecoration == "") {
        return true;
    }

    // "It is an a, font, s, span, strike, or u element with exactly one
    // attribute, which is style, and the style attribute sets exactly one CSS
    // property (including invalid or unrecognized properties), which is
    // "text-decoration", which is set to "line-through" or "underline" or
    // "overline" or "none"."
    //
    // The weird extra node.style.length check is for Firefox, which as of
    // 8.0a2 has annoying and weird behavior here.
    if (["A", "FONT", "S", "SPAN", "STRIKE", "U"].indexOf(node.tagName) != -1
    && node.hasAttribute("style")
    && (node.style.length == 1
        || (node.style.length == 4
            && "MozTextBlink" in node.style
            && "MozTextDecorationColor" in node.style
            && "MozTextDecorationLine" in node.style
            && "MozTextDecorationStyle" in node.style)
        || (node.style.length == 4
            && "MozTextBlink" in node.style
            && "textDecorationColor" in node.style
            && "textDecorationLine" in node.style
            && "textDecorationStyle" in node.style)
    )
    && (node.style.textDecoration == "line-through"
    || node.style.textDecoration == "underline"
    || node.style.textDecoration == "overline"
    || node.style.textDecoration == "none")) {
        return true;
    }

    return false;
}

// "A formattable node is an editable visible node that is either a Text node,
// an img, or a br."
function isFormattableNode(node) {
    return isEditable(node)
        && isVisible(node)
        && (node.nodeType == Node.TEXT_NODE
        || isHtmlElement(node, ["img", "br"]));
}

// "Two quantities are equivalent values for a command if either both are null,
// or both are strings and they're equal and the command does not define any
// equivalent values, or both are strings and the command defines equivalent
// values and they match the definition."
function areEquivalentValues(command, val1, val2) {
    if (val1 === null && val2 === null) {
        return true;
    }

    if (typeof val1 == "string"
    && typeof val2 == "string"
    && val1 == val2
    && !("equivalentValues" in commands[command])) {
        return true;
    }

    if (typeof val1 == "string"
    && typeof val2 == "string"
    && "equivalentValues" in commands[command]
    && commands[command].equivalentValues(val1, val2)) {
        return true;
    }

    return false;
}

// "Two quantities are loosely equivalent values for a command if either they
// are equivalent values for the command, or if the command is the fontSize
// command; one of the quantities is one of "x-small", "small", "medium",
// "large", "x-large", "xx-large", or "xxx-large"; and the other quantity is
// the resolved value of "font-size" on a font element whose size attribute has
// the corresponding value set ("1" through "7" respectively)."
function areLooselyEquivalentValues(command, val1, val2) {
    if (areEquivalentValues(command, val1, val2)) {
        return true;
    }

    if (command != "fontsize"
    || typeof val1 != "string"
    || typeof val2 != "string") {
        return false;
    }

    // Static variables in JavaScript?
    var callee = areLooselyEquivalentValues;
    if (callee.sizeMap === undefined) {
        callee.sizeMap = {};
        var font = document.createElement("font");
        document.body.appendChild(font);
        ["x-small", "small", "medium", "large", "x-large", "xx-large",
        "xxx-large"].forEach(function(keyword) {
            font.size = cssSizeToLegacy(keyword);
            callee.sizeMap[keyword] = getComputedStyle(font).fontSize;
        });
        document.body.removeChild(font);
    }

    return val1 === callee.sizeMap[val2]
        || val2 === callee.sizeMap[val1];
}

//@}
///// Assorted inline formatting command algorithms /////
//@{

function getEffectiveCommandValue(node, command) {
    // "If neither node nor its parent is an Element, return null."
    if (node.nodeType != Node.ELEMENT_NODE
    && (!node.parentNode || node.parentNode.nodeType != Node.ELEMENT_NODE)) {
        return null;
    }

    // "If node is not an Element, return the effective command value of its
    // parent for command."
    if (node.nodeType != Node.ELEMENT_NODE) {
        return getEffectiveCommandValue(node.parentNode, command);
    }

    // "If command is "createLink" or "unlink":"
    if (command == "createlink" || command == "unlink") {
        // "While node is not null, and is not an a element that has an href
        // attribute, set node to its parent."
        while (node
        && (!isHtmlElement(node)
        || node.tagName != "A"
        || !node.hasAttribute("href"))) {
            node = node.parentNode;
        }

        // "If node is null, return null."
        if (!node) {
            return null;
        }

        // "Return the value of node's href attribute."
        return node.getAttribute("href");
    }

    // "If command is "backColor" or "hiliteColor":"
    if (command == "backcolor"
    || command == "hilitecolor") {
        // "While the resolved value of "background-color" on node is any
        // fully transparent value, and node's parent is an Element, set
        // node to its parent."
        //
        // Another lame hack to avoid flawed APIs.
        while ((getComputedStyle(node).backgroundColor == "rgba(0, 0, 0, 0)"
        || getComputedStyle(node).backgroundColor === ""
        || getComputedStyle(node).backgroundColor == "transparent")
        && node.parentNode
        && node.parentNode.nodeType == Node.ELEMENT_NODE) {
            node = node.parentNode;
        }

        // "Return the resolved value of "background-color" for node."
        return getComputedStyle(node).backgroundColor;
    }

    // "If command is "subscript" or "superscript":"
    if (command == "subscript" || command == "superscript") {
        // "Let affected by subscript and affected by superscript be two
        // boolean variables, both initially false."
        var affectedBySubscript = false;
        var affectedBySuperscript = false;

        // "While node is an inline node:"
        while (isInlineNode(node)) {
            var verticalAlign = getComputedStyle(node).verticalAlign;

            // "If node is a sub, set affected by subscript to true."
            if (isHtmlElement(node, "sub")) {
                affectedBySubscript = true;
            // "Otherwise, if node is a sup, set affected by superscript to
            // true."
            } else if (isHtmlElement(node, "sup")) {
                affectedBySuperscript = true;
            }

            // "Set node to its parent."
            node = node.parentNode;
        }

        // "If affected by subscript and affected by superscript are both true,
        // return the string "mixed"."
        if (affectedBySubscript && affectedBySuperscript) {
            return "mixed";
        }

        // "If affected by subscript is true, return "subscript"."
        if (affectedBySubscript) {
            return "subscript";
        }

        // "If affected by superscript is true, return "superscript"."
        if (affectedBySuperscript) {
            return "superscript";
        }

        // "Return null."
        return null;
    }

    // "If command is "strikethrough", and the "text-decoration" property of
    // node or any of its ancestors has resolved value containing
    // "line-through", return "line-through". Otherwise, return null."
    if (command == "strikethrough") {
        do {
            if (getComputedStyle(node).textDecoration.indexOf("line-through") != -1) {
                return "line-through";
            }
            node = node.parentNode;
        } while (node && node.nodeType == Node.ELEMENT_NODE);
        return null;
    }

    // "If command is "underline", and the "text-decoration" property of node
    // or any of its ancestors has resolved value containing "underline",
    // return "underline". Otherwise, return null."
    if (command == "underline") {
        do {
            if (getComputedStyle(node).textDecoration.indexOf("underline") != -1) {
                return "underline";
            }
            node = node.parentNode;
        } while (node && node.nodeType == Node.ELEMENT_NODE);
        return null;
    }

    if (!("relevantCssProperty" in commands[command])) {
        throw "Bug: no relevantCssProperty for " + command + " in getEffectiveCommandValue";
    }

    // "Return the resolved value for node of the relevant CSS property for
    // command."
    return getComputedStyle(node)[commands[command].relevantCssProperty];
}

function getSpecifiedCommandValue(element, command) {
    // "If command is "backColor" or "hiliteColor" and element's display
    // property does not have resolved value "inline", return null."
    if ((command == "backcolor" || command == "hilitecolor")
    && getComputedStyle(element).display != "inline") {
        return null;
    }

    // "If command is "createLink" or "unlink":"
    if (command == "createlink" || command == "unlink") {
        // "If element is an a element and has an href attribute, return the
        // value of that attribute."
        if (isHtmlElement(element)
        && element.tagName == "A"
        && element.hasAttribute("href")) {
            return element.getAttribute("href");
        }

        // "Return null."
        return null;
    }

    // "If command is "subscript" or "superscript":"
    if (command == "subscript" || command == "superscript") {
        // "If element is a sup, return "superscript"."
        if (isHtmlElement(element, "sup")) {
            return "superscript";
        }

        // "If element is a sub, return "subscript"."
        if (isHtmlElement(element, "sub")) {
            return "subscript";
        }

        // "Return null."
        return null;
    }

    // "If command is "strikethrough", and element has a style attribute set,
    // and that attribute sets "text-decoration":"
    if (command == "strikethrough"
    && element.style.textDecoration != "") {
        // "If element's style attribute sets "text-decoration" to a value
        // containing "line-through", return "line-through"."
        if (element.style.textDecoration.indexOf("line-through") != -1) {
            return "line-through";
        }

        // "Return null."
        return null;
    }

    // "If command is "strikethrough" and element is a s or strike element,
    // return "line-through"."
    if (command == "strikethrough"
    && isHtmlElement(element, ["S", "STRIKE"])) {
        return "line-through";
    }

    // "If command is "underline", and element has a style attribute set, and
    // that attribute sets "text-decoration":"
    if (command == "underline"
    && element.style.textDecoration != "") {
        // "If element's style attribute sets "text-decoration" to a value
        // containing "underline", return "underline"."
        if (element.style.textDecoration.indexOf("underline") != -1) {
            return "underline";
        }

        // "Return null."
        return null;
    }

    // "If command is "underline" and element is a u element, return
    // "underline"."
    if (command == "underline"
    && isHtmlElement(element, "U")) {
        return "underline";
    }

    // "Let property be the relevant CSS property for command."
    var property = commands[command].relevantCssProperty;

    // "If property is null, return null."
    if (property === null) {
        return null;
    }

    // "If element has a style attribute set, and that attribute has the
    // effect of setting property, return the value that it sets property to."
    if (element.style[property] != "") {
        return element.style[property];
    }

    // "If element is a font element that has an attribute whose effect is
    // to create a presentational hint for property, return the value that the
    // hint sets property to.  (For a size of 7, this will be the non-CSS value
    // "xxx-large".)"
    if (isHtmlNamespace(element.namespaceURI)
    && element.tagName == "FONT") {
        if (property == "color" && element.hasAttribute("color")) {
            return element.color;
        }
        if (property == "fontFamily" && element.hasAttribute("face")) {
            return element.face;
        }
        if (property == "fontSize" && element.hasAttribute("size")) {
            // This is not even close to correct in general.
            var size = parseInt(element.size);
            if (size < 1) {
                size = 1;
            }
            if (size > 7) {
                size = 7;
            }
            return {
                1: "x-small",
                2: "small",
                3: "medium",
                4: "large",
                5: "x-large",
                6: "xx-large",
                7: "xxx-large"
            }[size];
        }
    }

    // "If element is in the following list, and property is equal to the
    // CSS property name listed for it, return the string listed for it."
    //
    // A list follows, whose meaning is copied here.
    if (property == "fontWeight"
    && (element.tagName == "B" || element.tagName == "STRONG")) {
        return "bold";
    }
    if (property == "fontStyle"
    && (element.tagName == "I" || element.tagName == "EM")) {
        return "italic";
    }

    // "Return null."
    return null;
}

function reorderModifiableDescendants(node, command, newValue) {
    // "Let candidate equal node."
    var candidate = node;

    // "While candidate is a modifiable element, and candidate has exactly one
    // child, and that child is also a modifiable element, and candidate is not
    // a simple modifiable element or candidate's specified command value for
    // command is not equivalent to new value, set candidate to its child."
    while (isModifiableElement(candidate)
    && candidate.childNodes.length == 1
    && isModifiableElement(candidate.firstChild)
    && (!isSimpleModifiableElement(candidate)
    || !areEquivalentValues(command, getSpecifiedCommandValue(candidate, command), newValue))) {
        candidate = candidate.firstChild;
    }

    // "If candidate is node, or is not a simple modifiable element, or its
    // specified command value is not equivalent to new value, or its effective
    // command value is not loosely equivalent to new value, abort these
    // steps."
    if (candidate == node
    || !isSimpleModifiableElement(candidate)
    || !areEquivalentValues(command, getSpecifiedCommandValue(candidate, command), newValue)
    || !areLooselyEquivalentValues(command, getEffectiveCommandValue(candidate, command), newValue)) {
        return;
    }

    // "While candidate has children, insert the first child of candidate into
    // candidate's parent immediately before candidate, preserving ranges."
    while (candidate.hasChildNodes()) {
        movePreservingRanges(candidate.firstChild, candidate.parentNode, getNodeIndex(candidate));
    }

    // "Insert candidate into node's parent immediately after node."
    node.parentNode.insertBefore(candidate, node.nextSibling);

    // "Append the node as the last child of candidate, preserving ranges."
    movePreservingRanges(node, candidate, -1);
}

function recordValues(nodeList) {
    // "Let values be a list of (node, command, specified command value)
    // triples, initially empty."
    var values = [];

    // "For each node in node list, for each command in the list "subscript",
    // "bold", "fontName", "fontSize", "foreColor", "hiliteColor", "italic",
    // "strikethrough", and "underline" in that order:"
    nodeList.forEach(function(node) {
        ["subscript", "bold", "fontname", "fontsize", "forecolor",
        "hilitecolor", "italic", "strikethrough", "underline"].forEach(function(command) {
            // "Let ancestor equal node."
            var ancestor = node;

            // "If ancestor is not an Element, set it to its parent."
            if (ancestor.nodeType != Node.ELEMENT_NODE) {
                ancestor = ancestor.parentNode;
            }

            // "While ancestor is an Element and its specified command value
            // for command is null, set it to its parent."
            while (ancestor
            && ancestor.nodeType == Node.ELEMENT_NODE
            && getSpecifiedCommandValue(ancestor, command) === null) {
                ancestor = ancestor.parentNode;
            }

            // "If ancestor is an Element, add (node, command, ancestor's
            // specified command value for command) to values. Otherwise add
            // (node, command, null) to values."
            if (ancestor && ancestor.nodeType == Node.ELEMENT_NODE) {
                values.push([node, command, getSpecifiedCommandValue(ancestor, command)]);
            } else {
                values.push([node, command, null]);
            }
        });
    });

    // "Return values."
    return values;
}

function restoreValues(values) {
    // "For each (node, command, value) triple in values:"
    values.forEach(function(triple) {
        var node = triple[0];
        var command = triple[1];
        var value = triple[2];

        // "Let ancestor equal node."
        var ancestor = node;

        // "If ancestor is not an Element, set it to its parent."
        if (!ancestor || ancestor.nodeType != Node.ELEMENT_NODE) {
            ancestor = ancestor.parentNode;
        }

        // "While ancestor is an Element and its specified command value for
        // command is null, set it to its parent."
        while (ancestor
        && ancestor.nodeType == Node.ELEMENT_NODE
        && getSpecifiedCommandValue(ancestor, command) === null) {
            ancestor = ancestor.parentNode;
        }

        // "If value is null and ancestor is an Element, push down values on
        // node for command, with new value null."
        if (value === null
        && ancestor
        && ancestor.nodeType == Node.ELEMENT_NODE) {
            pushDownValues(node, command, null);

        // "Otherwise, if ancestor is an Element and its specified command
        // value for command is not equivalent to value, or if ancestor is not
        // an Element and value is not null, force the value of command to
        // value on node."
        } else if ((ancestor
        && ancestor.nodeType == Node.ELEMENT_NODE
        && !areEquivalentValues(command, getSpecifiedCommandValue(ancestor, command), value))
        || ((!ancestor || ancestor.nodeType != Node.ELEMENT_NODE)
        && value !== null)) {
            forceValue(node, command, value);
        }
    });
}


//@}
///// Clearing an element's value /////
//@{

function clearValue(element, command) {
    // "If element is not editable, return the empty list."
    if (!isEditable(element)) {
        return [];
    }

    // "If element's specified command value for command is null, return the
    // empty list."
    if (getSpecifiedCommandValue(element, command) === null) {
        return [];
    }

    // "If element is a simple modifiable element:"
    if (isSimpleModifiableElement(element)) {
        // "Let children be the children of element."
        var children = Array.prototype.slice.call(element.childNodes);

        // "For each child in children, insert child into element's parent
        // immediately before element, preserving ranges."
        for (var i = 0; i < children.length; i++) {
            movePreservingRanges(children[i], element.parentNode, getNodeIndex(element));
        }

        // "Remove element from its parent."
        element.parentNode.removeChild(element);

        // "Return children."
        return children;
    }

    // "If command is "strikethrough", and element has a style attribute that
    // sets "text-decoration" to some value containing "line-through", delete
    // "line-through" from the value."
    if (command == "strikethrough"
    && element.style.textDecoration.indexOf("line-through") != -1) {
        if (element.style.textDecoration == "line-through") {
            element.style.textDecoration = "";
        } else {
            element.style.textDecoration = element.style.textDecoration.replace("line-through", "");
        }
        if (element.getAttribute("style") == "") {
            element.removeAttribute("style");
        }
    }

    // "If command is "underline", and element has a style attribute that sets
    // "text-decoration" to some value containing "underline", delete
    // "underline" from the value."
    if (command == "underline"
    && element.style.textDecoration.indexOf("underline") != -1) {
        if (element.style.textDecoration == "underline") {
            element.style.textDecoration = "";
        } else {
            element.style.textDecoration = element.style.textDecoration.replace("underline", "");
        }
        if (element.getAttribute("style") == "") {
            element.removeAttribute("style");
        }
    }

    // "If the relevant CSS property for command is not null, unset the CSS
    // property property of element."
    if (commands[command].relevantCssProperty !== null) {
        element.style[commands[command].relevantCssProperty] = '';
        if (element.getAttribute("style") == "") {
            element.removeAttribute("style");
        }
    }

    // "If element is a font element:"
    if (isHtmlNamespace(element.namespaceURI) && element.tagName == "FONT") {
        // "If command is "foreColor", unset element's color attribute, if set."
        if (command == "forecolor") {
            element.removeAttribute("color");
        }

        // "If command is "fontName", unset element's face attribute, if set."
        if (command == "fontname") {
            element.removeAttribute("face");
        }

        // "If command is "fontSize", unset element's size attribute, if set."
        if (command == "fontsize") {
            element.removeAttribute("size");
        }
    }

    // "If element is an a element and command is "createLink" or "unlink",
    // unset the href property of element."
    if (isHtmlElement(element, "A")
    && (command == "createlink" || command == "unlink")) {
        element.removeAttribute("href");
    }

    // "If element's specified command value for command is null, return the
    // empty list."
    if (getSpecifiedCommandValue(element, command) === null) {
        return [];
    }

    // "Set the tag name of element to "span", and return the one-node list
    // consisting of the result."
    return [setTagName(element, "span")];
}


//@}
///// Pushing down values /////
//@{

function pushDownValues(node, command, newValue) {
    // "If node's parent is not an Element, abort this algorithm."
    if (!node.parentNode
    || node.parentNode.nodeType != Node.ELEMENT_NODE) {
        return;
    }

    // "If the effective command value of command is loosely equivalent to new
    // value on node, abort this algorithm."
    if (areLooselyEquivalentValues(command, getEffectiveCommandValue(node, command), newValue)) {
        return;
    }

    // "Let current ancestor be node's parent."
    var currentAncestor = node.parentNode;

    // "Let ancestor list be a list of Nodes, initially empty."
    var ancestorList = [];

    // "While current ancestor is an editable Element and the effective command
    // value of command is not loosely equivalent to new value on it, append
    // current ancestor to ancestor list, then set current ancestor to its
    // parent."
    while (isEditable(currentAncestor)
    && currentAncestor.nodeType == Node.ELEMENT_NODE
    && !areLooselyEquivalentValues(command, getEffectiveCommandValue(currentAncestor, command), newValue)) {
        ancestorList.push(currentAncestor);
        currentAncestor = currentAncestor.parentNode;
    }

    // "If ancestor list is empty, abort this algorithm."
    if (!ancestorList.length) {
        return;
    }

    // "Let propagated value be the specified command value of command on the
    // last member of ancestor list."
    var propagatedValue = getSpecifiedCommandValue(ancestorList[ancestorList.length - 1], command);

    // "If propagated value is null and is not equal to new value, abort this
    // algorithm."
    if (propagatedValue === null && propagatedValue != newValue) {
        return;
    }

    // "If the effective command value for the parent of the last member of
    // ancestor list is not loosely equivalent to new value, and new value is
    // not null, abort this algorithm."
    if (newValue !== null
    && !areLooselyEquivalentValues(command, getEffectiveCommandValue(ancestorList[ancestorList.length - 1].parentNode, command), newValue)) {
        return;
    }

    // "While ancestor list is not empty:"
    while (ancestorList.length) {
        // "Let current ancestor be the last member of ancestor list."
        // "Remove the last member from ancestor list."
        var currentAncestor = ancestorList.pop();

        // "If the specified command value of current ancestor for command is
        // not null, set propagated value to that value."
        if (getSpecifiedCommandValue(currentAncestor, command) !== null) {
            propagatedValue = getSpecifiedCommandValue(currentAncestor, command);
        }

        // "Let children be the children of current ancestor."
        var children = Array.prototype.slice.call(currentAncestor.childNodes);

        // "If the specified command value of current ancestor for command is
        // not null, clear the value of current ancestor."
        if (getSpecifiedCommandValue(currentAncestor, command) !== null) {
            clearValue(currentAncestor, command);
        }

        // "For every child in children:"
        for (var i = 0; i < children.length; i++) {
            var child = children[i];

            // "If child is node, continue with the next child."
            if (child == node) {
                continue;
            }

            // "If child is an Element whose specified command value for
            // command is neither null nor equivalent to propagated value,
            // continue with the next child."
            if (child.nodeType == Node.ELEMENT_NODE
            && getSpecifiedCommandValue(child, command) !== null
            && !areEquivalentValues(command, propagatedValue, getSpecifiedCommandValue(child, command))) {
                continue;
            }

            // "If child is the last member of ancestor list, continue with the
            // next child."
            if (child == ancestorList[ancestorList.length - 1]) {
                continue;
            }

            // "Force the value of child, with command as in this algorithm
            // and new value equal to propagated value."
            forceValue(child, command, propagatedValue);
        }
    }
}


//@}
///// Forcing the value of a node /////
//@{

function forceValue(node, command, newValue) {
    // "If node's parent is null, abort this algorithm."
    if (!node.parentNode) {
        return;
    }

    // "If new value is null, abort this algorithm."
    if (newValue === null) {
        return;
    }

    // "If node is an allowed child of "span":"
    if (isAllowedChild(node, "span")) {
        // "Reorder modifiable descendants of node's previousSibling."
        reorderModifiableDescendants(node.previousSibling, command, newValue);

        // "Reorder modifiable descendants of node's nextSibling."
        reorderModifiableDescendants(node.nextSibling, command, newValue);

        // "Wrap the one-node list consisting of node, with sibling criteria
        // returning true for a simple modifiable element whose specified
        // command value is equivalent to new value and whose effective command
        // value is loosely equivalent to new value and false otherwise, and
        // with new parent instructions returning null."
        wrap([node],
            function(node) {
                return isSimpleModifiableElement(node)
                    && areEquivalentValues(command, getSpecifiedCommandValue(node, command), newValue)
                    && areLooselyEquivalentValues(command, getEffectiveCommandValue(node, command), newValue);
            },
            function() { return null }
        );
    }

    // "If node is invisible, abort this algorithm."
    if (isInvisible(node)) {
        return;
    }

    // "If the effective command value of command is loosely equivalent to new
    // value on node, abort this algorithm."
    if (areLooselyEquivalentValues(command, getEffectiveCommandValue(node, command), newValue)) {
        return;
    }

    // "If node is not an allowed child of "span":"
    if (!isAllowedChild(node, "span")) {
        // "Let children be all children of node, omitting any that are
        // Elements whose specified command value for command is neither null
        // nor equivalent to new value."
        var children = [];
        for (var i = 0; i < node.childNodes.length; i++) {
            if (node.childNodes[i].nodeType == Node.ELEMENT_NODE) {
                var specifiedValue = getSpecifiedCommandValue(node.childNodes[i], command);

                if (specifiedValue !== null
                && !areEquivalentValues(command, newValue, specifiedValue)) {
                    continue;
                }
            }
            children.push(node.childNodes[i]);
        }

        // "Force the value of each Node in children, with command and new
        // value as in this invocation of the algorithm."
        for (var i = 0; i < children.length; i++) {
            forceValue(children[i], command, newValue);
        }

        // "Abort this algorithm."
        return;
    }

    // "If the effective command value of command is loosely equivalent to new
    // value on node, abort this algorithm."
    if (areLooselyEquivalentValues(command, getEffectiveCommandValue(node, command), newValue)) {
        return;
    }

    // "Let new parent be null."
    var newParent = null;

    // "If the CSS styling flag is false:"
    if (!cssStylingFlag) {
        // "If command is "bold" and new value is "bold", let new parent be the
        // result of calling createElement("b") on the ownerDocument of node."
        if (command == "bold" && (newValue == "bold" || newValue == "700")) {
            newParent = node.ownerDocument.createElement("b");
        }

        // "If command is "italic" and new value is "italic", let new parent be
        // the result of calling createElement("i") on the ownerDocument of
        // node."
        if (command == "italic" && newValue == "italic") {
            newParent = node.ownerDocument.createElement("i");
        }

        // "If command is "strikethrough" and new value is "line-through", let
        // new parent be the result of calling createElement("s") on the
        // ownerDocument of node."
        if (command == "strikethrough" && newValue == "line-through") {
            newParent = node.ownerDocument.createElement("s");
        }

        // "If command is "underline" and new value is "underline", let new
        // parent be the result of calling createElement("u") on the
        // ownerDocument of node."
        if (command == "underline" && newValue == "underline") {
            newParent = node.ownerDocument.createElement("u");
        }

        // "If command is "foreColor", and new value is fully opaque with red,
        // green, and blue components in the range 0 to 255:"
        if (command == "forecolor" && parseSimpleColor(newValue)) {
            // "Let new parent be the result of calling createElement("font")
            // on the ownerDocument of node."
            newParent = node.ownerDocument.createElement("font");

            // "Set the color attribute of new parent to the result of applying
            // the rules for serializing simple color values to new value
            // (interpreted as a simple color)."
            newParent.setAttribute("color", parseSimpleColor(newValue));
        }

        // "If command is "fontName", let new parent be the result of calling
        // createElement("font") on the ownerDocument of node, then set the
        // face attribute of new parent to new value."
        if (command == "fontname") {
            newParent = node.ownerDocument.createElement("font");
            newParent.face = newValue;
        }
    }

    // "If command is "createLink" or "unlink":"
    if (command == "createlink" || command == "unlink") {
        // "Let new parent be the result of calling createElement("a") on the
        // ownerDocument of node."
        newParent = node.ownerDocument.createElement("a");

        // "Set the href attribute of new parent to new value."
        newParent.setAttribute("href", newValue);

        // "Let ancestor be node's parent."
        var ancestor = node.parentNode;

        // "While ancestor is not null:"
        while (ancestor) {
            // "If ancestor is an a, set the tag name of ancestor to "span",
            // and let ancestor be the result."
            if (isHtmlElement(ancestor, "A")) {
                ancestor = setTagName(ancestor, "span");
            }

            // "Set ancestor to its parent."
            ancestor = ancestor.parentNode;
        }
    }

    // "If command is "fontSize"; and new value is one of "x-small", "small",
    // "medium", "large", "x-large", "xx-large", or "xxx-large"; and either the
    // CSS styling flag is false, or new value is "xxx-large": let new parent
    // be the result of calling createElement("font") on the ownerDocument of
    // node, then set the size attribute of new parent to the number from the
    // following table based on new value: [table omitted]"
    if (command == "fontsize"
    && ["x-small", "small", "medium", "large", "x-large", "xx-large", "xxx-large"].indexOf(newValue) != -1
    && (!cssStylingFlag || newValue == "xxx-large")) {
        newParent = node.ownerDocument.createElement("font");
        newParent.size = cssSizeToLegacy(newValue);
    }

    // "If command is "subscript" or "superscript" and new value is
    // "subscript", let new parent be the result of calling
    // createElement("sub") on the ownerDocument of node."
    if ((command == "subscript" || command == "superscript")
    && newValue == "subscript") {
        newParent = node.ownerDocument.createElement("sub");
    }

    // "If command is "subscript" or "superscript" and new value is
    // "superscript", let new parent be the result of calling
    // createElement("sup") on the ownerDocument of node."
    if ((command == "subscript" || command == "superscript")
    && newValue == "superscript") {
        newParent = node.ownerDocument.createElement("sup");
    }

    // "If new parent is null, let new parent be the result of calling
    // createElement("span") on the ownerDocument of node."
    if (!newParent) {
        newParent = node.ownerDocument.createElement("span");
    }

    // "Insert new parent in node's parent before node."
    node.parentNode.insertBefore(newParent, node);

    // "If the effective command value of command for new parent is not loosely
    // equivalent to new value, and the relevant CSS property for command is
    // not null, set that CSS property of new parent to new value (if the new
    // value would be valid)."
    var property = commands[command].relevantCssProperty;
    if (property !== null
    && !areLooselyEquivalentValues(command, getEffectiveCommandValue(newParent, command), newValue)) {
        newParent.style[property] = newValue;
    }

    // "If command is "strikethrough", and new value is "line-through", and the
    // effective command value of "strikethrough" for new parent is not
    // "line-through", set the "text-decoration" property of new parent to
    // "line-through"."
    if (command == "strikethrough"
    && newValue == "line-through"
    && getEffectiveCommandValue(newParent, "strikethrough") != "line-through") {
        newParent.style.textDecoration = "line-through";
    }

    // "If command is "underline", and new value is "underline", and the
    // effective command value of "underline" for new parent is not
    // "underline", set the "text-decoration" property of new parent to
    // "underline"."
    if (command == "underline"
    && newValue == "underline"
    && getEffectiveCommandValue(newParent, "underline") != "underline") {
        newParent.style.textDecoration = "underline";
    }

    // "Append node to new parent as its last child, preserving ranges."
    movePreservingRanges(node, newParent, newParent.childNodes.length);

    // "If node is an Element and the effective command value of command for
    // node is not loosely equivalent to new value:"
    if (node.nodeType == Node.ELEMENT_NODE
    && !areEquivalentValues(command, getEffectiveCommandValue(node, command), newValue)) {
        // "Insert node into the parent of new parent before new parent,
        // preserving ranges."
        movePreservingRanges(node, newParent.parentNode, getNodeIndex(newParent));

        // "Remove new parent from its parent."
        newParent.parentNode.removeChild(newParent);

        // "Let children be all children of node, omitting any that are
        // Elements whose specified command value for command is neither null
        // nor equivalent to new value."
        var children = [];
        for (var i = 0; i < node.childNodes.length; i++) {
            if (node.childNodes[i].nodeType == Node.ELEMENT_NODE) {
                var specifiedValue = getSpecifiedCommandValue(node.childNodes[i], command);

                if (specifiedValue !== null
                && !areEquivalentValues(command, newValue, specifiedValue)) {
                    continue;
                }
            }
            children.push(node.childNodes[i]);
        }

        // "Force the value of each Node in children, with command and new
        // value as in this invocation of the algorithm."
        for (var i = 0; i < children.length; i++) {
            forceValue(children[i], command, newValue);
        }
    }
}


//@}
///// Setting the selection's value /////
//@{

function setSelectionValue(command, newValue) {
    // "If there is no formattable node effectively contained in the active
    // range:"
    if (!getAllEffectivelyContainedNodes(getActiveRange())
    .some(isFormattableNode)) {
        // "If command has inline command activated values, set the state
        // override to true if new value is among them and false if it's not."
        if ("inlineCommandActivatedValues" in commands[command]) {
            setStateOverride(command, commands[command].inlineCommandActivatedValues
                .indexOf(newValue) != -1);
        }

        // "If command is "subscript", unset the state override for
        // "superscript"."
        if (command == "subscript") {
            unsetStateOverride("superscript");
        }

        // "If command is "superscript", unset the state override for
        // "subscript"."
        if (command == "superscript") {
            unsetStateOverride("subscript");
        }

        // "If new value is null, unset the value override (if any)."
        if (newValue === null) {
            unsetValueOverride(command);

        // "Otherwise, if command is "createLink" or it has a value specified,
        // set the value override to new value."
        } else if (command == "createlink" || "value" in commands[command]) {
            setValueOverride(command, newValue);
        }

        // "Abort these steps."
        return;
    }

    // "If the active range's start node is an editable Text node, and its
    // start offset is neither zero nor its start node's length, call
    // splitText() on the active range's start node, with argument equal to the
    // active range's start offset. Then set the active range's start node to
    // the result, and its start offset to zero."
    if (isEditable(getActiveRange().startContainer)
    && getActiveRange().startContainer.nodeType == Node.TEXT_NODE
    && getActiveRange().startOffset != 0
    && getActiveRange().startOffset != getNodeLength(getActiveRange().startContainer)) {
        // Account for browsers not following range mutation rules
        var newActiveRange = document.createRange();
        var newNode;
        if (getActiveRange().startContainer == getActiveRange().endContainer) {
            var newEndOffset = getActiveRange().endOffset - getActiveRange().startOffset;
            newNode = getActiveRange().startContainer.splitText(getActiveRange().startOffset);
            newActiveRange.setEnd(newNode, newEndOffset);
            getActiveRange().setEnd(newNode, newEndOffset);
        } else {
            newNode = getActiveRange().startContainer.splitText(getActiveRange().startOffset);
        }
        newActiveRange.setStart(newNode, 0);
        getSelection().removeAllRanges();
        getSelection().addRange(newActiveRange);

        getActiveRange().setStart(newNode, 0);
    }

    // "If the active range's end node is an editable Text node, and its end
    // offset is neither zero nor its end node's length, call splitText() on
    // the active range's end node, with argument equal to the active range's
    // end offset."
    if (isEditable(getActiveRange().endContainer)
    && getActiveRange().endContainer.nodeType == Node.TEXT_NODE
    && getActiveRange().endOffset != 0
    && getActiveRange().endOffset != getNodeLength(getActiveRange().endContainer)) {
        // IE seems to mutate the range incorrectly here, so we need correction
        // here as well.  The active range will be temporarily in orphaned
        // nodes, so calling getActiveRange() after splitText() but before
        // fixing the range will throw an exception.
        var activeRange = getActiveRange();
        var newStart = [activeRange.startContainer, activeRange.startOffset];
        var newEnd = [activeRange.endContainer, activeRange.endOffset];
        activeRange.endContainer.splitText(activeRange.endOffset);
        activeRange.setStart(newStart[0], newStart[1]);
        activeRange.setEnd(newEnd[0], newEnd[1]);

        getSelection().removeAllRanges();
        getSelection().addRange(activeRange);
    }

    // "Let element list be all editable Elements effectively contained in the
    // active range.
    //
    // "For each element in element list, clear the value of element."
    getAllEffectivelyContainedNodes(getActiveRange(), function(node) {
        return isEditable(node) && node.nodeType == Node.ELEMENT_NODE;
    }).forEach(function(element) {
        clearValue(element, command);
    });

    // "Let node list be all editable nodes effectively contained in the active
    // range.
    //
    // "For each node in node list:"
    getAllEffectivelyContainedNodes(getActiveRange(), isEditable).forEach(function(node) {
        // "Push down values on node."
        pushDownValues(node, command, newValue);

        // "If node is an allowed child of span, force the value of node."
        if (isAllowedChild(node, "span")) {
            forceValue(node, command, newValue);
        }
    });
}


//@}
///// The backColor command /////
//@{
commands.backcolor = {
    // Copy-pasted, same as hiliteColor
    action: function(value) {
        // Action is further copy-pasted, same as foreColor

        // "If value is not a valid CSS color, prepend "#" to it."
        //
        // "If value is still not a valid CSS color, or if it is currentColor,
        // return false."
        //
        // Cheap hack for testing, no attempt to be comprehensive.
        if (/^([0-9a-fA-F]{3}){1,2}$/.test(value)) {
            value = "#" + value;
        }
        if (!/^(rgba?|hsla?)\(.*\)$/.test(value)
        && !parseSimpleColor(value)
        && value.toLowerCase() != "transparent") {
            return false;
        }

        // "Set the selection's value to value."
        setSelectionValue("backcolor", value);

        // "Return true."
        return true;
    }, standardInlineValueCommand: true, relevantCssProperty: "backgroundColor",
    equivalentValues: function(val1, val2) {
        // "Either both strings are valid CSS colors and have the same red,
        // green, blue, and alpha components, or neither string is a valid CSS
        // color."
        return normalizeColor(val1) === normalizeColor(val2);
    },
};

//@}
///// The bold command /////
//@{
commands.bold = {
    action: function() {
        // "If queryCommandState("bold") returns true, set the selection's
        // value to "normal". Otherwise set the selection's value to "bold".
        // Either way, return true."
        if (myQueryCommandState("bold")) {
            setSelectionValue("bold", "normal");
        } else {
            setSelectionValue("bold", "bold");
        }
        return true;
    }, inlineCommandActivatedValues: ["bold", "600", "700", "800", "900"],
    relevantCssProperty: "fontWeight",
    equivalentValues: function(val1, val2) {
        // "Either the two strings are equal, or one is "bold" and the other is
        // "700", or one is "normal" and the other is "400"."
        return val1 == val2
            || (val1 == "bold" && val2 == "700")
            || (val1 == "700" && val2 == "bold")
            || (val1 == "normal" && val2 == "400")
            || (val1 == "400" && val2 == "normal");
    },
};

//@}
///// The createLink command /////
//@{
commands.createlink = {
    action: function(value) {
        // "If value is the empty string, return false."
        if (value === "") {
            return false;
        }

        // "For each editable a element that has an href attribute and is an
        // ancestor of some node effectively contained in the active range, set
        // that a element's href attribute to value."
        //
        // TODO: We don't actually do this in tree order, not that it matters
        // unless you're spying with mutation events.
        getAllEffectivelyContainedNodes(getActiveRange()).forEach(function(node) {
            getAncestors(node).forEach(function(ancestor) {
                if (isEditable(ancestor)
                && isHtmlElement(ancestor, "a")
                && ancestor.hasAttribute("href")) {
                    ancestor.setAttribute("href", value);
                }
            });
        });

        // "Set the selection's value to value."
        setSelectionValue("createlink", value);

        // "Return true."
        return true;
    }
};

//@}
///// The fontName command /////
//@{
commands.fontname = {
    action: function(value) {
        // "Set the selection's value to value, then return true."
        setSelectionValue("fontname", value);
        return true;
    }, standardInlineValueCommand: true, relevantCssProperty: "fontFamily"
};

//@}
///// The fontSize command /////
//@{

// Helper function for fontSize's action plus queryOutputHelper.  It's just the
// middle of fontSize's action, ripped out into its own function.  Returns null
// if the size is invalid.
function normalizeFontSize(value) {
    // "Strip leading and trailing whitespace from value."
    //
    // Cheap hack, not following the actual algorithm.
    value = value.trim();

    // "If value is not a valid floating point number, and would not be a valid
    // floating point number if a single leading "+" character were stripped,
    // return false."
    if (!/^[-+]?[0-9]+(\.[0-9]+)?([eE][-+]?[0-9]+)?$/.test(value)) {
        return null;
    }

    var mode;

    // "If the first character of value is "+", delete the character and let
    // mode be "relative-plus"."
    if (value[0] == "+") {
        value = value.slice(1);
        mode = "relative-plus";
    // "Otherwise, if the first character of value is "-", delete the character
    // and let mode be "relative-minus"."
    } else if (value[0] == "-") {
        value = value.slice(1);
        mode = "relative-minus";
    // "Otherwise, let mode be "absolute"."
    } else {
        mode = "absolute";
    }

    // "Apply the rules for parsing non-negative integers to value, and let
    // number be the result."
    //
    // Another cheap hack.
    var num = parseInt(value);

    // "If mode is "relative-plus", add three to number."
    if (mode == "relative-plus") {
        num += 3;
    }

    // "If mode is "relative-minus", negate number, then add three to it."
    if (mode == "relative-minus") {
        num = 3 - num;
    }

    // "If number is less than one, let number equal 1."
    if (num < 1) {
        num = 1;
    }

    // "If number is greater than seven, let number equal 7."
    if (num > 7) {
        num = 7;
    }

    // "Set value to the string here corresponding to number:" [table omitted]
    value = {
        1: "x-small",
        2: "small",
        3: "medium",
        4: "large",
        5: "x-large",
        6: "xx-large",
        7: "xxx-large"
    }[num];

    return value;
}

commands.fontsize = {
    action: function(value) {
        value = normalizeFontSize(value);
        if (value === null) {
            return false;
        }

        // "Set the selection's value to value."
        setSelectionValue("fontsize", value);

        // "Return true."
        return true;
    }, indeterm: function() {
        // "True if among formattable nodes that are effectively contained in
        // the active range, there are two that have distinct effective command
        // values.  Otherwise false."
        return getAllEffectivelyContainedNodes(getActiveRange(), isFormattableNode)
        .map(function(node) {
            return getEffectiveCommandValue(node, "fontsize");
        }).filter(function(value, i, arr) {
            return arr.slice(0, i).indexOf(value) == -1;
        }).length >= 2;
    }, value: function() {
        // "If the active range is null, return the empty string."
        if (!getActiveRange()) {
            return "";
        }

        // "Let pixel size be the effective command value of the first
        // formattable node that is effectively contained in the active range,
        // or if there is no such node, the effective command value of the
        // active range's start node, in either case interpreted as a number of
        // pixels."
        var node = getAllEffectivelyContainedNodes(getActiveRange(), isFormattableNode)[0];
        if (node === undefined) {
            node = getActiveRange().startContainer;
        }
        var pixelSize = getEffectiveCommandValue(node, "fontsize");

        // "Return the legacy font size for pixel size."
        return getLegacyFontSize(pixelSize);
    }, relevantCssProperty: "fontSize"
};

function getLegacyFontSize(size) {
    if (getLegacyFontSize.resultCache === undefined) {
        getLegacyFontSize.resultCache = {};
    }

    if (getLegacyFontSize.resultCache[size] !== undefined) {
        return getLegacyFontSize.resultCache[size];
    }

    // For convenience in other places in my code, I handle all sizes, not just
    // pixel sizes as the spec says.  This means pixel sizes have to be passed
    // in suffixed with "px", not as plain numbers.
    if (normalizeFontSize(size) !== null) {
        return getLegacyFontSize.resultCache[size] = cssSizeToLegacy(normalizeFontSize(size));
    }

    if (["x-small", "x-small", "small", "medium", "large", "x-large", "xx-large", "xxx-large"].indexOf(size) == -1
    && !/^[0-9]+(\.[0-9]+)?(cm|mm|in|pt|pc|px)$/.test(size)) {
        // There is no sensible legacy size for things like "2em".
        return getLegacyFontSize.resultCache[size] = null;
    }

    var font = document.createElement("font");
    document.body.appendChild(font);
    if (size == "xxx-large") {
        font.size = 7;
    } else {
        font.style.fontSize = size;
    }
    var pixelSize = parseInt(getComputedStyle(font).fontSize);
    document.body.removeChild(font);

    // "Let returned size be 1."
    var returnedSize = 1;

    // "While returned size is less than 7:"
    while (returnedSize < 7) {
        // "Let lower bound be the resolved value of "font-size" in pixels
        // of a font element whose size attribute is set to returned size."
        var font = document.createElement("font");
        font.size = returnedSize;
        document.body.appendChild(font);
        var lowerBound = parseInt(getComputedStyle(font).fontSize);

        // "Let upper bound be the resolved value of "font-size" in pixels
        // of a font element whose size attribute is set to one plus
        // returned size."
        font.size = 1 + returnedSize;
        var upperBound = parseInt(getComputedStyle(font).fontSize);
        document.body.removeChild(font);

        // "Let average be the average of upper bound and lower bound."
        var average = (upperBound + lowerBound)/2;

        // "If pixel size is less than average, return the one-element
        // string consisting of the digit returned size."
        if (pixelSize < average) {
            return getLegacyFontSize.resultCache[size] = String(returnedSize);
        }

        // "Add one to returned size."
        returnedSize++;
    }

    // "Return "7"."
    return getLegacyFontSize.resultCache[size] = "7";
}

//@}
///// The foreColor command /////
//@{
commands.forecolor = {
    action: function(value) {
        // Copy-pasted, same as backColor and hiliteColor

        // "If value is not a valid CSS color, prepend "#" to it."
        //
        // "If value is still not a valid CSS color, or if it is currentColor,
        // return false."
        //
        // Cheap hack for testing, no attempt to be comprehensive.
        if (/^([0-9a-fA-F]{3}){1,2}$/.test(value)) {
            value = "#" + value;
        }
        if (!/^(rgba?|hsla?)\(.*\)$/.test(value)
        && !parseSimpleColor(value)
        && value.toLowerCase() != "transparent") {
            return false;
        }

        // "Set the selection's value to value."
        setSelectionValue("forecolor", value);

        // "Return true."
        return true;
    }, standardInlineValueCommand: true, relevantCssProperty: "color",
    equivalentValues: function(val1, val2) {
        // "Either both strings are valid CSS colors and have the same red,
        // green, blue, and alpha components, or neither string is a valid CSS
        // color."
        return normalizeColor(val1) === normalizeColor(val2);
    },
};

//@}
///// The hiliteColor command /////
//@{
commands.hilitecolor = {
    // Copy-pasted, same as backColor
    action: function(value) {
        // Action is further copy-pasted, same as foreColor

        // "If value is not a valid CSS color, prepend "#" to it."
        //
        // "If value is still not a valid CSS color, or if it is currentColor,
        // return false."
        //
        // Cheap hack for testing, no attempt to be comprehensive.
        if (/^([0-9a-fA-F]{3}){1,2}$/.test(value)) {
            value = "#" + value;
        }
        if (!/^(rgba?|hsla?)\(.*\)$/.test(value)
        && !parseSimpleColor(value)
        && value.toLowerCase() != "transparent") {
            return false;
        }

        // "Set the selection's value to value."
        setSelectionValue("hilitecolor", value);

        // "Return true."
        return true;
    }, indeterm: function() {
        // "True if among editable Text nodes that are effectively contained in
        // the active range, there are two that have distinct effective command
        // values.  Otherwise false."
        return getAllEffectivelyContainedNodes(getActiveRange(), function(node) {
            return isEditable(node) && node.nodeType == Node.TEXT_NODE;
        }).map(function(node) {
            return getEffectiveCommandValue(node, "hilitecolor");
        }).filter(function(value, i, arr) {
            return arr.slice(0, i).indexOf(value) == -1;
        }).length >= 2;
    }, standardInlineValueCommand: true, relevantCssProperty: "backgroundColor",
    equivalentValues: function(val1, val2) {
        // "Either both strings are valid CSS colors and have the same red,
        // green, blue, and alpha components, or neither string is a valid CSS
        // color."
        return normalizeColor(val1) === normalizeColor(val2);
    },
};

//@}
///// The italic command /////
//@{
commands.italic = {
    action: function() {
        // "If queryCommandState("italic") returns true, set the selection's
        // value to "normal". Otherwise set the selection's value to "italic".
        // Either way, return true."
        if (myQueryCommandState("italic")) {
            setSelectionValue("italic", "normal");
        } else {
            setSelectionValue("italic", "italic");
        }
        return true;
    }, inlineCommandActivatedValues: ["italic", "oblique"],
    relevantCssProperty: "fontStyle"
};

//@}
///// The removeFormat command /////
//@{
commands.removeformat = {
    action: function() {
        // "A removeFormat candidate is an editable HTML element with local
        // name "abbr", "acronym", "b", "bdi", "bdo", "big", "blink", "cite",
        // "code", "dfn", "em", "font", "i", "ins", "kbd", "mark", "nobr", "q",
        // "s", "samp", "small", "span", "strike", "strong", "sub", "sup",
        // "tt", "u", or "var"."
        function isRemoveFormatCandidate(node) {
            return isEditable(node)
                && isHtmlElement(node, ["abbr", "acronym", "b", "bdi", "bdo",
                "big", "blink", "cite", "code", "dfn", "em", "font", "i",
                "ins", "kbd", "mark", "nobr", "q", "s", "samp", "small",
                "span", "strike", "strong", "sub", "sup", "tt", "u", "var"]);
        }

        // "Let elements to remove be a list of every removeFormat candidate
        // effectively contained in the active range."
        var elementsToRemove = getAllEffectivelyContainedNodes(getActiveRange(), isRemoveFormatCandidate);

        // "For each element in elements to remove:"
        elementsToRemove.forEach(function(element) {
            // "While element has children, insert the first child of element
            // into the parent of element immediately before element,
            // preserving ranges."
            while (element.hasChildNodes()) {
                movePreservingRanges(element.firstChild, element.parentNode, getNodeIndex(element));
            }

            // "Remove element from its parent."
            element.parentNode.removeChild(element);
        });

        // "If the active range's start node is an editable Text node, and its
        // start offset is neither zero nor its start node's length, call
        // splitText() on the active range's start node, with argument equal to
        // the active range's start offset. Then set the active range's start
        // node to the result, and its start offset to zero."
        if (isEditable(getActiveRange().startContainer)
        && getActiveRange().startContainer.nodeType == Node.TEXT_NODE
        && getActiveRange().startOffset != 0
        && getActiveRange().startOffset != getNodeLength(getActiveRange().startContainer)) {
            // Account for browsers not following range mutation rules
            if (getActiveRange().startContainer == getActiveRange().endContainer) {
                var newEnd = getActiveRange().endOffset - getActiveRange().startOffset;
                var newNode = getActiveRange().startContainer.splitText(getActiveRange().startOffset);
                getActiveRange().setStart(newNode, 0);
                getActiveRange().setEnd(newNode, newEnd);
            } else {
                getActiveRange().setStart(getActiveRange().startContainer.splitText(getActiveRange().startOffset), 0);
            }
        }

        // "If the active range's end node is an editable Text node, and its
        // end offset is neither zero nor its end node's length, call
        // splitText() on the active range's end node, with argument equal to
        // the active range's end offset."
        if (isEditable(getActiveRange().endContainer)
        && getActiveRange().endContainer.nodeType == Node.TEXT_NODE
        && getActiveRange().endOffset != 0
        && getActiveRange().endOffset != getNodeLength(getActiveRange().endContainer)) {
            // IE seems to mutate the range incorrectly here, so we need
            // correction here as well.  Have to be careful to set the range to
            // something not including the text node so that getActiveRange()
            // doesn't throw an exception due to a temporarily detached
            // endpoint.
            var newStart = [getActiveRange().startContainer, getActiveRange().startOffset];
            var newEnd = [getActiveRange().endContainer, getActiveRange().endOffset];
            getActiveRange().setEnd(document.documentElement, 0);
            newEnd[0].splitText(newEnd[1]);
            getActiveRange().setStart(newStart[0], newStart[1]);
            getActiveRange().setEnd(newEnd[0], newEnd[1]);
        }

        // "Let node list consist of all editable nodes effectively contained
        // in the active range."
        //
        // "For each node in node list, while node's parent is a removeFormat
        // candidate in the same editing host as node, split the parent of the
        // one-node list consisting of node."
        getAllEffectivelyContainedNodes(getActiveRange(), isEditable).forEach(function(node) {
            while (isRemoveFormatCandidate(node.parentNode)
            && inSameEditingHost(node.parentNode, node)) {
                splitParent([node]);
            }
        });

        // "For each of the entries in the following list, in the given order,
        // set the selection's value to null, with command as given."
        [
            "subscript",
            "bold",
            "fontname",
            "fontsize",
            "forecolor",
            "hilitecolor",
            "italic",
            "strikethrough",
            "underline",
        ].forEach(function(command) {
            setSelectionValue(command, null);
        });

        // "Return true."
        return true;
    }
};

//@}
///// The strikethrough command /////
//@{
commands.strikethrough = {
    action: function() {
        // "If queryCommandState("strikethrough") returns true, set the
        // selection's value to null. Otherwise set the selection's value to
        // "line-through".  Either way, return true."
        if (myQueryCommandState("strikethrough")) {
            setSelectionValue("strikethrough", null);
        } else {
            setSelectionValue("strikethrough", "line-through");
        }
        return true;
    }, inlineCommandActivatedValues: ["line-through"]
};

//@}
///// The subscript command /////
//@{
commands.subscript = {
    action: function() {
        // "Call queryCommandState("subscript"), and let state be the result."
        var state = myQueryCommandState("subscript");

        // "Set the selection's value to null."
        setSelectionValue("subscript", null);

        // "If state is false, set the selection's value to "subscript"."
        if (!state) {
            setSelectionValue("subscript", "subscript");
        }

        // "Return true."
        return true;
    }, indeterm: function() {
        // "True if either among formattable nodes that are effectively
        // contained in the active range, there is at least one with effective
        // command value "subscript" and at least one with some other effective
        // command value; or if there is some formattable node effectively
        // contained in the active range with effective command value "mixed".
        // Otherwise false."
        var nodes = getAllEffectivelyContainedNodes(getActiveRange(), isFormattableNode);
        return (nodes.some(function(node) { return getEffectiveCommandValue(node, "subscript") == "subscript" })
            && nodes.some(function(node) { return getEffectiveCommandValue(node, "subscript") != "subscript" }))
            || nodes.some(function(node) { return getEffectiveCommandValue(node, "subscript") == "mixed" });
    }, inlineCommandActivatedValues: ["subscript"],
};

//@}
///// The superscript command /////
//@{
commands.superscript = {
    action: function() {
        // "Call queryCommandState("superscript"), and let state be the
        // result."
        var state = myQueryCommandState("superscript");

        // "Set the selection's value to null."
        setSelectionValue("superscript", null);

        // "If state is false, set the selection's value to "superscript"."
        if (!state) {
            setSelectionValue("superscript", "superscript");
        }

        // "Return true."
        return true;
    }, indeterm: function() {
        // "True if either among formattable nodes that are effectively
        // contained in the active range, there is at least one with effective
        // command value "superscript" and at least one with some other
        // effective command value; or if there is some formattable node
        // effectively contained in the active range with effective command
        // value "mixed".  Otherwise false."
        var nodes = getAllEffectivelyContainedNodes(getActiveRange(), isFormattableNode);
        return (nodes.some(function(node) { return getEffectiveCommandValue(node, "superscript") == "superscript" })
            && nodes.some(function(node) { return getEffectiveCommandValue(node, "superscript") != "superscript" }))
            || nodes.some(function(node) { return getEffectiveCommandValue(node, "superscript") == "mixed" });
    }, inlineCommandActivatedValues: ["superscript"],
};

//@}
///// The underline command /////
//@{
commands.underline = {
    action: function() {
        // "If queryCommandState("underline") returns true, set the selection's
        // value to null. Otherwise set the selection's value to "underline".
        // Either way, return true."
        if (myQueryCommandState("underline")) {
            setSelectionValue("underline", null);
        } else {
            setSelectionValue("underline", "underline");
        }
        return true;
    }, inlineCommandActivatedValues: ["underline"]
};

//@}
///// The unlink command /////
//@{
commands.unlink = {
    action: function() {
        // "Let hyperlinks be a list of every a element that has an href
        // attribute and is contained in the active range or is an ancestor of
        // one of its boundary points."
        //
        // As usual, take care to ensure it's tree order.  The correctness of
        // the following is left as an exercise for the reader.
        var range = getActiveRange();
        var hyperlinks = [];
        for (
            var node = range.startContainer;
            node;
            node = node.parentNode
        ) {
            if (isHtmlElement(node, "A")
            && node.hasAttribute("href")) {
                hyperlinks.unshift(node);
            }
        }
        for (
            var node = range.startContainer;
            node != nextNodeDescendants(range.endContainer);
            node = nextNode(node)
        ) {
            if (isHtmlElement(node, "A")
            && node.hasAttribute("href")
            && (isContained(node, range)
            || isAncestor(node, range.endContainer)
            || node == range.endContainer)) {
                hyperlinks.push(node);
            }
        }

        // "Clear the value of each member of hyperlinks."
        for (var i = 0; i < hyperlinks.length; i++) {
            clearValue(hyperlinks[i], "unlink");
        }

        // "Return true."
        return true;
    }
};

//@}

/////////////////////////////////////
///// Block formatting commands /////
/////////////////////////////////////

///// Block formatting command definitions /////
//@{

// "An indentation element is either a blockquote, or a div that has a style
// attribute that sets "margin" or some subproperty of it."
function isIndentationElement(node) {
    if (!isHtmlElement(node)) {
        return false;
    }

    if (node.tagName == "BLOCKQUOTE") {
        return true;
    }

    if (node.tagName != "DIV") {
        return false;
    }

    for (var i = 0; i < node.style.length; i++) {
        // Approximate check
        if (/^(-[a-z]+-)?margin/.test(node.style[i])) {
            return true;
        }
    }

    return false;
}

// "A simple indentation element is an indentation element that has no
// attributes except possibly
//
//   * "a style attribute that sets no properties other than "margin",
//     "border", "padding", or subproperties of those; and/or
//   * "a dir attribute."
function isSimpleIndentationElement(node) {
    if (!isIndentationElement(node)) {
        return false;
    }

    for (var i = 0; i < node.attributes.length; i++) {
        if (!isHtmlNamespace(node.attributes[i].namespaceURI)
        || ["style", "dir"].indexOf(node.attributes[i].name) == -1) {
            return false;
        }
    }

    for (var i = 0; i < node.style.length; i++) {
        // This is approximate, but it works well enough for my purposes.
        if (!/^(-[a-z]+-)?(margin|border|padding)/.test(node.style[i])) {
            return false;
        }
    }

    return true;
}

// "A non-list single-line container is an HTML element with local name
// "address", "div", "h1", "h2", "h3", "h4", "h5", "h6", "listing", "p", "pre",
// or "xmp"."
function isNonListSingleLineContainer(node) {
    return isHtmlElement(node, ["address", "div", "h1", "h2", "h3", "h4", "h5",
        "h6", "listing", "p", "pre", "xmp"]);
}

// "A single-line container is either a non-list single-line container, or an
// HTML element with local name "li", "dt", or "dd"."
function isSingleLineContainer(node) {
    return isNonListSingleLineContainer(node)
        || isHtmlElement(node, ["li", "dt", "dd"]);
}

function getBlockNodeOf(node) {
    // "While node is an inline node, set node to its parent."
    while (isInlineNode(node)) {
        node = node.parentNode;
    }

    // "Return node."
    return node;
}

//@}
///// Assorted block formatting command algorithms /////
//@{

function fixDisallowedAncestors(node) {
    // "If node is not editable, abort these steps."
    if (!isEditable(node)) {
        return;
    }

    // "If node is not an allowed child of any of its ancestors in the same
    // editing host:"
    if (getAncestors(node).every(function(ancestor) {
        return !inSameEditingHost(node, ancestor)
            || !isAllowedChild(node, ancestor)
    })) {
        // "If node is a dd or dt, wrap the one-node list consisting of node,
        // with sibling criteria returning true for any dl with no attributes
        // and false otherwise, and new parent instructions returning the
        // result of calling createElement("dl") on the context object. Then
        // abort these steps."
        if (isHtmlElement(node, ["dd", "dt"])) {
            wrap([node],
                function(sibling) { return isHtmlElement(sibling, "dl") && !sibling.attributes.length },
                function() { return document.createElement("dl") });
            return;
        }

        // "If "p" is not an allowed child of the editing host of node, abort
        // these steps."
        if (!isAllowedChild("p", getEditingHostOf(node))) {
            return;
        }

        // "If node is not a prohibited paragraph child, abort these steps."
        if (!isProhibitedParagraphChild(node)) {
            return;
        }

        // "Set the tag name of node to the default single-line container name,
        // and let node be the result."
        node = setTagName(node, defaultSingleLineContainerName);

        // "Fix disallowed ancestors of node."
        fixDisallowedAncestors(node);

        // "Let children be node's children."
        var children = [].slice.call(node.childNodes);

        // "For each child in children, if child is a prohibited paragraph
        // child:"
        children.filter(isProhibitedParagraphChild)
        .forEach(function(child) {
            // "Record the values of the one-node list consisting of child, and
            // let values be the result."
            var values = recordValues([child]);

            // "Split the parent of the one-node list consisting of child."
            splitParent([child]);

            // "Restore the values from values."
            restoreValues(values);
        });

        // "Abort these steps."
        return;
    }

    // "Record the values of the one-node list consisting of node, and let
    // values be the result."
    var values = recordValues([node]);

    // "While node is not an allowed child of its parent, split the parent of
    // the one-node list consisting of node."
    while (!isAllowedChild(node, node.parentNode)) {
        splitParent([node]);
    }

    // "Restore the values from values."
    restoreValues(values);
}

function normalizeSublists(item) {
    // "If item is not an li or it is not editable or its parent is not
    // editable, abort these steps."
    if (!isHtmlElement(item, "LI")
    || !isEditable(item)
    || !isEditable(item.parentNode)) {
        return;
    }

    // "Let new item be null."
    var newItem = null;

    // "While item has an ol or ul child:"
    while ([].some.call(item.childNodes, function (node) { return isHtmlElement(node, ["OL", "UL"]) })) {
        // "Let child be the last child of item."
        var child = item.lastChild;

        // "If child is an ol or ul, or new item is null and child is a Text
        // node whose data consists of zero of more space characters:"
        if (isHtmlElement(child, ["OL", "UL"])
        || (!newItem && child.nodeType == Node.TEXT_NODE && /^[ \t\n\f\r]*$/.test(child.data))) {
            // "Set new item to null."
            newItem = null;

            // "Insert child into the parent of item immediately following
            // item, preserving ranges."
            movePreservingRanges(child, item.parentNode, 1 + getNodeIndex(item));

        // "Otherwise:"
        } else {
            // "If new item is null, let new item be the result of calling
            // createElement("li") on the ownerDocument of item, then insert
            // new item into the parent of item immediately after item."
            if (!newItem) {
                newItem = item.ownerDocument.createElement("li");
                item.parentNode.insertBefore(newItem, item.nextSibling);
            }

            // "Insert child into new item as its first child, preserving
            // ranges."
            movePreservingRanges(child, newItem, 0);
        }
    }
}

function getSelectionListState() {
    // "If the active range is null, return "none"."
    if (!getActiveRange()) {
        return "none";
    }

    // "Block-extend the active range, and let new range be the result."
    var newRange = blockExtend(getActiveRange());

    // "Let node list be a list of nodes, initially empty."
    //
    // "For each node contained in new range, append node to node list if the
    // last member of node list (if any) is not an ancestor of node; node is
    // editable; node is not an indentation element; and node is either an ol
    // or ul, or the child of an ol or ul, or an allowed child of "li"."
    var nodeList = getContainedNodes(newRange, function(node) {
        return isEditable(node)
            && !isIndentationElement(node)
            && (isHtmlElement(node, ["ol", "ul"])
            || isHtmlElement(node.parentNode, ["ol", "ul"])
            || isAllowedChild(node, "li"));
    });

    // "If node list is empty, return "none"."
    if (!nodeList.length) {
        return "none";
    }

    // "If every member of node list is either an ol or the child of an ol or
    // the child of an li child of an ol, and none is a ul or an ancestor of a
    // ul, return "ol"."
    if (nodeList.every(function(node) {
        return isHtmlElement(node, "ol")
            || isHtmlElement(node.parentNode, "ol")
            || (isHtmlElement(node.parentNode, "li") && isHtmlElement(node.parentNode.parentNode, "ol"));
    })
    && !nodeList.some(function(node) { return isHtmlElement(node, "ul") || ("querySelector" in node && node.querySelector("ul")) })) {
        return "ol";
    }

    // "If every member of node list is either a ul or the child of a ul or the
    // child of an li child of a ul, and none is an ol or an ancestor of an ol,
    // return "ul"."
    if (nodeList.every(function(node) {
        return isHtmlElement(node, "ul")
            || isHtmlElement(node.parentNode, "ul")
            || (isHtmlElement(node.parentNode, "li") && isHtmlElement(node.parentNode.parentNode, "ul"));
    })
    && !nodeList.some(function(node) { return isHtmlElement(node, "ol") || ("querySelector" in node && node.querySelector("ol")) })) {
        return "ul";
    }

    var hasOl = nodeList.some(function(node) {
        return isHtmlElement(node, "ol")
            || isHtmlElement(node.parentNode, "ol")
            || ("querySelector" in node && node.querySelector("ol"))
            || (isHtmlElement(node.parentNode, "li") && isHtmlElement(node.parentNode.parentNode, "ol"));
    });
    var hasUl = nodeList.some(function(node) {
        return isHtmlElement(node, "ul")
            || isHtmlElement(node.parentNode, "ul")
            || ("querySelector" in node && node.querySelector("ul"))
            || (isHtmlElement(node.parentNode, "li") && isHtmlElement(node.parentNode.parentNode, "ul"));
    });
    // "If some member of node list is either an ol or the child or ancestor of
    // an ol or the child of an li child of an ol, and some member of node list
    // is either a ul or the child or ancestor of a ul or the child of an li
    // child of a ul, return "mixed"."
    if (hasOl && hasUl) {
        return "mixed";
    }

    // "If some member of node list is either an ol or the child or ancestor of
    // an ol or the child of an li child of an ol, return "mixed ol"."
    if (hasOl) {
        return "mixed ol";
    }

    // "If some member of node list is either a ul or the child or ancestor of
    // a ul or the child of an li child of a ul, return "mixed ul"."
    if (hasUl) {
        return "mixed ul";
    }

    // "Return "none"."
    return "none";
}

function getAlignmentValue(node) {
    // "While node is neither null nor an Element, or it is an Element but its
    // "display" property has resolved value "inline" or "none", set node to
    // its parent."
    while ((node && node.nodeType != Node.ELEMENT_NODE)
    || (node.nodeType == Node.ELEMENT_NODE
    && ["inline", "none"].indexOf(getComputedStyle(node).display) != -1)) {
        node = node.parentNode;
    }

    // "If node is not an Element, return "left"."
    if (!node || node.nodeType != Node.ELEMENT_NODE) {
        return "left";
    }

    var resolvedValue = getComputedStyle(node).textAlign
        // Hack around browser non-standardness
        .replace(/^-(moz|webkit)-/, "")
        .replace(/^auto$/, "start");

    // "If node's "text-align" property has resolved value "start", return
    // "left" if the directionality of node is "ltr", "right" if it is "rtl"."
    if (resolvedValue == "start") {
        return getDirectionality(node) == "ltr" ? "left" : "right";
    }

    // "If node's "text-align" property has resolved value "end", return
    // "right" if the directionality of node is "ltr", "left" if it is "rtl"."
    if (resolvedValue == "end") {
        return getDirectionality(node) == "ltr" ? "right" : "left";
    }

    // "If node's "text-align" property has resolved value "center", "justify",
    // "left", or "right", return that value."
    if (["center", "justify", "left", "right"].indexOf(resolvedValue) != -1) {
        return resolvedValue;
    }

    // "Return "left"."
    return "left";
}

function getNextEquivalentPoint(node, offset) {
    // "If node's length is zero, return null."
    if (getNodeLength(node) == 0) {
        return null;
    }

    // "If offset is node's length, and node's parent is not null, and node is
    // an inline node, return (node's parent, 1 + node's index)."
    if (offset == getNodeLength(node)
    && node.parentNode
    && isInlineNode(node)) {
        return [node.parentNode, 1 + getNodeIndex(node)];
    }

    // "If node has a child with index offset, and that child's length is not
    // zero, and that child is an inline node, return (that child, 0)."
    if (0 <= offset
    && offset < node.childNodes.length
    && getNodeLength(node.childNodes[offset]) != 0
    && isInlineNode(node.childNodes[offset])) {
        return [node.childNodes[offset], 0];
    }

    // "Return null."
    return null;
}

function getPreviousEquivalentPoint(node, offset) {
    // "If node's length is zero, return null."
    if (getNodeLength(node) == 0) {
        return null;
    }

    // "If offset is 0, and node's parent is not null, and node is an inline
    // node, return (node's parent, node's index)."
    if (offset == 0
    && node.parentNode
    && isInlineNode(node)) {
        return [node.parentNode, getNodeIndex(node)];
    }

    // "If node has a child with index offset − 1, and that child's length is
    // not zero, and that child is an inline node, return (that child, that
    // child's length)."
    if (0 <= offset - 1
    && offset - 1 < node.childNodes.length
    && getNodeLength(node.childNodes[offset - 1]) != 0
    && isInlineNode(node.childNodes[offset - 1])) {
        return [node.childNodes[offset - 1], getNodeLength(node.childNodes[offset - 1])];
    }

    // "Return null."
    return null;
}

function getFirstEquivalentPoint(node, offset) {
    // "While (node, offset)'s previous equivalent point is not null, set
    // (node, offset) to its previous equivalent point."
    var prev;
    while (prev = getPreviousEquivalentPoint(node, offset)) {
        node = prev[0];
        offset = prev[1];
    }

    // "Return (node, offset)."
    return [node, offset];
}

function getLastEquivalentPoint(node, offset) {
    // "While (node, offset)'s next equivalent point is not null, set (node,
    // offset) to its next equivalent point."
    var next;
    while (next = getNextEquivalentPoint(node, offset)) {
        node = next[0];
        offset = next[1];
    }

    // "Return (node, offset)."
    return [node, offset];
}

//@}
///// Block-extending a range /////
//@{

// "A boundary point (node, offset) is a block start point if either node's
// parent is null and offset is zero; or node has a child with index offset −
// 1, and that child is either a visible block node or a visible br."
function isBlockStartPoint(node, offset) {
    return (!node.parentNode && offset == 0)
        || (0 <= offset - 1
        && offset - 1 < node.childNodes.length
        && isVisible(node.childNodes[offset - 1])
        && (isBlockNode(node.childNodes[offset - 1])
        || isHtmlElement(node.childNodes[offset - 1], "br")));
}

// "A boundary point (node, offset) is a block end point if either node's
// parent is null and offset is node's length; or node has a child with index
// offset, and that child is a visible block node."
function isBlockEndPoint(node, offset) {
    return (!node.parentNode && offset == getNodeLength(node))
        || (offset < node.childNodes.length
        && isVisible(node.childNodes[offset])
        && isBlockNode(node.childNodes[offset]));
}

// "A boundary point is a block boundary point if it is either a block start
// point or a block end point."
function isBlockBoundaryPoint(node, offset) {
    return isBlockStartPoint(node, offset)
        || isBlockEndPoint(node, offset);
}

function blockExtend(range) {
    // "Let start node, start offset, end node, and end offset be the start
    // and end nodes and offsets of the range."
    var startNode = range.startContainer;
    var startOffset = range.startOffset;
    var endNode = range.endContainer;
    var endOffset = range.endOffset;

    // "If some ancestor container of start node is an li, set start offset to
    // the index of the last such li in tree order, and set start node to that
    // li's parent."
    var liAncestors = getAncestors(startNode).concat(startNode)
        .filter(function(ancestor) { return isHtmlElement(ancestor, "li") })
        .slice(-1);
    if (liAncestors.length) {
        startOffset = getNodeIndex(liAncestors[0]);
        startNode = liAncestors[0].parentNode;
    }

    // "If (start node, start offset) is not a block start point, repeat the
    // following steps:"
    if (!isBlockStartPoint(startNode, startOffset)) do {
        // "If start offset is zero, set it to start node's index, then set
        // start node to its parent."
        if (startOffset == 0) {
            startOffset = getNodeIndex(startNode);
            startNode = startNode.parentNode;

        // "Otherwise, subtract one from start offset."
        } else {
            startOffset--;
        }

        // "If (start node, start offset) is a block boundary point, break from
        // this loop."
    } while (!isBlockBoundaryPoint(startNode, startOffset));

    // "While start offset is zero and start node's parent is not null, set
    // start offset to start node's index, then set start node to its parent."
    while (startOffset == 0
    && startNode.parentNode) {
        startOffset = getNodeIndex(startNode);
        startNode = startNode.parentNode;
    }

    // "If some ancestor container of end node is an li, set end offset to one
    // plus the index of the last such li in tree order, and set end node to
    // that li's parent."
    var liAncestors = getAncestors(endNode).concat(endNode)
        .filter(function(ancestor) { return isHtmlElement(ancestor, "li") })
        .slice(-1);
    if (liAncestors.length) {
        endOffset = 1 + getNodeIndex(liAncestors[0]);
        endNode = liAncestors[0].parentNode;
    }

    // "If (end node, end offset) is not a block end point, repeat the
    // following steps:"
    if (!isBlockEndPoint(endNode, endOffset)) do {
        // "If end offset is end node's length, set it to one plus end node's
        // index, then set end node to its parent."
        if (endOffset == getNodeLength(endNode)) {
            endOffset = 1 + getNodeIndex(endNode);
            endNode = endNode.parentNode;

        // "Otherwise, add one to end offset.
        } else {
            endOffset++;
        }

        // "If (end node, end offset) is a block boundary point, break from
        // this loop."
    } while (!isBlockBoundaryPoint(endNode, endOffset));

    // "While end offset is end node's length and end node's parent is not
    // null, set end offset to one plus end node's index, then set end node to
    // its parent."
    while (endOffset == getNodeLength(endNode)
    && endNode.parentNode) {
        endOffset = 1 + getNodeIndex(endNode);
        endNode = endNode.parentNode;
    }

    // "Let new range be a new range whose start and end nodes and offsets
    // are start node, start offset, end node, and end offset."
    var newRange = startNode.ownerDocument.createRange();
    newRange.setStart(startNode, startOffset);
    newRange.setEnd(endNode, endOffset);

    // "Return new range."
    return newRange;
}

function followsLineBreak(node) {
    // "Let offset be zero."
    var offset = 0;

    // "While (node, offset) is not a block boundary point:"
    while (!isBlockBoundaryPoint(node, offset)) {
        // "If node has a visible child with index offset minus one, return
        // false."
        if (0 <= offset - 1
        && offset - 1 < node.childNodes.length
        && isVisible(node.childNodes[offset - 1])) {
            return false;
        }

        // "If offset is zero or node has no children, set offset to node's
        // index, then set node to its parent."
        if (offset == 0
        || !node.hasChildNodes()) {
            offset = getNodeIndex(node);
            node = node.parentNode;

        // "Otherwise, set node to its child with index offset minus one, then
        // set offset to node's length."
        } else {
            node = node.childNodes[offset - 1];
            offset = getNodeLength(node);
        }
    }

    // "Return true."
    return true;
}

function precedesLineBreak(node) {
    // "Let offset be node's length."
    var offset = getNodeLength(node);

    // "While (node, offset) is not a block boundary point:"
    while (!isBlockBoundaryPoint(node, offset)) {
        // "If node has a visible child with index offset, return false."
        if (offset < node.childNodes.length
        && isVisible(node.childNodes[offset])) {
            return false;
        }

        // "If offset is node's length or node has no children, set offset to
        // one plus node's index, then set node to its parent."
        if (offset == getNodeLength(node)
        || !node.hasChildNodes()) {
            offset = 1 + getNodeIndex(node);
            node = node.parentNode;

        // "Otherwise, set node to its child with index offset and set offset
        // to zero."
        } else {
            node = node.childNodes[offset];
            offset = 0;
        }
    }

    // "Return true."
    return true;
}

//@}
///// Recording and restoring overrides /////
//@{

function recordCurrentOverrides() {
    // "Let overrides be a list of (string, string or boolean) ordered pairs,
    // initially empty."
    var overrides = [];

    // "If there is a value override for "createLink", add ("createLink", value
    // override for "createLink") to overrides."
    if (getValueOverride("createlink") !== undefined) {
        overrides.push(["createlink", getValueOverride("createlink")]);
    }

    // "For each command in the list "bold", "italic", "strikethrough",
    // "subscript", "superscript", "underline", in order: if there is a state
    // override for command, add (command, command's state override) to
    // overrides."
    ["bold", "italic", "strikethrough", "subscript", "superscript",
    "underline"].forEach(function(command) {
        if (getStateOverride(command) !== undefined) {
            overrides.push([command, getStateOverride(command)]);
        }
    });

    // "For each command in the list "fontName", "fontSize", "foreColor",
    // "hiliteColor", in order: if there is a value override for command, add
    // (command, command's value override) to overrides."
    ["fontname", "fontsize", "forecolor",
    "hilitecolor"].forEach(function(command) {
        if (getValueOverride(command) !== undefined) {
            overrides.push([command, getValueOverride(command)]);
        }
    });

    // "Return overrides."
    return overrides;
}

function recordCurrentStatesAndValues() {
    // "Let overrides be a list of (string, string or boolean) ordered pairs,
    // initially empty."
    var overrides = [];

    // "Let node be the first formattable node effectively contained in the
    // active range, or null if there is none."
    var node = getAllEffectivelyContainedNodes(getActiveRange())
        .filter(isFormattableNode)[0];

    // "If node is null, return overrides."
    if (!node) {
        return overrides;
    }

    // "Add ("createLink", node's effective command value for "createLink") to
    // overrides."
    overrides.push(["createlink", getEffectiveCommandValue(node, "createlink")]);

    // "For each command in the list "bold", "italic", "strikethrough",
    // "subscript", "superscript", "underline", in order: if node's effective
    // command value for command is one of its inline command activated values,
    // add (command, true) to overrides, and otherwise add (command, false) to
    // overrides."
    ["bold", "italic", "strikethrough", "subscript", "superscript",
    "underline"].forEach(function(command) {
        if (commands[command].inlineCommandActivatedValues
        .indexOf(getEffectiveCommandValue(node, command)) != -1) {
            overrides.push([command, true]);
        } else {
            overrides.push([command, false]);
        }
    });

    // "For each command in the list "fontName", "foreColor", "hiliteColor", in
    // order: add (command, command's value) to overrides."
    ["fontname", "fontsize", "forecolor", "hilitecolor"].forEach(function(command) {
        overrides.push([command, commands[command].value()]);
    });

    // "Add ("fontSize", node's effective command value for "fontSize") to
    // overrides."
    overrides.push(["fontsize", getEffectiveCommandValue(node, "fontsize")]);

    // "Return overrides."
    return overrides;
}

function restoreStatesAndValues(overrides) {
    // "Let node be the first formattable node effectively contained in the
    // active range, or null if there is none."
    var node = getAllEffectivelyContainedNodes(getActiveRange())
        .filter(isFormattableNode)[0];

    // "If node is not null, then for each (command, override) pair in
    // overrides, in order:"
    if (node) {
        for (var i = 0; i < overrides.length; i++) {
            var command = overrides[i][0];
            var override = overrides[i][1];

            // "If override is a boolean, and queryCommandState(command)
            // returns something different from override, take the action for
            // command, with value equal to the empty string."
            if (typeof override == "boolean"
            && myQueryCommandState(command) != override) {
                commands[command].action("");

            // "Otherwise, if override is a string, and command is neither
            // "createLink" nor "fontSize", and queryCommandValue(command)
            // returns something not equivalent to override, take the action
            // for command, with value equal to override."
            } else if (typeof override == "string"
            && command != "createlink"
            && command != "fontsize"
            && !areEquivalentValues(command, myQueryCommandValue(command), override)) {
                commands[command].action(override);

            // "Otherwise, if override is a string; and command is
            // "createLink"; and either there is a value override for
            // "createLink" that is not equal to override, or there is no value
            // override for "createLink" and node's effective command value for
            // "createLink" is not equal to override: take the action for
            // "createLink", with value equal to override."
            } else if (typeof override == "string"
            && command == "createlink"
            && (
                (
                    getValueOverride("createlink") !== undefined
                    && getValueOverride("createlink") !== override
                ) || (
                    getValueOverride("createlink") === undefined
                    && getEffectiveCommandValue(node, "createlink") !== override
                )
            )) {
                commands.createlink.action(override);

            // "Otherwise, if override is a string; and command is "fontSize";
            // and either there is a value override for "fontSize" that is not
            // equal to override, or there is no value override for "fontSize"
            // and node's effective command value for "fontSize" is not loosely
            // equivalent to override:"
            } else if (typeof override == "string"
            && command == "fontsize"
            && (
                (
                    getValueOverride("fontsize") !== undefined
                    && getValueOverride("fontsize") !== override
                ) || (
                    getValueOverride("fontsize") === undefined
                    && !areLooselyEquivalentValues(command, getEffectiveCommandValue(node, "fontsize"), override)
                )
            )) {
                // "Convert override to an integer number of pixels, and set
                // override to the legacy font size for the result."
                override = getLegacyFontSize(override);

                // "Take the action for "fontSize", with value equal to
                // override."
                commands.fontsize.action(override);

            // "Otherwise, continue this loop from the beginning."
            } else {
                continue;
            }

            // "Set node to the first formattable node effectively contained in
            // the active range, if there is one."
            node = getAllEffectivelyContainedNodes(getActiveRange())
                .filter(isFormattableNode)[0]
                || node;
        }

    // "Otherwise, for each (command, override) pair in overrides, in order:"
    } else {
        for (var i = 0; i < overrides.length; i++) {
            var command = overrides[i][0];
            var override = overrides[i][1];

            // "If override is a boolean, set the state override for command to
            // override."
            if (typeof override == "boolean") {
                setStateOverride(command, override);
            }

            // "If override is a string, set the value override for command to
            // override."
            if (typeof override == "string") {
                setValueOverride(command, override);
            }
        }
    }
}

//@}
///// Deleting the selection /////
//@{

// The flags argument is a dictionary that can have blockMerging,
// stripWrappers, and/or direction as keys.
function deleteSelection(flags) {
    if (flags === undefined) {
        flags = {};
    }

    var blockMerging = "blockMerging" in flags ? Boolean(flags.blockMerging) : true;
    var stripWrappers = "stripWrappers" in flags ? Boolean(flags.stripWrappers) : true;
    var direction = "direction" in flags ? flags.direction : "forward";

    // "If the active range is null, abort these steps and do nothing."
    if (!getActiveRange()) {
        return;
    }

    // "Canonicalize whitespace at the active range's start."
    canonicalizeWhitespace(getActiveRange().startContainer, getActiveRange().startOffset);

    // "Canonicalize whitespace at the active range's end."
    canonicalizeWhitespace(getActiveRange().endContainer, getActiveRange().endOffset);

    // "Let (start node, start offset) be the last equivalent point for the
    // active range's start."
    var start = getLastEquivalentPoint(getActiveRange().startContainer, getActiveRange().startOffset);
    var startNode = start[0];
    var startOffset = start[1];

    // "Let (end node, end offset) be the first equivalent point for the active
    // range's end."
    var end = getFirstEquivalentPoint(getActiveRange().endContainer, getActiveRange().endOffset);
    var endNode = end[0];
    var endOffset = end[1];

    // "If (end node, end offset) is not after (start node, start offset):"
    if (getPosition(endNode, endOffset, startNode, startOffset) !== "after") {
        // "If direction is "forward", call collapseToStart() on the context
        // object's Selection."
        //
        // Here and in a few other places, we check rangeCount to work around a
        // WebKit bug: it will sometimes incorrectly remove ranges from the
        // selection if nodes are removed, so collapseToStart() will throw.
        // This will break everything if we're using an actual selection, but
        // if getActiveRange() is really just returning globalRange and that's
        // all we care about, it will work fine.  I only add the extra check
        // for errors I actually hit in testing.
        if (direction == "forward") {
            if (getSelection().rangeCount) {
                getSelection().collapseToStart();
            }
            getActiveRange().collapse(true);

        // "Otherwise, call collapseToEnd() on the context object's Selection."
        } else {
            getSelection().collapseToEnd();
            getActiveRange().collapse(false);
        }

        // "Abort these steps."
        return;
    }

    // "If start node is a Text node and start offset is 0, set start offset to
    // the index of start node, then set start node to its parent."
    if (startNode.nodeType == Node.TEXT_NODE
    && startOffset == 0) {
        startOffset = getNodeIndex(startNode);
        startNode = startNode.parentNode;
    }

    // "If end node is a Text node and end offset is its length, set end offset
    // to one plus the index of end node, then set end node to its parent."
    if (endNode.nodeType == Node.TEXT_NODE
    && endOffset == getNodeLength(endNode)) {
        endOffset = 1 + getNodeIndex(endNode);
        endNode = endNode.parentNode;
    }

    // "Call collapse(start node, start offset) on the context object's
    // Selection."
    getSelection().collapse(startNode, startOffset);
    getActiveRange().setStart(startNode, startOffset);

    // "Call extend(end node, end offset) on the context object's Selection."
    getSelection().extend(endNode, endOffset);
    getActiveRange().setEnd(endNode, endOffset);

    // "Let start block be the active range's start node."
    var startBlock = getActiveRange().startContainer;

    // "While start block's parent is in the same editing host and start block
    // is an inline node, set start block to its parent."
    while (inSameEditingHost(startBlock, startBlock.parentNode)
    && isInlineNode(startBlock)) {
        startBlock = startBlock.parentNode;
    }

    // "If start block is neither a block node nor an editing host, or "span"
    // is not an allowed child of start block, or start block is a td or th,
    // set start block to null."
    if ((!isBlockNode(startBlock) && !isEditingHost(startBlock))
    || !isAllowedChild("span", startBlock)
    || isHtmlElement(startBlock, ["td", "th"])) {
        startBlock = null;
    }

    // "Let end block be the active range's end node."
    var endBlock = getActiveRange().endContainer;

    // "While end block's parent is in the same editing host and end block is
    // an inline node, set end block to its parent."
    while (inSameEditingHost(endBlock, endBlock.parentNode)
    && isInlineNode(endBlock)) {
        endBlock = endBlock.parentNode;
    }

    // "If end block is neither a block node nor an editing host, or "span" is
    // not an allowed child of end block, or end block is a td or th, set end
    // block to null."
    if ((!isBlockNode(endBlock) && !isEditingHost(endBlock))
    || !isAllowedChild("span", endBlock)
    || isHtmlElement(endBlock, ["td", "th"])) {
        endBlock = null;
    }

    // "Record current states and values, and let overrides be the result."
    var overrides = recordCurrentStatesAndValues();

    // "If start node and end node are the same, and start node is an editable
    // Text node:"
    if (startNode == endNode
    && isEditable(startNode)
    && startNode.nodeType == Node.TEXT_NODE) {
        // "Call deleteData(start offset, end offset − start offset) on start
        // node."
        startNode.deleteData(startOffset, endOffset - startOffset);

        // "Canonicalize whitespace at (start node, start offset), with fix
        // collapsed space false."
        canonicalizeWhitespace(startNode, startOffset, false);

        // "If direction is "forward", call collapseToStart() on the context
        // object's Selection."
        if (direction == "forward") {
            if (getSelection().rangeCount) {
                getSelection().collapseToStart();
            }
            getActiveRange().collapse(true);

        // "Otherwise, call collapseToEnd() on the context object's Selection."
        } else {
            getSelection().collapseToEnd();
            getActiveRange().collapse(false);
        }

        // "Restore states and values from overrides."
        restoreStatesAndValues(overrides);

        // "Abort these steps."
        return;
    }

    // "If start node is an editable Text node, call deleteData() on it, with
    // start offset as the first argument and (length of start node − start
    // offset) as the second argument."
    if (isEditable(startNode)
    && startNode.nodeType == Node.TEXT_NODE) {
        startNode.deleteData(startOffset, getNodeLength(startNode) - startOffset);
    }

    // "Let node list be a list of nodes, initially empty."
    //
    // "For each node contained in the active range, append node to node list
    // if the last member of node list (if any) is not an ancestor of node;
    // node is editable; and node is not a thead, tbody, tfoot, tr, th, or td."
    var nodeList = getContainedNodes(getActiveRange(),
        function(node) {
            return isEditable(node)
                && !isHtmlElement(node, ["thead", "tbody", "tfoot", "tr", "th", "td"]);
        }
    );

    // "For each node in node list:"
    for (var i = 0; i < nodeList.length; i++) {
        var node = nodeList[i];

        // "Let parent be the parent of node."
        var parent_ = node.parentNode;

        // "Remove node from parent."
        parent_.removeChild(node);

        // "If the block node of parent has no visible children, and parent is
        // editable or an editing host, call createElement("br") on the context
        // object and append the result as the last child of parent."
        if (![].some.call(getBlockNodeOf(parent_).childNodes, isVisible)
        && (isEditable(parent_) || isEditingHost(parent_))) {
            parent_.appendChild(document.createElement("br"));
        }

        // "If strip wrappers is true or parent is not an ancestor container of
        // start node, while parent is an editable inline node with length 0,
        // let grandparent be the parent of parent, then remove parent from
        // grandparent, then set parent to grandparent."
        if (stripWrappers
        || (!isAncestor(parent_, startNode) && parent_ != startNode)) {
            while (isEditable(parent_)
            && isInlineNode(parent_)
            && getNodeLength(parent_) == 0) {
                var grandparent = parent_.parentNode;
                grandparent.removeChild(parent_);
                parent_ = grandparent;
            }
        }
    }

    // "If end node is an editable Text node, call deleteData(0, end offset) on
    // it."
    if (isEditable(endNode)
    && endNode.nodeType == Node.TEXT_NODE) {
        endNode.deleteData(0, endOffset);
    }

    // "Canonicalize whitespace at the active range's start, with fix collapsed
    // space false."
    canonicalizeWhitespace(getActiveRange().startContainer, getActiveRange().startOffset, false);

    // "Canonicalize whitespace at the active range's end, with fix collapsed
    // space false."
    canonicalizeWhitespace(getActiveRange().endContainer, getActiveRange().endOffset, false);

    // "If block merging is false, or start block or end block is null, or
    // start block is not in the same editing host as end block, or start block
    // and end block are the same:"
    if (!blockMerging
    || !startBlock
    || !endBlock
    || !inSameEditingHost(startBlock, endBlock)
    || startBlock == endBlock) {
        // "If direction is "forward", call collapseToStart() on the context
        // object's Selection."
        if (direction == "forward") {
            if (getSelection().rangeCount) {
                getSelection().collapseToStart();
            }
            getActiveRange().collapse(true);

        // "Otherwise, call collapseToEnd() on the context object's Selection."
        } else {
            if (getSelection().rangeCount) {
                getSelection().collapseToEnd();
            }
            getActiveRange().collapse(false);
        }

        // "Restore states and values from overrides."
        restoreStatesAndValues(overrides);

        // "Abort these steps."
        return;
    }

    // "If start block has one child, which is a collapsed block prop, remove
    // its child from it."
    if (startBlock.children.length == 1
    && isCollapsedBlockProp(startBlock.firstChild)) {
        startBlock.removeChild(startBlock.firstChild);
    }

    // "If start block is an ancestor of end block:"
    if (isAncestor(startBlock, endBlock)) {
        // "Let reference node be end block."
        var referenceNode = endBlock;

        // "While reference node is not a child of start block, set reference
        // node to its parent."
        while (referenceNode.parentNode != startBlock) {
            referenceNode = referenceNode.parentNode;
        }

        // "Call collapse() on the context object's Selection, with first
        // argument start block and second argument the index of reference
        // node."
        getSelection().collapse(startBlock, getNodeIndex(referenceNode));
        getActiveRange().setStart(startBlock, getNodeIndex(referenceNode));
        getActiveRange().collapse(true);

        // "If end block has no children:"
        if (!endBlock.hasChildNodes()) {
            // "While end block is editable and is the only child of its parent
            // and is not a child of start block, let parent equal end block,
            // then remove end block from parent, then set end block to
            // parent."
            while (isEditable(endBlock)
            && endBlock.parentNode.childNodes.length == 1
            && endBlock.parentNode != startBlock) {
                var parent_ = endBlock;
                parent_.removeChild(endBlock);
                endBlock = parent_;
            }

            // "If end block is editable and is not an inline node, and its
            // previousSibling and nextSibling are both inline nodes, call
            // createElement("br") on the context object and insert it into end
            // block's parent immediately after end block."
            if (isEditable(endBlock)
            && !isInlineNode(endBlock)
            && isInlineNode(endBlock.previousSibling)
            && isInlineNode(endBlock.nextSibling)) {
                endBlock.parentNode.insertBefore(document.createElement("br"), endBlock.nextSibling);
            }

            // "If end block is editable, remove it from its parent."
            if (isEditable(endBlock)) {
                endBlock.parentNode.removeChild(endBlock);
            }

            // "Restore states and values from overrides."
            restoreStatesAndValues(overrides);

            // "Abort these steps."
            return;
        }

        // "If end block's firstChild is not an inline node, restore states and
        // values from overrides, then abort these steps."
        if (!isInlineNode(endBlock.firstChild)) {
            restoreStatesAndValues(overrides);
            return;
        }

        // "Let children be a list of nodes, initially empty."
        var children = [];

        // "Append the first child of end block to children."
        children.push(endBlock.firstChild);

        // "While children's last member is not a br, and children's last
        // member's nextSibling is an inline node, append children's last
        // member's nextSibling to children."
        while (!isHtmlElement(children[children.length - 1], "br")
        && isInlineNode(children[children.length - 1].nextSibling)) {
            children.push(children[children.length - 1].nextSibling);
        }

        // "Record the values of children, and let values be the result."
        var values = recordValues(children);

        // "While children's first member's parent is not start block, split
        // the parent of children."
        while (children[0].parentNode != startBlock) {
            splitParent(children);
        }

        // "If children's first member's previousSibling is an editable br,
        // remove that br from its parent."
        if (isEditable(children[0].previousSibling)
        && isHtmlElement(children[0].previousSibling, "br")) {
            children[0].parentNode.removeChild(children[0].previousSibling);
        }

    // "Otherwise, if start block is a descendant of end block:"
    } else if (isDescendant(startBlock, endBlock)) {
        // "Call collapse() on the context object's Selection, with first
        // argument start block and second argument start block's length."
        getSelection().collapse(startBlock, getNodeLength(startBlock));
        getActiveRange().setStart(startBlock, getNodeLength(startBlock));
        getActiveRange().collapse(true);

        // "Let reference node be start block."
        var referenceNode = startBlock;

        // "While reference node is not a child of end block, set reference
        // node to its parent."
        while (referenceNode.parentNode != endBlock) {
            referenceNode = referenceNode.parentNode;
        }

        // "If reference node's nextSibling is an inline node and start block's
        // lastChild is a br, remove start block's lastChild from it."
        if (isInlineNode(referenceNode.nextSibling)
        && isHtmlElement(startBlock.lastChild, "br")) {
            startBlock.removeChild(startBlock.lastChild);
        }

        // "Let nodes to move be a list of nodes, initially empty."
        var nodesToMove = [];

        // "If reference node's nextSibling is neither null nor a block node,
        // append it to nodes to move."
        if (referenceNode.nextSibling
        && !isBlockNode(referenceNode.nextSibling)) {
            nodesToMove.push(referenceNode.nextSibling);
        }

        // "While nodes to move is nonempty and its last member isn't a br and
        // its last member's nextSibling is neither null nor a block node,
        // append its last member's nextSibling to nodes to move."
        if (nodesToMove.length
        && !isHtmlElement(nodesToMove[nodesToMove.length - 1], "br")
        && nodesToMove[nodesToMove.length - 1].nextSibling
        && !isBlockNode(nodesToMove[nodesToMove.length - 1].nextSibling)) {
            nodesToMove.push(nodesToMove[nodesToMove.length - 1].nextSibling);
        }

        // "Record the values of nodes to move, and let values be the result."
        var values = recordValues(nodesToMove);

        // "For each node in nodes to move, append node as the last child of
        // start block, preserving ranges."
        nodesToMove.forEach(function(node) {
            movePreservingRanges(node, startBlock, -1);
        });

    // "Otherwise:"
    } else {
        // "Call collapse() on the context object's Selection, with first
        // argument start block and second argument start block's length."
        getSelection().collapse(startBlock, getNodeLength(startBlock));
        getActiveRange().setStart(startBlock, getNodeLength(startBlock));
        getActiveRange().collapse(true);

        // "If end block's firstChild is an inline node and start block's
        // lastChild is a br, remove start block's lastChild from it."
        if (isInlineNode(endBlock.firstChild)
        && isHtmlElement(startBlock.lastChild, "br")) {
            startBlock.removeChild(startBlock.lastChild);
        }

        // "Record the values of end block's children, and let values be the
        // result."
        var values = recordValues([].slice.call(endBlock.childNodes));

        // "While end block has children, append the first child of end block
        // to start block, preserving ranges."
        while (endBlock.hasChildNodes()) {
            movePreservingRanges(endBlock.firstChild, startBlock, -1);
        }

        // "While end block has no children, let parent be the parent of end
        // block, then remove end block from parent, then set end block to
        // parent."
        while (!endBlock.hasChildNodes()) {
            var parent_ = endBlock.parentNode;
            parent_.removeChild(endBlock);
            endBlock = parent_;
        }
    }

    // "Let ancestor be start block."
    var ancestor = startBlock;

    // "While ancestor has an inclusive ancestor ol in the same editing host
    // whose nextSibling is also an ol in the same editing host, or an
    // inclusive ancestor ul in the same editing host whose nextSibling is also
    // a ul in the same editing host:"
    while (getInclusiveAncestors(ancestor).some(function(node) {
        return inSameEditingHost(ancestor, node)
            && (
                (isHtmlElement(node, "ol") && isHtmlElement(node.nextSibling, "ol"))
                || (isHtmlElement(node, "ul") && isHtmlElement(node.nextSibling, "ul"))
            ) && inSameEditingHost(ancestor, node.nextSibling);
    })) {
        // "While ancestor and its nextSibling are not both ols in the same
        // editing host, and are also not both uls in the same editing host,
        // set ancestor to its parent."
        while (!(
            isHtmlElement(ancestor, "ol")
            && isHtmlElement(ancestor.nextSibling, "ol")
            && inSameEditingHost(ancestor, ancestor.nextSibling)
        ) && !(
            isHtmlElement(ancestor, "ul")
            && isHtmlElement(ancestor.nextSibling, "ul")
            && inSameEditingHost(ancestor, ancestor.nextSibling)
        )) {
            ancestor = ancestor.parentNode;
        }

        // "While ancestor's nextSibling has children, append ancestor's
        // nextSibling's firstChild as the last child of ancestor, preserving
        // ranges."
        while (ancestor.nextSibling.hasChildNodes()) {
            movePreservingRanges(ancestor.nextSibling.firstChild, ancestor, -1);
        }

        // "Remove ancestor's nextSibling from its parent."
        ancestor.parentNode.removeChild(ancestor.nextSibling);
    }

    // "Restore the values from values."
    restoreValues(values);

    // "If start block has no children, call createElement("br") on the context
    // object and append the result as the last child of start block."
    if (!startBlock.hasChildNodes()) {
        startBlock.appendChild(document.createElement("br"));
    }

    // "Remove extraneous line breaks at the end of start block."
    removeExtraneousLineBreaksAtTheEndOf(startBlock);

    // "Restore states and values from overrides."
    restoreStatesAndValues(overrides);
}


//@}
///// Splitting a node list's parent /////
//@{

function splitParent(nodeList) {
    // "Let original parent be the parent of the first member of node list."
    var originalParent = nodeList[0].parentNode;

    // "If original parent is not editable or its parent is null, do nothing
    // and abort these steps."
    if (!isEditable(originalParent)
    || !originalParent.parentNode) {
        return;
    }

    // "If the first child of original parent is in node list, remove
    // extraneous line breaks before original parent."
    if (nodeList.indexOf(originalParent.firstChild) != -1) {
        removeExtraneousLineBreaksBefore(originalParent);
    }

    // "If the first child of original parent is in node list, and original
    // parent follows a line break, set follows line break to true. Otherwise,
    // set follows line break to false."
    var followsLineBreak_ = nodeList.indexOf(originalParent.firstChild) != -1
        && followsLineBreak(originalParent);

    // "If the last child of original parent is in node list, and original
    // parent precedes a line break, set precedes line break to true.
    // Otherwise, set precedes line break to false."
    var precedesLineBreak_ = nodeList.indexOf(originalParent.lastChild) != -1
        && precedesLineBreak(originalParent);

    // "If the first child of original parent is not in node list, but its last
    // child is:"
    if (nodeList.indexOf(originalParent.firstChild) == -1
    && nodeList.indexOf(originalParent.lastChild) != -1) {
        // "For each node in node list, in reverse order, insert node into the
        // parent of original parent immediately after original parent,
        // preserving ranges."
        for (var i = nodeList.length - 1; i >= 0; i--) {
            movePreservingRanges(nodeList[i], originalParent.parentNode, 1 + getNodeIndex(originalParent));
        }

        // "If precedes line break is true, and the last member of node list
        // does not precede a line break, call createElement("br") on the
        // context object and insert the result immediately after the last
        // member of node list."
        if (precedesLineBreak_
        && !precedesLineBreak(nodeList[nodeList.length - 1])) {
            nodeList[nodeList.length - 1].parentNode.insertBefore(document.createElement("br"), nodeList[nodeList.length - 1].nextSibling);
        }

        // "Remove extraneous line breaks at the end of original parent."
        removeExtraneousLineBreaksAtTheEndOf(originalParent);

        // "Abort these steps."
        return;
    }

    // "If the first child of original parent is not in node list:"
    if (nodeList.indexOf(originalParent.firstChild) == -1) {
        // "Let cloned parent be the result of calling cloneNode(false) on
        // original parent."
        var clonedParent = originalParent.cloneNode(false);

        // "If original parent has an id attribute, unset it."
        originalParent.removeAttribute("id");

        // "Insert cloned parent into the parent of original parent immediately
        // before original parent."
        originalParent.parentNode.insertBefore(clonedParent, originalParent);

        // "While the previousSibling of the first member of node list is not
        // null, append the first child of original parent as the last child of
        // cloned parent, preserving ranges."
        while (nodeList[0].previousSibling) {
            movePreservingRanges(originalParent.firstChild, clonedParent, clonedParent.childNodes.length);
        }
    }

    // "For each node in node list, insert node into the parent of original
    // parent immediately before original parent, preserving ranges."
    for (var i = 0; i < nodeList.length; i++) {
        movePreservingRanges(nodeList[i], originalParent.parentNode, getNodeIndex(originalParent));
    }

    // "If follows line break is true, and the first member of node list does
    // not follow a line break, call createElement("br") on the context object
    // and insert the result immediately before the first member of node list."
    if (followsLineBreak_
    && !followsLineBreak(nodeList[0])) {
        nodeList[0].parentNode.insertBefore(document.createElement("br"), nodeList[0]);
    }

    // "If the last member of node list is an inline node other than a br, and
    // the first child of original parent is a br, and original parent is not
    // an inline node, remove the first child of original parent from original
    // parent."
    if (isInlineNode(nodeList[nodeList.length - 1])
    && !isHtmlElement(nodeList[nodeList.length - 1], "br")
    && isHtmlElement(originalParent.firstChild, "br")
    && !isInlineNode(originalParent)) {
        originalParent.removeChild(originalParent.firstChild);
    }

    // "If original parent has no children:"
    if (!originalParent.hasChildNodes()) {
        // "Remove original parent from its parent."
        originalParent.parentNode.removeChild(originalParent);

        // "If precedes line break is true, and the last member of node list
        // does not precede a line break, call createElement("br") on the
        // context object and insert the result immediately after the last
        // member of node list."
        if (precedesLineBreak_
        && !precedesLineBreak(nodeList[nodeList.length - 1])) {
            nodeList[nodeList.length - 1].parentNode.insertBefore(document.createElement("br"), nodeList[nodeList.length - 1].nextSibling);
        }

    // "Otherwise, remove extraneous line breaks before original parent."
    } else {
        removeExtraneousLineBreaksBefore(originalParent);
    }

    // "If node list's last member's nextSibling is null, but its parent is not
    // null, remove extraneous line breaks at the end of node list's last
    // member's parent."
    if (!nodeList[nodeList.length - 1].nextSibling
    && nodeList[nodeList.length - 1].parentNode) {
        removeExtraneousLineBreaksAtTheEndOf(nodeList[nodeList.length - 1].parentNode);
    }
}

// "To remove a node node while preserving its descendants, split the parent of
// node's children if it has any. If it has no children, instead remove it from
// its parent."
function removePreservingDescendants(node) {
    if (node.hasChildNodes()) {
        splitParent([].slice.call(node.childNodes));
    } else {
        node.parentNode.removeChild(node);
    }
}


//@}
///// Canonical space sequences /////
//@{

function canonicalSpaceSequence(n, nonBreakingStart, nonBreakingEnd) {
    // "If n is zero, return the empty string."
    if (n == 0) {
        return "";
    }

    // "If n is one and both non-breaking start and non-breaking end are false,
    // return a single space (U+0020)."
    if (n == 1 && !nonBreakingStart && !nonBreakingEnd) {
        return " ";
    }

    // "If n is one, return a single non-breaking space (U+00A0)."
    if (n == 1) {
        return "\xa0";
    }

    // "Let buffer be the empty string."
    var buffer = "";

    // "If non-breaking start is true, let repeated pair be U+00A0 U+0020.
    // Otherwise, let it be U+0020 U+00A0."
    var repeatedPair;
    if (nonBreakingStart) {
        repeatedPair = "\xa0 ";
    } else {
        repeatedPair = " \xa0";
    }

    // "While n is greater than three, append repeated pair to buffer and
    // subtract two from n."
    while (n > 3) {
        buffer += repeatedPair;
        n -= 2;
    }

    // "If n is three, append a three-element string to buffer depending on
    // non-breaking start and non-breaking end:"
    if (n == 3) {
        buffer +=
            !nonBreakingStart && !nonBreakingEnd ? " \xa0 "
            : nonBreakingStart && !nonBreakingEnd ? "\xa0\xa0 "
            : !nonBreakingStart && nonBreakingEnd ? " \xa0\xa0"
            : nonBreakingStart && nonBreakingEnd ? "\xa0 \xa0"
            : "impossible";

    // "Otherwise, append a two-element string to buffer depending on
    // non-breaking start and non-breaking end:"
    } else {
        buffer +=
            !nonBreakingStart && !nonBreakingEnd ? "\xa0 "
            : nonBreakingStart && !nonBreakingEnd ? "\xa0 "
            : !nonBreakingStart && nonBreakingEnd ? " \xa0"
            : nonBreakingStart && nonBreakingEnd ? "\xa0\xa0"
            : "impossible";
    }

    // "Return buffer."
    return buffer;
}

function canonicalizeWhitespace(node, offset, fixCollapsedSpace) {
    if (fixCollapsedSpace === undefined) {
        // "an optional boolean argument fix collapsed space that defaults to
        // true"
        fixCollapsedSpace = true;
    }

    // "If node is neither editable nor an editing host, abort these steps."
    if (!isEditable(node) && !isEditingHost(node)) {
        return;
    }

    // "Let start node equal node and let start offset equal offset."
    var startNode = node;
    var startOffset = offset;

    // "Repeat the following steps:"
    while (true) {
        // "If start node has a child in the same editing host with index start
        // offset minus one, set start node to that child, then set start
        // offset to start node's length."
        if (0 <= startOffset - 1
        && inSameEditingHost(startNode, startNode.childNodes[startOffset - 1])) {
            startNode = startNode.childNodes[startOffset - 1];
            startOffset = getNodeLength(startNode);

        // "Otherwise, if start offset is zero and start node does not follow a
        // line break and start node's parent is in the same editing host, set
        // start offset to start node's index, then set start node to its
        // parent."
        } else if (startOffset == 0
        && !followsLineBreak(startNode)
        && inSameEditingHost(startNode, startNode.parentNode)) {
            startOffset = getNodeIndex(startNode);
            startNode = startNode.parentNode;

        // "Otherwise, if start node is a Text node and its parent's resolved
        // value for "white-space" is neither "pre" nor "pre-wrap" and start
        // offset is not zero and the (start offset − 1)st element of start
        // node's data is a space (0x0020) or non-breaking space (0x00A0),
        // subtract one from start offset."
        } else if (startNode.nodeType == Node.TEXT_NODE
        && ["pre", "pre-wrap"].indexOf(getComputedStyle(startNode.parentNode).whiteSpace) == -1
        && startOffset != 0
        && /[ \xa0]/.test(startNode.data[startOffset - 1])) {
            startOffset--;

        // "Otherwise, break from this loop."
        } else {
            break;
        }
    }

    // "Let end node equal start node and end offset equal start offset."
    var endNode = startNode;
    var endOffset = startOffset;

    // "Let length equal zero."
    var length = 0;

    // "Let collapse spaces be true if start offset is zero and start node
    // follows a line break, otherwise false."
    var collapseSpaces = startOffset == 0 && followsLineBreak(startNode);

    // "Repeat the following steps:"
    while (true) {
        // "If end node has a child in the same editing host with index end
        // offset, set end node to that child, then set end offset to zero."
        if (endOffset < endNode.childNodes.length
        && inSameEditingHost(endNode, endNode.childNodes[endOffset])) {
            endNode = endNode.childNodes[endOffset];
            endOffset = 0;

        // "Otherwise, if end offset is end node's length and end node does not
        // precede a line break and end node's parent is in the same editing
        // host, set end offset to one plus end node's index, then set end node
        // to its parent."
        } else if (endOffset == getNodeLength(endNode)
        && !precedesLineBreak(endNode)
        && inSameEditingHost(endNode, endNode.parentNode)) {
            endOffset = 1 + getNodeIndex(endNode);
            endNode = endNode.parentNode;

        // "Otherwise, if end node is a Text node and its parent's resolved
        // value for "white-space" is neither "pre" nor "pre-wrap" and end
        // offset is not end node's length and the end offsetth element of
        // end node's data is a space (0x0020) or non-breaking space (0x00A0):"
        } else if (endNode.nodeType == Node.TEXT_NODE
        && ["pre", "pre-wrap"].indexOf(getComputedStyle(endNode.parentNode).whiteSpace) == -1
        && endOffset != getNodeLength(endNode)
        && /[ \xa0]/.test(endNode.data[endOffset])) {
            // "If fix collapsed space is true, and collapse spaces is true,
            // and the end offsetth code unit of end node's data is a space
            // (0x0020): call deleteData(end offset, 1) on end node, then
            // continue this loop from the beginning."
            if (fixCollapsedSpace
            && collapseSpaces
            && " " == endNode.data[endOffset]) {
                endNode.deleteData(endOffset, 1);
                continue;
            }

            // "Set collapse spaces to true if the end offsetth element of end
            // node's data is a space (0x0020), false otherwise."
            collapseSpaces = " " == endNode.data[endOffset];

            // "Add one to end offset."
            endOffset++;

            // "Add one to length."
            length++;

        // "Otherwise, break from this loop."
        } else {
            break;
        }
    }

    // "If fix collapsed space is true, then while (start node, start offset)
    // is before (end node, end offset):"
    if (fixCollapsedSpace) {
        while (getPosition(startNode, startOffset, endNode, endOffset) == "before") {
            // "If end node has a child in the same editing host with index end
            // offset − 1, set end node to that child, then set end offset to end
            // node's length."
            if (0 <= endOffset - 1
            && endOffset - 1 < endNode.childNodes.length
            && inSameEditingHost(endNode, endNode.childNodes[endOffset - 1])) {
                endNode = endNode.childNodes[endOffset - 1];
                endOffset = getNodeLength(endNode);

            // "Otherwise, if end offset is zero and end node's parent is in the
            // same editing host, set end offset to end node's index, then set end
            // node to its parent."
            } else if (endOffset == 0
            && inSameEditingHost(endNode, endNode.parentNode)) {
                endOffset = getNodeIndex(endNode);
                endNode = endNode.parentNode;

            // "Otherwise, if end node is a Text node and its parent's resolved
            // value for "white-space" is neither "pre" nor "pre-wrap" and end
            // offset is end node's length and the last code unit of end node's
            // data is a space (0x0020) and end node precedes a line break:"
            } else if (endNode.nodeType == Node.TEXT_NODE
            && ["pre", "pre-wrap"].indexOf(getComputedStyle(endNode.parentNode).whiteSpace) == -1
            && endOffset == getNodeLength(endNode)
            && endNode.data[endNode.data.length - 1] == " "
            && precedesLineBreak(endNode)) {
                // "Subtract one from end offset."
                endOffset--;

                // "Subtract one from length."
                length--;

                // "Call deleteData(end offset, 1) on end node."
                endNode.deleteData(endOffset, 1);

            // "Otherwise, break from this loop."
            } else {
                break;
            }
        }
    }

    // "Let replacement whitespace be the canonical space sequence of length
    // length. non-breaking start is true if start offset is zero and start
    // node follows a line break, and false otherwise. non-breaking end is true
    // if end offset is end node's length and end node precedes a line break,
    // and false otherwise."
    var replacementWhitespace = canonicalSpaceSequence(length,
        startOffset == 0 && followsLineBreak(startNode),
        endOffset == getNodeLength(endNode) && precedesLineBreak(endNode));

    // "While (start node, start offset) is before (end node, end offset):"
    while (getPosition(startNode, startOffset, endNode, endOffset) == "before") {
        // "If start node has a child with index start offset, set start node
        // to that child, then set start offset to zero."
        if (startOffset < startNode.childNodes.length) {
            startNode = startNode.childNodes[startOffset];
            startOffset = 0;

        // "Otherwise, if start node is not a Text node or if start offset is
        // start node's length, set start offset to one plus start node's
        // index, then set start node to its parent."
        } else if (startNode.nodeType != Node.TEXT_NODE
        || startOffset == getNodeLength(startNode)) {
            startOffset = 1 + getNodeIndex(startNode);
            startNode = startNode.parentNode;

        // "Otherwise:"
        } else {
            // "Remove the first element from replacement whitespace, and let
            // element be that element."
            var element = replacementWhitespace[0];
            replacementWhitespace = replacementWhitespace.slice(1);

            // "If element is not the same as the start offsetth element of
            // start node's data:"
            if (element != startNode.data[startOffset]) {
                // "Call insertData(start offset, element) on start node."
                startNode.insertData(startOffset, element);

                // "Call deleteData(start offset + 1, 1) on start node."
                startNode.deleteData(startOffset + 1, 1);
            }

            // "Add one to start offset."
            startOffset++;
        }
    }
}


//@}
///// Indenting and outdenting /////
//@{

function indentNodes(nodeList) {
    // "If node list is empty, do nothing and abort these steps."
    if (!nodeList.length) {
        return;
    }

    // "Let first node be the first member of node list."
    var firstNode = nodeList[0];

    // "If first node's parent is an ol or ul:"
    if (isHtmlElement(firstNode.parentNode, ["OL", "UL"])) {
        // "Let tag be the local name of the parent of first node."
        var tag = firstNode.parentNode.tagName;

        // "Wrap node list, with sibling criteria returning true for an HTML
        // element with local name tag and false otherwise, and new parent
        // instructions returning the result of calling createElement(tag) on
        // the ownerDocument of first node."
        wrap(nodeList,
            function(node) { return isHtmlElement(node, tag) },
            function() { return firstNode.ownerDocument.createElement(tag) });

        // "Abort these steps."
        return;
    }

    // "Wrap node list, with sibling criteria returning true for a simple
    // indentation element and false otherwise, and new parent instructions
    // returning the result of calling createElement("blockquote") on the
    // ownerDocument of first node. Let new parent be the result."
    var newParent = wrap(nodeList,
        function(node) { return isSimpleIndentationElement(node) },
        function() { return firstNode.ownerDocument.createElement("blockquote") });

    // "Fix disallowed ancestors of new parent."
    fixDisallowedAncestors(newParent);
}

function outdentNode(node) {
    // "If node is not editable, abort these steps."
    if (!isEditable(node)) {
        return;
    }

    // "If node is a simple indentation element, remove node, preserving its
    // descendants.  Then abort these steps."
    if (isSimpleIndentationElement(node)) {
        removePreservingDescendants(node);
        return;
    }

    // "If node is an indentation element:"
    if (isIndentationElement(node)) {
        // "Unset the dir attribute of node, if any."
        node.removeAttribute("dir");

        // "Unset the margin, padding, and border CSS properties of node."
        node.style.margin = "";
        node.style.padding = "";
        node.style.border = "";
        if (node.getAttribute("style") == ""
        // Crazy WebKit bug: https://bugs.webkit.org/show_bug.cgi?id=68551
        || node.getAttribute("style") == "border-width: initial; border-color: initial; ") {
            node.removeAttribute("style");
        }

        // "Set the tag name of node to "div"."
        setTagName(node, "div");

        // "Abort these steps."
        return;
    }

    // "Let current ancestor be node's parent."
    var currentAncestor = node.parentNode;

    // "Let ancestor list be a list of nodes, initially empty."
    var ancestorList = [];

    // "While current ancestor is an editable Element that is neither a simple
    // indentation element nor an ol nor a ul, append current ancestor to
    // ancestor list and then set current ancestor to its parent."
    while (isEditable(currentAncestor)
    && currentAncestor.nodeType == Node.ELEMENT_NODE
    && !isSimpleIndentationElement(currentAncestor)
    && !isHtmlElement(currentAncestor, ["ol", "ul"])) {
        ancestorList.push(currentAncestor);
        currentAncestor = currentAncestor.parentNode;
    }

    // "If current ancestor is not an editable simple indentation element:"
    if (!isEditable(currentAncestor)
    || !isSimpleIndentationElement(currentAncestor)) {
        // "Let current ancestor be node's parent."
        currentAncestor = node.parentNode;

        // "Let ancestor list be the empty list."
        ancestorList = [];

        // "While current ancestor is an editable Element that is neither an
        // indentation element nor an ol nor a ul, append current ancestor to
        // ancestor list and then set current ancestor to its parent."
        while (isEditable(currentAncestor)
        && currentAncestor.nodeType == Node.ELEMENT_NODE
        && !isIndentationElement(currentAncestor)
        && !isHtmlElement(currentAncestor, ["ol", "ul"])) {
            ancestorList.push(currentAncestor);
            currentAncestor = currentAncestor.parentNode;
        }
    }

    // "If node is an ol or ul and current ancestor is not an editable
    // indentation element:"
    if (isHtmlElement(node, ["OL", "UL"])
    && (!isEditable(currentAncestor)
    || !isIndentationElement(currentAncestor))) {
        // "Unset the reversed, start, and type attributes of node, if any are
        // set."
        node.removeAttribute("reversed");
        node.removeAttribute("start");
        node.removeAttribute("type");

        // "Let children be the children of node."
        var children = [].slice.call(node.childNodes);

        // "If node has attributes, and its parent is not an ol or ul, set the
        // tag name of node to "div"."
        if (node.attributes.length
        && !isHtmlElement(node.parentNode, ["OL", "UL"])) {
            setTagName(node, "div");

        // "Otherwise:"
        } else {
            // "Record the values of node's children, and let values be the
            // result."
            var values = recordValues([].slice.call(node.childNodes));

            // "Remove node, preserving its descendants."
            removePreservingDescendants(node);

            // "Restore the values from values."
            restoreValues(values);
        }

        // "Fix disallowed ancestors of each member of children."
        for (var i = 0; i < children.length; i++) {
            fixDisallowedAncestors(children[i]);
        }

        // "Abort these steps."
        return;
    }

    // "If current ancestor is not an editable indentation element, abort these
    // steps."
    if (!isEditable(currentAncestor)
    || !isIndentationElement(currentAncestor)) {
        return;
    }

    // "Append current ancestor to ancestor list."
    ancestorList.push(currentAncestor);

    // "Let original ancestor be current ancestor."
    var originalAncestor = currentAncestor;

    // "While ancestor list is not empty:"
    while (ancestorList.length) {
        // "Let current ancestor be the last member of ancestor list."
        //
        // "Remove the last member of ancestor list."
        currentAncestor = ancestorList.pop();

        // "Let target be the child of current ancestor that is equal to either
        // node or the last member of ancestor list."
        var target = node.parentNode == currentAncestor
            ? node
            : ancestorList[ancestorList.length - 1];

        // "If target is an inline node that is not a br, and its nextSibling
        // is a br, remove target's nextSibling from its parent."
        if (isInlineNode(target)
        && !isHtmlElement(target, "BR")
        && isHtmlElement(target.nextSibling, "BR")) {
            target.parentNode.removeChild(target.nextSibling);
        }

        // "Let preceding siblings be the preceding siblings of target, and let
        // following siblings be the following siblings of target."
        var precedingSiblings = [].slice.call(currentAncestor.childNodes, 0, getNodeIndex(target));
        var followingSiblings = [].slice.call(currentAncestor.childNodes, 1 + getNodeIndex(target));

        // "Indent preceding siblings."
        indentNodes(precedingSiblings);

        // "Indent following siblings."
        indentNodes(followingSiblings);
    }

    // "Outdent original ancestor."
    outdentNode(originalAncestor);
}


//@}
///// Toggling lists /////
//@{

function toggleLists(tagName) {
    // "Let mode be "disable" if the selection's list state is tag name, and
    // "enable" otherwise."
    var mode = getSelectionListState() == tagName ? "disable" : "enable";

    var range = getActiveRange();
    tagName = tagName.toUpperCase();

    // "Let other tag name be "ol" if tag name is "ul", and "ul" if tag name is
    // "ol"."
    var otherTagName = tagName == "OL" ? "UL" : "OL";

    // "Let items be a list of all lis that are ancestor containers of the
    // range's start and/or end node."
    //
    // It's annoying to get this in tree order using functional stuff without
    // doing getDescendants(document), which is slow, so I do it imperatively.
    var items = [];
    (function(){
        for (
            var ancestorContainer = range.endContainer;
            ancestorContainer != range.commonAncestorContainer;
            ancestorContainer = ancestorContainer.parentNode
        ) {
            if (isHtmlElement(ancestorContainer, "li")) {
                items.unshift(ancestorContainer);
            }
        }
        for (
            var ancestorContainer = range.startContainer;
            ancestorContainer;
            ancestorContainer = ancestorContainer.parentNode
        ) {
            if (isHtmlElement(ancestorContainer, "li")) {
                items.unshift(ancestorContainer);
            }
        }
    })();

    // "For each item in items, normalize sublists of item."
    items.forEach(normalizeSublists);

    // "Block-extend the range, and let new range be the result."
    var newRange = blockExtend(range);

    // "If mode is "enable", then let lists to convert consist of every
    // editable HTML element with local name other tag name that is contained
    // in new range, and for every list in lists to convert:"
    if (mode == "enable") {
        getAllContainedNodes(newRange, function(node) {
            return isEditable(node)
                && isHtmlElement(node, otherTagName);
        }).forEach(function(list) {
            // "If list's previousSibling or nextSibling is an editable HTML
            // element with local name tag name:"
            if ((isEditable(list.previousSibling) && isHtmlElement(list.previousSibling, tagName))
            || (isEditable(list.nextSibling) && isHtmlElement(list.nextSibling, tagName))) {
                // "Let children be list's children."
                var children = [].slice.call(list.childNodes);

                // "Record the values of children, and let values be the
                // result."
                var values = recordValues(children);

                // "Split the parent of children."
                splitParent(children);

                // "Wrap children, with sibling criteria returning true for an
                // HTML element with local name tag name and false otherwise."
                wrap(children, function(node) { return isHtmlElement(node, tagName) });

                // "Restore the values from values."
                restoreValues(values);

            // "Otherwise, set the tag name of list to tag name."
            } else {
                setTagName(list, tagName);
            }
        });
    }

    // "Let node list be a list of nodes, initially empty."
    //
    // "For each node node contained in new range, if node is editable; the
    // last member of node list (if any) is not an ancestor of node; node
    // is not an indentation element; and either node is an ol or ul, or its
    // parent is an ol or ul, or it is an allowed child of "li"; then append
    // node to node list."
    var nodeList = getContainedNodes(newRange, function(node) {
        return isEditable(node)
        && !isIndentationElement(node)
        && (isHtmlElement(node, ["OL", "UL"])
        || isHtmlElement(node.parentNode, ["OL", "UL"])
        || isAllowedChild(node, "li"));
    });

    // "If mode is "enable", remove from node list any ol or ul whose parent is
    // not also an ol or ul."
    if (mode == "enable") {
        nodeList = nodeList.filter(function(node) {
            return !isHtmlElement(node, ["ol", "ul"])
                || isHtmlElement(node.parentNode, ["ol", "ul"]);
        });
    }

    // "If mode is "disable", then while node list is not empty:"
    if (mode == "disable") {
        while (nodeList.length) {
            // "Let sublist be an empty list of nodes."
            var sublist = [];

            // "Remove the first member from node list and append it to
            // sublist."
            sublist.push(nodeList.shift());

            // "If the first member of sublist is an HTML element with local
            // name tag name, outdent it and continue this loop from the
            // beginning."
            if (isHtmlElement(sublist[0], tagName)) {
                outdentNode(sublist[0]);
                continue;
            }

            // "While node list is not empty, and the first member of node list
            // is the nextSibling of the last member of sublist and is not an
            // HTML element with local name tag name, remove the first member
            // from node list and append it to sublist."
            while (nodeList.length
            && nodeList[0] == sublist[sublist.length - 1].nextSibling
            && !isHtmlElement(nodeList[0], tagName)) {
                sublist.push(nodeList.shift());
            }

            // "Record the values of sublist, and let values be the result."
            var values = recordValues(sublist);

            // "Split the parent of sublist."
            splitParent(sublist);

            // "Fix disallowed ancestors of each member of sublist."
            for (var i = 0; i < sublist.length; i++) {
                fixDisallowedAncestors(sublist[i]);
            }

            // "Restore the values from values."
            restoreValues(values);
        }

    // "Otherwise, while node list is not empty:"
    } else {
        while (nodeList.length) {
            // "Let sublist be an empty list of nodes."
            var sublist = [];

            // "While either sublist is empty, or node list is not empty and
            // its first member is the nextSibling of sublist's last member:"
            while (!sublist.length
            || (nodeList.length
            && nodeList[0] == sublist[sublist.length - 1].nextSibling)) {
                // "If node list's first member is a p or div, set the tag name
                // of node list's first member to "li", and append the result
                // to sublist. Remove the first member from node list."
                if (isHtmlElement(nodeList[0], ["p", "div"])) {
                    sublist.push(setTagName(nodeList[0], "li"));
                    nodeList.shift();

                // "Otherwise, if the first member of node list is an li or ol
                // or ul, remove it from node list and append it to sublist."
                } else if (isHtmlElement(nodeList[0], ["li", "ol", "ul"])) {
                    sublist.push(nodeList.shift());

                // "Otherwise:"
                } else {
                    // "Let nodes to wrap be a list of nodes, initially empty."
                    var nodesToWrap = [];

                    // "While nodes to wrap is empty, or node list is not empty
                    // and its first member is the nextSibling of nodes to
                    // wrap's last member and the first member of node list is
                    // an inline node and the last member of nodes to wrap is
                    // an inline node other than a br, remove the first member
                    // from node list and append it to nodes to wrap."
                    while (!nodesToWrap.length
                    || (nodeList.length
                    && nodeList[0] == nodesToWrap[nodesToWrap.length - 1].nextSibling
                    && isInlineNode(nodeList[0])
                    && isInlineNode(nodesToWrap[nodesToWrap.length - 1])
                    && !isHtmlElement(nodesToWrap[nodesToWrap.length - 1], "br"))) {
                        nodesToWrap.push(nodeList.shift());
                    }

                    // "Wrap nodes to wrap, with new parent instructions
                    // returning the result of calling createElement("li") on
                    // the context object. Append the result to sublist."
                    sublist.push(wrap(nodesToWrap,
                        undefined,
                        function() { return document.createElement("li") }));
                }
            }

            // "If sublist's first member's parent is an HTML element with
            // local name tag name, or if every member of sublist is an ol or
            // ul, continue this loop from the beginning."
            if (isHtmlElement(sublist[0].parentNode, tagName)
            || sublist.every(function(node) { return isHtmlElement(node, ["ol", "ul"]) })) {
                continue;
            }

            // "If sublist's first member's parent is an HTML element with
            // local name other tag name:"
            if (isHtmlElement(sublist[0].parentNode, otherTagName)) {
                // "Record the values of sublist, and let values be the
                // result."
                var values = recordValues(sublist);

                // "Split the parent of sublist."
                splitParent(sublist);

                // "Wrap sublist, with sibling criteria returning true for an
                // HTML element with local name tag name and false otherwise,
                // and new parent instructions returning the result of calling
                // createElement(tag name) on the context object."
                wrap(sublist,
                    function(node) { return isHtmlElement(node, tagName) },
                    function() { return document.createElement(tagName) });

                // "Restore the values from values."
                restoreValues(values);

                // "Continue this loop from the beginning."
                continue;
            }

            // "Wrap sublist, with sibling criteria returning true for an HTML
            // element with local name tag name and false otherwise, and new
            // parent instructions being the following:"
            // . . .
            // "Fix disallowed ancestors of the previous step's result."
            fixDisallowedAncestors(wrap(sublist,
                function(node) { return isHtmlElement(node, tagName) },
                function() {
                    // "If sublist's first member's parent is not an editable
                    // simple indentation element, or sublist's first member's
                    // parent's previousSibling is not an editable HTML element
                    // with local name tag name, call createElement(tag name)
                    // on the context object and return the result."
                    if (!isEditable(sublist[0].parentNode)
                    || !isSimpleIndentationElement(sublist[0].parentNode)
                    || !isEditable(sublist[0].parentNode.previousSibling)
                    || !isHtmlElement(sublist[0].parentNode.previousSibling, tagName)) {
                        return document.createElement(tagName);
                    }

                    // "Let list be sublist's first member's parent's
                    // previousSibling."
                    var list = sublist[0].parentNode.previousSibling;

                    // "Normalize sublists of list's lastChild."
                    normalizeSublists(list.lastChild);

                    // "If list's lastChild is not an editable HTML element
                    // with local name tag name, call createElement(tag name)
                    // on the context object, and append the result as the last
                    // child of list."
                    if (!isEditable(list.lastChild)
                    || !isHtmlElement(list.lastChild, tagName)) {
                        list.appendChild(document.createElement(tagName));
                    }

                    // "Return the last child of list."
                    return list.lastChild;
                }
            ));
        }
    }
}


//@}
///// Justifying the selection /////
//@{

function justifySelection(alignment) {
    // "Block-extend the active range, and let new range be the result."
    var newRange = blockExtend(globalRange);

    // "Let element list be a list of all editable Elements contained in new
    // range that either has an attribute in the HTML namespace whose local
    // name is "align", or has a style attribute that sets "text-align", or is
    // a center."
    var elementList = getAllContainedNodes(newRange, function(node) {
        return node.nodeType == Node.ELEMENT_NODE
            && isEditable(node)
            // Ignoring namespaces here
            && (
                node.hasAttribute("align")
                || node.style.textAlign != ""
                || isHtmlElement(node, "center")
            );
    });

    // "For each element in element list:"
    for (var i = 0; i < elementList.length; i++) {
        var element = elementList[i];

        // "If element has an attribute in the HTML namespace whose local name
        // is "align", remove that attribute."
        element.removeAttribute("align");

        // "Unset the CSS property "text-align" on element, if it's set by a
        // style attribute."
        element.style.textAlign = "";
        if (element.getAttribute("style") == "") {
            element.removeAttribute("style");
        }

        // "If element is a div or span or center with no attributes, remove
        // it, preserving its descendants."
        if (isHtmlElement(element, ["div", "span", "center"])
        && !element.attributes.length) {
            removePreservingDescendants(element);
        }

        // "If element is a center with one or more attributes, set the tag
        // name of element to "div"."
        if (isHtmlElement(element, "center")
        && element.attributes.length) {
            setTagName(element, "div");
        }
    }

    // "Block-extend the active range, and let new range be the result."
    newRange = blockExtend(globalRange);

    // "Let node list be a list of nodes, initially empty."
    var nodeList = [];

    // "For each node node contained in new range, append node to node list if
    // the last member of node list (if any) is not an ancestor of node; node
    // is editable; node is an allowed child of "div"; and node's alignment
    // value is not alignment."
    nodeList = getContainedNodes(newRange, function(node) {
        return isEditable(node)
            && isAllowedChild(node, "div")
            && getAlignmentValue(node) != alignment;
    });

    // "While node list is not empty:"
    while (nodeList.length) {
        // "Let sublist be a list of nodes, initially empty."
        var sublist = [];

        // "Remove the first member of node list and append it to sublist."
        sublist.push(nodeList.shift());

        // "While node list is not empty, and the first member of node list is
        // the nextSibling of the last member of sublist, remove the first
        // member of node list and append it to sublist."
        while (nodeList.length
        && nodeList[0] == sublist[sublist.length - 1].nextSibling) {
            sublist.push(nodeList.shift());
        }

        // "Wrap sublist. Sibling criteria returns true for any div that has
        // one or both of the following two attributes and no other attributes,
        // and false otherwise:"
        //
        //   * "An align attribute whose value is an ASCII case-insensitive
        //     match for alignment.
        //   * "A style attribute which sets exactly one CSS property
        //     (including unrecognized or invalid attributes), which is
        //     "text-align", which is set to alignment.
        //
        // "New parent instructions are to call createElement("div") on the
        // context object, then set its CSS property "text-align" to alignment
        // and return the result."
        wrap(sublist,
            function(node) {
                return isHtmlElement(node, "div")
                    && [].every.call(node.attributes, function(attr) {
                        return (attr.name == "align" && attr.value.toLowerCase() == alignment)
                            || (attr.name == "style" && node.style.length == 1 && node.style.textAlign == alignment);
                    });
            },
            function() {
                var newParent = document.createElement("div");
                newParent.setAttribute("style", "text-align: " + alignment);
                return newParent;
            }
        );
    }
}


//@}
///// Automatic linking /////
//@{
// "An autolinkable URL is a string of the following form:"
var autolinkableUrlRegexp =
    // "Either a string matching the scheme pattern from RFC 3986 section 3.1
    // followed by the literal string ://, or the literal string mailto:;
    // followed by"
    //
    // From the RFC: scheme      = ALPHA *( ALPHA / DIGIT / "+" / "-" / "." )
    "([a-zA-Z][a-zA-Z0-9+.-]*://|mailto:)"
    // "Zero or more characters other than space characters; followed by"
    + "[^ \t\n\f\r]*"
    // "A character that is not one of the ASCII characters !"'(),-.:;<>[]`{}."
    + "[^!\"'(),\\-.:;<>[\\]`{}]";

// "A valid e-mail address is a string that matches the ABNF production 1*(
// atext / "." ) "@" ldh-str *( "." ldh-str ) where atext is defined in RFC
// 5322 section 3.2.3, and ldh-str is defined in RFC 1034 section 3.5."
//
// atext: ALPHA / DIGIT / "!" / "#" / "$" / "%" / "&" / "'" / "*" / "+" / "-" /
// "/" / "=" / "?" / "^" / "_" / "`" / "{" / "|" / "}" / "~"
//
//<ldh-str> ::= <let-dig-hyp> | <let-dig-hyp> <ldh-str>
//<let-dig-hyp> ::= <let-dig> | "-"
//<let-dig> ::= <letter> | <digit>
var validEmailRegexp =
    "[a-zA-Z0-9!#$%&'*+\\-/=?^_`{|}~.]+@[a-zA-Z0-9-]+(\.[a-zA-Z0-9-]+)*";

function autolink(node, endOffset) {
    // "While (node, end offset)'s previous equivalent point is not null, set
    // it to its previous equivalent point."
    while (getPreviousEquivalentPoint(node, endOffset)) {
        var prev = getPreviousEquivalentPoint(node, endOffset);
        node = prev[0];
        endOffset = prev[1];
    }

    // "If node is not a Text node, or has an a ancestor, do nothing and abort
    // these steps."
    if (node.nodeType != Node.TEXT_NODE
    || getAncestors(node).some(function(ancestor) { return isHtmlElement(ancestor, "a") })) {
        return;
    }

    // "Let search be the largest substring of node's data whose end is end
    // offset and that contains no space characters."
    var search = /[^ \t\n\f\r]*$/.exec(node.substringData(0, endOffset))[0];

    // "If some substring of search is an autolinkable URL:"
    if (new RegExp(autolinkableUrlRegexp).test(search)) {
        // "While there is no substring of node's data ending at end offset
        // that is an autolinkable URL, decrement end offset."
        while (!(new RegExp(autolinkableUrlRegexp + "$").test(node.substringData(0, endOffset)))) {
            endOffset--;
        }

        // "Let start offset be the start index of the longest substring of
        // node's data that is an autolinkable URL ending at end offset."
        var startOffset = new RegExp(autolinkableUrlRegexp + "$").exec(node.substringData(0, endOffset)).index;

        // "Let href be the substring of node's data starting at start offset
        // and ending at end offset."
        var href = node.substringData(startOffset, endOffset - startOffset);

    // "Otherwise, if some substring of search is a valid e-mail address:"
    } else if (new RegExp(validEmailRegexp).test(search)) {
        // "While there is no substring of node's data ending at end offset
        // that is a valid e-mail address, decrement end offset."
        while (!(new RegExp(validEmailRegexp + "$").test(node.substringData(0, endOffset)))) {
            endOffset--;
        }

        // "Let start offset be the start index of the longest substring of
        // node's data that is a valid e-mail address ending at end offset."
        var startOffset = new RegExp(validEmailRegexp + "$").exec(node.substringData(0, endOffset)).index;

        // "Let href be "mailto:" concatenated with the substring of node's
        // data starting at start offset and ending at end offset."
        var href = "mailto:" + node.substringData(startOffset, endOffset - startOffset);

    // "Otherwise, do nothing and abort these steps."
    } else {
        return;
    }

    // "Let original range be the active range."
    var originalRange = getActiveRange();

    // "Create a new range with start (node, start offset) and end (node, end
    // offset), and set the context object's selection's range to it."
    var newRange = document.createRange();
    newRange.setStart(node, startOffset);
    newRange.setEnd(node, endOffset);
    getSelection().removeAllRanges();
    getSelection().addRange(newRange);
    globalRange = newRange;

    // "Take the action for "createLink", with value equal to href."
    commands.createlink.action(href);

    // "Set the context object's selection's range to original range."
    getSelection().removeAllRanges();
    getSelection().addRange(originalRange);
    globalRange = originalRange;
}
//@}
///// The delete command /////
//@{
commands["delete"] = {
    preservesOverrides: true,
    action: function() {
        // "If the active range is not collapsed, delete the selection and
        // return true."
        if (!getActiveRange().collapsed) {
            deleteSelection();
            return true;
        }

        // "Canonicalize whitespace at the active range's start."
        canonicalizeWhitespace(getActiveRange().startContainer, getActiveRange().startOffset);

        // "Let node and offset be the active range's start node and offset."
        var node = getActiveRange().startContainer;
        var offset = getActiveRange().startOffset;

        // "Repeat the following steps:"
        while (true) {
            // "If offset is zero and node's previousSibling is an editable
            // invisible node, remove node's previousSibling from its parent."
            if (offset == 0
            && isEditable(node.previousSibling)
            && isInvisible(node.previousSibling)) {
                node.parentNode.removeChild(node.previousSibling);

            // "Otherwise, if node has a child with index offset − 1 and that
            // child is an editable invisible node, remove that child from
            // node, then subtract one from offset."
            } else if (0 <= offset - 1
            && offset - 1 < node.childNodes.length
            && isEditable(node.childNodes[offset - 1])
            && isInvisible(node.childNodes[offset - 1])) {
                node.removeChild(node.childNodes[offset - 1]);
                offset--;

            // "Otherwise, if offset is zero and node is an inline node, or if
            // node is an invisible node, set offset to the index of node, then
            // set node to its parent."
            } else if ((offset == 0
            && isInlineNode(node))
            || isInvisible(node)) {
                offset = getNodeIndex(node);
                node = node.parentNode;

            // "Otherwise, if node has a child with index offset − 1 and that
            // child is an editable a, remove that child from node, preserving
            // its descendants. Then return true."
            } else if (0 <= offset - 1
            && offset - 1 < node.childNodes.length
            && isEditable(node.childNodes[offset - 1])
            && isHtmlElement(node.childNodes[offset - 1], "a")) {
                removePreservingDescendants(node.childNodes[offset - 1]);
                return true;

            // "Otherwise, if node has a child with index offset − 1 and that
            // child is not a block node or a br or an img, set node to that
            // child, then set offset to the length of node."
            } else if (0 <= offset - 1
            && offset - 1 < node.childNodes.length
            && !isBlockNode(node.childNodes[offset - 1])
            && !isHtmlElement(node.childNodes[offset - 1], ["br", "img"])) {
                node = node.childNodes[offset - 1];
                offset = getNodeLength(node);

            // "Otherwise, break from this loop."
            } else {
                break;
            }
        }

        // "If node is a Text node and offset is not zero, or if node is a
        // block node that has a child with index offset − 1 and that child is
        // a br or hr or img:"
        if ((node.nodeType == Node.TEXT_NODE
        && offset != 0)
        || (isBlockNode(node)
        && 0 <= offset - 1
        && offset - 1 < node.childNodes.length
        && isHtmlElement(node.childNodes[offset - 1], ["br", "hr", "img"]))) {
            // "Call collapse(node, offset) on the context object's Selection."
            getSelection().collapse(node, offset);
            getActiveRange().setEnd(node, offset);

            // "Call extend(node, offset − 1) on the context object's
            // Selection."
            getSelection().extend(node, offset - 1);
            getActiveRange().setStart(node, offset - 1);

            // "Delete the selection."
            deleteSelection();

            // "Return true."
            return true;
        }

        // "If node is an inline node, return true."
        if (isInlineNode(node)) {
            return true;
        }

        // "If node is an li or dt or dd and is the first child of its parent,
        // and offset is zero:"
        if (isHtmlElement(node, ["li", "dt", "dd"])
        && node == node.parentNode.firstChild
        && offset == 0) {
            // "Let items be a list of all lis that are ancestors of node."
            //
            // Remember, must be in tree order.
            var items = [];
            for (var ancestor = node.parentNode; ancestor; ancestor = ancestor.parentNode) {
                if (isHtmlElement(ancestor, "li")) {
                    items.unshift(ancestor);
                }
            }

            // "Normalize sublists of each item in items."
            for (var i = 0; i < items.length; i++) {
                normalizeSublists(items[i]);
            }

            // "Record the values of the one-node list consisting of node, and
            // let values be the result."
            var values = recordValues([node]);

            // "Split the parent of the one-node list consisting of node."
            splitParent([node]);

            // "Restore the values from values."
            restoreValues(values);

            // "If node is a dd or dt, and it is not an allowed child of any of
            // its ancestors in the same editing host, set the tag name of node
            // to the default single-line container name and let node be the
            // result."
            if (isHtmlElement(node, ["dd", "dt"])
            && getAncestors(node).every(function(ancestor) {
                return !inSameEditingHost(node, ancestor)
                    || !isAllowedChild(node, ancestor)
            })) {
                node = setTagName(node, defaultSingleLineContainerName);
            }

            // "Fix disallowed ancestors of node."
            fixDisallowedAncestors(node);

            // "Return true."
            return true;
        }

        // "Let start node equal node and let start offset equal offset."
        var startNode = node;
        var startOffset = offset;

        // "Repeat the following steps:"
        while (true) {
            // "If start offset is zero, set start offset to the index of start
            // node and then set start node to its parent."
            if (startOffset == 0) {
                startOffset = getNodeIndex(startNode);
                startNode = startNode.parentNode;

            // "Otherwise, if start node has an editable invisible child with
            // index start offset minus one, remove it from start node and
            // subtract one from start offset."
            } else if (0 <= startOffset - 1
            && startOffset - 1 < startNode.childNodes.length
            && isEditable(startNode.childNodes[startOffset - 1])
            && isInvisible(startNode.childNodes[startOffset - 1])) {
                startNode.removeChild(startNode.childNodes[startOffset - 1]);
                startOffset--;

            // "Otherwise, break from this loop."
            } else {
                break;
            }
        }

        // "If offset is zero, and node has an editable ancestor container in
        // the same editing host that's an indentation element:"
        if (offset == 0
        && getAncestors(node).concat(node).filter(function(ancestor) {
            return isEditable(ancestor)
                && inSameEditingHost(ancestor, node)
                && isIndentationElement(ancestor);
        }).length) {
            // "Block-extend the range whose start and end are both (node, 0),
            // and let new range be the result."
            var newRange = document.createRange();
            newRange.setStart(node, 0);
            newRange = blockExtend(newRange);

            // "Let node list be a list of nodes, initially empty."
            //
            // "For each node current node contained in new range, append
            // current node to node list if the last member of node list (if
            // any) is not an ancestor of current node, and current node is
            // editable but has no editable descendants."
            var nodeList = getContainedNodes(newRange, function(currentNode) {
                return isEditable(currentNode)
                    && !hasEditableDescendants(currentNode);
            });

            // "Outdent each node in node list."
            for (var i = 0; i < nodeList.length; i++) {
                outdentNode(nodeList[i]);
            }

            // "Return true."
            return true;
        }

        // "If the child of start node with index start offset is a table,
        // return true."
        if (isHtmlElement(startNode.childNodes[startOffset], "table")) {
            return true;
        }

        // "If start node has a child with index start offset − 1, and that
        // child is a table:"
        if (0 <= startOffset - 1
        && startOffset - 1 < startNode.childNodes.length
        && isHtmlElement(startNode.childNodes[startOffset - 1], "table")) {
            // "Call collapse(start node, start offset − 1) on the context
            // object's Selection."
            getSelection().collapse(startNode, startOffset - 1);
            getActiveRange().setStart(startNode, startOffset - 1);

            // "Call extend(start node, start offset) on the context object's
            // Selection."
            getSelection().extend(startNode, startOffset);
            getActiveRange().setEnd(startNode, startOffset);

            // "Return true."
            return true;
        }

        // "If offset is zero; and either the child of start node with index
        // start offset minus one is an hr, or the child is a br whose
        // previousSibling is either a br or not an inline node:"
        if (offset == 0
        && (isHtmlElement(startNode.childNodes[startOffset - 1], "hr")
            || (
                isHtmlElement(startNode.childNodes[startOffset - 1], "br")
                && (
                    isHtmlElement(startNode.childNodes[startOffset - 1].previousSibling, "br")
                    || !isInlineNode(startNode.childNodes[startOffset - 1].previousSibling)
                )
            )
        )) {
            // "Call collapse(start node, start offset − 1) on the context
            // object's Selection."
            getSelection().collapse(startNode, startOffset - 1);
            getActiveRange().setStart(startNode, startOffset - 1);

            // "Call extend(start node, start offset) on the context object's
            // Selection."
            getSelection().extend(startNode, startOffset);
            getActiveRange().setEnd(startNode, startOffset);

            // "Delete the selection."
            deleteSelection();

            // "Call collapse(node, offset) on the Selection."
            getSelection().collapse(node, offset);
            getActiveRange().setStart(node, offset);
            getActiveRange().collapse(true);

            // "Return true."
            return true;
        }

        // "If the child of start node with index start offset is an li or dt
        // or dd, and that child's firstChild is an inline node, and start
        // offset is not zero:"
        if (isHtmlElement(startNode.childNodes[startOffset], ["li", "dt", "dd"])
        && isInlineNode(startNode.childNodes[startOffset].firstChild)
        && startOffset != 0) {
            // "Let previous item be the child of start node with index start
            // offset minus one."
            var previousItem = startNode.childNodes[startOffset - 1];

            // "If previous item's lastChild is an inline node other than a br,
            // call createElement("br") on the context object and append the
            // result as the last child of previous item."
            if (isInlineNode(previousItem.lastChild)
            && !isHtmlElement(previousItem.lastChild, "br")) {
                previousItem.appendChild(document.createElement("br"));
            }

            // "If previous item's lastChild is an inline node, call
            // createElement("br") on the context object and append the result
            // as the last child of previous item."
            if (isInlineNode(previousItem.lastChild)) {
                previousItem.appendChild(document.createElement("br"));
            }
        }

        // "If start node's child with index start offset is an li or dt or dd,
        // and that child's previousSibling is also an li or dt or dd:"
        if (isHtmlElement(startNode.childNodes[startOffset], ["li", "dt", "dd"])
        && isHtmlElement(startNode.childNodes[startOffset].previousSibling, ["li", "dt", "dd"])) {
            // "Call cloneRange() on the active range, and let original range
            // be the result."
            //
            // We need to add it to extraRanges so it will actually get updated
            // when moving preserving ranges.
            var originalRange = getActiveRange().cloneRange();
            extraRanges.push(originalRange);

            // "Set start node to its child with index start offset − 1."
            startNode = startNode.childNodes[startOffset - 1];

            // "Set start offset to start node's length."
            startOffset = getNodeLength(startNode);

            // "Set node to start node's nextSibling."
            node = startNode.nextSibling;

            // "Call collapse(start node, start offset) on the context object's
            // Selection."
            getSelection().collapse(startNode, startOffset);
            getActiveRange().setStart(startNode, startOffset);

            // "Call extend(node, 0) on the context object's Selection."
            getSelection().extend(node, 0);
            getActiveRange().setEnd(node, 0);

            // "Delete the selection."
            deleteSelection();

            // "Call removeAllRanges() on the context object's Selection."
            getSelection().removeAllRanges();

            // "Call addRange(original range) on the context object's
            // Selection."
            getSelection().addRange(originalRange);
            getActiveRange().setStart(originalRange.startContainer, originalRange.startOffset);
            getActiveRange().setEnd(originalRange.endContainer, originalRange.endOffset);

            // "Return true."
            extraRanges.pop();
            return true;
        }

        // "While start node has a child with index start offset minus one:"
        while (0 <= startOffset - 1
        && startOffset - 1 < startNode.childNodes.length) {
            // "If start node's child with index start offset minus one is
            // editable and invisible, remove it from start node, then subtract
            // one from start offset."
            if (isEditable(startNode.childNodes[startOffset - 1])
            && isInvisible(startNode.childNodes[startOffset - 1])) {
                startNode.removeChild(startNode.childNodes[startOffset - 1]);
                startOffset--;

            // "Otherwise, set start node to its child with index start offset
            // minus one, then set start offset to the length of start node."
            } else {
                startNode = startNode.childNodes[startOffset - 1];
                startOffset = getNodeLength(startNode);
            }
        }

        // "Call collapse(start node, start offset) on the context object's
        // Selection."
        getSelection().collapse(startNode, startOffset);
        getActiveRange().setStart(startNode, startOffset);

        // "Call extend(node, offset) on the context object's Selection."
        getSelection().extend(node, offset);
        getActiveRange().setEnd(node, offset);

        // "Delete the selection, with direction "backward"."
        deleteSelection({direction: "backward"});

        // "Return true."
        return true;
    }
};

//@}
///// The formatBlock command /////
//@{
// "A formattable block name is "address", "dd", "div", "dt", "h1", "h2", "h3",
// "h4", "h5", "h6", "p", or "pre"."
var formattableBlockNames = ["address", "dd", "div", "dt", "h1", "h2", "h3",
    "h4", "h5", "h6", "p", "pre"];

commands.formatblock = {
    preservesOverrides: true,
    action: function(value) {
        // "If value begins with a "<" character and ends with a ">" character,
        // remove the first and last characters from it."
        if (/^<.*>$/.test(value)) {
            value = value.slice(1, -1);
        }

        // "Let value be converted to ASCII lowercase."
        value = value.toLowerCase();

        // "If value is not a formattable block name, return false."
        if (formattableBlockNames.indexOf(value) == -1) {
            return false;
        }

        // "Block-extend the active range, and let new range be the result."
        var newRange = blockExtend(getActiveRange());

        // "Let node list be an empty list of nodes."
        //
        // "For each node node contained in new range, append node to node list
        // if it is editable, the last member of original node list (if any) is
        // not an ancestor of node, node is either a non-list single-line
        // container or an allowed child of "p" or a dd or dt, and node is not
        // the ancestor of a prohibited paragraph child."
        var nodeList = getContainedNodes(newRange, function(node) {
            return isEditable(node)
                && (isNonListSingleLineContainer(node)
                || isAllowedChild(node, "p")
                || isHtmlElement(node, ["dd", "dt"]))
                && !getDescendants(node).some(isProhibitedParagraphChild);
        });

        // "Record the values of node list, and let values be the result."
        var values = recordValues(nodeList);

        // "For each node in node list, while node is the descendant of an
        // editable HTML element in the same editing host, whose local name is
        // a formattable block name, and which is not the ancestor of a
        // prohibited paragraph child, split the parent of the one-node list
        // consisting of node."
        for (var i = 0; i < nodeList.length; i++) {
            var node = nodeList[i];
            while (getAncestors(node).some(function(ancestor) {
                return isEditable(ancestor)
                    && inSameEditingHost(ancestor, node)
                    && isHtmlElement(ancestor, formattableBlockNames)
                    && !getDescendants(ancestor).some(isProhibitedParagraphChild);
            })) {
                splitParent([node]);
            }
        }

        // "Restore the values from values."
        restoreValues(values);

        // "While node list is not empty:"
        while (nodeList.length) {
            var sublist;

            // "If the first member of node list is a single-line
            // container:"
            if (isSingleLineContainer(nodeList[0])) {
                // "Let sublist be the children of the first member of node
                // list."
                sublist = [].slice.call(nodeList[0].childNodes);

                // "Record the values of sublist, and let values be the
                // result."
                var values = recordValues(sublist);

                // "Remove the first member of node list from its parent,
                // preserving its descendants."
                removePreservingDescendants(nodeList[0]);

                // "Restore the values from values."
                restoreValues(values);

                // "Remove the first member from node list."
                nodeList.shift();

            // "Otherwise:"
            } else {
                // "Let sublist be an empty list of nodes."
                sublist = [];

                // "Remove the first member of node list and append it to
                // sublist."
                sublist.push(nodeList.shift());

                // "While node list is not empty, and the first member of
                // node list is the nextSibling of the last member of
                // sublist, and the first member of node list is not a
                // single-line container, and the last member of sublist is
                // not a br, remove the first member of node list and
                // append it to sublist."
                while (nodeList.length
                && nodeList[0] == sublist[sublist.length - 1].nextSibling
                && !isSingleLineContainer(nodeList[0])
                && !isHtmlElement(sublist[sublist.length - 1], "BR")) {
                    sublist.push(nodeList.shift());
                }
            }

            // "Wrap sublist. If value is "div" or "p", sibling criteria
            // returns false; otherwise it returns true for an HTML element
            // with local name value and no attributes, and false otherwise.
            // New parent instructions return the result of running
            // createElement(value) on the context object. Then fix disallowed
            // ancestors of the result."
            fixDisallowedAncestors(wrap(sublist,
                ["div", "p"].indexOf(value) == - 1
                    ? function(node) { return isHtmlElement(node, value) && !node.attributes.length }
                    : function() { return false },
                function() { return document.createElement(value) }));
        }

        // "Return true."
        return true;
    }, indeterm: function() {
        // "If the active range is null, return false."
        if (!getActiveRange()) {
            return false;
        }

        // "Block-extend the active range, and let new range be the result."
        var newRange = blockExtend(getActiveRange());

        // "Let node list be all visible editable nodes that are contained in
        // new range and have no children."
        var nodeList = getAllContainedNodes(newRange, function(node) {
            return isVisible(node)
                && isEditable(node)
                && !node.hasChildNodes();
        });

        // "If node list is empty, return false."
        if (!nodeList.length) {
            return false;
        }

        // "Let type be null."
        var type = null;

        // "For each node in node list:"
        for (var i = 0; i < nodeList.length; i++) {
            var node = nodeList[i];

            // "While node's parent is editable and in the same editing host as
            // node, and node is not an HTML element whose local name is a
            // formattable block name, set node to its parent."
            while (isEditable(node.parentNode)
            && inSameEditingHost(node, node.parentNode)
            && !isHtmlElement(node, formattableBlockNames)) {
                node = node.parentNode;
            }

            // "Let current type be the empty string."
            var currentType = "";

            // "If node is an editable HTML element whose local name is a
            // formattable block name, and node is not the ancestor of a
            // prohibited paragraph child, set current type to node's local
            // name."
            if (isEditable(node)
            && isHtmlElement(node, formattableBlockNames)
            && !getDescendants(node).some(isProhibitedParagraphChild)) {
                currentType = node.tagName;
            }

            // "If type is null, set type to current type."
            if (type === null) {
                type = currentType;

            // "Otherwise, if type does not equal current type, return true."
            } else if (type != currentType) {
                return true;
            }
        }

        // "Return false."
        return false;
    }, value: function() {
        // "If the active range is null, return the empty string."
        if (!getActiveRange()) {
            return "";
        }

        // "Block-extend the active range, and let new range be the result."
        var newRange = blockExtend(getActiveRange());

        // "Let node be the first visible editable node that is contained in
        // new range and has no children. If there is no such node, return the
        // empty string."
        var nodes = getAllContainedNodes(newRange, function(node) {
            return isVisible(node)
                && isEditable(node)
                && !node.hasChildNodes();
        });
        if (!nodes.length) {
            return "";
        }
        var node = nodes[0];

        // "While node's parent is editable and in the same editing host as
        // node, and node is not an HTML element whose local name is a
        // formattable block name, set node to its parent."
        while (isEditable(node.parentNode)
        && inSameEditingHost(node, node.parentNode)
        && !isHtmlElement(node, formattableBlockNames)) {
            node = node.parentNode;
        }

        // "If node is an editable HTML element whose local name is a
        // formattable block name, and node is not the ancestor of a prohibited
        // paragraph child, return node's local name, converted to ASCII
        // lowercase."
        if (isEditable(node)
        && isHtmlElement(node, formattableBlockNames)
        && !getDescendants(node).some(isProhibitedParagraphChild)) {
            return node.tagName.toLowerCase();
        }

        // "Return the empty string."
        return "";
    }
};

//@}
///// The forwardDelete command /////
//@{
commands.forwarddelete = {
    preservesOverrides: true,
    action: function() {
        // "If the active range is not collapsed, delete the selection and
        // return true."
        if (!getActiveRange().collapsed) {
            deleteSelection();
            return true;
        }

        // "Canonicalize whitespace at the active range's start."
        canonicalizeWhitespace(getActiveRange().startContainer, getActiveRange().startOffset);

        // "Let node and offset be the active range's start node and offset."
        var node = getActiveRange().startContainer;
        var offset = getActiveRange().startOffset;

        // "Repeat the following steps:"
        while (true) {
            // "If offset is the length of node and node's nextSibling is an
            // editable invisible node, remove node's nextSibling from its
            // parent."
            if (offset == getNodeLength(node)
            && isEditable(node.nextSibling)
            && isInvisible(node.nextSibling)) {
                node.parentNode.removeChild(node.nextSibling);

            // "Otherwise, if node has a child with index offset and that child
            // is an editable invisible node, remove that child from node."
            } else if (offset < node.childNodes.length
            && isEditable(node.childNodes[offset])
            && isInvisible(node.childNodes[offset])) {
                node.removeChild(node.childNodes[offset]);

            // "Otherwise, if offset is the length of node and node is an
            // inline node, or if node is invisible, set offset to one plus the
            // index of node, then set node to its parent."
            } else if ((offset == getNodeLength(node)
            && isInlineNode(node))
            || isInvisible(node)) {
                offset = 1 + getNodeIndex(node);
                node = node.parentNode;

            // "Otherwise, if node has a child with index offset and that child
            // is neither a block node nor a br nor an img nor a collapsed
            // block prop, set node to that child, then set offset to zero."
            } else if (offset < node.childNodes.length
            && !isBlockNode(node.childNodes[offset])
            && !isHtmlElement(node.childNodes[offset], ["br", "img"])
            && !isCollapsedBlockProp(node.childNodes[offset])) {
                node = node.childNodes[offset];
                offset = 0;

            // "Otherwise, break from this loop."
            } else {
                break;
            }
        }

        // "If node is a Text node and offset is not node's length:"
        if (node.nodeType == Node.TEXT_NODE
        && offset != getNodeLength(node)) {
            // "Let end offset be offset plus one."
            var endOffset = offset + 1;

            // "While end offset is not node's length and the end offsetth
            // element of node's data has general category M when interpreted
            // as a Unicode code point, add one to end offset."
            //
            // TODO: Not even going to try handling anything beyond the most
            // basic combining marks, since I couldn't find a good list.  I
            // special-case a few Hebrew diacritics too to test basic coverage
            // of non-Latin stuff.
            while (endOffset != node.length
            && /^[\u0300-\u036f\u0591-\u05bd\u05c1\u05c2]$/.test(node.data[endOffset])) {
                endOffset++;
            }

            // "Call collapse(node, offset) on the context object's Selection."
            getSelection().collapse(node, offset);
            getActiveRange().setStart(node, offset);

            // "Call extend(node, end offset) on the context object's
            // Selection."
            getSelection().extend(node, endOffset);
            getActiveRange().setEnd(node, endOffset);

            // "Delete the selection."
            deleteSelection();

            // "Return true."
            return true;
        }

        // "If node is an inline node, return true."
        if (isInlineNode(node)) {
            return true;
        }

        // "If node has a child with index offset and that child is a br or hr
        // or img, but is not a collapsed block prop:"
        if (offset < node.childNodes.length
        && isHtmlElement(node.childNodes[offset], ["br", "hr", "img"])
        && !isCollapsedBlockProp(node.childNodes[offset])) {
            // "Call collapse(node, offset) on the context object's Selection."
            getSelection().collapse(node, offset);
            getActiveRange().setStart(node, offset);

            // "Call extend(node, offset + 1) on the context object's
            // Selection."
            getSelection().extend(node, offset + 1);
            getActiveRange().setEnd(node, offset + 1);

            // "Delete the selection."
            deleteSelection();

            // "Return true."
            return true;
        }

        // "Let end node equal node and let end offset equal offset."
        var endNode = node;
        var endOffset = offset;

        // "If end node has a child with index end offset, and that child is a
        // collapsed block prop, add one to end offset."
        if (endOffset < endNode.childNodes.length
        && isCollapsedBlockProp(endNode.childNodes[endOffset])) {
            endOffset++;
        }

        // "Repeat the following steps:"
        while (true) {
            // "If end offset is the length of end node, set end offset to one
            // plus the index of end node and then set end node to its parent."
            if (endOffset == getNodeLength(endNode)) {
                endOffset = 1 + getNodeIndex(endNode);
                endNode = endNode.parentNode;

            // "Otherwise, if end node has a an editable invisible child with
            // index end offset, remove it from end node."
            } else if (endOffset < endNode.childNodes.length
            && isEditable(endNode.childNodes[endOffset])
            && isInvisible(endNode.childNodes[endOffset])) {
                endNode.removeChild(endNode.childNodes[endOffset]);

            // "Otherwise, break from this loop."
            } else {
                break;
            }
        }

        // "If the child of end node with index end offset minus one is a
        // table, return true."
        if (isHtmlElement(endNode.childNodes[endOffset - 1], "table")) {
            return true;
        }

        // "If the child of end node with index end offset is a table:"
        if (isHtmlElement(endNode.childNodes[endOffset], "table")) {
            // "Call collapse(end node, end offset) on the context object's
            // Selection."
            getSelection().collapse(endNode, endOffset);
            getActiveRange().setStart(endNode, endOffset);

            // "Call extend(end node, end offset + 1) on the context object's
            // Selection."
            getSelection().extend(endNode, endOffset + 1);
            getActiveRange().setEnd(endNode, endOffset + 1);

            // "Return true."
            return true;
        }

        // "If offset is the length of node, and the child of end node with
        // index end offset is an hr or br:"
        if (offset == getNodeLength(node)
        && isHtmlElement(endNode.childNodes[endOffset], ["br", "hr"])) {
            // "Call collapse(end node, end offset) on the context object's
            // Selection."
            getSelection().collapse(endNode, endOffset);
            getActiveRange().setStart(endNode, endOffset);

            // "Call extend(end node, end offset + 1) on the context object's
            // Selection."
            getSelection().extend(endNode, endOffset + 1);
            getActiveRange().setEnd(endNode, endOffset + 1);

            // "Delete the selection."
            deleteSelection();

            // "Call collapse(node, offset) on the Selection."
            getSelection().collapse(node, offset);
            getActiveRange().setStart(node, offset);
            getActiveRange().collapse(true);

            // "Return true."
            return true;
        }

        // "While end node has a child with index end offset:"
        while (endOffset < endNode.childNodes.length) {
            // "If end node's child with index end offset is editable and
            // invisible, remove it from end node."
            if (isEditable(endNode.childNodes[endOffset])
            && isInvisible(endNode.childNodes[endOffset])) {
                endNode.removeChild(endNode.childNodes[endOffset]);

            // "Otherwise, set end node to its child with index end offset and
            // set end offset to zero."
            } else {
                endNode = endNode.childNodes[endOffset];
                endOffset = 0;
            }
        }

        // "Call collapse(node, offset) on the context object's Selection."
        getSelection().collapse(node, offset);
        getActiveRange().setStart(node, offset);

        // "Call extend(end node, end offset) on the context object's
        // Selection."
        getSelection().extend(endNode, endOffset);
        getActiveRange().setEnd(endNode, endOffset);

        // "Delete the selection."
        deleteSelection();

        // "Return true."
        return true;
    }
};

//@}
///// The indent command /////
//@{
commands.indent = {
    preservesOverrides: true,
    action: function() {
        // "Let items be a list of all lis that are ancestor containers of the
        // active range's start and/or end node."
        //
        // Has to be in tree order, remember!
        var items = [];
        for (var node = getActiveRange().endContainer; node != getActiveRange().commonAncestorContainer; node = node.parentNode) {
            if (isHtmlElement(node, "LI")) {
                items.unshift(node);
            }
        }
        for (var node = getActiveRange().startContainer; node != getActiveRange().commonAncestorContainer; node = node.parentNode) {
            if (isHtmlElement(node, "LI")) {
                items.unshift(node);
            }
        }
        for (var node = getActiveRange().commonAncestorContainer; node; node = node.parentNode) {
            if (isHtmlElement(node, "LI")) {
                items.unshift(node);
            }
        }

        // "For each item in items, normalize sublists of item."
        for (var i = 0; i < items.length; i++) {
            normalizeSublists(items[i]);
        }

        // "Block-extend the active range, and let new range be the result."
        var newRange = blockExtend(getActiveRange());

        // "Let node list be a list of nodes, initially empty."
        var nodeList = [];

        // "For each node node contained in new range, if node is editable and
        // is an allowed child of "div" or "ol" and if the last member of node
        // list (if any) is not an ancestor of node, append node to node list."
        nodeList = getContainedNodes(newRange, function(node) {
            return isEditable(node)
                && (isAllowedChild(node, "div")
                || isAllowedChild(node, "ol"));
        });

        // "If the first visible member of node list is an li whose parent is
        // an ol or ul:"
        if (isHtmlElement(nodeList.filter(isVisible)[0], "li")
        && isHtmlElement(nodeList.filter(isVisible)[0].parentNode, ["ol", "ul"])) {
            // "Let sibling be node list's first visible member's
            // previousSibling."
            var sibling = nodeList.filter(isVisible)[0].previousSibling;

            // "While sibling is invisible, set sibling to its
            // previousSibling."
            while (isInvisible(sibling)) {
                sibling = sibling.previousSibling;
            }

            // "If sibling is an li, normalize sublists of sibling."
            if (isHtmlElement(sibling, "li")) {
                normalizeSublists(sibling);
            }
        }

        // "While node list is not empty:"
        while (nodeList.length) {
            // "Let sublist be a list of nodes, initially empty."
            var sublist = [];

            // "Remove the first member of node list and append it to sublist."
            sublist.push(nodeList.shift());

            // "While the first member of node list is the nextSibling of the
            // last member of sublist, remove the first member of node list and
            // append it to sublist."
            while (nodeList.length
            && nodeList[0] == sublist[sublist.length - 1].nextSibling) {
                sublist.push(nodeList.shift());
            }

            // "Indent sublist."
            indentNodes(sublist);
        }

        // "Return true."
        return true;
    }
};

//@}
///// The insertHorizontalRule command /////
//@{
commands.inserthorizontalrule = {
    preservesOverrides: true,
    action: function() {
        // "Let start node, start offset, end node, and end offset be the
        // active range's start and end nodes and offsets."
        var startNode = getActiveRange().startContainer;
        var startOffset = getActiveRange().startOffset;
        var endNode = getActiveRange().endContainer;
        var endOffset = getActiveRange().endOffset;

        // "While start offset is 0 and start node's parent is not null, set
        // start offset to start node's index, then set start node to its
        // parent."
        while (startOffset == 0
        && startNode.parentNode) {
            startOffset = getNodeIndex(startNode);
            startNode = startNode.parentNode;
        }

        // "While end offset is end node's length, and end node's parent is not
        // null, set end offset to one plus end node's index, then set end node
        // to its parent."
        while (endOffset == getNodeLength(endNode)
        && endNode.parentNode) {
            endOffset = 1 + getNodeIndex(endNode);
            endNode = endNode.parentNode;
        }

        // "Call collapse(start node, start offset) on the context object's
        // Selection."
        getSelection().collapse(startNode, startOffset);
        getActiveRange().setStart(startNode, startOffset);

        // "Call extend(end node, end offset) on the context object's
        // Selection."
        getSelection().extend(endNode, endOffset);
        getActiveRange().setEnd(endNode, endOffset);

        // "Delete the selection, with block merging false."
        deleteSelection({blockMerging: false});

        // "If the active range's start node is neither editable nor an editing
        // host, return true."
        if (!isEditable(getActiveRange().startContainer)
        && !isEditingHost(getActiveRange().startContainer)) {
            return true;
        }

        // "If the active range's start node is a Text node and its start
        // offset is zero, call collapse() on the context object's Selection,
        // with first argument the active range's start node's parent and
        // second argument the active range's start node's index."
        if (getActiveRange().startContainer.nodeType == Node.TEXT_NODE
        && getActiveRange().startOffset == 0) {
            var newNode = getActiveRange().startContainer.parentNode;
            var newOffset = getNodeIndex(getActiveRange().startContainer);
            getSelection().collapse(newNode, newOffset);
            getActiveRange().setStart(newNode, newOffset);
            getActiveRange().collapse(true);
        }

        // "If the active range's start node is a Text node and its start
        // offset is the length of its start node, call collapse() on the
        // context object's Selection, with first argument the active range's
        // start node's parent, and the second argument one plus the active
        // range's start node's index."
        if (getActiveRange().startContainer.nodeType == Node.TEXT_NODE
        && getActiveRange().startOffset == getNodeLength(getActiveRange().startContainer)) {
            var newNode = getActiveRange().startContainer.parentNode;
            var newOffset = 1 + getNodeIndex(getActiveRange().startContainer);
            getSelection().collapse(newNode, newOffset);
            getActiveRange().setStart(newNode, newOffset);
            getActiveRange().collapse(true);
        }

        // "Let hr be the result of calling createElement("hr") on the
        // context object."
        var hr = document.createElement("hr");

        // "Run insertNode(hr) on the active range."
        getActiveRange().insertNode(hr);

        // "Fix disallowed ancestors of hr."
        fixDisallowedAncestors(hr);

        // "Run collapse() on the context object's Selection, with first
        // argument hr's parent and the second argument equal to one plus hr's
        // index."
        getSelection().collapse(hr.parentNode, 1 + getNodeIndex(hr));
        getActiveRange().setStart(hr.parentNode, 1 + getNodeIndex(hr));
        getActiveRange().collapse(true);

        // "Return true."
        return true;
    }
};

//@}
///// The insertHTML command /////
//@{
commands.inserthtml = {
    preservesOverrides: true,
    action: function(value) {
        // "Delete the selection."
        deleteSelection();

        // "If the active range's start node is neither editable nor an editing
        // host, return true."
        if (!isEditable(getActiveRange().startContainer)
        && !isEditingHost(getActiveRange().startContainer)) {
            return true;
        }

        // "Let frag be the result of calling createContextualFragment(value)
        // on the active range."
        var frag = getActiveRange().createContextualFragment(value);

        // "Let last child be the lastChild of frag."
        var lastChild = frag.lastChild;

        // "If last child is null, return true."
        if (!lastChild) {
            return true;
        }

        // "Let descendants be all descendants of frag."
        var descendants = getDescendants(frag);

        // "If the active range's start node is a block node:"
        if (isBlockNode(getActiveRange().startContainer)) {
            // "Let collapsed block props be all editable collapsed block prop
            // children of the active range's start node that have index
            // greater than or equal to the active range's start offset."
            //
            // "For each node in collapsed block props, remove node from its
            // parent."
            [].filter.call(getActiveRange().startContainer.childNodes, function(node) {
                return isEditable(node)
                    && isCollapsedBlockProp(node)
                    && getNodeIndex(node) >= getActiveRange().startOffset;
            }).forEach(function(node) {
                node.parentNode.removeChild(node);
            });
        }

        // "Call insertNode(frag) on the active range."
        getActiveRange().insertNode(frag);

        // "If the active range's start node is a block node with no visible
        // children, call createElement("br") on the context object and append
        // the result as the last child of the active range's start node."
        if (isBlockNode(getActiveRange().startContainer)
        && ![].some.call(getActiveRange().startContainer.childNodes, isVisible)) {
            getActiveRange().startContainer.appendChild(document.createElement("br"));
        }

        // "Call collapse() on the context object's Selection, with last
        // child's parent as the first argument and one plus its index as the
        // second."
        getActiveRange().setStart(lastChild.parentNode, 1 + getNodeIndex(lastChild));
        getActiveRange().setEnd(lastChild.parentNode, 1 + getNodeIndex(lastChild));

        // "Fix disallowed ancestors of each member of descendants."
        for (var i = 0; i < descendants.length; i++) {
            fixDisallowedAncestors(descendants[i]);
        }

        // "Return true."
        return true;
    }
};

//@}
///// The insertImage command /////
//@{
commands.insertimage = {
    preservesOverrides: true,
    action: function(value) {
        // "If value is the empty string, return false."
        if (value === "") {
            return false;
        }

        // "Delete the selection, with strip wrappers false."
        deleteSelection({stripWrappers: false});

        // "Let range be the active range."
        var range = getActiveRange();

        // "If the active range's start node is neither editable nor an editing
        // host, return true."
        if (!isEditable(getActiveRange().startContainer)
        && !isEditingHost(getActiveRange().startContainer)) {
            return true;
        }

        // "If range's start node is a block node whose sole child is a br, and
        // its start offset is 0, remove its start node's child from it."
        if (isBlockNode(range.startContainer)
        && range.startContainer.childNodes.length == 1
        && isHtmlElement(range.startContainer.firstChild, "br")
        && range.startOffset == 0) {
            range.startContainer.removeChild(range.startContainer.firstChild);
        }

        // "Let img be the result of calling createElement("img") on the
        // context object."
        var img = document.createElement("img");

        // "Run setAttribute("src", value) on img."
        img.setAttribute("src", value);

        // "Run insertNode(img) on the range."
        range.insertNode(img);

        // "Run collapse() on the Selection, with first argument equal to the
        // parent of img and the second argument equal to one plus the index of
        // img."
        //
        // Not everyone actually supports collapse(), so we do it manually
        // instead.  Also, we need to modify the actual range we're given as
        // well, for the sake of autoimplementation.html's range-filling-in.
        range.setStart(img.parentNode, 1 + getNodeIndex(img));
        range.setEnd(img.parentNode, 1 + getNodeIndex(img));
        getSelection().removeAllRanges();
        getSelection().addRange(range);

        // IE adds width and height attributes for some reason, so remove those
        // to actually do what the spec says.
        img.removeAttribute("width");
        img.removeAttribute("height");

        // "Return true."
        return true;
    }
};

//@}
///// The insertLineBreak command /////
//@{
commands.insertlinebreak = {
    preservesOverrides: true,
    action: function(value) {
        // "Delete the selection, with strip wrappers false."
        deleteSelection({stripWrappers: false});

        // "If the active range's start node is neither editable nor an editing
        // host, return true."
        if (!isEditable(getActiveRange().startContainer)
        && !isEditingHost(getActiveRange().startContainer)) {
            return true;
        }

        // "If the active range's start node is an Element, and "br" is not an
        // allowed child of it, return true."
        if (getActiveRange().startContainer.nodeType == Node.ELEMENT_NODE
        && !isAllowedChild("br", getActiveRange().startContainer)) {
            return true;
        }

        // "If the active range's start node is not an Element, and "br" is not
        // an allowed child of the active range's start node's parent, return
        // true."
        if (getActiveRange().startContainer.nodeType != Node.ELEMENT_NODE
        && !isAllowedChild("br", getActiveRange().startContainer.parentNode)) {
            return true;
        }

        // "If the active range's start node is a Text node and its start
        // offset is zero, call collapse() on the context object's Selection,
        // with first argument equal to the active range's start node's parent
        // and second argument equal to the active range's start node's index."
        if (getActiveRange().startContainer.nodeType == Node.TEXT_NODE
        && getActiveRange().startOffset == 0) {
            var newNode = getActiveRange().startContainer.parentNode;
            var newOffset = getNodeIndex(getActiveRange().startContainer);
            getSelection().collapse(newNode, newOffset);
            getActiveRange().setStart(newNode, newOffset);
            getActiveRange().setEnd(newNode, newOffset);
        }

        // "If the active range's start node is a Text node and its start
        // offset is the length of its start node, call collapse() on the
        // context object's Selection, with first argument equal to the active
        // range's start node's parent and second argument equal to one plus
        // the active range's start node's index."
        if (getActiveRange().startContainer.nodeType == Node.TEXT_NODE
        && getActiveRange().startOffset == getNodeLength(getActiveRange().startContainer)) {
            var newNode = getActiveRange().startContainer.parentNode;
            var newOffset = 1 + getNodeIndex(getActiveRange().startContainer);
            getSelection().collapse(newNode, newOffset);
            getActiveRange().setStart(newNode, newOffset);
            getActiveRange().setEnd(newNode, newOffset);
        }

        // "Let br be the result of calling createElement("br") on the context
        // object."
        var br = document.createElement("br");

        // "Call insertNode(br) on the active range."
        getActiveRange().insertNode(br);

        // "Call collapse() on the context object's Selection, with br's parent
        // as the first argument and one plus br's index as the second
        // argument."
        getSelection().collapse(br.parentNode, 1 + getNodeIndex(br));
        getActiveRange().setStart(br.parentNode, 1 + getNodeIndex(br));
        getActiveRange().setEnd(br.parentNode, 1 + getNodeIndex(br));

        // "If br is a collapsed line break, call createElement("br") on the
        // context object and let extra br be the result, then call
        // insertNode(extra br) on the active range."
        if (isCollapsedLineBreak(br)) {
            getActiveRange().insertNode(document.createElement("br"));

            // Compensate for nonstandard implementations of insertNode
            getSelection().collapse(br.parentNode, 1 + getNodeIndex(br));
            getActiveRange().setStart(br.parentNode, 1 + getNodeIndex(br));
            getActiveRange().setEnd(br.parentNode, 1 + getNodeIndex(br));
        }

        // "Return true."
        return true;
    }
};

//@}
///// The insertOrderedList command /////
//@{
commands.insertorderedlist = {
    preservesOverrides: true,
    // "Toggle lists with tag name "ol", then return true."
    action: function() { toggleLists("ol"); return true },
    // "True if the selection's list state is "mixed" or "mixed ol", false
    // otherwise."
    indeterm: function() { return /^mixed( ol)?$/.test(getSelectionListState()) },
    // "True if the selection's list state is "ol", false otherwise."
    state: function() { return getSelectionListState() == "ol" },
};

//@}
///// The insertParagraph command /////
//@{
commands.insertparagraph = {
    preservesOverrides: true,
    action: function() {
        // "Delete the selection."
        deleteSelection();

        // "If the active range's start node is neither editable nor an editing
        // host, return true."
        if (!isEditable(getActiveRange().startContainer)
        && !isEditingHost(getActiveRange().startContainer)) {
            return true;
        }

        // "Let node and offset be the active range's start node and offset."
        var node = getActiveRange().startContainer;
        var offset = getActiveRange().startOffset;

        // "If node is a Text node, and offset is neither 0 nor the length of
        // node, call splitText(offset) on node."
        if (node.nodeType == Node.TEXT_NODE
        && offset != 0
        && offset != getNodeLength(node)) {
            node.splitText(offset);
        }

        // "If node is a Text node and offset is its length, set offset to one
        // plus the index of node, then set node to its parent."
        if (node.nodeType == Node.TEXT_NODE
        && offset == getNodeLength(node)) {
            offset = 1 + getNodeIndex(node);
            node = node.parentNode;
        }

        // "If node is a Text or Comment node, set offset to the index of node,
        // then set node to its parent."
        if (node.nodeType == Node.TEXT_NODE
        || node.nodeType == Node.COMMENT_NODE) {
            offset = getNodeIndex(node);
            node = node.parentNode;
        }

        // "Call collapse(node, offset) on the context object's Selection."
        getSelection().collapse(node, offset);
        getActiveRange().setStart(node, offset);
        getActiveRange().setEnd(node, offset);

        // "Let container equal node."
        var container = node;

        // "While container is not a single-line container, and container's
        // parent is editable and in the same editing host as node, set
        // container to its parent."
        while (!isSingleLineContainer(container)
        && isEditable(container.parentNode)
        && inSameEditingHost(node, container.parentNode)) {
            container = container.parentNode;
        }

        // "If container is an editable single-line container in the same
        // editing host as node, and its local name is "p" or "div":"
        if (isEditable(container)
        && isSingleLineContainer(container)
        && inSameEditingHost(node, container.parentNode)
        && (container.tagName == "P" || container.tagName == "DIV")) {
            // "Let outer container equal container."
            var outerContainer = container;

            // "While outer container is not a dd or dt or li, and outer
            // container's parent is editable, set outer container to its
            // parent."
            while (!isHtmlElement(outerContainer, ["dd", "dt", "li"])
            && isEditable(outerContainer.parentNode)) {
                outerContainer = outerContainer.parentNode;
            }

            // "If outer container is a dd or dt or li, set container to outer
            // container."
            if (isHtmlElement(outerContainer, ["dd", "dt", "li"])) {
                container = outerContainer;
            }
        }

        // "If container is not editable or not in the same editing host as
        // node or is not a single-line container:"
        if (!isEditable(container)
        || !inSameEditingHost(container, node)
        || !isSingleLineContainer(container)) {
            // "Let tag be the default single-line container name."
            var tag = defaultSingleLineContainerName;

            // "Block-extend the active range, and let new range be the
            // result."
            var newRange = blockExtend(getActiveRange());

            // "Let node list be a list of nodes, initially empty."
            //
            // "Append to node list the first node in tree order that is
            // contained in new range and is an allowed child of "p", if any."
            var nodeList = getContainedNodes(newRange, function(node) { return isAllowedChild(node, "p") })
                .slice(0, 1);

            // "If node list is empty:"
            if (!nodeList.length) {
                // "If tag is not an allowed child of the active range's start
                // node, return true."
                if (!isAllowedChild(tag, getActiveRange().startContainer)) {
                    return true;
                }

                // "Set container to the result of calling createElement(tag)
                // on the context object."
                container = document.createElement(tag);

                // "Call insertNode(container) on the active range."
                getActiveRange().insertNode(container);

                // "Call createElement("br") on the context object, and append
                // the result as the last child of container."
                container.appendChild(document.createElement("br"));

                // "Call collapse(container, 0) on the context object's
                // Selection."
                getSelection().collapse(container, 0);
                getActiveRange().setStart(container, 0);
                getActiveRange().setEnd(container, 0);

                // "Return true."
                return true;
            }

            // "While the nextSibling of the last member of node list is not
            // null and is an allowed child of "p", append it to node list."
            while (nodeList[nodeList.length - 1].nextSibling
            && isAllowedChild(nodeList[nodeList.length - 1].nextSibling, "p")) {
                nodeList.push(nodeList[nodeList.length - 1].nextSibling);
            }

            // "Wrap node list, with sibling criteria returning false and new
            // parent instructions returning the result of calling
            // createElement(tag) on the context object. Set container to the
            // result."
            container = wrap(nodeList,
                function() { return false },
                function() { return document.createElement(tag) }
            );
        }

        // "If container's local name is "address", "listing", or "pre":"
        if (container.tagName == "ADDRESS"
        || container.tagName == "LISTING"
        || container.tagName == "PRE") {
            // "Let br be the result of calling createElement("br") on the
            // context object."
            var br = document.createElement("br");

            // "Call insertNode(br) on the active range."
            getActiveRange().insertNode(br);

            // "Call collapse(node, offset + 1) on the context object's
            // Selection."
            getSelection().collapse(node, offset + 1);
            getActiveRange().setStart(node, offset + 1);
            getActiveRange().setEnd(node, offset + 1);

            // "If br is the last descendant of container, let br be the result
            // of calling createElement("br") on the context object, then call
            // insertNode(br) on the active range."
            //
            // Work around browser bugs: some browsers select the
            // newly-inserted node, not per spec.
            if (!isDescendant(nextNode(br), container)) {
                getActiveRange().insertNode(document.createElement("br"));
                getSelection().collapse(node, offset + 1);
                getActiveRange().setEnd(node, offset + 1);
            }

            // "Return true."
            return true;
        }

        // "If container's local name is "li", "dt", or "dd"; and either it has
        // no children or it has a single child and that child is a br:"
        if (["LI", "DT", "DD"].indexOf(container.tagName) != -1
        && (!container.hasChildNodes()
        || (container.childNodes.length == 1
        && isHtmlElement(container.firstChild, "br")))) {
            // "Split the parent of the one-node list consisting of container."
            splitParent([container]);

            // "If container has no children, call createElement("br") on the
            // context object and append the result as the last child of
            // container."
            if (!container.hasChildNodes()) {
                container.appendChild(document.createElement("br"));
            }

            // "If container is a dd or dt, and it is not an allowed child of
            // any of its ancestors in the same editing host, set the tag name
            // of container to the default single-line container name and let
            // container be the result."
            if (isHtmlElement(container, ["dd", "dt"])
            && getAncestors(container).every(function(ancestor) {
                return !inSameEditingHost(container, ancestor)
                    || !isAllowedChild(container, ancestor)
            })) {
                container = setTagName(container, defaultSingleLineContainerName);
            }

            // "Fix disallowed ancestors of container."
            fixDisallowedAncestors(container);

            // "Return true."
            return true;
        }

        // "Let new line range be a new range whose start is the same as
        // the active range's, and whose end is (container, length of
        // container)."
        var newLineRange = document.createRange();
        newLineRange.setStart(getActiveRange().startContainer, getActiveRange().startOffset);
        newLineRange.setEnd(container, getNodeLength(container));

        // "While new line range's start offset is zero and its start node is
        // not a prohibited paragraph child, set its start to (parent of start
        // node, index of start node)."
        while (newLineRange.startOffset == 0
        && !isProhibitedParagraphChild(newLineRange.startContainer)) {
            newLineRange.setStart(newLineRange.startContainer.parentNode, getNodeIndex(newLineRange.startContainer));
        }

        // "While new line range's start offset is the length of its start node
        // and its start node is not a prohibited paragraph child, set its
        // start to (parent of start node, 1 + index of start node)."
        while (newLineRange.startOffset == getNodeLength(newLineRange.startContainer)
        && !isProhibitedParagraphChild(newLineRange.startContainer)) {
            newLineRange.setStart(newLineRange.startContainer.parentNode, 1 + getNodeIndex(newLineRange.startContainer));
        }

        // "Let end of line be true if new line range contains either nothing
        // or a single br, and false otherwise."
        var containedInNewLineRange = getContainedNodes(newLineRange);
        var endOfLine = !containedInNewLineRange.length
            || (containedInNewLineRange.length == 1
            && isHtmlElement(containedInNewLineRange[0], "br"));

        // "If the local name of container is "h1", "h2", "h3", "h4", "h5", or
        // "h6", and end of line is true, let new container name be the default
        // single-line container name."
        var newContainerName;
        if (/^H[1-6]$/.test(container.tagName)
        && endOfLine) {
            newContainerName = defaultSingleLineContainerName;

        // "Otherwise, if the local name of container is "dt" and end of line
        // is true, let new container name be "dd"."
        } else if (container.tagName == "DT"
        && endOfLine) {
            newContainerName = "dd";

        // "Otherwise, if the local name of container is "dd" and end of line
        // is true, let new container name be "dt"."
        } else if (container.tagName == "DD"
        && endOfLine) {
            newContainerName = "dt";

        // "Otherwise, let new container name be the local name of container."
        } else {
            newContainerName = container.tagName.toLowerCase();
        }

        // "Let new container be the result of calling createElement(new
        // container name) on the context object."
        var newContainer = document.createElement(newContainerName);

        // "Copy all attributes of container to new container."
        for (var i = 0; i < container.attributes.length; i++) {
            newContainer.setAttributeNS(container.attributes[i].namespaceURI, container.attributes[i].name, container.attributes[i].value);
        }

        // "If new container has an id attribute, unset it."
        newContainer.removeAttribute("id");

        // "Insert new container into the parent of container immediately after
        // container."
        container.parentNode.insertBefore(newContainer, container.nextSibling);

        // "Let contained nodes be all nodes contained in new line range."
        var containedNodes = getAllContainedNodes(newLineRange);

        // "Let frag be the result of calling extractContents() on new line
        // range."
        var frag = newLineRange.extractContents();

        // "Unset the id attribute (if any) of each Element descendant of frag
        // that is not in contained nodes."
        var descendants = getDescendants(frag);
        for (var i = 0; i < descendants.length; i++) {
            if (descendants[i].nodeType == Node.ELEMENT_NODE
            && containedNodes.indexOf(descendants[i]) == -1) {
                descendants[i].removeAttribute("id");
            }
        }

        // "Call appendChild(frag) on new container."
        newContainer.appendChild(frag);

        // "While container's lastChild is a prohibited paragraph child, set
        // container to its lastChild."
        while (isProhibitedParagraphChild(container.lastChild)) {
            container = container.lastChild;
        }

        // "While new container's lastChild is a prohibited paragraph child,
        // set new container to its lastChild."
        while (isProhibitedParagraphChild(newContainer.lastChild)) {
            newContainer = newContainer.lastChild;
        }

        // "If container has no visible children, call createElement("br") on
        // the context object, and append the result as the last child of
        // container."
        if (![].some.call(container.childNodes, isVisible)) {
            container.appendChild(document.createElement("br"));
        }

        // "If new container has no visible children, call createElement("br")
        // on the context object, and append the result as the last child of
        // new container."
        if (![].some.call(newContainer.childNodes, isVisible)) {
            newContainer.appendChild(document.createElement("br"));
        }

        // "Call collapse(new container, 0) on the context object's Selection."
        getSelection().collapse(newContainer, 0);
        getActiveRange().setStart(newContainer, 0);
        getActiveRange().setEnd(newContainer, 0);

        // "Return true."
        return true;
    }
};

//@}
///// The insertText command /////
//@{
commands.inserttext = {
    action: function(value) {
        // "Delete the selection, with strip wrappers false."
        deleteSelection({stripWrappers: false});

        // "If the active range's start node is neither editable nor an editing
        // host, return true."
        if (!isEditable(getActiveRange().startContainer)
        && !isEditingHost(getActiveRange().startContainer)) {
            return true;
        }

        // "If value's length is greater than one:"
        if (value.length > 1) {
            // "For each element el in value, take the action for the
            // insertText command, with value equal to el."
            for (var i = 0; i < value.length; i++) {
                commands.inserttext.action(value[i]);
            }

            // "Return true."
            return true;
        }

        // "If value is the empty string, return true."
        if (value == "") {
            return true;
        }

        // "If value is a newline (U+00A0), take the action for the
        // insertParagraph command and return true."
        if (value == "\n") {
            commands.insertparagraph.action();
            return true;
        }

        // "Let node and offset be the active range's start node and offset."
        var node = getActiveRange().startContainer;
        var offset = getActiveRange().startOffset;

        // "If node has a child whose index is offset − 1, and that child is a
        // Text node, set node to that child, then set offset to node's
        // length."
        if (0 <= offset - 1
        && offset - 1 < node.childNodes.length
        && node.childNodes[offset - 1].nodeType == Node.TEXT_NODE) {
            node = node.childNodes[offset - 1];
            offset = getNodeLength(node);
        }

        // "If node has a child whose index is offset, and that child is a Text
        // node, set node to that child, then set offset to zero."
        if (0 <= offset
        && offset < node.childNodes.length
        && node.childNodes[offset].nodeType == Node.TEXT_NODE) {
            node = node.childNodes[offset];
            offset = 0;
        }

        // "Record current overrides, and let overrides be the result."
        var overrides = recordCurrentOverrides();

        // "Call collapse(node, offset) on the context object's Selection."
        getSelection().collapse(node, offset);
        getActiveRange().setStart(node, offset);
        getActiveRange().setEnd(node, offset);

        // "Canonicalize whitespace at (node, offset)."
        canonicalizeWhitespace(node, offset);

        // "Let (node, offset) be the active range's start."
        node = getActiveRange().startContainer;
        offset = getActiveRange().startOffset;

        // "If node is a Text node:"
        if (node.nodeType == Node.TEXT_NODE) {
            // "Call insertData(offset, value) on node."
            node.insertData(offset, value);

            // "Call collapse(node, offset) on the context object's Selection."
            getSelection().collapse(node, offset);
            getActiveRange().setStart(node, offset);

            // "Call extend(node, offset + 1) on the context object's
            // Selection."
            //
            // Work around WebKit bug: the extend() can throw if the text we're
            // adding is trailing whitespace.
            try { getSelection().extend(node, offset + 1); } catch(e) {}
            getActiveRange().setEnd(node, offset + 1);

        // "Otherwise:"
        } else {
            // "If node has only one child, which is a collapsed line break,
            // remove its child from it."
            //
            // FIXME: IE incorrectly returns false here instead of true
            // sometimes?
            if (node.childNodes.length == 1
            && isCollapsedLineBreak(node.firstChild)) {
                node.removeChild(node.firstChild);
            }

            // "Let text be the result of calling createTextNode(value) on the
            // context object."
            var text = document.createTextNode(value);

            // "Call insertNode(text) on the active range."
            getActiveRange().insertNode(text);

            // "Call collapse(text, 0) on the context object's Selection."
            getSelection().collapse(text, 0);
            getActiveRange().setStart(text, 0);

            // "Call extend(text, 1) on the context object's Selection."
            getSelection().extend(text, 1);
            getActiveRange().setEnd(text, 1);
        }

        // "Restore states and values from overrides."
        restoreStatesAndValues(overrides);

        // "Canonicalize whitespace at the active range's start, with fix
        // collapsed space false."
        canonicalizeWhitespace(getActiveRange().startContainer, getActiveRange().startOffset, false);

        // "Canonicalize whitespace at the active range's end, with fix
        // collapsed space false."
        canonicalizeWhitespace(getActiveRange().endContainer, getActiveRange().endOffset, false);

        // "If value is a space character, autolink the active range's start."
        if (/^[ \t\n\f\r]$/.test(value)) {
            autolink(getActiveRange().startContainer, getActiveRange().startOffset);
        }

        // "Call collapseToEnd() on the context object's Selection."
        //
        // Work around WebKit bug: sometimes it blows up the selection and
        // throws, which we don't want.
        try { getSelection().collapseToEnd(); } catch(e) {}
        getActiveRange().collapse(false);

        // "Return true."
        return true;
    }
};

//@}
///// The insertUnorderedList command /////
//@{
commands.insertunorderedlist = {
    preservesOverrides: true,
    // "Toggle lists with tag name "ul", then return true."
    action: function() { toggleLists("ul"); return true },
    // "True if the selection's list state is "mixed" or "mixed ul", false
    // otherwise."
    indeterm: function() { return /^mixed( ul)?$/.test(getSelectionListState()) },
    // "True if the selection's list state is "ul", false otherwise."
    state: function() { return getSelectionListState() == "ul" },
};

//@}
///// The justifyCenter command /////
//@{
commands.justifycenter = {
    preservesOverrides: true,
    // "Justify the selection with alignment "center", then return true."
    action: function() { justifySelection("center"); return true },
    indeterm: function() {
        // "Return false if the active range is null.  Otherwise, block-extend
        // the active range. Return true if among visible editable nodes that
        // are contained in the result and have no children, at least one has
        // alignment value "center" and at least one does not. Otherwise return
        // false."
        if (!getActiveRange()) {
            return false;
        }
        var nodes = getAllContainedNodes(blockExtend(getActiveRange()), function(node) {
            return isEditable(node) && isVisible(node) && !node.hasChildNodes();
        });
        return nodes.some(function(node) { return getAlignmentValue(node) == "center" })
            && nodes.some(function(node) { return getAlignmentValue(node) != "center" });
    }, state: function() {
        // "Return false if the active range is null.  Otherwise, block-extend
        // the active range. Return true if there is at least one visible
        // editable node that is contained in the result and has no children,
        // and all such nodes have alignment value "center".  Otherwise return
        // false."
        if (!getActiveRange()) {
            return false;
        }
        var nodes = getAllContainedNodes(blockExtend(getActiveRange()), function(node) {
            return isEditable(node) && isVisible(node) && !node.hasChildNodes();
        });
        return nodes.length
            && nodes.every(function(node) { return getAlignmentValue(node) == "center" });
    }, value: function() {
        // "Return the empty string if the active range is null.  Otherwise,
        // block-extend the active range, and return the alignment value of the
        // first visible editable node that is contained in the result and has
        // no children. If there is no such node, return "left"."
        if (!getActiveRange()) {
            return "";
        }
        var nodes = getAllContainedNodes(blockExtend(getActiveRange()), function(node) {
            return isEditable(node) && isVisible(node) && !node.hasChildNodes();
        });
        if (nodes.length) {
            return getAlignmentValue(nodes[0]);
        } else {
            return "left";
        }
    },
};

//@}
///// The justifyFull command /////
//@{
commands.justifyfull = {
    preservesOverrides: true,
    // "Justify the selection with alignment "justify", then return true."
    action: function() { justifySelection("justify"); return true },
    indeterm: function() {
        // "Return false if the active range is null.  Otherwise, block-extend
        // the active range. Return true if among visible editable nodes that
        // are contained in the result and have no children, at least one has
        // alignment value "justify" and at least one does not. Otherwise
        // return false."
        if (!getActiveRange()) {
            return false;
        }
        var nodes = getAllContainedNodes(blockExtend(getActiveRange()), function(node) {
            return isEditable(node) && isVisible(node) && !node.hasChildNodes();
        });
        return nodes.some(function(node) { return getAlignmentValue(node) == "justify" })
            && nodes.some(function(node) { return getAlignmentValue(node) != "justify" });
    }, state: function() {
        // "Return false if the active range is null.  Otherwise, block-extend
        // the active range. Return true if there is at least one visible
        // editable node that is contained in the result and has no children,
        // and all such nodes have alignment value "justify".  Otherwise return
        // false."
        if (!getActiveRange()) {
            return false;
        }
        var nodes = getAllContainedNodes(blockExtend(getActiveRange()), function(node) {
            return isEditable(node) && isVisible(node) && !node.hasChildNodes();
        });
        return nodes.length
            && nodes.every(function(node) { return getAlignmentValue(node) == "justify" });
    }, value: function() {
        // "Return the empty string if the active range is null.  Otherwise,
        // block-extend the active range, and return the alignment value of the
        // first visible editable node that is contained in the result and has
        // no children. If there is no such node, return "left"."
        if (!getActiveRange()) {
            return "";
        }
        var nodes = getAllContainedNodes(blockExtend(getActiveRange()), function(node) {
            return isEditable(node) && isVisible(node) && !node.hasChildNodes();
        });
        if (nodes.length) {
            return getAlignmentValue(nodes[0]);
        } else {
            return "left";
        }
    },
};

//@}
///// The justifyLeft command /////
//@{
commands.justifyleft = {
    preservesOverrides: true,
    // "Justify the selection with alignment "left", then return true."
    action: function() { justifySelection("left"); return true },
    indeterm: function() {
        // "Return false if the active range is null.  Otherwise, block-extend
        // the active range. Return true if among visible editable nodes that
        // are contained in the result and have no children, at least one has
        // alignment value "left" and at least one does not. Otherwise return
        // false."
        if (!getActiveRange()) {
            return false;
        }
        var nodes = getAllContainedNodes(blockExtend(getActiveRange()), function(node) {
            return isEditable(node) && isVisible(node) && !node.hasChildNodes();
        });
        return nodes.some(function(node) { return getAlignmentValue(node) == "left" })
            && nodes.some(function(node) { return getAlignmentValue(node) != "left" });
    }, state: function() {
        // "Return false if the active range is null.  Otherwise, block-extend
        // the active range. Return true if there is at least one visible
        // editable node that is contained in the result and has no children,
        // and all such nodes have alignment value "left".  Otherwise return
        // false."
        if (!getActiveRange()) {
            return false;
        }
        var nodes = getAllContainedNodes(blockExtend(getActiveRange()), function(node) {
            return isEditable(node) && isVisible(node) && !node.hasChildNodes();
        });
        return nodes.length
            && nodes.every(function(node) { return getAlignmentValue(node) == "left" });
    }, value: function() {
        // "Return the empty string if the active range is null.  Otherwise,
        // block-extend the active range, and return the alignment value of the
        // first visible editable node that is contained in the result and has
        // no children. If there is no such node, return "left"."
        if (!getActiveRange()) {
            return "";
        }
        var nodes = getAllContainedNodes(blockExtend(getActiveRange()), function(node) {
            return isEditable(node) && isVisible(node) && !node.hasChildNodes();
        });
        if (nodes.length) {
            return getAlignmentValue(nodes[0]);
        } else {
            return "left";
        }
    },
};

//@}
///// The justifyRight command /////
//@{
commands.justifyright = {
    preservesOverrides: true,
    // "Justify the selection with alignment "right", then return true."
    action: function() { justifySelection("right"); return true },
    indeterm: function() {
        // "Return false if the active range is null.  Otherwise, block-extend
        // the active range. Return true if among visible editable nodes that
        // are contained in the result and have no children, at least one has
        // alignment value "right" and at least one does not. Otherwise return
        // false."
        if (!getActiveRange()) {
            return false;
        }
        var nodes = getAllContainedNodes(blockExtend(getActiveRange()), function(node) {
            return isEditable(node) && isVisible(node) && !node.hasChildNodes();
        });
        return nodes.some(function(node) { return getAlignmentValue(node) == "right" })
            && nodes.some(function(node) { return getAlignmentValue(node) != "right" });
    }, state: function() {
        // "Return false if the active range is null.  Otherwise, block-extend
        // the active range. Return true if there is at least one visible
        // editable node that is contained in the result and has no children,
        // and all such nodes have alignment value "right".  Otherwise return
        // false."
        if (!getActiveRange()) {
            return false;
        }
        var nodes = getAllContainedNodes(blockExtend(getActiveRange()), function(node) {
            return isEditable(node) && isVisible(node) && !node.hasChildNodes();
        });
        return nodes.length
            && nodes.every(function(node) { return getAlignmentValue(node) == "right" });
    }, value: function() {
        // "Return the empty string if the active range is null.  Otherwise,
        // block-extend the active range, and return the alignment value of the
        // first visible editable node that is contained in the result and has
        // no children. If there is no such node, return "left"."
        if (!getActiveRange()) {
            return "";
        }
        var nodes = getAllContainedNodes(blockExtend(getActiveRange()), function(node) {
            return isEditable(node) && isVisible(node) && !node.hasChildNodes();
        });
        if (nodes.length) {
            return getAlignmentValue(nodes[0]);
        } else {
            return "left";
        }
    },
};

//@}
///// The outdent command /////
//@{
commands.outdent = {
    preservesOverrides: true,
    action: function() {
        // "Let items be a list of all lis that are ancestor containers of the
        // range's start and/or end node."
        //
        // It's annoying to get this in tree order using functional stuff
        // without doing getDescendants(document), which is slow, so I do it
        // imperatively.
        var items = [];
        (function(){
            for (
                var ancestorContainer = getActiveRange().endContainer;
                ancestorContainer != getActiveRange().commonAncestorContainer;
                ancestorContainer = ancestorContainer.parentNode
            ) {
                if (isHtmlElement(ancestorContainer, "li")) {
                    items.unshift(ancestorContainer);
                }
            }
            for (
                var ancestorContainer = getActiveRange().startContainer;
                ancestorContainer;
                ancestorContainer = ancestorContainer.parentNode
            ) {
                if (isHtmlElement(ancestorContainer, "li")) {
                    items.unshift(ancestorContainer);
                }
            }
        })();

        // "For each item in items, normalize sublists of item."
        items.forEach(normalizeSublists);

        // "Block-extend the active range, and let new range be the result."
        var newRange = blockExtend(getActiveRange());

        // "Let node list be a list of nodes, initially empty."
        //
        // "For each node node contained in new range, append node to node list
        // if the last member of node list (if any) is not an ancestor of node;
        // node is editable; and either node has no editable descendants, or is
        // an ol or ul, or is an li whose parent is an ol or ul."
        var nodeList = getContainedNodes(newRange, function(node) {
            return isEditable(node)
                && (!getDescendants(node).some(isEditable)
                || isHtmlElement(node, ["ol", "ul"])
                || (isHtmlElement(node, "li") && isHtmlElement(node.parentNode, ["ol", "ul"])));
        });

        // "While node list is not empty:"
        while (nodeList.length) {
            // "While the first member of node list is an ol or ul or is not
            // the child of an ol or ul, outdent it and remove it from node
            // list."
            while (nodeList.length
            && (isHtmlElement(nodeList[0], ["OL", "UL"])
            || !isHtmlElement(nodeList[0].parentNode, ["OL", "UL"]))) {
                outdentNode(nodeList.shift());
            }

            // "If node list is empty, break from these substeps."
            if (!nodeList.length) {
                break;
            }

            // "Let sublist be a list of nodes, initially empty."
            var sublist = [];

            // "Remove the first member of node list and append it to sublist."
            sublist.push(nodeList.shift());

            // "While the first member of node list is the nextSibling of the
            // last member of sublist, and the first member of node list is not
            // an ol or ul, remove the first member of node list and append it
            // to sublist."
            while (nodeList.length
            && nodeList[0] == sublist[sublist.length - 1].nextSibling
            && !isHtmlElement(nodeList[0], ["OL", "UL"])) {
                sublist.push(nodeList.shift());
            }

            // "Record the values of sublist, and let values be the result."
            var values = recordValues(sublist);

            // "Split the parent of sublist, with new parent null."
            splitParent(sublist);

            // "Fix disallowed ancestors of each member of sublist."
            sublist.forEach(fixDisallowedAncestors);

            // "Restore the values from values."
            restoreValues(values);
        }

        // "Return true."
        return true;
    }
};

//@}

//////////////////////////////////
///// Miscellaneous commands /////
//////////////////////////////////

///// The defaultParagraphSeparator command /////
//@{
commands.defaultparagraphseparator = {
    action: function(value) {
        // "Let value be converted to ASCII lowercase. If value is then equal
        // to "p" or "div", set the context object's default single-line
        // container name to value and return true. Otherwise, return false."
        value = value.toLowerCase();
        if (value == "p" || value == "div") {
            defaultSingleLineContainerName = value;
            return true;
        }
        return false;
    }, value: function() {
        // "Return the context object's default single-line container name."
        return defaultSingleLineContainerName;
    },
};

//@}
///// The selectAll command /////
//@{
commands.selectall = {
    // Note, this ignores the whole globalRange/getActiveRange() thing and
    // works with actual selections.  Not suitable for autoimplementation.html.
    action: function() {
        // "Let target be the body element of the context object."
        var target = document.body;

        // "If target is null, let target be the context object's
        // documentElement."
        if (!target) {
            target = document.documentElement;
        }

        // "If target is null, call getSelection() on the context object, and
        // call removeAllRanges() on the result."
        if (!target) {
            getSelection().removeAllRanges();

        // "Otherwise, call getSelection() on the context object, and call
        // selectAllChildren(target) on the result."
        } else {
            getSelection().selectAllChildren(target);
        }

        // "Return true."
        return true;
    }
};

//@}
///// The styleWithCSS command /////
//@{
commands.stylewithcss = {
    action: function(value) {
        // "If value is an ASCII case-insensitive match for the string
        // "false", set the CSS styling flag to false. Otherwise, set the
        // CSS styling flag to true.  Either way, return true."
        cssStylingFlag = String(value).toLowerCase() != "false";
        return true;
    }, state: function() { return cssStylingFlag }
};

//@}
///// The useCSS command /////
//@{
commands.usecss = {
    action: function(value) {
        // "If value is an ASCII case-insensitive match for the string "false",
        // set the CSS styling flag to true. Otherwise, set the CSS styling
        // flag to false.  Either way, return true."
        cssStylingFlag = String(value).toLowerCase() == "false";
        return true;
    }
};
//@}

// Some final setup
//@{
(function() {
// Opera 11.50 doesn't implement Object.keys, so I have to make an explicit
// temporary, which means I need an extra closure to not leak the temporaries
// into the global namespace.  >:(
var commandNames = [];
for (var command in commands) {
    commandNames.push(command);
}
commandNames.forEach(function(command) {
    // "If a command does not have a relevant CSS property specified, it
    // defaults to null."
    if (!("relevantCssProperty" in commands[command])) {
        commands[command].relevantCssProperty = null;
    }

    // "If a command has inline command activated values defined but nothing
    // else defines when it is indeterminate, it is indeterminate if among
    // formattable nodes effectively contained in the active range, there is at
    // least one whose effective command value is one of the given values and
    // at least one whose effective command value is not one of the given
    // values."
    if ("inlineCommandActivatedValues" in commands[command]
    && !("indeterm" in commands[command])) {
        commands[command].indeterm = function() {
            if (!getActiveRange()) {
                return false;
            }

            var values = getAllEffectivelyContainedNodes(getActiveRange(), isFormattableNode)
                .map(function(node) { return getEffectiveCommandValue(node, command) });

            var matchingValues = values.filter(function(value) {
                return commands[command].inlineCommandActivatedValues.indexOf(value) != -1;
            });

            return matchingValues.length >= 1
                && values.length - matchingValues.length >= 1;
        };
    }

    // "If a command has inline command activated values defined, its state is
    // true if either no formattable node is effectively contained in the
    // active range, and the active range's start node's effective command
    // value is one of the given values; or if there is at least one
    // formattable node effectively contained in the active range, and all of
    // them have an effective command value equal to one of the given values."
    if ("inlineCommandActivatedValues" in commands[command]) {
        commands[command].state = function() {
            if (!getActiveRange()) {
                return false;
            }

            var nodes = getAllEffectivelyContainedNodes(getActiveRange(), isFormattableNode);

            if (nodes.length == 0) {
                return commands[command].inlineCommandActivatedValues
                    .indexOf(getEffectiveCommandValue(getActiveRange().startContainer, command)) != -1;
            } else {
                return nodes.every(function(node) {
                    return commands[command].inlineCommandActivatedValues
                        .indexOf(getEffectiveCommandValue(node, command)) != -1;
                });
            }
        };
    }

    // "If a command is a standard inline value command, it is indeterminate if
    // among formattable nodes that are effectively contained in the active
    // range, there are two that have distinct effective command values. Its
    // value is the effective command value of the first formattable node that
    // is effectively contained in the active range; or if there is no such
    // node, the effective command value of the active range's start node; or
    // if that is null, the empty string."
    if ("standardInlineValueCommand" in commands[command]) {
        commands[command].indeterm = function() {
            if (!getActiveRange()) {
                return false;
            }

            var values = getAllEffectivelyContainedNodes(getActiveRange())
                .filter(isFormattableNode)
                .map(function(node) { return getEffectiveCommandValue(node, command) });
            for (var i = 1; i < values.length; i++) {
                if (values[i] != values[i - 1]) {
                    return true;
                }
            }
            return false;
        };

        commands[command].value = function() {
            if (!getActiveRange()) {
                return "";
            }

            var refNode = getAllEffectivelyContainedNodes(getActiveRange(), isFormattableNode)[0];

            if (typeof refNode == "undefined") {
                refNode = getActiveRange().startContainer;
            }

            var ret = getEffectiveCommandValue(refNode, command);
            if (ret === null) {
                return "";
            }
            return ret;
        };
    }

    // "If a command preserves overrides, then before taking its action, the
    // user agent must record current overrides. After taking the action, if
    // the active range is collapsed, it must restore states and values from
    // the recorded list."
    if ("preservesOverrides" in commands[command]) {
        var oldAction = commands[command].action;

        commands[command].action = function(value) {
            var overrides = recordCurrentOverrides();
            var ret = oldAction(value);
            if (getActiveRange().collapsed) {
                restoreStatesAndValues(overrides);
            }
            return ret;
        };
    }
});
})();
//@}

// vim: foldmarker=@{,@} foldmethod=marker
