#include <v8.h>
#include <node.h>
#include <node_buffer.h>

#include <errno.h>
#include <unistd.h>

#include <sys/mman.h>

namespace {

using namespace v8;
using namespace node;

Persistent<String> parent_sym;

void Unmap(char* data, void* hint) {
	munmap(data, (size_t) hint);
}

Handle<Value> Map(const Arguments& args) {
	HandleScope scope;

	if (args.Length() <= 3) {
		return ThrowException(Exception::Error(
				String::New("Constructor takes 4 arguments: size, protection, flags, fd and offset.")));
	}

	const size_t size = args[0]->ToInteger()->Value();
	const int protection = args[1]->ToInteger()->Value();
	const int flags = args[2]->ToInteger()->Value();
	const int fd = args[3]->ToInteger()->Value();
	const off_t offset = args[4]->ToInteger()->Value();

	char* data = (char *) mmap(
			0, size, protection, flags, fd, offset);

	if (data == MAP_FAILED) {
		return ThrowException(
				ErrnoException(errno, "mmap", ""));
	}
	else {
		Buffer* buffer = Buffer::New(
				data, size, Unmap, (void *) size);

		// simulate fast buffer
		buffer->handle_->Set(parent_sym, buffer->handle_);

		return buffer->handle_;
	}
}

extern "C" void init(Handle<Object> target) {
	HandleScope scope;

	const PropertyAttribute attribs = (PropertyAttribute) (ReadOnly | DontDelete);

	parent_sym = Persistent<String>::New(String::NewSymbol("parent"));

	target->Set(String::New("PROT_READ"), Integer::New(PROT_READ), attribs);
	target->Set(String::New("PROT_WRITE"), Integer::New(PROT_WRITE), attribs);
	target->Set(String::New("PROT_EXEC"), Integer::New(PROT_EXEC), attribs);
	target->Set(String::New("PROT_NONE"), Integer::New(PROT_NONE), attribs);
	target->Set(String::New("MAP_SHARED"), Integer::New(MAP_SHARED), attribs);
	target->Set(String::New("MAP_PRIVATE"), Integer::New(MAP_PRIVATE), attribs);
	target->Set(String::New("PAGESIZE"), Integer::New(sysconf(_SC_PAGESIZE)),
			attribs);

	target->Set(String::NewSymbol("map"), FunctionTemplate::New(Map)->GetFunction());
}

}
