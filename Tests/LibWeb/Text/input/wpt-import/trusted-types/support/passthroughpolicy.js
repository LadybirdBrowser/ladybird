'use strict'

const passthroughpolicy = trustedTypes.createPolicy("passthroughpolicy", {
  createHTML: s => s,
  createScript: s => s,
  createScriptURL: s => s,
});
