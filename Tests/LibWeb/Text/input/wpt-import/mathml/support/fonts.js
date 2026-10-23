function loadAllFonts() {
  // Use this to wait for all fonts in a testcase to load rather than just using
  // `document.fonts.ready.then(...)` in the load event, since there are compat
  // issues between browsers as to whether content initiated font loads are
  // guaranteed to have been started by this point.

  // FIXME: Use Promise.all() to cause an obvious failure when a font fails to load.
  return Promise.allSettled([...document.fonts].map(f => f.load()));
}
