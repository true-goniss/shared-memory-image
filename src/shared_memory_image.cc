/*
    gon_iss (c) 2025

    https://github.com/true-goniss/shared-memory-image

*/

#include <node.h>
#include <node_buffer.h>
#include <node_object_wrap.h> // class instances
#include <Windows.h>
#include <cstdint>
#include <cstring>
#include <string>
#include <emmintrin.h> // _mm_pause()

using v8::FunctionCallbackInfo;
using v8::FunctionTemplate;
using v8::Isolate;
using v8::Local;
using v8::Object;
using v8::String;
using v8::Value;
using v8::Number;
using v8::Integer;
using v8::Boolean;
using v8::Context;
using v8::Exception;
using v8::Function;

#define SHARED_MAGIC 0x5348444D

// helper: portable 64-bit atomic increment
static inline uint64_t AtomicIncrement64(volatile LONG64* p) {
#if defined(_WIN64)
    return InterlockedIncrement64(p);
#else
    // 32-bit: implement via CAS loop using InterlockedCompareExchange64
    LONG64 oldval, newval;
    do {
        oldval = *p;
        newval = oldval + 1;
    } while (InterlockedCompareExchange64(p, newval, oldval) != oldval);
    return static_cast<uint64_t>(newval);
#endif
}

// no-op free for external Buffer
static void noop_free(char* /*data*/, void* /*hint*/) { /* no-op */ }

// Shared header layout
#pragma pack(push,1)
struct SharedHeader {
  uint32_t magic;        // 0x5348444D 'SHDM'
  uint32_t version;      // 1
  volatile LONG seq;     // sequence counter (seqlock)
  uint32_t width;
  uint32_t height;
  uint32_t channels;
  uint32_t frame_size;   // bytes of current frame
  uint64_t frame_index;  // ever increasing frame counter
  uint64_t mapping_size; // total mapping size
  uint8_t reserved[48];
};
#pragma pack(pop)

static const size_t HEADER_SIZE = ((sizeof(SharedHeader) + 63) / 64) * 64;

class SharedMemory : public node::ObjectWrap {
public:
  static void Init(Local<Object> exports);

private:
  explicit SharedMemory();
  ~SharedMemory();

  static void New(const FunctionCallbackInfo<Value>& args);
  
  // Methods mapped to JS
  static void Create(const FunctionCallbackInfo<Value>& args);
  static void SetFormat(const FunctionCallbackInfo<Value>& args);
  static void GetFrameBuffer(const FunctionCallbackInfo<Value>& args);
  static void GetCapacity(const FunctionCallbackInfo<Value>& args);
  static void PublishFrame(const FunctionCallbackInfo<Value>& args);
  static void ReadFrame(const FunctionCallbackInfo<Value>& args);
  static void Close(const FunctionCallbackInfo<Value>& args);
  static void GetMetadata(const FunctionCallbackInfo<Value>& args);

  // Internal helpers
  SharedHeader* headerPtr() { return reinterpret_cast<SharedHeader*>((uint8_t*)base_); }
  void* dataPtr() { return (uint8_t*)base_ + HEADER_SIZE; }
  size_t dataCapacity();

  // Member variables
  HANDLE hMap_ = nullptr;
  void* base_ = nullptr;
  size_t mapSize_ = 0;
  HANDLE hEvent_ = nullptr;
  std::string eventName_;
};

// --- Implementation ---

SharedMemory::SharedMemory() {}

SharedMemory::~SharedMemory() {
  if (base_) UnmapViewOfFile(base_);
  if (hMap_) CloseHandle(hMap_);
  if (hEvent_) CloseHandle(hEvent_);
}

size_t SharedMemory::dataCapacity() {
  if (mapSize_ <= HEADER_SIZE) return 0;
  return mapSize_ - HEADER_SIZE;
}

