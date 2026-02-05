function evaluateStyleChange(element, phase, expectedProperty, expectedResult) {
    element.className += " " + phase;
    element.setAttribute(expectedProperty, expectedResult);
    checkLayout("." + phase, false);
}
function evaluateStyleChangeMultiple(phase, expectedResult) {
    for (var item in expectedResult) {
        var element = document.getElementById(item);
        element.className += " " + phase;
        for (var key in expectedResult[item])
            element.setAttribute(key, expectedResult[item][key]);
    }
    checkLayout("." + phase, false);
}
