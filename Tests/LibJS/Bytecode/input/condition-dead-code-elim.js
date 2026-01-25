function alive() {
    return "alive";
}
function dead() {
    return "dead";
}

function ternary_true() {
    return true ? 1 : dead();
}
function ternary_truthy() {
    return 1 ? 1 : dead();
}
function ternary_false() {
    return false ? dead() : 1;
}
function ternary_falsey() {
    return undefined ? dead() : 1;
}

function while_falsey() {
    while (false) {
        dead();
    }
    while (null) {
        dead();
    }
    while (undefined) {
        dead();
    }

    // ensure blocks after eliminated blocks run
    alive();
}
function do_while_falsey() {
    do {
        alive();
    } while (false);
    do {
        alive();
    } while (null);
    do {
        alive();
    } while (undefined);

    // ensure blocks after eliminated blocks run
    alive();
}

function if_falsely() {
    if (false) {
        dead();
    }
    if (null) {
        dead();
    }
    if (undefined) {
        dead();
    }

    // ensure blocks after eliminated blocks run
    alive();
}
function if_truthy() {
    if (true) {
        alive();
    } else {
        dead();
    }
    if ("abc") {
        alive();
    } else {
        dead();
    }
    if (123) {
        alive();
    } else {
        dead();
    }

    // ensure blocks after eliminated blocks run
    alive();
}
function if_exhausted() {
    if (false) {
        dead();
    } else if (false) {
        dead();
    } else {
        alive();
    }
    if (true) {
        alive();
    } else if (true) {
        dead();
    } else {
        dead();
    }

    // ensure blocks after eliminated blocks run
    alive();
}

function call_this() {
    return 1;
}

function for_false() {
    for (; false; ) {
        dead();
    }
    for (; null; ) {
        dead();
    }
    for (; undefined; ) {
        dead();
    }
    // ensure that `call_this()` is still called
    for (let x = call_this(); false; ) {
        dead();
    }

    // ensure blocks after eliminated blocks run
    alive();
}
function for_true(x) {
    // Block should not be optimized away here
    for (; true; ) {
        alive();
        if (x) break;
    }
    for (; 1; ) {
        alive();
        if (x) break;
    }

    // ensure blocks after eliminated blocks run
    alive();
}

ternary_true();
ternary_truthy();
ternary_false();
ternary_falsey();

while_falsey();
do_while_falsey();

if_falsely();
if_truthy();
if_exhausted();

for_false();
for_true(true);
