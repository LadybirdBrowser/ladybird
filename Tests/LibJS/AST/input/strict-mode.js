"use strict";

function strict_function(a, b) {
    let c = a + b;
    return c;
}

function inner_strict() {
    "use strict";
    let x = 1;
    return x;
}
