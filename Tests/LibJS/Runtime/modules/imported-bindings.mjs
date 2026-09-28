import defaultValue, { counter, increment, nestedNamespace } from "./imported-bindings-source.mjs";
import * as source from "./imported-bindings-source.mjs";

function readImports() {
    return () => [counter, source.counter, typeof counter, defaultValue, nestedNamespace.passed];
}

export const valuesBeforeIncrement = readImports()();
export const incrementThisValue = (() => increment())();
export const valuesAfterIncrement = readImports()();

export const assignmentThrows = (() => {
    try {
        counter = 1;
    } catch (error) {
        return error instanceof TypeError;
    }
    return false;
})();

export const passed = true;
