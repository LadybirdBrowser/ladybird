// Same name at different scope levels: each gets its own local slot.
function shadow_let() {
    let x = 1;
    {
        let x = 2;
        {
            let x = 3;
            x;
        }
        x;
    }
    x;
}

// var and let with the same name: var is function-scoped.
function var_and_let() {
    var x = 1;
    {
        let x = 2;
        x;
    }
    x;
}

// Parameter shadows a global.
var globalName = 1;
function param_shadows_global(globalName) {
    return globalName;
}

// Catch parameter shadows outer variable.
function catch_shadow() {
    let e = "outer";
    try {
        throw "inner";
    } catch (e) {
        e;
    }
    e;
}
