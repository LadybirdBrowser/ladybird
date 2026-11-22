
self.GLOBAL = {
  isWindow: function() { return false; },
  isWorker: function() { return true; },
  isShadowRealm: function() { return false; },
};
importScripts("/resources/testharness.js");
self.META_TITLE = "EventSource: a data before final empty line";
importScripts("/eventsource/format-data-before-final-empty-line.any.js");
done();
