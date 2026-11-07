import {
    a,
    "a\u0000" as oneNul,
    "a\u0000\u0000" as twoNuls,
    "ab\u0000" as twoByteName,
    b,
    "b\u0000\u0000" as nulBeforeIdentifier,
    "" as empty,
    "\u0000" as nul,
    "\u0000\u0000" as justNuls,
    "only\u0000\u0000" as only,
    readB,
} from "./trailing-nul-export-names.mjs";

export const passed =
    a === 1 &&
    oneNul === 2 &&
    twoNuls === 3 &&
    twoByteName === 2 &&
    b === 4 &&
    nulBeforeIdentifier === 2 &&
    empty === 1 &&
    nul === 2 &&
    justNuls === 3 &&
    only === 3 &&
    readB() === 4;
