import { permissionTest } from "./permission-helper.js"

const params = new URL(import.meta.url).searchParams;
permissionTest(null, params.get("sender"));