void SharedMemory::Init(Local<Object> exports) {
  Isolate* isolate = exports->GetIsolate();
  Local<Context> context = isolate->GetCurrentContext();

  Local<FunctionTemplate> tpl = FunctionTemplate::New(isolate, New);
  tpl->SetClassName(String::NewFromUtf8(isolate, "SharedMemory").ToLocalChecked());
  tpl->InstanceTemplate()->SetInternalFieldCount(1);

  // Prototype methods
  NODE_SET_PROTOTYPE_METHOD(tpl, "create", Create);
  NODE_SET_PROTOTYPE_METHOD(tpl, "setFormat", SetFormat);
  NODE_SET_PROTOTYPE_METHOD(tpl, "getFrameBuffer", GetFrameBuffer);
  NODE_SET_PROTOTYPE_METHOD(tpl, "getCapacity", GetCapacity);
  NODE_SET_PROTOTYPE_METHOD(tpl, "publishFrame", PublishFrame);
  NODE_SET_PROTOTYPE_METHOD(tpl, "readFrame", ReadFrame);
  NODE_SET_PROTOTYPE_METHOD(tpl, "close", Close);
  NODE_SET_PROTOTYPE_METHOD(tpl, "getMetadata", GetMetadata);

  Local<Function> constructor = tpl->GetFunction(context).ToLocalChecked();
  exports->Set(context, String::NewFromUtf8(isolate, "SharedMemory").ToLocalChecked(), constructor).Check();
}

void SharedMemory::New(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = args.GetIsolate();
  Local<Context> context = isolate->GetCurrentContext();

  if (args.IsConstructCall()) {
    // been called with 'new SharedMemory()' - ok
    SharedMemory* obj = new SharedMemory();
    obj->Wrap(args.This());
    args.GetReturnValue().Set(args.This());
  } else {
    // been called as 'SharedMemory()' without new;
    // throwing an exception.
    isolate->ThrowException(Exception::TypeError(
        String::NewFromUtf8(isolate, "Class constructors cannot be invoked without 'new'").ToLocalChecked()));
  }
}

// --- Method Implementations ---

void SharedMemory::Create(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = args.GetIsolate();
  SharedMemory* obj = ObjectWrap::Unwrap<SharedMemory>(args.Holder());

  if (args.Length() < 2 || !args[0]->IsString() || !args[1]->IsNumber()) {
    isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "Args: name, size").ToLocalChecked()));
    return;
  }

  // Cleanup if already opened
  if (obj->base_) { UnmapViewOfFile(obj->base_); obj->base_ = nullptr; }
  if (obj->hMap_) { CloseHandle(obj->hMap_); obj->hMap_ = nullptr; }

  String::Utf8Value name(isolate, args[0]);
  std::string mapName(*name);
  uint64_t requestedSize = (uint64_t)args[1]->IntegerValue(isolate->GetCurrentContext()).FromJust();

  uint32_t width = 0, height = 0, channels = 0;
  if (args.Length() >= 5) {
    width = (uint32_t)args[2]->IntegerValue(isolate->GetCurrentContext()).FromJust();
    height = (uint32_t)args[3]->IntegerValue(isolate->GetCurrentContext()).FromJust();
    channels = (uint32_t)args[4]->IntegerValue(isolate->GetCurrentContext()).FromJust();
  }

  obj->hMap_ = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, mapName.c_str());
  bool isCreator = false;

  if (!obj->hMap_) {
    DWORD sizeLow = static_cast<DWORD>(requestedSize & 0xFFFFFFFF);
    DWORD sizeHigh = static_cast<DWORD>((requestedSize >> 32) & 0xFFFFFFFF);
    obj->hMap_ = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, sizeHigh, sizeLow, mapName.c_str());
    if (!obj->hMap_) {
      isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "CreateFileMapping failed").ToLocalChecked()));
      return;
    }
    isCreator = true;
  }

  obj->base_ = MapViewOfFile(obj->hMap_, FILE_MAP_ALL_ACCESS, 0, 0, 0);
  if (!obj->base_) {
    isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "MapViewOfFile failed").ToLocalChecked()));
    return;
  }

  MEMORY_BASIC_INFORMATION mbi;
  VirtualQuery(obj->base_, &mbi, sizeof(mbi));
  obj->mapSize_ = mbi.RegionSize;

  // Event setup
  obj->eventName_ = "Global\\SHM_EV_" + mapName;
  obj->hEvent_ = OpenEventA(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, obj->eventName_.c_str());
  if (!obj->hEvent_) {
    obj->eventName_ = "Local\\SHM_EV_" + mapName;
    obj->hEvent_ = CreateEventA(nullptr, FALSE, FALSE, obj->eventName_.c_str());
  }

  SharedHeader* hdr = obj->headerPtr();
  if (isCreator) {
    // Initialize header
    ZeroMemory(hdr, sizeof(SharedHeader));
    hdr->magic = SHARED_MAGIC;
    hdr->version = 1;
    hdr->width = width;
    hdr->height = height;
    hdr->channels = channels;
    hdr->mapping_size = obj->mapSize_;
  }

  args.GetReturnValue().Set(String::NewFromUtf8(isolate, "ok").ToLocalChecked());
}

