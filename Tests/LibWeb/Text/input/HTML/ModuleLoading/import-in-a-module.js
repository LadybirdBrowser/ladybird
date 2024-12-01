// NOTE: Doesn't matter what this imports, but this imports itself so there is no import error in test logs.
import("./import-in-a-module.js");

const returnValue = "PASS! (Didn't crash)";
export default returnValue;
