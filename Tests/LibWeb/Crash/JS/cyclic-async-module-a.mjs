import { foo } from "./cyclic-async-module-b.mjs";

await new Promise(resolve => setTimeout(resolve, 200));
