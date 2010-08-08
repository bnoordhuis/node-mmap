# node-mmap

mmap(2) bindings for node.js - stop slurping, start mapping.

## Compiling

Easy as pie:

	node-waf configure build install

## Usage

    buffer = new mmap.Buffer(n_bytes, protection, flags, fd, offset);

<table>
  <tr>
    <td><i>n_bytes</i></td>
    <td>The number of bytes to map into memory.</td>
  </tr>
  <tr>
    <td><i>protection</i></td>
    <td>Memory protection: either <b>PROT_NONE</b> or a bitwise OR of <b>PROT_READ</b>, <b>PROT_WRITE</b> and <b>PROT_EXEC</b>.</td>
  </tr>
  <tr>
    <td><i>flags</i></td>
    <td>Flags: either <b>MAP_SHARED</b> or <b>MAP_PRIVATE</b>.</td>
  </tr>
  <tr>
    <td><i>fd</i></td>
    <td>File descriptor.</td>
  </tr>
  <tr>
    <td><i>offset</i></td>
    <td>File offset. Must be either zero or a multiple of <b>mmap.PAGESIZE</b>.</td>
  </tr>
</table>

See http://www.opengroup.org/onlinepubs/000095399/functions/mmap.html for more details.

## Examples

Map a file into memory:

    fs = require('fs'), mmap = require('mmap');
    fd = fs.openSync('/path/to/file', 'r');
    size = fs.fstatSync(fd).size;
    buffer = new mmap.Buffer(size, mmap.PROT_READ, mmap.MAP_SHARED, fd, 0);
    // calculate faux checksum
    var checksum = 0;
    for (var i = 0; i < buffer.length; i++) {
      checksum ^= buffer[i];
    }

The file is automatically unmapped when the buffer object is garbage collected.

## Limitations

* Specifying the memory address is not implemented. I couldn't think of a reason why you would want to do that from JavaScript. Convince me otherwise. :-)