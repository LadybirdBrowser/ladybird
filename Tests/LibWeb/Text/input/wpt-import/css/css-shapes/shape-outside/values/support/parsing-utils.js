var ParsingUtils = (function() {
function testInlineStyle(value, expected) {
    var div = document.createElement('div');
    div.style.setProperty('shape-outside', value);
    var actual = div.style.getPropertyValue('shape-outside');
    assert_equals(actual, expected);
}

function testComputedStyle(value, expected) {
    var div = document.createElement('div');
    div.style.setProperty('shape-outside', value);
    document.body.appendChild(div);
    var style = getComputedStyle(div);
    var actual = style.getPropertyValue('shape-outside');
    actual = roundResultStr(actual);
    document.body.removeChild(div);

    // Some of the tests in this suite have either/or expected results
    // so this check allows for testing that at least one of them passes.
    // Description of the 2 expecteds is below near calcTestValues.
    if(Object.prototype.toString.call( expected ) === '[object Array]' && expected.length == 2) {
        assert_in_array(actual, expected);
    } else {
        assert_equals(actual, typeof expected !== 'undefined' ? expected : value);
    }
}

function testShapeMarginInlineStyle(value, expected) {
    var div = document.createElement('div');
    div.style.setProperty('shape-outside', "border-box inset(10px)");
    div.style.setProperty('shape-margin', value);
    var actual = div.style.getPropertyValue('shape-margin');
    actual = roundResultStr(actual);
    expected = roundResultStr(expected);
    assert_equals(actual, expected);
}

function testShapeMarginComputedStyle(value, expected) {

    var outerDiv = document.createElement('div');
    outerDiv.style.setProperty('width', '100px');

    var innerDiv = document.createElement('div');
    innerDiv.style.setProperty('shape-outside', "border-box inset(10px)");
    innerDiv.style.setProperty('shape-margin', value);

    outerDiv.appendChild(innerDiv);
    document.body.appendChild(outerDiv);

    var style = getComputedStyle(innerDiv);
    var actual = style.getPropertyValue('shape-margin');

    assert_not_equals(actual, null);
    if(actual.indexOf('calc') == -1 )
        actual = roundResultStr(actual);
    document.body.removeChild(outerDiv);

    // See comment above about multiple expected results
    if(Object.prototype.toString.call( expected ) === '[object Array]' && expected.length == 2) {
        assert_in_array(actual, expected);
    } else {
        assert_equals(actual, !expected ? '0px' : expected);
    }
}

function testShapeThresholdInlineStyle(value, expected) {
    var div = document.createElement('div');
    div.style.setProperty('shape-outside', 'url(someimage.png)');
    div.style.setProperty('shape-image-threshold', value);
    var actual = div.style.getPropertyValue('shape-image-threshold');
    assert_equals(actual, expected);
}

function testShapeThresholdComputedStyle(value, expected) {

    var div = document.createElement('div');
    div.style.setProperty('shape-outside', 'url(someimage.png)');
    div.style.setProperty('shape-image-threshold', value);
    document.body.appendChild(div);

    var style = getComputedStyle(div);
    var actual = style.getPropertyValue('shape-image-threshold');

    assert_not_equals(actual, null);
    if(actual.indexOf('calc') == -1 )
        actual = roundResultStr(actual);
    document.body.removeChild(div);

    // See comment above about multiple expected results
    if(Object.prototype.toString.call( expected ) === '[object Array]' && expected.length == 2) {
        assert_in_array(actual, expected);
    } else {
        assert_equals(actual, !expected ? '0' : expected);
    }
}

// Builds an array of test cases to send to testharness.js where one test case is: [name, actual, expected]
// These test cases will verify results from testInlineStyle() or testComputedStyle()
function buildTestCases(testCases, testType) {
    var results = [];

    // If test_type isn't specified, test inline style
    var type = typeof testType == 'undefined' ? 'invalid': testType;

    testCases.forEach(function(test) {
        oneTestCase = [];

        // name - annotated by type (inline vs. computed)
        if ( test.hasOwnProperty('name') ) {
            oneTestCase.push(test['name'] +' - '+ type);
        } else {
            // If test_name isn't specified, use the actual
            oneTestCase.push(test['actual'] +' - '+ type);
        }

        // actual
        oneTestCase.push(test['actual'])

        // expected
        if( type.indexOf('invalid') != -1 ){
            oneTestCase.push("")
        } else if( type == 'inline' ) {
            oneTestCase.push(test['expected_inline']);
        } else if( type == 'computed' ){
            oneTestCase.push( convertToPx(test['expected_computed']) );
        }
        results.push(oneTestCase);
    });
    return results;
}

function isAbsoluteLength(unit) {
    return unit == "cm" || unit == "mm" || unit == "Q" || unit == "in" ||
           unit == "pt" || unit == "pc" || unit == "px";
}

function buildPositionTests(shape, valid, type, units) {
    var results = new Array();
    var is_computed = type.indexOf('computed') != -1 ? true : false;

    if(Object.prototype.toString.call( units ) === '[object Array]') {
        units.forEach(function(unit) {
            positionTests = buildPositionTests(shape, valid, type, unit);
            results = results.concat(positionTests);
        });
    } else {
        if (valid) {
            validPositions.forEach(function(test) {
                var testCase = [], testName, actual, expected;
                // skip if this isn't explicitly testing length units
                if (!(type.indexOf('lengthUnit') != -1 && test[0].indexOf("u1") == -1)) {
                    // actual
                    actual = shape + '(at ' + setUnit(test[0], false, units) +')';

                    let position = test[1];
                    let convert = is_computed;
                    if (!is_computed) {
                      // For specified values.
                      // Note: "[convert]" tag is used only for the specified
                      // value.
                      if (position.includes('[convert]')) {
                        // We should convert the absolute length into the
                        // canonical unit in calc(), for specified values.
                        // e.g.
                        // 1. "circle(at 1pt 50%)" serializes as
                        //    "circle(at 1pt 50%)".
                        // 2. "circle(at calc(1pt) 50%)" serializes as
                        //    "circle(at calc(1.33px) 50%)".
                        convert = isAbsoluteLength(units);
                      }
                    } else if (test.length == 3) {
                      // Use the 3rd element as the expected computed value.
                      position = test[2];
                    }

                    // Remove the tag if there is.
                    position = position.replace('[convert] ', '');

                    // expected
                    //  if(convert && shape == 'circle')
                    //      expected = shape + '(at ' + setUnit(test[1], convert, units) +')';
                    //  else if(convert && shape == 'ellipse')
                    //      expected = shape + '(at ' + setUnit(test[1], convert, units) +')';
                    //  else
                    expected = shape + '(at ' + setUnit(position, convert, units) +')';

                    // name
                    if (type == 'lengthUnit + inline')
                        testName = 'test unit (inline): ' + units +' - '+ actual;
                    else if (type == 'lengthUnit + computed')
                        testName = 'test unit (computed): ' + units +' - '+ actual;
                    else
                        testName = (actual + ' serializes as ' + expected +' - '+ type);

                    testCase.push(testName)
                    testCase.push(actual);
                    testCase.push(expected);
                    results.push(testCase);
                }
            });
        } else {
            invalidPositions.forEach(function(test) {
                var testValue = shape + '(at ' + setUnit(test, false, units) +')';
                testCase = new Array();
                testCase.push(testValue + ' is invalid');
                testCase.push(testValue);
                testCase.push("");
                results.push(testCase);
            });
        }
    }
    return unique(results);
}

function buildRadiiTests(shape, type, units) {
    var results = new Array();
    var testUnits = typeof units == 'undefined' ? 'px': units;
    var convert = type.indexOf('computed') != -1 ? true : false;

    if(Object.prototype.toString.call( testUnits ) === '[object Array]') {
           testUnits.forEach(function(unit) {
               radiiTests = buildRadiiTests(shape, type, unit);
               results = results.concat(radiiTests);
           });
    } else {
        var validRadii = shape == 'circle' ? validCircleRadii : validEllipseRadii;
        validRadii.forEach(function(test) {
            var testCase = [], name, actual, expected;

            // skip if this isn't explicitly testing length units
            if( !(type.indexOf('lengthUnit') != -1 && test[0].indexOf("u1") == -1) ) {
                actual = shape + '(' + setUnit(test[0], false, testUnits) +')';
                // name
                if (type.indexOf('lengthUnit') != -1) {
                    name = 'test unit: ' + units +' - '+ actual;
                    if(type.indexOf('computed') != -1)
                        name = name + ' - computed';
                    else
                        name = name + ' - inline';
                }
                else
                    name = actual +' - '+ type;

                testCase.push(name);

                // actual
                testCase.push(actual);

                // expected
                if(type.indexOf('computed') != -1 && test.length == 3) {
                    expected = shape + '(' + setUnit(test[2], convert, testUnits) +')';
                } else {
                    expected = shape + '(' + setUnit(test[1], convert, testUnits) +')';
                }
                testCase.push(expected);
                results.push(testCase);
            }
        });
    }
    return unique(results);
}

function buildInsetTests(unit1, unit2, type) {
    var results = new Array();
    var convert = type == 'computed' ? true : false;

    if(Object.prototype.toString.call( unit1 ) === '[object Array]') {
        unit1.forEach(function(unit) {
            insetTests = buildInsetTests(unit, unit2, type);
            results = results.concat(insetTests);
        });
    } else {
        validInsets.forEach(function(test) {
            var testCase = [], name, actual, expected;

            name = setUnit(test[0], false, unit1, unit2) +' - '+ type;
            actual = 'inset(' + setUnit(test[1], convert, unit1, unit2) +')';
            expected = actual;

            testCase.push(name);
            testCase.push(actual);
            testCase.push(expected);

            results.push(testCase);
        });
    }
    return unique(results);
}

function buildPolygonTests(unitSet, type) {
    var results = new Array();
    var convert = type == 'computed' ? true : false;

    unitSet.forEach(function(set) {
        validPolygons.forEach(function(test) {
            var testCase = [];
            // name
            testCase.push(setUnit(test[0], false, set[0], set[1], set[2]) +' - '+ type);
            // actual
            testCase.push('polygon(' + setUnit(test[1], false, set[0], set[1], set[2]) +')');
            // expected
            testCase.push('polygon(' + setUnit(test[1], convert, set[0], set[1], set[2]) +')');
            results.push(testCase);
        });
    });
    return unique(results);
}

function buildCalcTests(testCases, type) {
    var results = new Array();
    testCases.forEach(function(test){
        var testCase = [];
        if(type == 'computed') {
            testCase.push(test[0] + ' - computed style');
            testCase.push(test[0]);
            testCase.push(test[2]);
        }
        else {
            testCase.push(test[0] + ' - inline style');
            testCase.push(test[0]);
            testCase.push(test[1]);
        }
        testCase.push(type);
        results.push(testCase)
    });
    return unique(results);
}

function unique(tests) {
    var list = tests.concat();
    for(var i = 0; i< list.length; ++i) {
        for(var j = i+1; j < list.length; ++j) {
            if(list[i][0] === list[j][0])
                list.splice(j--, 1);
        }
    }
    return list;
}

function setUnit(str, convert, unit1, unit2, unit3) {
    var retStr = str;
    if(typeof unit1 !== 'undefined') {
        retStr = retStr.replace(new RegExp('u1', 'g'), unit1);
    }
    if(typeof unit2 !== 'undefined') {
        retStr = retStr.replace(new RegExp("u2", 'g'), unit2);
    }
    if(typeof unit3 !== 'undefined') {
        retStr = retStr.replace(new RegExp("u3", 'g'), unit3);
    }
    retStr = convert ? convertToPx(retStr) : retStr;
    return retStr;
}

function roundCssNumber(n) {
    // See https://drafts.csswg.org/cssom/#serializing-css-values for numbers.
    return parseFloat(n.toPrecision(6));
}

function convertToPx(origValue) {

    var valuesToConvert = origValue.match(/[0-9]+(\.[0-9]+)?([a-z]{2,4}|%|)/g);
    if(!valuesToConvert)
        return origValue;

    var retStr = origValue;
    for(var i = 0; i < valuesToConvert.length; i++) {
        var unit = (valuesToConvert[i].match(/[a-z]{2,4}|%/) || '').toString();
        var numberStr = valuesToConvert[i].match(/[0-9]+(\.[0-9]+)?/)[0];

        var number = parseFloat(numberStr);
        var convertedUnit = 'px';
        if( typeof number !== 'NaN' )
        {
             if (unit == 'in') {
                 number = (96 * number);
             } else if (unit == 'cm') {
                 number = (37.795275591 * number);
             } else if (unit == 'mm') {
                 number = (3.779527559 * number);
             } else if (unit == 'pt') {
                 number = (1.333333333333 * number);
             } else if (unit == 'pc') {
                 number = (16 * number);
             } else if (unit == 'em') {
                 number = (16 * number);
             } else if (unit == 'ex') {
                 number = (12.8 * number);
             } else if (unit == 'ch') {
                 number = (16 * number);
             } else if (unit == 'rem') {
                 number = (16 * number);
             } else if (unit == 'vw') {
                 number = ((.01 * window.innerWidth) * number);
             } else if (unit == 'vh') {
                 number = ((.01 * window.innerHeight) * number);
             } else if (unit == 'vmin') {
                 number = Math.min( (.01 * window.innerWidth), (.01 * window.innerHeight) ) * number;
             } else if (unit == 'vmax') {
                number = Math.max( (.01 * window.innerWidth), (.01 * window.innerHeight) ) * number;
             }
             else {
                 convertedUnit = unit;
             }
            number = roundCssNumber(number);
            var find = valuesToConvert[i];
            var replace = number.toString() + convertedUnit;
            retStr = retStr.replace(valuesToConvert[i], number.toString() + convertedUnit);
      }
    }
    return retStr.replace(',,', ',');
}

function roundResultStr(str) {
    if(Object.prototype.toString.call( str ) !== '[object String]')
        return str;

    var numbersToRound = str.match(/[0-9]+\.[0-9]+/g);
    if(!numbersToRound)
        return str;

    var retStr = str;
    for(var i = 0; i < numbersToRound.length; i++) {
        num = parseFloat(numbersToRound[i]);
        if( !isNaN(num) ) {
            roundedNum = roundCssNumber(num);
            retStr = retStr.replace(numbersToRound[i].toString(), roundedNum.toString());
        }
    }

    return retStr;
}

function generateInsetRoundCases(units, testType) {
    var convert = testType.indexOf('computed') != -1 ? true : false;
    var testUnit = units;
    var sizes = [
        '10' + units,
        '20' + units,
        '30' + units,
        '40' + units
    ];

    function insetRound(value) {
        return 'inset(10' +testUnit+ ' round ' + value + ')';
    }

    function serializedInsetRound(lhsValues, rhsValues, convert) {
        var retStr = '';
        if(!rhsValues)
            retStr = 'inset(10' +testUnit+ ' round ' + lhsValues +')';
        else
            retStr = 'inset(10' +testUnit+ ' round ' + lhsValues +' / '+ rhsValues +')';

        if(convert)
            return convertToPx(retStr);

        return retStr;
    }

    var results = [], left, lhs, right, rhs;
    for (left = 1; left <= 4; left++) {
        lhs = sizes.slice(0, left).join(' ');
        results.push([insetRound(lhs) +' - '+ testType, insetRound(lhs), serializedInsetRound(lhs, null, convert)]);
        for (right = 1; right <= 4; right++) {
            rhs = sizes.slice(0, right).join(' ');
            if(lhs == rhs)
                results.push([insetRound(lhs + ' / ' + rhs) +' - '+ testType, insetRound(lhs + ' / ' + rhs), serializedInsetRound(lhs, null, convert)]);
            else
                results.push([insetRound(lhs + ' / ' + rhs) +' - '+ testType, insetRound(lhs + ' / ' + rhs), serializedInsetRound(lhs, rhs, convert)]);
        }
    }
    return results;
}

function each(object, func) {
    for (var prop in object) {
        if (object.hasOwnProperty(prop)) {
            func(prop, object[prop]);
        }
    }
}

/// For saving and restoring font properties
var savedFontValues = { };

function setupFonts() {
    var fontProperties = {
        'font-family': 'Ahem',
        'font-size': '16px',
        'line-height': '1'
    };
    savedFontValues = { };
    each(fontProperties, function (key, value) {
        savedFontValues[key] = document.body.style.getPropertyValue(key);
        document.body.style.setProperty(key, value);
    });
}

function restoreFonts() {
    each(savedFontValues, function (key, value) {
        if (value) {
            document.body.style.setProperty(key, value);
        }
        else {
            document.body.style.removeProperty(key);
        }
    });
    savedFontValues = { };
}

var validUnits = [
                    "cm","mm","in","pt","pc",  // Absolute length units (omitting px b/c we default to that in all tests)
                    "em","ex","ch","rem",      // Font relative length units
                    "vw","vh","vmin","vmax"    // Viewport percentage units
                 ]

/// [actual, expected]
var validPositions = [

/// [ percent ], [ length ], [ percent | percent ], [ percent | length ], [ length | percent ], [ length | length ]
    ["50%", "50% 50%"],
    ["50u1", "50u1 50%"],
    ["50% 50%", "50% 50%"],
    ["50% 50u1", "50% 50u1"],
    ["50u1 50%", "50u1 50%"],
    ["50u1 50u1", "50u1 50u1"],

///// [ keyword ], [ keyword keyword ] x 5 keywords
    ["left", "0% 50%"],
    ["top", "50% 0%"],
    ["right", "100% 50%"],
    ["bottom", "50% 100%"],
    ["center", "50% 50%"],

    ["left top", "0% 0%"],
    ["left bottom", "0% 100%"],
    ["left center", "0% 50%"],

    ["top left", "0% 0%"],
    ["top right", "100% 0%"],
    ["top center", "50% 0%"],

    ["right top", "100% 0%"],
    ["right bottom", "100% 100%"],
    ["right center", "100% 50%"],

    ["bottom left", "0% 100%"],
    ["bottom right", "100% 100%"],
    ["bottom center", "50% 100%"],

    ["center top", "50% 0%"],
    ["center left", "0% 50%"],
    ["center right", "100% 50%"],
    ["center bottom", "50% 100%"],
    ["center center", "50% 50%"],

////// [ keyword | percent ], [ keyword | length ], [ percent | keyword ], [ length | keyword ] x 5 keywords
    ["left 50%", "0% 50%"],
    ["left 50u1", "0% 50u1"],

    ["50% top", "50% 0%"],
    ["50u1 top", "50u1 0%"],

    ["right 80%", "100% 80%"],
    ["right 80u1", "100% 80u1"],

    ["70% bottom", "70% 100%"],
    ["70u1 bottom", "70u1 100%"],

    ["center 60%", "50% 60%"],
    ["center 60u1", "50% 60u1"],
    ["60% center", "60% 50%"],
    ["60u1 center", "60u1 50%"],

////// [ keyword percent |  keyword percent], [ keyword percent |  keyword length],
////// [ keyword length | keyword length],  [ keyword length | keyword percent] x 5 keywords
    ["left 50% top 50%", "50% 50%"],
    ["left 50% top 50u1", "50% 50u1"],
    ["left 50% bottom 70%", "50% calc(30%)", "50% 30%"],
    ["left 50% bottom 70u1", "[convert] 50% calc(100% - 70u1)"],
    ["left 50u1 top 50%", "50u1 50%"],
    ["left 50u1 top 50u1", "50u1 50u1"],
    ["left 50u1 bottom 70%", "50u1 calc(30%)", "50u1 30%"],

    ["top 50% left 50%", "50% 50%"],
    ["top 50% left 50u1", "50u1 50%"],
    ["top 50% right 80%", "calc(20%) 50%", "20% 50%"],
    ["top 50% right 80u1", "[convert] calc(100% - 80u1) 50%"],
    ["top 50u1 left 50%", "50% 50u1"],
    ["top 50u1 left 50u1", "50u1 50u1"],
    ["top 50u1 right 80%", "calc(20%) 50u1", "20% 50u1"],

    ["bottom 70% left 50%", "50% calc(30%)", "50% 30%"],
    ["bottom 70% left 50u1", "50u1 calc(30%)", "50u1 30%"],
    ["bottom 70% right 80%", "calc(20%) calc(30%)", "20% 30%"],
    ["bottom 70% right 80u1", "[convert] calc(100% - 80u1) calc(30%)", "calc(100% - 80u1) 30%"],
    ["bottom 70u1 left 50%", "[convert] 50% calc(100% - 70u1)"],
    ["bottom 70u1 right 50%", "[convert] calc(50%) calc(100% - 70u1)", "50% calc(100% - 70u1)"],
    ["bottom 70u1 right 80u1", "[convert] calc(100% - 80u1) calc(100% - 70u1)"],

    ["right 80% top 50%", "calc(20%) 50%", "20% 50%"],
    ["right 80% top 50u1", "calc(20%) 50u1", "20% 50u1"],
    ["right 80% bottom 70%", "calc(20%) calc(30%)", "20% 30%"],
    ["right 80% bottom 70u1", "[convert] calc(20%) calc(100% - 70u1)", "20% calc(100% - 70u1)"],
    ["right 80u1 top 50%", "[convert] calc(100% - 80u1) 50%"],
    ["right 80u1 bottom 70%", "[convert] calc(100% - 80u1) calc(30%)", "calc(100% - 80u1) 30%"],
    ["right 80u1 bottom 70u1", "[convert] calc(100% - 80u1) calc(100% - 70u1)"],
];

var invalidPositions = [
////// [ keyword | percent ], [ keyword | length ], [ percent | keyword ], [ length | keyword ] x 5 keywords
    "50% left",
    "50px left",
    "top 50%",
    "80% right",
    "80px right",
    "bottom 70%",
    "bottom 70px",

//////  [ keyword | keyword percent ], [ keyword | keyword length ] x 5 keywords
    "center center 60%",
    "center center 60px",

    "left center 60%",
    "left center 60px",
    "left right 80%",
    "left right 80px",
    "left left 50%",
    "left left 50px",

    "top center 60%",
    "top center 60px",
    "top bottom 80%",
    "top bottom 80px",
    "top top 50%",
    "top top 50px",

    "bottom center 60%",
    "bottom center 60px",
    "bottom top 50%",
    "bottom top 50px",
    "bottom bottom 50%",
    "bottom bottom 50px",

    "right center 60%",
    "right center 60px",
    "right left 50%",
    "right left 50px",
    "right right 70%",
    "right right 70px",

////// [ keyword percent | keyword], [ keyword length | keyword ] x 5 keywords
    "center 60% top",
    "center 60px top",
    "center 60% bottom",
    "center 60px bottom",
    "center 60% left",
    "center 60px left",
    "center 60% right",
    "center 60px right",
    "center 60% center",
    "center 60px center",

    "left 50% right",
    "left 50px right",
    "left 50% left",
    "left 50px left",

    "top 50% bottom",
    "top 50px bottom",
    "top 50% top",
    "top 50px top",

    "bottom 70% top",
    "bottom 70px top",
    "bottom 70% bottom",
    "bottom 70px bottom",

    "right 80% left",
    "right 80px left",

////// [ keyword percent |  keyword percent], [ keyword percent |  keyword length],
////// [ keyword length | keyword length],  [ keyword length | keyword percent] x 5 keywords
    "center 60% top 50%",
    "center 60% top 50px",
    "center 60% bottom 70%",
    "center 60% bottom 70px",
    "center 60% left 50%",
    "center 60% left 50px",
    "center 60% right 70%",
    "center 60% right 70px",
    "center 60% center 65%",
    "center 60% center 65px",
    "center 60px top 50%",
    "center 60px top 50px",
    "center 60px bottom 70%",
    "center 60px bottom 70px",
    "center 60px left 50%",
    "center 60px left 50px",
    "center 60px right 70%",
    "center 60px right 70px",
    "center 60px center 65%",
    "center 60px center 65px",

    "left 50% center 60%",
    "left 50% center 60px",
    "left 50% right 80%",
    "left 50% right 80px",
    "left 50% left 50%",
    "left 50% left 50px",
    "left 50px center 60%",
    "left 50px center 60px",
    "left 50px right 80%",
    "left 50px right 80px",
    "left 50px left 50%",
    "left 50px left 50px",

    "top 50% center 60%",
    "top 50% center 60px",
    "top 50% bottom 50%",
    "top 50% bottom 50px",
    "top 50% top 50%",
    "top 50% top 50px",
    "top 50px center 60%",
    "top 50px center 60px",
    "top 50px bottom 70%",
    "top 50px bottom 70px",
    "top 50px top 50%",
    "top 50px top 50px",

    "bottom 70% center 60%",
    "bottom 70% center 60px",
    "bottom 70% top 50%",
    "bottom 70% top 50px",
    "bottom 70% bottom 50%",
    "bottom 70% bottom 50px",
    "bottom 70px center 60%",
    "bottom 70px center 60px",
    "bottom 70px top 50%",
    "bottom 70px top 50px",
    "bottom 70px bottom 50%",
    "bottom 70px bottom 50px",

    "right 80% center 60%",
    "right 80% center 60px",
    "right 80% left 50%",
    "right 80% left 50px",
    "right 80% right 85%",
    "right 80% right 85px",
    "right 80px center 60%",
    "right 80px center 60px",
    "right 80px left 50%",
    "right 80px left 50px",
    "right 80px right 85%",
    "right 80px right 85px"
];

// valid radii values for circle + ellipse
// [value, expected_inline, [expected_computed?]]
var validCircleRadii = [
    ['at 50% 50%', 'at 50% 50%'],
    ['50u1 at 50% 50%', '50u1 at 50% 50%'],
    ['50% at 50% 50%', '50% at 50% 50%'],
    ['closest-side at 50% 50%', 'at 50% 50%'],
    ['farthest-side at 50% 50%', 'farthest-side at 50% 50%'],
    ['', ''],
    ['50u1', '50u1'],
    ['50%', '50%'],
    ['closest-side', ''],
    ['farthest-side', 'farthest-side']
]
var validEllipseRadii = [
    ['at 50% 50%', 'at 50% 50%', 'at 50% 50%'],
    ['50u1 100u1 at 50% 50%', '50u1 100u1 at 50% 50%'],
    ['100u1 100px at 50% 50%', '100u1 100px at 50% 50%'],
    ['25% 50% at 50% 50%', '25% 50% at 50% 50%'],
    ['50u1 25% at 50% 50%', '50u1 25% at 50% 50%'],
    ['25% 50u1 at 50% 50%', '25% 50u1 at 50% 50%'],
    ['25% closest-side at 50% 50%', '25% closest-side at 50% 50%'],
    ['25u1 closest-side at 50% 50%', '25u1 closest-side at 50% 50%'],
    ['closest-side 75% at 50% 50%', 'closest-side 75% at 50% 50%'],
    ['closest-side 75u1 at 50% 50%', 'closest-side 75u1 at 50% 50%'],
    ['25% farthest-side at 50% 50%', '25% farthest-side at 50% 50%'],
    ['25u1 farthest-side at 50% 50%', '25u1 farthest-side at 50% 50%'],
    ['farthest-side 75% at 50% 50%', 'farthest-side 75% at 50% 50%'],
    ['farthest-side 75u1 at 50% 50%', 'farthest-side 75u1 at 50% 50%'],
    ['closest-side closest-side at 50% 50%', 'at 50% 50%'],
    ['farthest-side farthest-side at 50% 50%', 'farthest-side farthest-side at 50% 50%'],
    ['closest-side farthest-side at 50% 50%', 'closest-side farthest-side at 50% 50%'],
    ['farthest-side closest-side at 50% 50%', 'farthest-side closest-side at 50% 50%'],
    ['', ''],
    ['50u1 100u1', '50u1 100u1'],
    ['100u1 100px', '100u1 100px'],
    ['25% 50%', '25% 50%'],
    ['50u1 25%', '50u1 25%'],
    ['25% 50u1', '25% 50u1'],
    ['25% closest-side', '25% closest-side'],
    ['25u1 closest-side', '25u1 closest-side'],
    ['closest-side 75%', 'closest-side 75%'],
    ['closest-side 75u1', 'closest-side 75u1'],
    ['25% farthest-side', '25% farthest-side'],
    ['25u1 farthest-side', '25u1 farthest-side'],
    ['farthest-side 75%', 'farthest-side 75%'],
    ['farthest-side 75u1', 'farthest-side 75u1'],
    ['closest-side closest-side', ''],
    ['farthest-side farthest-side', 'farthest-side farthest-side'],
    ['closest-side farthest-side', 'closest-side farthest-side'],
    ['farthest-side closest-side', 'farthest-side closest-side']
]

var validInsets = [
    ["One arg - u1", "10u1"],
    ["One arg - u2", "10u2"],
    ["Two args - u1 u1", "10u1 20u1"],
    ["Two args - u1 u2", "10u1 20u2"],
    ["Two args - u2 u1", "10u2 20u1"],
    ["Two args - u2 u2", "10u2 20u2"],
    ["Three args - u1 u1 u1", "10u1 20u1 30u1"],
    ["Three args - u1 u1 u2", "10u1 20u1 30u2"],
    ["Three args - u1 u2 u1", "10u1 20u2 30u1"],
    ["Three args - u1 u2 u2 ", "10u1 20u2 30u2"],
    ["Three args - u2 u1 u1", "10u2 20u1 30u1"],
    ["Three args - u2 u1 u2 ", "10u2 20u1 30u2"],
    ["Three args - u2 u2 u1 ", "10u2 20u2 30u1"],
    ["Three args - u2 u2 u2 ","10u2 20u2 30u2"],
    ["Four args - u1 u1 u1 u1", "10u1 20u1 30u1 40u1"],
    ["Four args - u1 u1 u1 u2", "10u1 20u1 30u1 40u2"],
    ["Four args - u1 u1 u2 u1", "10u1 20u1 30u2 40u1"],
    ["Four args - u1 u1 u2 u2", "10u1 20u1 30u2 40u2"],
    ["Four args - u1 u2 u1 u1", "10u1 20u2 30u1 40u1"],
    ["Four args - u1 u2 u1 u2", "10u1 20u2 30u1 40u2"],
    ["Four args - u1 u2 u2 u1", "10u1 20u2 30u2 40u1"],
    ["Four args - u1 u2 u2 u2", "10u1 20u2 30u2 40u2"],
    ["Four args - u2 u1 u1 u1", "10u2 20u1 30u1 40u1"],
    ["Four args - u2 u1 u1 u2", "10u2 20u1 30u1 40u2"],
    ["Four args - u2 u1 u2 u1", "10u2 20u1 30u2 40u1"],
    ["Four args - u2 u1 u2 u2", "10u2 20u1 30u2 40u2"],
    ["Four args - u2 u2 u1 u1", "10u2 20u2 30u1 40u1"],
    ["Four args - u2 u2 u1 u2", "10u2 20u2 30u1 40u2"],
    ["Four args - u2 u2 u2 u1", "10u2 20u2 30u2 40u1"],
    ["Four args - u2 u2 u2 u2", "10u2 20u2 30u2 40u2"]
]

var validPolygons = [
    ["One vertex - u1 u1", "10u1 20u1"],
    ["One vertex - u1 u2", "10u1 20u2"],
    ["Two vertices - u1 u1, u1 u1", "10u1 20u1, 30u1 40u1"],
    ["Two vertices - u1 u1, u2 u2", "10u1 20u1, 30u2 40u2"],
    ["Two vertices - u2 u2, u1 u1", "10u2 20u2, 30u1 40u1"],
    ["Two vertices - u1 u2, u2 u1", "10u1 20u2, 30u2 40u1"],
    ["Three vertices - u1 u1, u1 u1, u1 u1", "10u1 20u1, 30u1 40u1, 50u1 60u1"],
    ["Three vertices - u2 u2, u2 u2, u2 u2", "10u2 20u2, 30u2 40u2, 50u2 60u2"],
    ["Three vertices - u3 u3, u3 u3, u3 u3", "10u3 20u3, 30u3 40u3, 50u3 60u3"],
    ["Three vertices - u1 u1, u2 u2, u3 u3", "10u1 20u1, 30u2 40u2, 50u3 60u3"],
    ["Three vertices - u3 u3, u1, u1, u2 u2", "10u3 20u3, 30u1 40u1, 50u2 60u2"],
]

// [test value, expected property value, expected computed style]
// See https://github.com/w3c/csswg-drafts/issues/4399#issuecomment-556160413
// for the latest resolution to this respect.
var calcTestValues = [
    ["calc(10in)", "calc(960px)", "960px"],
    ["calc(10in + 20px)", "calc(980px)", "980px"],
    ["calc(30%)", "calc(30%)", "30%"],
    ["calc(100%/4)", "calc(25%)", "25%"],
    ["calc(25%*3)", "calc(75%)", "75%"],
    ["calc(25%*3 - 10in)", "calc(75% - 960px)", "calc(75% - 960px)"],
    ["calc((12.5%*6 + 10in) / 4)", "calc(18.75% + 240px)", "calc(18.75% + 240px)"]
]

return {
    testInlineStyle: testInlineStyle,
    testComputedStyle: testComputedStyle,
    testShapeMarginInlineStyle: testShapeMarginInlineStyle,
    testShapeMarginComputedStyle: testShapeMarginComputedStyle,
    testShapeThresholdInlineStyle: testShapeThresholdInlineStyle,
    testShapeThresholdComputedStyle: testShapeThresholdComputedStyle,
    buildTestCases: buildTestCases,
    buildRadiiTests: buildRadiiTests,
    buildPositionTests: buildPositionTests,
    buildInsetTests: buildInsetTests,
    buildPolygonTests: buildPolygonTests,
    generateInsetRoundCases: generateInsetRoundCases,
    buildCalcTests: buildCalcTests,
    validUnits: validUnits,
    calcTestValues: calcTestValues,
    roundResultStr: roundResultStr,
    setupFonts: setupFonts,
    restoreFonts: restoreFonts,
}
})();
