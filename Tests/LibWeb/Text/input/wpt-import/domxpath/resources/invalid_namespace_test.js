"use strict";
setup({ allow_uncaught_exception: true });

const invalid_namespace_test = (t, resolver, resolverWindow = window) => {
  const result = new Promise((resolve, reject) => {
    const handler = event => {
      reject(event.error);
    };

    resolverWindow.addEventListener("error", handler);
    t.add_cleanup(() => {
      resolverWindow.removeEventListener("error", handler);
    });

    t.step_timeout(resolve, 0);
  });

  assert_throws_dom("NAMESPACE_ERR", () => {
    document.evaluate("/foo:bar", document.documentElement, resolver);
  });

  return result;
};
