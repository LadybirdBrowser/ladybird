/**
 * Replaces the current selection (if any) with a new range, after
 * it’s configured by the given function.
 *
 * See also: selectNodeContents
 * Example:
 *
 * selectRangeWith(range => {
 *     range.selectNodeContents(foo);
 *     range.setStart(foo.childNodes[0], 3);
 *     range.setEnd(foo.childNodes[0], 5);
 * });
 */
function selectRangeWith(fun) {
    const selection = getSelection();

    // Deselect any ranges that happen to be selected, to prevent the
    // Selection#addRange call from ignoring our new range (see
    // <https://www.chromestatus.com/feature/6680566019653632> for
    // more details).
    selection.removeAllRanges();

    // Create and configure a range.
    const range = document.createRange();
    fun(range);

    // Select our new range.
    selection.addRange(range);
}

/**
 * Replaces the current selection (if any) with a new range, spanning
 * the contents of the given node.
 */
function selectNodeContents(node) {
    const previousActive = document.activeElement;

    selectRangeWith(range => range.selectNodeContents(node));

    // If the selection update causes the node or an ancestor to be
    // focused (Chromium 80+), unfocus it, to avoid any focus-related
    // styling such as outlines.
    if (document.activeElement != previousActive) {
        document.activeElement.blur();
    }
}

/**
 * Tries to convince a UA with lazy spellcheck to check and highlight
 * the contents of the given nodes (form fields or @contenteditables).
 *
 * Each node is focused then immediately unfocused. Both focus and
 * selection can be used for this purpose, but only focus works for
 * @contenteditables.
 */
function trySpellcheck(...nodes) {
    // This is inherently a flaky test risk, but Chromium (as of 87)
    // seems to cancel spellcheck on a node if it wasn’t the last one
    // focused for “long enough” (though immediate unfocus is ok).
    // Using requestAnimationFrame or setInterval(0) are usually not
    // long enough (see <https://bucket.daz.cat/work/igalia/0/0.html>
    // under “trySpellcheck strategy” for an example).
    const interval = setInterval(() => {
        if (nodes.length > 0) {
            const node = nodes.shift();
            node.focus();
            node.blur();
        } else {
            clearInterval(interval);
        }
    }, 250);
}

function createRangeForTextOnly(element, start, end) {
    const textNode = element.firstChild;
    if (element.childNodes.length != 1 || textNode.nodeName != '#text') {
        throw new Error('element must contain a single #text node only');
    }
    const range = document.createRange();
    range.setStart(textNode, start);
    range.setEnd(textNode, end);
    return range;
}
