#include <v8.h>
#include <node.h>
#include <errno.h>
#include <unistd.h>
#include <sys/mman.h>

using namespace v8;
using namespace node;

namespace {

class Buffer: public ObjectWrap {
public:
  Buffer(size_t size, int protection, int flags, int fd, off_t offset);
  ~Buffer();

  static Handle<Value> New(const Arguments& args);

private:
  static Persistent<String> length_symbol;
  const size_t size;
  void* const data;
};

Persistent<String> Buffer::length_symbol = Persistent<String>::New(String::NewSymbol("length"));

Buffer::Buffer(size_t size, int protection, int flags, int fd, off_t offset): size(size), data(mmap(0, size, protection, flags, fd, 0)) {
  if (data == MAP_FAILED) {
    ThrowException(ErrnoException(errno, "mmap"));
  }
}

Buffer::~Buffer() {
  if (data != MAP_FAILED) {
    if (munmap(data, size) < 0) {
      // does it make sense to raise an exception here? destructor is invoked by V8's GC
      ThrowException(ErrnoException(errno, "mmap"));
    }
  }
}

Handle<Value> Buffer::New(const Arguments& args) {
  HandleScope scope;

  if (args.Length() <= 3) {
    return ThrowException(Exception::Error(String::New("Constructor takes 4 arguments: size, protection, flags, fd and offset.")));
  }

  const size_t size    = args[0]->ToInteger()->Value();
  const int protection = args[1]->ToInteger()->Value();
  const int flags      = args[2]->ToInteger()->Value();
  const int fd         = args[3]->ToInteger()->Value();
  const off_t offset   = args[4]->ToInteger()->Value();

  Buffer* instance = new Buffer(size, protection, flags, fd, offset);
  instance->Wrap(args.Holder());

  args.This()->SetIndexedPropertiesToExternalArrayData(instance->data, kExternalUnsignedByteArray, instance->size);
  args.This()->Set(length_symbol, Integer::New(instance->size), (PropertyAttribute) (ReadOnly | DontDelete));
  return args.This();
}

extern "C" void init(Handle<Object> target) {
  HandleScope scope;

  Local<FunctionTemplate> t = FunctionTemplate::New(Buffer::New);
  t->InstanceTemplate()->SetInternalFieldCount(1);
  target->Set(String::NewSymbol("Buffer"), t->GetFunction());

  const PropertyAttribute attribs = (PropertyAttribute) (ReadOnly | DontDelete);
  target->Set(String::New("PROT_READ"),   Integer::New(PROT_READ),   attribs);
  target->Set(String::New("PROT_WRITE"),  Integer::New(PROT_WRITE),  attribs);
  target->Set(String::New("PROT_EXEC"),   Integer::New(PROT_EXEC),   attribs);
  target->Set(String::New("PROT_NONE"),   Integer::New(PROT_NONE),   attribs);
  target->Set(String::New("MAP_SHARED"),  Integer::New(MAP_SHARED),  attribs);
  target->Set(String::New("MAP_PRIVATE"), Integer::New(MAP_PRIVATE), attribs);
  target->Set(String::New("PAGESIZE"),    Integer::New(sysconf(_SC_PAGESIZE)), attribs);
}

}
