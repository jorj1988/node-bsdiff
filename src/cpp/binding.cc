/*

    Copyright 2012 Michael Phan-Ba

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

        http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.

*/

#include <vector>

#include <node.h>
#include <node_buffer.h>
#include <v8.h>
#include <uv.h>

#include "boost/endian.hpp"
#ifdef BOOST_BIG_ENDIAN
# include "gnuclib/byteswap.h"
#endif

#include "bsdiff.h"

using namespace node;
using namespace v8;

namespace node_bsdiff {
namespace {

class async_stub : public bsdiff_dat {
 public:
  int err;

  Persistent<Value> curHandle;
  Persistent<Value> refHandle;
  Persistent<Value> ctrlHandle;
  Persistent<Value> diffHandle;
  Persistent<Value> xtraHandle;

  Persistent<Function> callback;

  async_stub() : bsdiff_dat(), err(0) {
  }

};

static inline Handle<Value> ThrowTypeError(const char *err) {
  return Isolate::GetCurrent()->ThrowException(
    Exception::TypeError(String::NewFromUtf8(Isolate::GetCurrent(), err)));
}

static inline void Error(async_stub *shim) {
  const char *msg = shim->err == -1 ? "Corrupt data" : "Internal error";
  Handle<Value> argv[] = { Exception::Error(String::NewFromUtf8(Isolate::GetCurrent(), msg)) };
  TryCatch tryCatch;
  v8::Local<v8::Function> cb = v8::Local<v8::Function>::New(Isolate::GetCurrent(), shim->callback);
  v8::Local<v8::Context> context = Isolate::GetCurrent()->GetCurrentContext();
  cb->Call(context->Global(), 1, argv);
  if (tryCatch.HasCaught()) FatalException(tryCatch);
}

static void DeleteMemory(char *data, void *hint) {
  delete data;
}

static void AsyncDiff(uv_work_t *req) {
  async_stub *shim = static_cast<async_stub *>(req->data);
  shim->err = bsdiff(shim);
}

static void AfterDiff(uv_work_t *req, int) {
  HandleScope scope(Isolate::GetCurrent());
  async_stub *shim = static_cast<async_stub *>(req->data);

  if (shim->err != 0) return Error(shim);

#ifdef BOOST_BIG_ENDIAN
  for (size_t i = shim->ctrl.size() - 1; i >= 0; --it)
    shim->ctrl[i] = bswap_32(shim->ctrl[i]);
#endif

  v8::Local<v8::Object> ctrl = Buffer::New(reinterpret_cast<char *>(shim->ctrl.data()),
                             shim->ctrl.size() * sizeof(int));
  v8::Local<v8::Object> diff = Buffer::New(shim->diff, shim->difflen, DeleteMemory, NULL);
  v8::Local<v8::Object> xtra = Buffer::New(shim->xtra, shim->xtralen, DeleteMemory, NULL);

  Handle<Value> argv[] = { Null(Isolate::GetCurrent()), ctrl, diff, xtra };
  TryCatch tryCatch;

  v8::Local<v8::Function> cb = v8::Local<v8::Function>::New(Isolate::GetCurrent(), shim->callback);
  v8::Local<v8::Context> context = Isolate::GetCurrent()->GetCurrentContext();
  cb->Call(context->Global(), 4, argv);
  if (tryCatch.HasCaught()) FatalException(tryCatch);

  delete shim;
  delete req;
}

static void AsyncPatch(uv_work_t *req) {
  async_stub *shim = static_cast<async_stub *>(req->data);
  shim->err = bspatch(shim);
}

static void AfterPatch(uv_work_t *req, int) {
  HandleScope scope(Isolate::GetCurrent());
  async_stub *shim = static_cast<async_stub *>(req->data);

  if (shim->err != 0) return Error(shim);

  v8::Local<v8::Object> cur = Buffer::New(shim->curdat, shim->curlen, DeleteMemory, NULL);

  Handle<Value> argv[] = { Null(Isolate::GetCurrent()), cur };
  TryCatch tryCatch;

  v8::Local<v8::Function> cb = v8::Local<v8::Function>::New(Isolate::GetCurrent(), shim->callback);
  v8::Local<v8::Context> context = Isolate::GetCurrent()->GetCurrentContext();
  cb->Call(context->Global(), 2, argv);
  if (tryCatch.HasCaught()) FatalException(tryCatch);

  delete shim;
  delete req;
}

} // anonymous namespace

void Diff(const v8::FunctionCallbackInfo<v8::Value>& info) {
  HandleScope scope(Isolate::GetCurrent());

  if (info.Length() != 3 ||
      !Buffer::HasInstance(info[0]) ||  // current
      !Buffer::HasInstance(info[1]) ||  // reference
      !info[2]->IsFunction())           // callback
    ThrowTypeError("Invalid arguments");
    return;

  Local<Object> cur = info[0]->ToObject();
  Local<Object> ref = info[1]->ToObject();
  Local<Function> callback = Local<Function>::Cast(info[2]);

  uv_work_t *req = new uv_work_t;
  async_stub *shim = new async_stub;

  shim->curdat = Buffer::Data(cur);
  shim->refdat = Buffer::Data(ref);

  shim->curlen = Buffer::Length(cur);
  shim->reflen = Buffer::Length(ref);

  shim->curHandle.Reset(Isolate::GetCurrent(), cur);
  shim->refHandle.Reset(Isolate::GetCurrent(), ref);

  shim->callback.Reset(Isolate::GetCurrent(), callback);

  req->data = shim;
  uv_queue_work(uv_default_loop(), req, AsyncDiff, AfterDiff);
}

void Patch(const v8::FunctionCallbackInfo<v8::Value>& info) {
  HandleScope scope(Isolate::GetCurrent());

  if (info.Length() != 6 ||
      !info[0]->IsNumber() ||           // current
      !Buffer::HasInstance(info[1]) ||  // reference
      !Buffer::HasInstance(info[2]) ||  // control
      !Buffer::HasInstance(info[3]) ||  // diff
      !Buffer::HasInstance(info[4]) ||  // extra
      !info[5]->IsFunction())           // callback
    ThrowTypeError("Invalid arguments");
    return;

  uint32_t curlen = info[0]->Uint32Value();
  Local<Object> ref = info[1]->ToObject();
  Local<Object> ctrl = info[2]->ToObject();
  Local<Object> diff = info[3]->ToObject();
  Local<Object> xtra = info[4]->ToObject();
  Local<Function> callback = Local<Function>::Cast(info[5]);

  uv_work_t *req = new uv_work_t;
  async_stub *shim = new async_stub;

  const int *ctrldat = reinterpret_cast<int *>(Buffer::Data(ctrl));
  const size_t ctrllen = Buffer::Length(ctrl);

#ifdef BOOST_BIG_ENDIAN
  for (size_t i = ctrllen - 1; i >= 0; --it)
    ctrldat[i] = bswap_32(ctrldat[i]);
#endif

  shim->ctrl.assign(ctrldat, ctrldat + ctrllen);

  shim->refdat = Buffer::Data(ref);
  shim->diff = Buffer::Data(diff);
  shim->xtra = Buffer::Data(xtra);

  shim->curlen = curlen;
  shim->reflen = Buffer::Length(ref);
  shim->difflen = Buffer::Length(diff);
  shim->xtralen = Buffer::Length(xtra);

  shim->refHandle.Reset(Isolate::GetCurrent(), ref);
  shim->ctrlHandle.Reset(Isolate::GetCurrent(), ctrl);
  shim->diffHandle.Reset(Isolate::GetCurrent(), diff);
  shim->xtraHandle.Reset(Isolate::GetCurrent(), xtra);

  shim->callback.Reset(Isolate::GetCurrent(), callback);

  req->data = shim;
  uv_queue_work(uv_default_loop(), req, AsyncPatch, AfterPatch);
}

extern "C" {

static void init(Handle<Object> target) {
  HandleScope scope(Isolate::GetCurrent());
  NODE_SET_METHOD(target, "diff", Diff);
  NODE_SET_METHOD(target, "patch", Patch);
}

NODE_MODULE(bsdiff, init);

} // extern
} // namespace node_bsdiff
