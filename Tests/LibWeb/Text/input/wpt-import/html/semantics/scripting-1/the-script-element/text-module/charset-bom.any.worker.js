
self.GLOBAL = {
  isWindow: function() { return false; },
  isWorker: function() { return true; },
  isShadowRealm: function() { return false; },
};
importScripts("/resources/testharness.js");
importScripts("/common/utils.js")
importScripts("/html/semantics/scripting-1/the-script-element/text-module/charset-bom.any.js");
done();
