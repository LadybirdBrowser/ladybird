
self.GLOBAL = {
  isWindow: function() { return false; },
  isWorker: function() { return true; },
  isShadowRealm: function() { return false; },
};
importScripts("/resources/testharness.js");
importScripts("/wasm/jsapi/assertions.js")
importScripts("/wasm/jsapi/wasm-module-builder.js")
importScripts("/wasm/jsapi/js-string/polyfill.js")
importScripts("/wasm/jsapi/js-string/basic.any.js");
done();