void SharedMemory::SetFormat(const FunctionCallbackInfo<Value>& args) {
  SharedMemory* obj = ObjectWrap::Unwrap<SharedMemory>(args.Holder());
  if (!obj->base_) return;

  uint32_t w = args[0]->IntegerValue(args.GetIsolate()->GetCurrentContext()).FromJust();
  uint32_t h = args[1]->IntegerValue(args.GetIsolate()->GetCurrentContext()).FromJust();
  uint32_t c = args[2]->IntegerValue(args.GetIsolate()->GetCurrentContext()).FromJust();

  SharedHeader* hdr = obj->headerPtr();
  InterlockedIncrement(&hdr->seq);
  MemoryBarrier();
  hdr->width = w; hdr->height = h; hdr->channels = c;
  MemoryBarrier();
  InterlockedIncrement(&hdr->seq);

  if (obj->hEvent_) SetEvent(obj->hEvent_);
  args.GetReturnValue().Set(true);
}

void SharedMemory::GetFrameBuffer(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = args.GetIsolate();
  SharedMemory* obj = ObjectWrap::Unwrap<SharedMemory>(args.Holder());
  
  if (!obj->base_ || obj->dataCapacity() == 0) {
    args.GetReturnValue().Set(v8::Null(isolate));
    return;
  }

  // Zero-Copy view of the memory
  char* ptr = static_cast<char*>(obj->dataPtr());
  Local<Object> buf = node::Buffer::New(isolate, ptr, obj->dataCapacity(), noop_free, nullptr).ToLocalChecked();
  args.GetReturnValue().Set(buf);
}

void SharedMemory::GetCapacity(const FunctionCallbackInfo<Value>& args) {
  SharedMemory* obj = ObjectWrap::Unwrap<SharedMemory>(args.Holder());
  args.GetReturnValue().Set(Number::New(args.GetIsolate(), (double)obj->dataCapacity()));
}

void SharedMemory::PublishFrame(const FunctionCallbackInfo<Value>& args) {
  SharedMemory* obj = ObjectWrap::Unwrap<SharedMemory>(args.Holder());
  if (!obj->base_) return;

  uint32_t frameBytes = args[0]->IntegerValue(args.GetIsolate()->GetCurrentContext()).FromJust();
  if (frameBytes > obj->dataCapacity()) return;

  SharedHeader* hdr = obj->headerPtr();
  InterlockedIncrement(&hdr->seq); // Start write
  MemoryBarrier();
  
  hdr->frame_size = frameBytes;
  AtomicIncrement64((volatile LONG64*)&hdr->frame_index);
  
  MemoryBarrier();
  InterlockedIncrement(&hdr->seq); // End write

  if (obj->hEvent_) SetEvent(obj->hEvent_);
  args.GetReturnValue().Set(true);
}

