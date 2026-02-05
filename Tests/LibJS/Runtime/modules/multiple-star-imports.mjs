import * as ns1 from "./default-and-star-export.mjs";
import * as ns2 from "./default-and-star-export.mjs";

export const passed = ns1 === ns2;
