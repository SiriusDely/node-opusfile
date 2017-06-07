
var OpusFile = require('bindings')('node-opusfile');

console.log(OpusFile.WhoAmI('./output.opus', '/Users/sirius/Node/node-opusfile/opusfile/examples/input.opus'));
