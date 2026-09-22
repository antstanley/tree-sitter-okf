const root = require("path").join(__dirname, "..", "..");

module.exports =
  typeof process.versions.bun === "string"
    // Support `bun build --compile` by being statically analyzable enough to find the .node file at build-time
    ? require(`../../prebuilds/${process.platform}-${process.arch}/tree-sitter-okf.node`)
    : require("node-gyp-build")(root);

try {
  module.exports.nodeTypeInfo = require("../../src/node-types.json");
} catch (_) {}

// Host helpers: the derived facts a query cannot express (docs/host-helpers.md).
module.exports.okf = require("./okf");
