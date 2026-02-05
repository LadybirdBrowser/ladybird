'use strict';

/*
Provides functions to help test that two numeric values are equivalent.
These *do not* rely on you predicting what one value will serialize to;
instead, they set and serialize *both* values,
and just ensure that they serialize to the same thing.

They rely on a #target element existing in the document,
as this might rely on layout to resolve styles,
and so it needs to be in the document.

Three main functions are defined, with the same signatures:
test_math_used() (for testing used values),
test_math_computed() (for testing computed values),
and test_math_specified() (for testing specified values).
Signature for all is:

test_math_X(
    testString, // A numeric value; required.
    expectedString, // A hopefully-equivalent numeric value; required.
    { // all of these are optional
        type, // "number", "length", etc. See impl for full list. Defaults to "length".
        msg, // The message to display for the test; autogenned if not provided.
        msgExtra, // Extra info to put after the auto-genned message.
        prop, // If you want to override the automatic choice of tested property.
        extraStyle, // Styles that need to be set at the same time to properly test the value.
        approx, // The epsilon in order to compare numeric-ish values.
                // Note that it'd use parseFloat in order to extract the
                // values, so they can drop units or what not.
    }
);


Additionally, five specialized functions are provided
to test that a given value is ±∞, ±0, or NaN:

* test_plus_infinity(testString)
* test_minus_infinity(testString)
* test_plus_zero(testString)
* test_minus_zero(testString)
* test_nan(testString)

*/



function test_math_used(testString, expectedString, {approx, msg, msgExtra, type, prop, extraStyle={}}={}) {
    if(type === undefined) type = "length";
    if(!prop) {
        switch(type) {
            case "number":     prop = "scale"; break;
            case "integer":    prop = "z-index"; extraStyle.position="absolute"; break;
            case "length":     prop = "margin-left"; break;
            case "angle":      prop = "rotate"; break;
            case "time":       prop = "transition-delay"; break;
            case "resolution": prop = "image-resolution"; break;
            case "flex":       prop = "grid-template-rows"; break;
            default: throw Exception(`Value type '${type}' isn't capable of math.`);
        }

    }
    _test_math({stage:'used', testString, expectedString, type, approx, msg, msgExtra, prop, extraStyle});
}

function test_math_computed(testString, expectedString, {approx, msg, msgExtra, type, prop, extraStyle={}}={}) {
    if(type === undefined) type = "length";
    if(!prop) {
        switch(type) {
            case "number":     prop = "scale"; break;
            case "integer":    prop = "z-index"; extraStyle.position="absolute"; break;
            case "length":     prop = "flex-basis"; break;
            case "angle":      prop = "rotate"; break;
            case "time":       prop = "transition-delay"; break;
            case "resolution": prop = "image-resolution"; break;
            case "flex":       prop = "grid-template-rows"; break;
            default: throw Exception(`Value type '${type}' isn't capable of math.`);
        }

    }
    _test_math({stage:'computed', testString, expectedString, type, approx, msg, msgExtra, prop, extraStyle});
}

