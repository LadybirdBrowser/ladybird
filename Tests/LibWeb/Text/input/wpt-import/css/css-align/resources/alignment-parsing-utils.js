var selfPositionClasses = {"Start":"start", "End":"end", "SelfStart":"self-start", "SelfEnd":"self-end", "Center":"center", "FlexStart":"flex-start", "FlexEnd":"flex-end"};
var contentPositionClasses = {"Start":"start", "End":"end", "Center":"center", "FlexStart":"flex-start", "FlexEnd":"flex-end"};
var distributionClasses = {"Stretch":"stretch", "SpaceAround":"space-around", "SpaceBetween":"space-between", "SpaceEvenly":"space-evenly"};
var baselineClasses = {"Baseline":"baseline", "FirstBaseline":"first baseline", "LastBaseline":"last baseline"};
var overflowClasses = {"SafeFlexEnd":"safe flex-end", "UnsafeEnd":"unsafe end", "SafeEnd":"safe end", "UnsafeFlexStart":"unsafe flex-start", "SafeCenter":"safe center"};
var legacyClasses = {"LegacyLeft":"legacy left", "LegacyCenter":"legacy center", "LegacyRight":"legacy right"};

var invalidPositionValues = ["auto safe", "auto left", "normal unsafe", "normal stretch", "baseline normal",
                             "baseline center", "first baseline center", "last baseline center", "baseline last",
                             "baseline first", "stretch unsafe", "stretch right", "unsafe unsafe", "unsafe safe",
                             "center start", "unsafe stretch", "safe stretch", "baseline safe", "unsafe baseline",
                             "unsafe safe left", "unsafe left safe", "left safe unsafe safe", "start safe", "safe"];
var invalidLegacyValues = ["legacy start", "legacy end", "legacy right unsafe", "legacy auto", "legacy stretch",
                           "legacy left right"];
var invalidDistributionValues = ["space-between left", "space-around center", "space-evenly right",
                                 "stretch safe start", "space-around unsafe", "space-evenly safe flex-start",
                                 "space-between safe", "space-between stretch", "stretch start",
                                 "stretch baseline", "first baseline space-around"];

function checkPlaceShorhand(shorthand, shorthandValue, alignValue, justifyValue)
{
    var div = document.createElement("div");
    div.style[shorthand] = shorthandValue;
    document.body.appendChild(div);

    if (alignValue === "first baseline")
        alignValue = "baseline";
    if (justifyValue === "first baseline")
        justifyValue = "baseline";
    if (justifyValue === "")
        justifyValue = alignValue;

    let specifiedValue = (alignValue + " " + justifyValue).trim();
    if (alignValue === justifyValue)
        specifiedValue = alignValue;

    var resolvedValue = getComputedStyle(div).getPropertyValue(shorthand);
    var expectedResolvedValue = (alignValue + " " + justifyValue).trim();
    if (alignValue === justifyValue)
        expectedResolvedValue = alignValue;

    assert_equals(div.style[shorthand], specifiedValue, shorthandValue + " specified value");
    // FIXME: We need https://github.com/w3c/csswg-drafts/issues/1041 to clarify which
    // value is expected for the shorthand's 'resolved value".
    assert_in_array(resolvedValue, ["", expectedResolvedValue], shorthand + " resolved value");
}

function checkPlaceShorhandLonghands(shorthand, alignLonghand, justifyLonghand, alignValue, justifyValue = "")
{
    var div = document.createElement("div");
    div.setAttribute("style", shorthand + ": " + alignValue + " " + justifyValue);
    document.body.appendChild(div);
    if (alignValue === "first baseline")
        alignValue = "baseline";
    if (justifyValue === "first baseline")
        justifyValue = "baseline";
    if (justifyValue === "")
        justifyValue = alignValue;
    assert_equals(div.style[alignLonghand],
                  alignValue, alignLonghand + " expanded value");
    assert_equals(div.style[justifyLonghand],
                  justifyValue, justifyLonghand + " expanded value");
}

function checkPlaceShorthandInvalidValues(shorthand, alignLonghand, justifyLonghand, value)
{
    var div = document.createElement("div");
    var css = alignLonghand + ": start; " + justifyLonghand + ": end;" + shorthand + ": " + value;
    div.setAttribute("style", css);
    document.body.appendChild(div);
    assert_equals(div.style[alignLonghand],
                  "start", alignLonghand + " expanded value");
    assert_equals(div.style[justifyLonghand],
                  "end", justifyLonghand + " expanded value");
}

function checkValues(element, property, propertyID, value, computedValue)
{
    window.element = element;
    var elementID = element.id || "element";
    assert_equals(eval('element.style.' + property), value, propertyID + ' specified value is not what it should.');
    assert_equals(eval("window.getComputedStyle(" + elementID + ", '').getPropertyValue('" + propertyID + "')"), computedValue, propertyID + " computed style is not what is should.");
}

function checkBadValues(element, property, propertyID, value)
{
    var elementID = element.id || "element";
    element.style[property] = "";
    var initialValue = eval("window.getComputedStyle(" + elementID + " , '').getPropertyValue('" + propertyID + "')");
    element.style[property] = value;
    checkValues(element, property, propertyID, "", initialValue);
}

function checkInitialValues(element, property, propertyID, value, initial)
{
    element.style[property] = value;
    checkValues(element, property, propertyID, value, value);
    element.style[property] = "initial";
    checkValues(element, property, propertyID, "initial", initial);
}

function checkInheritValues(property, propertyID, value)
{
    var parentElement = document.createElement("div");
    document.body.appendChild(parentElement);
    parentElement.style[property] = value;
    checkValues(parentElement, property, propertyID, value, value);

    var element = document.createElement("div");
    parentElement.appendChild(element);
    element.style[property] = "inherit";
    checkValues(element, property, propertyID, "inherit", value);
}

function checkLegacyValues(property, propertyID, value)
{
    var parentElement = document.createElement("div");
    document.body.appendChild(parentElement);
    parentElement.style[property] = value;
    checkValues(parentElement, property, propertyID, value, value);

    var element = document.createElement("div");
    parentElement.appendChild(element);
    checkValues(element, property, propertyID, "", value);
}

function checkSupportedValues(elementID, property)
{
    var value = eval("window.getComputedStyle(" + elementID + " , '').getPropertyValue('" + property + "')");
    var value1 = eval("window.getComputedStyle(" + elementID + " , '')");
    shouldBeTrue("CSS.supports('" + property + "', '" + value + "')");
}
