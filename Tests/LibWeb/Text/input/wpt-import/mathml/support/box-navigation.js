function IsInFlow(element) {
    var style = window.getComputedStyle(element);
    return style.getPropertyValue("display") !== "none" &&
        style.getPropertyValue("position") !== "absolute" &&
        style.getPropertyValue("position") !== "fixed";
}

function firstInFlowChild(element) {
    var child = element.firstElementChild;
    if (!child || IsInFlow(child))
        return child;
    return nextInFlowSibling(child);
}

function nextInFlowSibling(element) {
    var child = element;
    do {
        child = child.nextElementSibling;
    } while (child && !IsInFlow(child));
    return child;
}

function previousInFlowSibling(element) {
    var child = element;
    do {
        child = child.previousElementSibling;
    } while (child && !IsInFlow(child));
    return child;
}
