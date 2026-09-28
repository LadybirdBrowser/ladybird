"use strict";

function outer(a) {
    let b = a;
    function callee() {
        return b;
    }
    return () =>
        function inner() {
            a = b;
            return typeof a + callee();
        };
}

function blockFunctions() {
    let c = 1;
    {
        function first() {
            return c + typeof second;
        }
        function second() {
            return c;
        }
        return first();
    }
}

outer(1)()();
blockFunctions();
