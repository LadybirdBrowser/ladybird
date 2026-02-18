"use strict";
function test() {
    let result;
    {
        function foo() { return 42; }
        result = foo();
    }
    return result;
}
test();
