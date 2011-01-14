var mmap = require('../build/default/mmap');

for (key in mmap) {
  if (mmap.hasOwnProperty(key)) {
    exports[key] = mmap[key];
  }
}

var MmapBuffer = exports.MmapBuffer;

function toHex (n) {
  if (n < 16) return "0" + n.toString(16);
  return n.toString(16);
}

MmapBuffer.isBuffer = function (b) {
  return b instanceof MmapBuffer;
};

MmapBuffer.prototype.inspect = function () {
  return "<MmapBuffer " + this.length +">";
};

MmapBuffer.prototype.toString = function (encoding, start, stop) {
  encoding = String(encoding || 'utf8').toLowerCase();
  start = +start || 0;
  if (typeof stop == "undefined") stop = this.length;

  // Fastpath empty strings
  if (+stop == start) {
    return '';
  }

  switch (encoding) {
    case 'utf8':
    case 'utf-8':
      return this.utf8Slice(start, stop);

    case 'ascii':
      return this.asciiSlice(start, stop);

    case 'binary':
      return this.binarySlice(start, stop);

    case 'base64':
      return this.base64Slice(start, stop);

    default:
      throw new Error('Unknown encoding');
  }
};

MmapBuffer.prototype.write = function (string, offset, encoding) {
  // Support both (string, offset, encoding)
  // and the legacy (string, encoding, offset)
  if (!isFinite(offset)) {
    var swap = encoding;
    encoding = offset;
    offset = swap;
  }

  offset = +offset || 0;
  encoding = String(encoding || 'utf8').toLowerCase();

  switch (encoding) {
    case 'utf8':
    case 'utf-8':
      return this.utf8Write(string, offset);

    case 'ascii':
      return this.asciiWrite(string, offset);

    case 'binary':
      return this.binaryWrite(string, offset);

    case 'base64':
      return this.base64Write(string, offset);

    default:
      throw new Error('Unknown encoding');
  }
};

MmapBuffer.prototype.get = function (index) {
  return this[index];
};

MmapBuffer.prototype.set = function (index, value) {
  return this[index] = value;
};
