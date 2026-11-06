import { "" as empty } from "./default-and-star-export.mjs";

const afterTheEmptyName = 1;

function read() {
    return afterTheEmptyName;
}

export const passed = read() === 1 && empty === "empty";
