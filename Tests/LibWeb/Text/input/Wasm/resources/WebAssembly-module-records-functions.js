import { add } from "./WebAssembly-module-records-provider.wasm";

export const localAdd = add;
export const seven = 7;

export function double(value) {
    return value * 2;
}
