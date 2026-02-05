importScripts("worker-importScripts-scriptToImport.js");
const fromImportedScript = importedFunction();
self.postMessage(fromImportedScript);
