fs = require('fs');
sys = require('sys');
mmap = require('mmap');
assert = require('assert');

Buffer = mmap.Buffer;
PAGESIZE = mmap.PAGESIZE;
PROT_READ = mmap.PROT_READ;
MAP_SHARED = mmap.MAP_SHARED;

_ = function(what) { sys.puts(sys.inspect(what)); };

// open self (this script)
fd = fs.openSync(process.argv[1], 'r');
size = fs.fstatSync(fd).size;

// full 5-arg constructor
buffer = new Buffer(size, PROT_READ, MAP_SHARED, fd, 0);
assert.equal(buffer.length, size);

// short-hand 4-arg constructor
buffer = new Buffer(size, PROT_READ, MAP_SHARED, fd);
assert.equal(buffer.length, size);

// page size is almost certainly >= 4K and this script isn't that large...
fd = fs.openSync(process.argv[1], 'r');
buffer = new Buffer(size, PROT_READ, MAP_SHARED, fd, PAGESIZE);
assert.equal(buffer.length, size);	// ...but this is according to spec

// zero size should throw exception 
fd = fs.openSync(process.argv[1], 'r');
try {
  buffer = new Buffer(0, PROT_READ, MAP_SHARED, fd, 0);
} catch (e) {
  assert.equal(e.errno, process.EINVAL);
}

// non-page size offset should throw exception
if (PAGESIZE != 1) {
  fd = fs.openSync(process.argv[1], 'r');
  try {
    buffer = new Buffer(size, PROT_READ, MAP_SHARED, fd, 1);
  } catch (e) {
    assert.equal(e.errno, process.EINVAL);
  }
}
