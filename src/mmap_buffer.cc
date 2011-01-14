#include "mmap_buffer.h"

#include <assert.h>
#include <stdlib.h> // malloc, free
#include <v8.h>

#include <string.h> // memcpy

#include <arpa/inet.h>  // htons, htonl

#include <node.h>

#include <errno.h>
#include <unistd.h>
#include <sys/mman.h>

#define MIN(a,b) ((a) < (b) ? (a) : (b))

namespace node {

using namespace v8;

#define SLICE_ARGS(start_arg, end_arg)                               \
  if (!start_arg->IsInt32() || !end_arg->IsInt32()) {                \
    return ThrowException(Exception::TypeError(                      \
          String::New("Bad argument.")));                            \
  }                                                                  \
  int32_t start = start_arg->Int32Value();                           \
  int32_t end = end_arg->Int32Value();                               \
  if (start < 0 || end < 0) {                                        \
    return ThrowException(Exception::TypeError(                      \
          String::New("Bad argument.")));                            \
  }                                                                  \
  if (!(start <= end)) {                                             \
    return ThrowException(Exception::Error(                          \
          String::New("Must have start <= end")));                   \
  }                                                                  \
  if ((size_t)end > parent->length_) {                               \
    return ThrowException(Exception::Error(                          \
          String::New("end cannot be longer than parent.length")));  \
  }


static Persistent<String> length_symbol;
static Persistent<String> chars_written_sym;
static Persistent<String> write_sym;
Persistent<FunctionTemplate> MmapBuffer::constructor_template;

// Each javascript Buffer object is backed by a Blob object.
// the Blob is just a C-level chunk of bytes.
// It has a reference count.
struct Blob_ {
  unsigned int refs;
  size_t length;
  char *data;
};
typedef struct Blob_ Blob;

static inline Blob * blob_new(char* data, size_t length) {
  Blob * blob  = (Blob*) malloc(sizeof(Blob));
  if (!blob) return NULL;

  blob->data = data; 

  V8::AdjustAmountOfExternalAllocatedMemory(sizeof(Blob));
  blob->length = length;
  blob->refs = 0;
  return blob;
}


static inline void blob_ref(Blob *blob) {
  blob->refs++;
}


static inline void blob_unref(Blob *blob) {
  assert(blob->refs > 0);
  if (--blob->refs == 0) {
    //fprintf(stderr, "free %d bytes\n", blob->length);
    V8::AdjustAmountOfExternalAllocatedMemory(-((int)sizeof(Blob)));
    char* data = blob->data;
    size_t length = blob->length;
    free(blob);
    if (data != MAP_FAILED) {
      if (munmap(data, length) < 0) {
        // does it make sense to raise an exception here? destructor is invoked by V8's GC
        ThrowException(ErrnoException(errno, "mmap"));
      }
    }
  }
}

static inline size_t base64_decoded_size(const char *src, size_t size) {
  const char *const end = src + size;
  const int remainder = size % 4;

  size = (size / 4) * 3;
  if (remainder) {
    if (size == 0 && remainder == 1) {
      // special case: 1-byte input cannot be decoded
      size = 0;
    } else {
      // non-padded input, add 1 or 2 extra bytes
      size += 1 + (remainder == 3);
    }
  }

  // check for trailing padding (1 or 2 bytes)
  if (size > 0) {
    if (end[-1] == '=') size--;
    if (end[-2] == '=') size--;
  }

  return size;
}

#if 0
// When someone calls buffer.asciiSlice, data is not copied. Instead V8
// references in the underlying Blob with this ExternalAsciiStringResource.
class AsciiSliceExt: public String::ExternalAsciiStringResource {
 friend class Buffer;
 public:
  AsciiSliceExt(Buffer *parent, size_t start, size_t end) {
    blob_ = parent->blob();
    blob_ref(blob_);

    assert(start <= end);
    length_ = end - start;
    assert(start + length_ <= parent->length());
    data_ = parent->data() + start;
  }


  ~AsciiSliceExt() {
    //fprintf(stderr, "free ascii slice (%d refs left)\n", blob_->refs);
    blob_unref(blob_);
  }


  const char* data() const { return data_; }
  size_t length() const { return length_; }

