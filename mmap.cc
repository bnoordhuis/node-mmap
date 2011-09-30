#include <v8.h>
#include <node.h>
#include <node_buffer.h>

#include <errno.h>
#include <unistd.h>

#include <sys/mman.h>

namespace {

using namespace v8;
using namespace node;

Handle<Value> MSync(const Arguments& args) {
	if (args.Length() < 2) {
		return ThrowException(Exception::Error(String::New("Msync takes two argument: Buffer, flag(MS_ASYNC or MS_SYNC).")));
	}
	Buffer* buffer = ObjectWrap::Unwrap<Buffer>(args[0]->ToObject());
	const int flag = args[1]->ToInteger()->Value();
	int err = msync((void *)Buffer::Data(buffer), Buffer::Length(buffer), flag);
	if (err == -1) {
		return ThrowException(ErrnoException(errno, "msync", ""));
	} else {
		return True();
	}
}

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

		return buffer->handle_;
	}
}

extern "C" void init(Handle<Object> target) {
	HandleScope scope;

	const PropertyAttribute attribs = (PropertyAttribute) (ReadOnly | DontDelete);

	target->Set(String::New("PROT_READ"), Integer::New(PROT_READ), attribs);
	target->Set(String::New("PROT_WRITE"), Integer::New(PROT_WRITE), attribs);
	target->Set(String::New("PROT_EXEC"), Integer::New(PROT_EXEC), attribs);
	target->Set(String::New("PROT_NONE"), Integer::New(PROT_NONE), attribs);
	target->Set(String::New("MAP_SHARED"), Integer::New(MAP_SHARED), attribs);
	target->Set(String::New("MAP_PRIVATE"), Integer::New(MAP_PRIVATE), attribs);
	target->Set(String::New("PAGESIZE"), Integer::New(sysconf(_SC_PAGESIZE)),
			attribs);
	target->Set(String::New("MS_ASYNC"), Integer::New(MS_ASYNC), attribs);
	target->Set(String::New("MS_SYNC"), Integer::New(MS_SYNC), attribs);

	target->Set(String::NewSymbol("map"), FunctionTemplate::New(Map)->GetFunction());
	target->Set(String::NewSymbol("msync"), FunctionTemplate::New(MSync)->GetFunction());
}

}
