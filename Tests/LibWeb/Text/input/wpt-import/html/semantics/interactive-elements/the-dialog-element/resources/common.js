function waitUntilLoadedAndAutofocused() {
  return new Promise(function(resolve) {
      var loaded = false;
      var autofocused = false;
      window.addEventListener('load', function() {
          loaded = true;
          if (autofocused)
              resolve();
      }, false);
      document.addEventListener('focusin', function() {
          if (autofocused)
              return;
          autofocused = true;
          if (loaded)
              resolve();
      }, false);
      test_driver.click(document.documentElement);
    });
}