function test_math_specified(testString, expectedString, {approx, msg, msgExtra, type, prop, extraStyle={}}={}) {
    if(type === undefined) type = "length";
    const stage = "specified";
    if(!prop) {
        switch(type) {
            case "number":     prop = "scale"; break;
            case "integer":    prop = "z-index"; extraStyle.position="absolute"; break;
            case "length":     prop = "flex-basis"; break;
            case "angle":      prop = "rotate"; break;
            case "time":       prop = "transition-delay"; break;
            case "resolution": prop = "image-resolution"; break;
            case "flex":       prop = "grid-template-rows"; break;
            default: throw Exception(`Value type '${type}' isn't capable of math.`);
        }

    }
    // Find the test element
    const testEl = document.getElementById('target');
    if(testEl == null) throw "Couldn't find #target element to run tests on.";
    // Then reset its styles
    testEl.style = "";
    for(const p in extraStyle) {
        testEl.style[p] = extraStyle[p];
    }
    if(!msg) {
        msg = `${testString} should be ${stage}-value-equivalent to ${expectedString}`;
        if(msgExtra) msg += "; " + msgExtra;
    }
    let t = testString;
    let e = expectedString;
    test(()=>{
        testEl.style[prop] = '';
        testEl.style[prop] = t;
        const usedValue = testEl.style[prop];
        assert_not_equals(usedValue, '', `${testString} isn't valid in '${prop}'; got the default value instead.`);
        testEl.style[prop] = '';
        testEl.style[prop] = e;
        const expectedValue = testEl.style[prop];
        assert_not_equals(expectedValue, '', `${expectedString} isn't valid in '${prop}'; got the default value instead.`)
        if (approx) {
            let extractValue = function(value) {
                if (value.startsWith("calc(")) {
                    value = value.slice("calc(".length, -1);
                }
                return parseFloat(value);
            };
            let parsedUsed = extractValue(usedValue);
            let parsedExpected = extractValue(expectedValue);
            assert_approx_equals(parsedUsed, parsedExpected, approx, `${testString} and ${expectedString} ${approx} serialize to the same thing in ${stage} values.`);
        } else {
            assert_equals(usedValue, expectedValue, `${testString} and ${expectedString} serialize to the same thing in ${stage} values.`);
        }
    }, msg);
}

/*
All of these expect the testString to evaluate to a <number>.
*/
function test_plus_infinity(testString) {
    test_math_used(testString, "calc(infinity)", {type:"number"});
}
function test_minus_infinity(testString) {
    test_math_used(testString, "calc(-infinity)", {type:"number"});
}
function test_plus_zero(testString) {
    test_math_used(`calc(1 / ${testString})`, "calc(infinity)", {type:"number"});
}
function test_minus_zero(testString) {
    test_math_used(`calc(1 / ${testString})`, "calc(-infinity)", {type:"number"});
}
function test_nan(testString) {
    // Make sure that it's NaN, not an infinity,
    // by making sure that it's the same value both pos and neg.
    test_math_used(testString, "calc(NaN)", {type:"number"});
    test_math_used(`calc(-1 * ${testString})`, "calc(NaN)", {type:"number"});
}


function _test_math({stage, testEl, testString, expectedString, type, approx, msg, msgExtra, prop, extraStyle}={}) {
    // Find the test element
    if(!testEl) testEl = document.getElementById('target');
    if(testEl == null) throw "Couldn't find #target element to run tests on.";
    // Then reset its styles
    testEl.style = "";
    for(const p in extraStyle) {
        testEl.style[p] = extraStyle[p];
    }
    if(!msg) {
        msg = `${testString} should be ${stage}-value-equivalent to ${expectedString}`;
        if(msgExtra) msg += "; " + msgExtra;
    }
    let t = testString;
    let e = expectedString;
    test(()=>{
        testEl.style[prop] = '';
        const defaultValue = getComputedStyle(testEl)[prop];
        testEl.style[prop] = t;
        const usedValue = getComputedStyle(testEl)[prop];
        assert_not_equals(usedValue, defaultValue, `${testString} isn't valid in '${prop}'; got the default value instead.`);
        testEl.style[prop] = '';
        testEl.style[prop] = e;
        const expectedValue = getComputedStyle(testEl)[prop];
        assert_not_equals(expectedValue, defaultValue, `${expectedString} isn't valid in '${prop}'; got the default value instead.`)
        if (approx) {
            let extractValue = function(value) {
                return parseFloat(value);
            };
            let parsedUsed = extractValue(usedValue);
            let parsedExpected = extractValue(expectedValue);
            assert_approx_equals(parsedUsed, parsedExpected, approx, `${testString} and ${expectedString} ${approx} serialize to the same thing in ${stage} values.`);
        } else {
            assert_equals(usedValue, expectedValue, `${testString} and ${expectedString} serialize to the same thing in ${stage} values.`);
        }
    }, msg);
}
