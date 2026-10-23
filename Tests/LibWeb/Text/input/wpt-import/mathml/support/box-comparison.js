function spaceBetween(childBox, parentBox) {
    return {
        left: childBox.left - parentBox.left,
        right: parentBox.right - childBox.right,
        top: childBox.top - parentBox.top,
        bottom: parentBox.bottom - childBox.bottom
    };
}

function measureSpaceAround(id) {
    var mrow = document.getElementById(id);
    var mrowBox = mrow.getBoundingClientRect();
    var parentBox = mrow.parentNode.getBoundingClientRect();
    var childBox = mrow.firstElementChild.getBoundingClientRect();
    return spaceBetween(childBox, parentBox);
}

function compareSpaceWithAndWithoutStyle(tag, style, parentStyle, direction) {
    if (!FragmentHelper.isValidChildOfMrow(tag) ||
        FragmentHelper.isEmpty(tag))
        throw `Invalid argument: ${tag}`;

    if (!direction)
      direction = "ltr";
    document.body.insertAdjacentHTML("beforeend", `<div style="position: absolute;">\
<div style="display: inline-block"><math display="block"><mrow dir="${direction}">${MathMLFragments[tag]}</mrow></math></div>\
<div style="display: inline-block"><math display="block"><mrow dir="${direction}">${MathMLFragments[tag]}</mrow></math></div>\
</div>`);
    var div = document.body.lastElementChild;

    var styleDiv = div.firstElementChild;
    var styleMath = styleDiv.firstElementChild;
    var styleParent = styleMath.firstElementChild;
    if (parentStyle)
        styleParent.setAttribute("style", parentStyle);
    var styleElement = FragmentHelper.element(styleMath);
    styleElement.setAttribute("style", style);
    var styleChild = FragmentHelper.forceNonEmptyElement(styleElement);
    FragmentHelper.forceNonEmptyDescendants(styleElement);
    var styleMathBox = styleMath.getBoundingClientRect();
    var styleElementBox = styleElement.getBoundingClientRect();
    var styleChildBox = styleChild.getBoundingClientRect();
    var styleSpace = spaceBetween(styleChildBox, styleMathBox);

    var noStyleDiv = div.lastElementChild;
    var noStyleMath = noStyleDiv.firstElementChild;
    var noStyleElement = FragmentHelper.element(noStyleMath);
    var noStyleChild = FragmentHelper.forceNonEmptyElement(noStyleElement);
    FragmentHelper.forceNonEmptyDescendants(noStyleElement);
    var noStyleMathBox = noStyleMath.getBoundingClientRect();
    var noStyleElementBox = noStyleElement.getBoundingClientRect();
    var noStyleChildBox = noStyleChild.getBoundingClientRect();
    var noStyleSpace = spaceBetween(noStyleChildBox, noStyleMathBox);

    var preferredWidthDelta =
        styleDiv.getBoundingClientRect().width -
        noStyleDiv.getBoundingClientRect().width;

    div.style = "display: none;"; // Hide the div after measurement.

    return {
        preferred_width_delta: preferredWidthDelta,
        left_delta: styleSpace.left - noStyleSpace.left,
        right_delta: styleSpace.right - noStyleSpace.right,
        top_delta: styleSpace.top - noStyleSpace.top,
        bottom_delta: styleSpace.bottom - noStyleSpace.bottom,
        element_width_delta: styleElementBox.width - noStyleElementBox.width,
        element_height_delta: styleElementBox.height - noStyleElementBox.height
    };
}

function compareSizeWithAndWithoutStyle(tag, style) {
    if (!FragmentHelper.isValidChildOfMrow(tag))
        throw `Invalid argument: ${tag}`;

    // FIXME <mrow> only needed as workaround for https://bugzilla.mozilla.org/show_bug.cgi?id=1658135
    document.body.insertAdjacentHTML("beforeend", `<div style="position: absolute;">\
<div style="display: inline-block"><math><mrow>${MathMLFragments[tag]}</mrow></math></div>\
<div style="display: inline-block"><math><mrow>${MathMLFragments[tag]}</mrow></math></div>\
</div>`);
    var div = document.body.lastElementChild;

    var styleDiv = div.firstElementChild;
    var styleParent = styleDiv.firstElementChild.firstElementChild;
    var styleElement = FragmentHelper.element(styleParent);
    styleElement.setAttribute("style", style);
    var styleParentBox = styleParent.getBoundingClientRect();
    var styleElementBox = styleElement.getBoundingClientRect();

    var noStyleDiv = div.lastElementChild;
    var noStyleParent = noStyleDiv.firstElementChild.firstElementChild;
    var noStyleElement = FragmentHelper.element(noStyleParent);
    var noStyleParentBox = noStyleParent.getBoundingClientRect();
    var noStyleElementBox = noStyleElement.getBoundingClientRect();

    var preferredWidthDelta =
        styleDiv.getBoundingClientRect().width -
        noStyleDiv.getBoundingClientRect().width;

    div.style = "display: none;"; // Hide the div after measurement.

    return {
        preferred_width_delta: preferredWidthDelta,
        width_delta: styleParentBox.width - noStyleParentBox.width,
        height_delta: styleParentBox.height - noStyleParentBox.height,
        element_width_delta: styleElementBox.width - noStyleElementBox.width,
        element_height_delta: styleElementBox.height - noStyleElementBox.height
    };
};
