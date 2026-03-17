"use strict";

function expect_type_error(callback) {
    let did_throw = false;
    try {
        callback();
    } catch (error) {
        if (!(error instanceof TypeError))
            throw error;
        did_throw = true;
    }
    if (!did_throw)
        throw new Error("Expected TypeError");
}

let object = Object.preventExtensions({ 0: 1 });
expect_type_error(() => {
    object[1] = 2;
});
if (object[1] !== undefined)
    throw new Error("Indexed object append mutated");

let array = [1, 2];
Object.defineProperty(array, "length", { writable: false });
expect_type_error(() => {
    array[array.length] = 3;
});
if (array.length !== 2)
    throw new Error("Array length changed");
if (array[2] !== undefined)
    throw new Error("Array append mutated");
