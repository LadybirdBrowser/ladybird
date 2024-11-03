'use strict';

/**
 * Create test that a CSS property computes to the expected value.
 * The document element #target is used to perform the test.
 *
 * @param {string} property  The name of the CSS property being tested.
 * @param {string} value     A specified value for the property.
 * @param {string|array} serializedValue  The expected serialized value,
 *                                 or an array of permitted serializations.
 *                                 If omitted, defaults to the specified value.
 * @param {object} options  Additional test information, such as a custom
 *                          comparison function required for color tests.
 *                          comparisonFunction is a function that takes two
 *                          arguments, actual and expected and contains asserts.
 */
function test_valid_value(property, value, serializedValue, options = {}) {
    if (arguments.length < 3)
        serializedValue = value;

    var stringifiedValue = JSON.stringify(value);

    test(function(){
        var div = document.getElementById('target') || document.createElement('div');
        div.style[property] = "";
        div.style[property] = value;
        var readValue = div.style.getPropertyValue(property);
        assert_not_equals(readValue, "", "property should be set");
        if (options.comparisonFunction)
            options.comparisonFunction(readValue, serializedValue);
        else if (Array.isArray(serializedValue))
            assert_in_array(readValue, serializedValue, "serialization should be sound");
        else
            assert_equals(readValue, serializedValue, "serialization should be canonical");

        div.style[property] = readValue;
        assert_equals(div.style.getPropertyValue(property), readValue, "serialization should round-trip");

    }, "e.style['" + property + "'] = " + stringifiedValue + " should set the property value");
}

function test_invalid_value(property, value) {
    var stringifiedValue = JSON.stringify(value);

    test(function(){
        var div = document.getElementById('target') || document.createElement('div');
        div.style[property] = "";
        div.style[property] = value;
        assert_equals(div.style.getPropertyValue(property), "");
    }, "e.style['" + property + "'] = " + stringifiedValue + " should not set the property value");
}

function test_valid_forgiving_selector(selector) {
  test_valid_selector(selector, selector, { onlyWhenForgiving: true });
}

// serializedSelector can be the expected serialization of selector,
// or an array of permitted serializations,
// or omitted if value should serialize as selector.
function test_valid_selector(selector, serializedSelector, options) {
    if (arguments.length < 2)
        serializedSelector = selector;

    const stringifiedSelector = JSON.stringify(selector);

    test(function(){
        document.querySelector(selector);
        assert_true(true, stringifiedSelector + " should not throw in querySelector");

        const style = document.createElement("style");
        document.head.append(style);
        const {sheet} = style;
        document.head.removeChild(style);
        const {cssRules} = sheet;

        assert_equals(cssRules.length, 0, "Sheet should have no rule");
        sheet.insertRule(selector + "{}");
        assert_equals(cssRules.length, 1, "Sheet should have 1 rule");

        const readSelector = cssRules[0].selectorText;
        if (Array.isArray(serializedSelector))
            assert_in_array(readSelector, serializedSelector, "serialization should be sound");
        else
            assert_equals(readSelector, serializedSelector, "serialization should be canonical");

        sheet.deleteRule(0);
        assert_equals(cssRules.length, 0, "Sheet should have no rule");
        sheet.insertRule(readSelector + "{}");
        assert_equals(cssRules.length, 1, "Sheet should have 1 rule");

        assert_equals(cssRules[0].selectorText, readSelector, "serialization should round-trip");

        let supports = !options?.onlyWhenForgiving;
        assert_equals(CSS.supports(`selector(${selector})`), supports, "CSS.supports() reports the expected value");
    }, stringifiedSelector + " should be a valid selector");
}

function test_invalid_selector(selector) {
    const stringifiedSelector = JSON.stringify(selector);

    test(function(){
        assert_throws_dom(
          DOMException.SYNTAX_ERR,
          () => document.querySelector(selector),
          stringifiedSelector + " should throw in querySelector");

        const style = document.createElement("style");
        document.head.append(style);
        const {sheet} = style;
        document.head.removeChild(style);

        assert_throws_dom(
          DOMException.SYNTAX_ERR,
          () => sheet.insertRule(selector + "{}"),
          stringifiedSelector + " should throw in insertRule");
    }, stringifiedSelector + " should be an invalid selector");
}

// serialized can be the expected serialization of rule, or an array of
// permitted serializations, or omitted if value should serialize as rule.
function test_valid_rule(rule, serialized) {
    if (serialized === undefined)
        serialized = rule;

    test(function(){
        const style = document.createElement("style");
        document.head.append(style);
        const {sheet} = style;
        document.head.removeChild(style);
        const {cssRules} = sheet;

        assert_equals(cssRules.length, 0, "Sheet should have no rules");
        sheet.insertRule(rule);
        assert_equals(cssRules.length, 1, "Sheet should have 1 rule");

        const serialization = cssRules[0].cssText;
        if (Array.isArray(serialized))
            assert_in_array(serialization, serialized, "serialization should be sound");
        else
            assert_equals(serialization, serialized, "serialization should be canonical");

        sheet.deleteRule(0);
        assert_equals(cssRules.length, 0, "Sheet should have no rule");
        sheet.insertRule(serialization);
        assert_equals(cssRules.length, 1, "Sheet should have 1 rule");

        assert_equals(cssRules[0].cssText, serialization, "serialization should round-trip");
    }, rule + " should be a valid rule");
}

function test_invalid_rule(rule) {
    test(function(){
        const style = document.createElement("style");
        document.head.append(style);
        const {sheet} = style;
        document.head.removeChild(style);

        assert_throws_dom(
          DOMException.SYNTAX_ERR,
          () => sheet.insertRule(rule),
          rule + " should throw in insertRule");
    }, rule + " should be an invalid rule");
}

function _set_style(rule) {
    const style = document.createElement('style');
    style.innerText = rule;
    document.head.append(style);
    const { sheet } = style;
    document.head.removeChild(style);
    return sheet;
}

function test_keyframes_name_valid(keyframes_name) {
    test(t => {
        const sheet = _set_style(`@keyframes ${keyframes_name} {}`);
        assert_equals(sheet.cssRules.length, 1);
    }, `valid: @keyframes ${keyframes_name} { }`);
}

function test_keyframes_name_invalid(keyframes_name) {
    test(t => {
        const sheet = _set_style(`@keyframes ${keyframes_name} {}`);
        assert_equals(sheet.cssRules.length, 0);
    }, `invalid: @keyframes ${keyframes_name} { }`);
}
