"use strict";


/* Functions to test serialization of properties.

Each takes (property, testString, expectedSerialization) arguments.

These functions depend on a #target element existing in the page,
and will error if they don't find one.

Note that test_computed_serialization and test_used_serialization
are identical except for assertion messages;
you need to choose properties with the correct resolved values
to test the value stage that you want.


For ease of use, it's recommended that you define and use
the following function in your test page:

function test_serialization(t,s,c,u, {prop}={}) {
    test_specified_serialization(prop || 'text-indent', t, s);
    test_computed_serialization(prop || 'text-indent', t, c);
    if(u) test_used_serialization(prop || 'margin-left', t, u);
}

(swapping the property names for what you're expecting to test)

Then you can write tests easily as:

test_serialization(
    'calc(min(1%, 2%) + max(3%, 4%) + 10%)', // test string
    'calc(15%)', // expected specified value
    '15%', // expected computed value
    '15px'); // expected used value

*/




function test_specified_serialization(prop, t, e) {
    const el = document.querySelector("#target");
    if(!el) throw new Exception("Couldn't find #target element to run tests on.");
    test(()=>{
        el.style[prop] = '';
        el.style[prop] = t;
        const tValue = el.style[prop];
        assert_not_equals(tValue, '', `'${t}' should be valid in ${prop}.`);

        el.style[prop] = '';
        el.style[prop] = e;
        const eValue = el.style[prop];
        assert_not_equals(eValue, '', `'${e}' should be valid in ${prop}.`);
        assert_equals(eValue, e, `'${e}' should round-trip exactly in specified values.`);

        assert_equals(tValue, e, `'${t}' and '${e}' should serialize the same in specified values.`);
    }, `'${t}' as a specified value should serialize as '${e}'.`);
}
function test_computed_serialization(prop, t, e) {
    const el = document.querySelector("#target");
    if(!el) throw new Exception("Couldn't find #target element to run tests on.");
    test(()=>{
        el.style[prop] = '';
        el.style[prop] = t;
        const tValue = getComputedStyle(el)[prop];
        assert_not_equals(tValue, '', `'${t}' should be valid in ${prop}.`);

        el.style[prop] = '';
        el.style[prop] = e;
        const eValue = getComputedStyle(el)[prop];
        assert_not_equals(eValue, '', `'${e}' should be valid in ${prop}.`);
        assert_equals(eValue, e, `'${e}' should round-trip exactly in computed values.`);

        assert_equals(tValue, e, `'${t}' and '${e}' should serialize the same in computed values.`);
    }, `'${t}' as a computed value should serialize as '${e}'.`);
}
function test_used_serialization(prop, t, e) {
    const el = document.querySelector("#target");
    if(!el) throw new Exception("Couldn't find #target element to run tests on.");
    test(()=>{
        el.style[prop] = '';
        el.style[prop] = t;
        const tValue = getComputedStyle(el)[prop];
        assert_not_equals(tValue, '', `'${t}' should be valid in ${prop}.`);

        el.style[prop] = '';
        el.style[prop] = e;
        const eValue = getComputedStyle(el)[prop];
        assert_not_equals(eValue, '', `'${e}' should be valid in ${prop}.`);
        assert_equals(eValue, e, `'${e}' should round-trip exactly in used values.`);

        assert_equals(tValue, e, `'${t}' and '${e}' should serialize the same in used values.`);
    }, `'${t}' as a used value should serialize as '${e}'.`);
}