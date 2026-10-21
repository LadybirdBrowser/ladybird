
self.GLOBAL = {
  isWindow: function() { return false; },
  isWorker: function() { return true; },
  isShadowRealm: function() { return false; },
};
importScripts("/resources/testharness.js");
self.META_TITLE = "EventSource: \"retry\" field";
importScripts("/eventsource/format-field-retry.any.js");
done();
