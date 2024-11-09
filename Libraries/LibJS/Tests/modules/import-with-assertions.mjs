import * as self from "./import-with-assertions.mjs" assert { key: "value", key2: "value2", default: "shouldwork" };
import "./import-with-assertions.mjs" assert { key: "value", key2: "value2", default: "shouldwork" };

export { passed } from "./module-with-default.mjs" assert { key: "value", key2: "value2", default: "shouldwork" };
