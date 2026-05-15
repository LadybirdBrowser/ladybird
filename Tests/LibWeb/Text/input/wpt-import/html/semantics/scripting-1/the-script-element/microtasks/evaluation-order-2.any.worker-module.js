
self.GLOBAL = {
  isWindow: function() { return false; },
  isWorker: function() { return true; },
  isShadowRealm: function() { return false; },
};
import "/resources/testharness.js";
import "./resources/evaluation-order-setup.js";
import "/html/semantics/scripting-1/the-script-element/microtasks/evaluation-order-2.any.js";
done();
