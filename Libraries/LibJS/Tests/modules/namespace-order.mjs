import * as ns from "./default-and-star-export.mjs";

const keys = Reflect.ownKeys(ns);
// The keys should be in alphabetical order and @@toString at the end
if (keys.length < 4) throw new Error("Expected at least 3 keys and @@toStringTag");

if (keys[0] !== "") throw new Error('Expected keys[0] === ""');

if (keys[1] !== "*") throw new Error('Expected keys[1] === "*"');

if (keys[2] !== "default") throw new Error('Expected keys[2] === "default"');

if (keys.indexOf(Symbol.toStringTag) <= 2)
    throw new Error("Expected Symbol.toStringTag to be behind string keys");

export const passed = true;