 private:
  const char *data_;
  size_t length_;
  Blob *blob_;
};
#endif

MmapBuffer* MmapBuffer::New(size_t size, int protection, int flags, int fd, off_t offset) {
  HandleScope scope;

  Local<Value> arg = Integer::NewFromUnsigned(size);
  Local<Object> b = constructor_template->GetFunction()->NewInstance(1, &arg);

  return ObjectWrap::Unwrap<MmapBuffer>(b);
}


Handle<Value> MmapBuffer::New(const Arguments &args) {
  HandleScope scope;

  if (!args.IsConstructCall()) {
    Local<Value> argv[10];
    for (int i = 0; i < MIN(args.Length(), 10); i++) {
      argv[i] = args[i];
    }
    Local<Object> instance =
      constructor_template->GetFunction()->NewInstance(args.Length(), argv);
    return scope.Close(instance);
  }

  MmapBuffer* buffer;

  if (MmapBuffer::HasInstance(args[0]) && args.Length() > 2) {
    // var slice = new Buffer(buffer, 123, 130);
    // args: parent, start, end
    MmapBuffer *parent = ObjectWrap::Unwrap<MmapBuffer>(args[0]->ToObject());
    SLICE_ARGS(args[1], args[2])
    buffer = new MmapBuffer(parent, start, end);
  } else {
    if (args.Length() <= 3) {
      return ThrowException(Exception::Error(String::New("Constructor takes 4 arguments: size, protection, flags, fd and offset.")));
    }
    const size_t size    = args[0]->ToInteger()->Value();
    const int protection = args[1]->ToInteger()->Value();
    const int flags      = args[2]->ToInteger()->Value();
    const int fd         = args[3]->ToInteger()->Value();
    const off_t offset   = args[4]->ToInteger()->Value();

    buffer = new MmapBuffer(size, protection, flags, fd, offset);
  }
  

  buffer->Wrap(args.This());
  args.This()->SetIndexedPropertiesToExternalArrayData(buffer->data(),
                                                       kExternalUnsignedByteArray,
                                                       buffer->length());
  args.This()->Set(length_symbol, Integer::New(buffer->length_));

  if (args[0]->IsString()) {
    if (write_sym.IsEmpty()) {
      write_sym = Persistent<String>::New(String::NewSymbol("write"));
    }

    Local<Value> write_v = args.This()->Get(write_sym);
    assert(write_v->IsFunction());
    Local<Function> write = Local<Function>::Cast(write_v);

    Local<Value> argv[2] = { args[0], args[1] };

    TryCatch try_catch;

    write->Call(args.This(), 2, argv);

    if (try_catch.HasCaught()) {
      FatalException(try_catch);
    }
  }

  return args.This();
}


MmapBuffer::MmapBuffer(size_t size, int protection, int flags, int fd, off_t offset) : ObjectWrap() {
  void* data = mmap(0, size, protection, flags, fd, 0);
  if (data == MAP_FAILED) {
    ThrowException(ErrnoException(errno, "mmap"));
  }

  off_ = 0;
  length_ = size;
  blob_ = blob_new((char*) data, size); 

  blob_ref(blob_);

  V8::AdjustAmountOfExternalAllocatedMemory(sizeof(MmapBuffer));
}


MmapBuffer::MmapBuffer(MmapBuffer *parent, size_t start, size_t end) : ObjectWrap() {
  blob_ = parent->blob_;
  assert(blob_->refs > 0);
  blob_ref(blob_);

  assert(start <= end);
  off_ = parent->off_ + start;
  length_ = end - start;
  assert(length_ <= parent->length_);

  V8::AdjustAmountOfExternalAllocatedMemory(sizeof(MmapBuffer));
}


MmapBuffer::~MmapBuffer() {
  assert(blob_->refs > 0);
  //fprintf(stderr, "free buffer (%d refs left)\n", blob_->refs);
  blob_unref(blob_);
  V8::AdjustAmountOfExternalAllocatedMemory(-static_cast<long int>(sizeof(MmapBuffer)));
}


char* MmapBuffer::data() {
  return blob_->data + off_;
}


Handle<Value> MmapBuffer::BinarySlice(const Arguments &args) {
  HandleScope scope;
  MmapBuffer *parent = ObjectWrap::Unwrap<MmapBuffer>(args.This());
  SLICE_ARGS(args[0], args[1])

  const char *data = const_cast<char*>(parent->data() + start);
  //Local<String> string = String::New(data, end - start);

  Local<Value> b =  Encode(data, end - start, BINARY);

  return scope.Close(b);
}


Handle<Value> MmapBuffer::AsciiSlice(const Arguments &args) {
  HandleScope scope;
  MmapBuffer *parent = ObjectWrap::Unwrap<MmapBuffer>(args.This());
  SLICE_ARGS(args[0], args[1])

#if 0
  AsciiSliceExt *ext = new AsciiSliceExt(parent, start, end);
  Local<String> string = String::NewExternal(ext);
  // There should be at least two references to the blob now - the parent
  // and the slice.
  assert(parent->blob_->refs >= 2);
#endif

  const char *data = const_cast<char*>(parent->data() + start);
  Local<String> string = String::New(data, end - start);


  return scope.Close(string);
}


Handle<Value> MmapBuffer::Utf8Slice(const Arguments &args) {
  HandleScope scope;
  MmapBuffer *parent = ObjectWrap::Unwrap<MmapBuffer>(args.This());
  SLICE_ARGS(args[0], args[1])
  const char *data = const_cast<char*>(parent->data() + start);
  Local<String> string = String::New(data, end - start);
  return scope.Close(string);
}

static const char *base64_table = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                  "abcdefghijklmnopqrstuvwxyz"
                                  "0123456789+/";
static const int unbase64_table[] =
  {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
  ,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
  ,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63
  ,52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1
  ,-1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14
  ,15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1
  ,-1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40
  ,41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1
  };


Handle<Value> MmapBuffer::Base64Slice(const Arguments &args) {
  HandleScope scope;
  MmapBuffer *parent = ObjectWrap::Unwrap<MmapBuffer>(args.This());
  SLICE_ARGS(args[0], args[1])

  int n = end - start;
  int out_len = (n + 2 - ((n + 2) % 3)) / 3 * 4;
  char *out = new char[out_len];

  uint8_t bitbuf[3];
  int i = start; // data() index
  int j = 0; // out index
  char c;
  bool b1_oob, b2_oob;

  while (i < end) {
    bitbuf[0] = parent->data()[i++];

    if (i < end) {
      bitbuf[1] = parent->data()[i];
      b1_oob = false;
    }  else {
      bitbuf[1] = 0;
      b1_oob = true;
    }
    i++;

    if (i < end) {
      bitbuf[2] = parent->data()[i];
      b2_oob = false;
    }  else {
      bitbuf[2] = 0;
      b2_oob = true;
    }
    i++;


    c = bitbuf[0] >> 2;
    assert(c < 64);
    out[j++] = base64_table[c];
    assert(j < out_len);

    c = ((bitbuf[0] & 0x03) << 4) | (bitbuf[1] >> 4);
    assert(c < 64);
    out[j++] = base64_table[c];
    assert(j < out_len);

    if (b1_oob) {
      out[j++] = '=';
    } else {
      c = ((bitbuf[1] & 0x0F) << 2) | (bitbuf[2] >> 6);
      assert(c < 64);
      out[j++] = base64_table[c];
    }
    assert(j < out_len);

    if (b2_oob) {
      out[j++] = '=';
    } else {
      c = bitbuf[2] & 0x3F;
      assert(c < 64);
      out[j++]  = base64_table[c];
    }
    assert(j <= out_len);
  }

  Local<String> string = String::New(out, out_len);
  delete [] out;
  return scope.Close(string);
}


Handle<Value> MmapBuffer::Slice(const Arguments &args) {
  HandleScope scope;
  Local<Value> argv[3] = { args.This(), args[0], args[1] };
  Local<Object> slice =
    constructor_template->GetFunction()->NewInstance(3, argv);
  return scope.Close(slice);
}


// var bytesCopied = buffer.copy(target, targetStart, sourceStart, sourceEnd);
Handle<Value> MmapBuffer::Copy(const Arguments &args) {
  HandleScope scope;

  MmapBuffer *source = ObjectWrap::Unwrap<MmapBuffer>(args.This());

  if (!MmapBuffer::HasInstance(args[0])) {
    return ThrowException(Exception::TypeError(String::New(
            "First arg should be a MmapBuffer")));
  }

  MmapBuffer *target = ObjectWrap::Unwrap<MmapBuffer>(args[0]->ToObject());

  ssize_t target_start = args[1]->Int32Value();
  ssize_t source_start = args[2]->Int32Value();
  ssize_t source_end = args[3]->IsInt32() ? args[3]->Int32Value()
                                          : source->length();

  if (source_end < source_start) {
    return ThrowException(Exception::Error(String::New(
            "sourceEnd < sourceStart")));
  }

  // Copy 0 bytes; we're done
  if (source_end == source_start) {
    return scope.Close(Integer::New(0));
  }

  if (target_start < 0 || target_start >= target->length()) {
    return ThrowException(Exception::Error(String::New(
            "targetStart out of bounds")));
  }

  if (source_start < 0 || source_start >= source->length()) {
    return ThrowException(Exception::Error(String::New(
            "sourceStart out of bounds")));
  }

  if (source_end < 0 || source_end > source->length()) {
    return ThrowException(Exception::Error(String::New(
            "sourceEnd out of bounds")));
  }

  ssize_t to_copy = MIN(source_end - source_start,
                        target->length() - target_start);

  if (target->blob_ == source->blob_) {
    // need to use slightly slower memmove is the ranges might overlap
    memmove((void*)(target->data() + target_start),
            (const void*)(source->data() + source_start),
            to_copy);
  } else {
    memcpy((void*)(target->data() + target_start),
           (const void*)(source->data() + source_start),
           to_copy);
  }

  return scope.Close(Integer::New(to_copy));
}


// var charsWritten = buffer.utf8Write(string, offset);
Handle<Value> MmapBuffer::Utf8Write(const Arguments &args) {
  HandleScope scope;
  MmapBuffer *buffer = ObjectWrap::Unwrap<MmapBuffer>(args.This());

  if (!args[0]->IsString()) {
    return ThrowException(Exception::TypeError(String::New(
            "Argument must be a string")));
  }

  Local<String> s = args[0]->ToString();

  size_t offset = args[1]->Int32Value();

  if (s->Utf8Length() > 0 && offset >= buffer->length_) {
    return ThrowException(Exception::TypeError(String::New(
            "Offset is out of bounds")));
  }

  const char *p = buffer->data() + offset;

  int char_written;

  int written = s->WriteUtf8((char*)p,
                             buffer->length_ - offset,
                             &char_written,
                             String::HINT_MANY_WRITES_EXPECTED);

  constructor_template->GetFunction()->Set(chars_written_sym,
                                           Integer::New(char_written));

  if (written > 0 && p[written-1] == '\0') written--;

  return scope.Close(Integer::New(written));
}


// var charsWritten = buffer.asciiWrite(string, offset);
Handle<Value> MmapBuffer::AsciiWrite(const Arguments &args) {
  HandleScope scope;

  MmapBuffer *buffer = ObjectWrap::Unwrap<MmapBuffer>(args.This());

  if (!args[0]->IsString()) {
    return ThrowException(Exception::TypeError(String::New(
            "Argument must be a string")));
  }

  Local<String> s = args[0]->ToString();

  size_t offset = args[1]->Int32Value();

  if (s->Length() > 0 && offset >= buffer->length_) {
    return ThrowException(Exception::TypeError(String::New(
            "Offset is out of bounds")));
  }

  const char *p = buffer->data() + offset;

  size_t towrite = MIN((unsigned long) s->Length(), buffer->length_ - offset);

  int written = s->WriteAscii((char*)p, 0, towrite, String::HINT_MANY_WRITES_EXPECTED);
  return scope.Close(Integer::New(written));
}

// var bytesWritten = buffer.base64Write(string, offset);
Handle<Value> MmapBuffer::Base64Write(const Arguments &args) {
  HandleScope scope;

  assert(unbase64_table['/'] == 63);
  assert(unbase64_table['+'] == 62);
  assert(unbase64_table['T'] == 19);
  assert(unbase64_table['Z'] == 25);
  assert(unbase64_table['t'] == 45);
  assert(unbase64_table['z'] == 51);

  MmapBuffer *buffer = ObjectWrap::Unwrap<MmapBuffer>(args.This());

  if (!args[0]->IsString()) {
    return ThrowException(Exception::TypeError(String::New(
            "Argument must be a string")));
  }

  String::AsciiValue s(args[0]->ToString());
  size_t offset = args[1]->Int32Value();

  // handle zero-length buffers graciously
  if (offset == 0 && buffer->length_ == 0) {
    return scope.Close(Integer::New(0));
  }

  if (offset >= buffer->length_) {
    return ThrowException(Exception::TypeError(String::New(
            "Offset is out of bounds")));
  }

  const size_t size = base64_decoded_size(*s, s.length());
  if (size > buffer->length_ - offset) {
    // throw exception, don't silently truncate
    return ThrowException(Exception::TypeError(String::New(
            "Buffer too small")));
  }

  char a, b, c, d;
  char *dst = buffer->data();
  const char *src = *s;
  const char *const srcEnd = src + s.length();

  while (src < srcEnd) {
    const int remaining = srcEnd - src;
    if (remaining == 0 || *src == '=') break;
    a = unbase64_table[*src++];

    if (remaining == 1 || *src == '=') break;
    b = unbase64_table[*src++];
    *dst++ = (a << 2) | ((b & 0x30) >> 4);

    if (remaining == 2 || *src == '=') break;
    c = unbase64_table[*src++];
    *dst++ = ((b & 0x0F) << 4) | ((c & 0x3C) >> 2);

    if (remaining == 3 || *src == '=') break;
    d = unbase64_table[*src++];
    *dst++ = ((c & 0x03) << 6) | (d & 0x3F);
  }

  return scope.Close(Integer::New(size));
}


Handle<Value> MmapBuffer::BinaryWrite(const Arguments &args) {
  HandleScope scope;

  MmapBuffer *buffer = ObjectWrap::Unwrap<MmapBuffer>(args.This());

  if (!args[0]->IsString()) {
    return ThrowException(Exception::TypeError(String::New(
            "Argument must be a string")));
  }

  Local<String> s = args[0]->ToString();

  size_t offset = args[1]->Int32Value();

  if (s->Length() > 0 && offset >= buffer->length_) {
    return ThrowException(Exception::TypeError(String::New(
            "Offset is out of bounds")));
  }

  char *p = (char*)buffer->data() + offset;

  size_t towrite = MIN((unsigned long) s->Length(), buffer->length_ - offset);

  int written = DecodeWrite(p, towrite, s, BINARY);
  return scope.Close(Integer::New(written));
}


// buffer.unpack(format, index);
// Starting at 'index', unpacks binary from the buffer into an array.
// 'format' is a string
//
//  FORMAT  RETURNS
//    N     uint32_t   a 32bit unsigned integer in network byte order
//    n     uint16_t   a 16bit unsigned integer in network byte order
//    o     uint8_t    a 8bit unsigned integer
Handle<Value> MmapBuffer::Unpack(const Arguments &args) {
  HandleScope scope;
  MmapBuffer *buffer = ObjectWrap::Unwrap<MmapBuffer>(args.This());

  if (!args[0]->IsString()) {
    return ThrowException(Exception::TypeError(String::New(
            "Argument must be a string")));
  }

  String::AsciiValue format(args[0]->ToString());
  uint32_t index = args[1]->Uint32Value();

#define OUT_OF_BOUNDS ThrowException(Exception::Error(String::New("Out of bounds")))

  Local<Array> array = Array::New(format.length());

  uint8_t  uint8;
  uint16_t uint16;
  uint32_t uint32;

  for (int i = 0; i < format.length(); i++) {
    switch ((*format)[i]) {
      // 32bit unsigned integer in network byte order
      case 'N':
        if (index + 3 >= buffer->length_) return OUT_OF_BOUNDS;
        uint32 = htonl(*(uint32_t*)(buffer->data() + index));
        array->Set(Integer::New(i), Integer::NewFromUnsigned(uint32));
        index += 4;
        break;

      // 16bit unsigned integer in network byte order
      case 'n':
        if (index + 1 >= buffer->length_) return OUT_OF_BOUNDS;
        uint16 = htons(*(uint16_t*)(buffer->data() + index));
        array->Set(Integer::New(i), Integer::NewFromUnsigned(uint16));
        index += 2;
        break;

      // a single octet, unsigned.
      case 'o':
        if (index >= buffer->length_) return OUT_OF_BOUNDS;
        uint8 = (uint8_t)buffer->data()[index];
        array->Set(Integer::New(i), Integer::NewFromUnsigned(uint8));
        index += 1;
        break;

      default:
        return ThrowException(Exception::Error(
              String::New("Unknown format character")));
    }
  }

  return scope.Close(array);
}


// var nbytes = MmapBuffer.byteLength("string", "utf8")
Handle<Value> MmapBuffer::ByteLength(const Arguments &args) {
  HandleScope scope;

  if (!args[0]->IsString()) {
    return ThrowException(Exception::TypeError(String::New(
            "Argument must be a string")));
  }

  Local<String> s = args[0]->ToString();
  enum encoding e = ParseEncoding(args[1], UTF8);

  Local<Integer> length =
    Integer::New(e == UTF8 ? s->Utf8Length() : s->Length());

  return scope.Close(length);
}


Handle<Value> MmapBuffer::MakeFastBuffer(const Arguments &args) {
  HandleScope scope;

  MmapBuffer *buffer = ObjectWrap::Unwrap<MmapBuffer>(args[0]->ToObject());
  Local<Object> fast_buffer = args[1]->ToObject();;
  uint32_t offset = args[2]->Uint32Value();
  uint32_t length = args[3]->Uint32Value();

  fast_buffer->SetIndexedPropertiesToPixelData((uint8_t*)buffer->data() + offset,
                                               length);

  return Undefined();
}

extern "C" void init(Handle<Object> target) {
    MmapBuffer::Initialize(target);
}

void MmapBuffer::Initialize(Handle<Object> target) {
  HandleScope scope;

  length_symbol = Persistent<String>::New(String::NewSymbol("length"));
  chars_written_sym = Persistent<String>::New(String::NewSymbol("_charsWritten"));

  Local<FunctionTemplate> t = FunctionTemplate::New(MmapBuffer::New);
  constructor_template = Persistent<FunctionTemplate>::New(t);
  constructor_template->InstanceTemplate()->SetInternalFieldCount(1);
  constructor_template->SetClassName(String::NewSymbol("MmapBuffer"));

  // copy free
  NODE_SET_PROTOTYPE_METHOD(constructor_template, "binarySlice", MmapBuffer::BinarySlice);
  NODE_SET_PROTOTYPE_METHOD(constructor_template, "asciiSlice", MmapBuffer::AsciiSlice);
  NODE_SET_PROTOTYPE_METHOD(constructor_template, "base64Slice", MmapBuffer::Base64Slice);
  NODE_SET_PROTOTYPE_METHOD(constructor_template, "slice", MmapBuffer::Slice);
  // TODO NODE_SET_PROTOTYPE_METHOD(t, "utf16Slice", Utf16Slice);
  // copy
  NODE_SET_PROTOTYPE_METHOD(constructor_template, "utf8Slice", MmapBuffer::Utf8Slice);

  NODE_SET_PROTOTYPE_METHOD(constructor_template, "utf8Write", MmapBuffer::Utf8Write);
  NODE_SET_PROTOTYPE_METHOD(constructor_template, "asciiWrite", MmapBuffer::AsciiWrite);
  NODE_SET_PROTOTYPE_METHOD(constructor_template, "binaryWrite", MmapBuffer::BinaryWrite);
  NODE_SET_PROTOTYPE_METHOD(constructor_template, "base64Write", MmapBuffer::Base64Write);
  NODE_SET_PROTOTYPE_METHOD(constructor_template, "unpack", MmapBuffer::Unpack);
  NODE_SET_PROTOTYPE_METHOD(constructor_template, "copy", MmapBuffer::Copy);

  NODE_SET_METHOD(constructor_template->GetFunction(),
                  "byteLength",
                  MmapBuffer::ByteLength);
  NODE_SET_METHOD(constructor_template->GetFunction(),
                  "makeFastBuffer",
                  MmapBuffer::MakeFastBuffer);

  target->Set(String::NewSymbol("MmapBuffer"), constructor_template->GetFunction());

  const PropertyAttribute attribs = (PropertyAttribute) (ReadOnly | DontDelete);
  target->Set(String::New("PROT_READ"),   Integer::New(PROT_READ),   attribs);
  target->Set(String::New("PROT_WRITE"),  Integer::New(PROT_WRITE),  attribs);
  target->Set(String::New("PROT_EXEC"),   Integer::New(PROT_EXEC),   attribs);
  target->Set(String::New("PROT_NONE"),   Integer::New(PROT_NONE),   attribs);
  target->Set(String::New("MAP_SHARED"),  Integer::New(MAP_SHARED),  attribs);
  target->Set(String::New("MAP_PRIVATE"), Integer::New(MAP_PRIVATE), attribs);
  target->Set(String::New("PAGESIZE"),    Integer::New(sysconf(_SC_PAGESIZE)), attribs);
}


}  // namespace node

//NODE_MODULE(node_buffer, node::MmapBuffer::Initialize);
