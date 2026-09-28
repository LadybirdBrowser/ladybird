export let counter = 0;

export function increment() {
    counter++;
    return this;
}

export * as nestedNamespace from "./single-const-export.mjs";

export default "default value";
