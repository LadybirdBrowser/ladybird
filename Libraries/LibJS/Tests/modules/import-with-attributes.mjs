import * as self from "./import-with-attributes.mjs" with { key: "value", key2: "value2", default: "shouldwork" };
import "./import-with-attributes.mjs" with { key: "value", key2: "value2", default: "shouldwork" };

export { passed } from "./module-with-default.mjs" with { key: "value", key2: "value2", default: "shouldwork" };
