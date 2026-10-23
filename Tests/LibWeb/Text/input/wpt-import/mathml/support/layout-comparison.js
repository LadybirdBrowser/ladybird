function getWritingMode(element, reference) {
    var style = window.getComputedStyle(reference);
    if (style.getPropertyValue("writing-mode") !== "horizontal-tb" ||
        style.getPropertyValue("direction") !== "ltr")
        throw "Reference should have writing mode horizontal-tb and ltr";

    style = window.getComputedStyle(element);
    var param =  {
        rtl: style.getPropertyValue("direction") === "rtl",
        mode: style.getPropertyValue("writing-mode")
    };

    return param;
}

function compareSize(element, reference, epsilon) {
    var param = getWritingMode(element, reference);
    var elementBox = element.getBoundingClientRect();
    var referenceBox = reference.getBoundingClientRect();

    switch(param.mode) {
    case "horizontal-tb":
        assert_approx_equals(elementBox.width, referenceBox.width, epsilon,
                             "inline size");
        assert_approx_equals(elementBox.height, referenceBox.height, epsilon,
                             "block size");
        break;
    case "vertical-lr":
    case "vertical-rl":
        assert_approx_equals(elementBox.width, referenceBox.height, epsilon,
                             "inline size");
        assert_approx_equals(elementBox.height, referenceBox.width, epsilon,
                             "block size");
        break;
    default:
        throw "compareSize: Unrecognized writing-mode value";
    }
}

function childrenHaveEmptyBoundingClientRects(element) {
    Array.from(element.children).forEach(child => {
        var childBox = child.getBoundingClientRect();
        assert_true(childBox.left == 0 && childBox.right == 0 && childBox.top == 0 && childBox.bottom == 0);
    })
}

function participateToParentLayout(child) {
    var style = window.getComputedStyle(child);
    return style.getPropertyValue("display") !== "none" &&
        style.getPropertyValue("position") !== "absolute" &&
        style.getPropertyValue("position") !== "fixed";
}

function childrenParticipatingToLayout(element) {
    var children = [];
    Array.from(element.children).forEach(child => {
        if (participateToParentLayout(child))
            children.push(child);
    })
    return children;
}

function compareLayout(element, reference, epsilon) {
    // Compare sizes of elements and children.
    var param = getWritingMode(element, reference);

    compareSize(element, reference, epsilon);
    var elementBox = element.getBoundingClientRect();
    var referenceBox = reference.getBoundingClientRect();

    var elementChildren = childrenParticipatingToLayout(element);
    var referenceChildren = childrenParticipatingToLayout(reference);
    if (elementChildren.length != referenceChildren.length)
        throw "Reference should have the same number of children participating to layout."

    for (var i = 0; i < elementChildren.length; i++) {
        compareSize(elementChildren[i], referenceChildren[i], epsilon);

        var childBox = elementChildren[i].getBoundingClientRect();
        var referenceChildBox = referenceChildren[i].getBoundingClientRect();

        switch(param.mode) {
        case "horizontal-tb":
            assert_approx_equals(param.rtl ?
                                 elementBox.right - childBox.right :
                                 childBox.left - elementBox.left,
                                 referenceChildBox.left - referenceBox.left,
                                 epsilon,
                                 `inline position (child ${i})`);
            assert_approx_equals(childBox.top - elementBox.top,
                                 referenceChildBox.top - referenceBox.top,
                                 epsilon,
                                 `block position (child ${i})`);
            break;
        case "vertical-lr":
        case "vertical-rl":
            assert_approx_equals(param.rtl ?
                                 elementBox.bottom - childBox.bottom :
                                 childBox.top - elementBox.top,
                                 referenceChildBox.left - referenceBox.left,
                                 epsilon,
                                 `inline position (child ${i})`);
            assert_approx_equals(elementBox.right - childBox.right,
                                 referenceChildBox.top - referenceBox.top,
                                 epsilon,
                                 `block position (child ${i})`);
            break;
        default:
            throw "compareLayout: Unrecognized writing-mode value";
        }
    }
}