void SharedMemory::ReadFrame(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = args.GetIsolate();
  SharedMemory* obj = ObjectWrap::Unwrap<SharedMemory>(args.Holder());
  
  if (!obj->base_) {
     isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Not connected").ToLocalChecked()));
     return;
  }

  DWORD timeout = INFINITE;
  if (args.Length() > 0 && args[0]->IsNumber()) {
    timeout = args[0]->IntegerValue(isolate->GetCurrentContext()).FromJust();
  }

  // Wait for event (sleeping wait)
  if (obj->hEvent_) {
    if (WaitForSingleObject(obj->hEvent_, timeout) == WAIT_TIMEOUT) {
      args.GetReturnValue().Set(v8::Null(isolate));
      return;
    }
  }

  SharedHeader* hdr = obj->headerPtr();
  uint32_t startSeq, endSeq;
  uint32_t frameBytes;
  const int MAX_RETRIES = 10;
  int retries = 0;
  
  // Seqlock read loop with spin-wait
  int spinCount = 0;
  const int SPIN_LIMIT = 2000; // Cycles to spin before yielding

  do {
    if (retries++ > MAX_RETRIES) {
       isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "ReadFrame contention").ToLocalChecked()));
       return;
    }

    startSeq = hdr->seq;
    
    // if seq number is odd, then write is in progress, waiting...
    if (startSeq & 1) {
        if (spinCount < SPIN_LIMIT) {
            spinCount++;
            _mm_pause(); 
            continue;
        } else {
            Sleep(0);    // if waited too long
            spinCount = 0;
            continue;
        }
    }

    MemoryBarrier();
    frameBytes = hdr->frame_size;
    if (frameBytes > obj->dataCapacity()) frameBytes = 0;

    // Copying data (deep copy)
    Local<Object> outBuf;
    if (frameBytes > 0) {
        outBuf = node::Buffer::Copy(isolate, (char*)obj->dataPtr(), frameBytes).ToLocalChecked();
    } else {
        outBuf = node::Buffer::New(isolate, 0).ToLocalChecked();
    }

    MemoryBarrier();
    endSeq = hdr->seq;

    if (startSeq == endSeq) {
       // Read successfully
       args.GetReturnValue().Set(outBuf);
       return;
    }

    // if seq was changed when reading, then image tearing happened, trying again  
    // Reset spin count to try eagerly again
    spinCount = 0;

  } while (true);
}

void SharedMemory::Close(const FunctionCallbackInfo<Value>& args) {
  SharedMemory* obj = ObjectWrap::Unwrap<SharedMemory>(args.Holder());
  if (obj->base_) { UnmapViewOfFile(obj->base_); obj->base_ = nullptr; }
  if (obj->hMap_) { CloseHandle(obj->hMap_); obj->hMap_ = nullptr; }
  if (obj->hEvent_) { CloseHandle(obj->hEvent_); obj->hEvent_ = nullptr; }
  args.GetReturnValue().Set(true);
}

void SharedMemory::GetMetadata(const FunctionCallbackInfo<Value>& args) {
    Isolate* isolate = args.GetIsolate();
    SharedMemory* obj = ObjectWrap::Unwrap<SharedMemory>(args.Holder());
    if (!obj->base_) return;

    SharedHeader* hdr = obj->headerPtr();
    Local<Object> ret = Object::New(isolate);
    Local<Context> ctx = isolate->GetCurrentContext();
    
    ret->Set(ctx, String::NewFromUtf8(isolate, "width").ToLocalChecked(), Integer::New(isolate, hdr->width));
    ret->Set(ctx, String::NewFromUtf8(isolate, "height").ToLocalChecked(), Integer::New(isolate, hdr->height));
    ret->Set(ctx, String::NewFromUtf8(isolate, "channels").ToLocalChecked(), Integer::New(isolate, hdr->channels));
    ret->Set(ctx, String::NewFromUtf8(isolate, "frame_index").ToLocalChecked(), Number::New(isolate, (double)hdr->frame_index)); // JS Number (lossy > 2^53)
    
    args.GetReturnValue().Set(ret);
}


NODE_MODULE(NODE_GYP_MODULE_NAME, SharedMemory::Init)