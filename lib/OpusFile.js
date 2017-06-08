
var OpusFile = require('bindings')('node-opusfile');

console.log(OpusFile.Normalize('./output.opus', './input.opus'));
