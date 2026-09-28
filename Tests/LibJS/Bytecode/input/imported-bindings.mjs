import * as namespace from "./imported-bindings.mjs";
import { value as importedValue, read as importedRead } from "./imported-bindings.mjs";

export let value = 1;

export function read() {
    return typeof namespace + importedValue;
}

function call() {
    return importedRead();
}

call();
