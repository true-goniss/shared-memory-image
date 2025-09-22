/*
    gon_iss (c) 2025

    https://github.com/true-goniss/shared-memory-image

*/

#include <node.h>
#include <node_buffer.h>
#include <Windows.h>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

using v8::FunctionCallbackInfo;
using v8::Isolate;
using v8::Local;
using v8::Object;
using v8::String;
using v8::Value;
using v8::Number;
using v8::Integer;
using v8::ArrayBuffer;
using v8::Uint8Array;
using v8::Exception;
using v8::Boolean;

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
  uint32_t magic;          // 0x5348444D 'SHDM'
  uint32_t version;        // 1
  volatile LONG seq;       // sequence counter (seqlock) - 32-bit
  uint32_t width;
  uint32_t height;
  uint32_t channels;
  uint32_t frame_size;     // bytes of current frame
  uint64_t frame_index;    // ever increasing frame counter (64-bit)
  uint64_t mapping_size;   // total mapping size written by creator
  uint8_t reserved[48];    // pad to 80+ bytes for future use/alignment
};
#pragma pack(pop)

static HANDLE g_hMap = nullptr;
static void* g_base = nullptr;
static size_t g_mapSize = 0;
static HANDLE g_hEvent = nullptr;
static std::string g_eventName;
static const uint32_t MAGIC = SHARED_MAGIC;
static const uint32_t VERSION = 1;
static const size_t HEADER_SIZE = ((sizeof(SharedHeader) + 63) / 64) * 64; // 64 byte aligned header
static size_t dataOffset() { return HEADER_SIZE; }

static SharedHeader* headerPtr() {
  return reinterpret_cast<SharedHeader*>((uint8_t*)g_base);
}

static void* dataPtr() {
  return (uint8_t*)g_base + dataOffset();
}

static size_t dataCapacity() {
  if (g_mapSize <= dataOffset()) return 0;
  return g_mapSize - dataOffset();
}

/*
Get mapping region size via VirtualQuery
*/  
static size_t queryRegionSize(void* addr) {
  MEMORY_BASIC_INFORMATION mbi;
  if (VirtualQuery(addr, &mbi, sizeof(mbi)) == 0) return 0;
  return mbi.RegionSize;
}

/*
Create or open mapping and event
*/  
void CreateSharedMemory(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = args.GetIsolate();

  if (args.Length() < 2) {
    isolate->ThrowException(
      Exception::TypeError(
        String::NewFromUtf8(isolate, "create(name, size[,width,height,channels]) expects at least 2 args").ToLocalChecked()
      ));
    return;
  }

  if (!args[0]->IsString() || !args[1]->IsNumber()) {
    isolate->ThrowException(
      Exception::TypeError(
        String::NewFromUtf8(isolate, "Wrong args").ToLocalChecked()
      ));
    return;
  }

  v8::String::Utf8Value name(isolate, args[0]);
  std::string mapName(*name);
  uint64_t requestedSize = (uint64_t)args[1]->IntegerValue(isolate->GetCurrentContext()).FromJust();

  uint32_t width = 0, height = 0, channels = 0;
  bool isCreator = false;

  if (args.Length() >= 5 && args[2]->IsNumber() && args[3]->IsNumber() && args[4]->IsNumber()) {
    width = (uint32_t)args[2]->IntegerValue(isolate->GetCurrentContext()).FromJust();
    height = (uint32_t)args[3]->IntegerValue(isolate->GetCurrentContext()).FromJust();
    channels = (uint32_t)args[4]->IntegerValue(isolate->GetCurrentContext()).FromJust();
  }

  // Ensure minimally large mapping to contain header
  if (requestedSize < HEADER_SIZE + 4) {
    isolate->ThrowException(
      Exception::Error(
        String::NewFromUtf8(isolate, "Requested size too small").ToLocalChecked()
      ));
    return;
  }

  // Try to open an existing mapping
  g_hMap = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, mapName.c_str());

  if (g_hMap == nullptr) {
    // Create new mapping
    DWORD sizeLow = static_cast<DWORD>(requestedSize & 0xFFFFFFFF);
    DWORD sizeHigh = static_cast<DWORD>((requestedSize >> 32) & 0xFFFFFFFF);

    g_hMap = CreateFileMappingA(
      INVALID_HANDLE_VALUE, 
      nullptr, 
      PAGE_READWRITE, 
      sizeHigh, 
      sizeLow, 
      mapName.c_str()
    );

    if (g_hMap == nullptr) {
      isolate->ThrowException(
        Exception::Error(
          String::NewFromUtf8(isolate, "Could not create file mapping object").ToLocalChecked()
        ));
      return;
    }

    isCreator = true;
  }

  // Map entire mapping (0 = whole object)
  g_base = MapViewOfFile(g_hMap, FILE_MAP_ALL_ACCESS, 0, 0, 0);
  if (g_base == nullptr) {
    CloseHandle(g_hMap);
    g_hMap = nullptr;
    isolate->ThrowException(
      Exception::Error(
        String::NewFromUtf8(isolate, "Could not map view of file").ToLocalChecked()
      ));
    return;
  }

  // Query actual mapping size (always reliable)
  size_t regionSize = queryRegionSize(g_base);
  if (regionSize == 0) {
    UnmapViewOfFile(g_base);
    g_base = nullptr;
    CloseHandle(g_hMap);
    g_hMap = nullptr;
    isolate->ThrowException(
      Exception::Error(
        String::NewFromUtf8(isolate, "Could not query mapping size").ToLocalChecked()
      ));
    return;
  }
  g_mapSize = regionSize;

  // Create/open named event
  g_eventName = "Global\\SHM_EV_" + mapName;
  g_hEvent = OpenEventA(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, g_eventName.c_str());
  
  if (g_hEvent == nullptr) {
    g_eventName = "Local\\SHM_EV_" + mapName;
    g_hEvent = CreateEventA(nullptr, FALSE, FALSE, g_eventName.c_str());
    // no event, but not fatal
  }

  SharedHeader* hdr = headerPtr();
  if (isCreator) {
    // Initialize header
    ZeroMemory(hdr, sizeof(SharedHeader));
    hdr->magic = MAGIC;
    hdr->version = VERSION;
    hdr->seq = 0;
    hdr->width = width;
    hdr->height = height;
    hdr->channels = channels;
    hdr->frame_size = 0;
    hdr->frame_index = 0;
    hdr->mapping_size = static_cast<uint64_t>(regionSize);
  } else {
    // Validate header
    if (hdr->magic != MAGIC || hdr->version != VERSION) {
      UnmapViewOfFile(g_base);
      g_base = nullptr;
      CloseHandle(g_hMap);
      g_hMap = nullptr;
      if (g_hEvent) { CloseHandle(g_hEvent); g_hEvent = nullptr; }

      isolate->ThrowException(
        Exception::Error(
          String::NewFromUtf8(isolate, "Mapping format mismatch (magic/version)").ToLocalChecked()
        ));
      return;
    }
  }

  args.GetReturnValue().Set(String::NewFromUtf8(isolate, "ok").ToLocalChecked());
}

/*
Set format
*/
void SetFormat(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = args.GetIsolate();

  if (g_base == nullptr) {
    isolate->ThrowException(
      Exception::Error(
        String::NewFromUtf8(isolate, "Shared memory not created").ToLocalChecked()
      )
    );
    return;
  }

  // Arguments exception
  if (args.Length() < 3 ||
      !args[0]->IsNumber() ||
      !args[1]->IsNumber() ||
      !args[2]->IsNumber()
  ) {
    isolate->ThrowException(
      Exception::TypeError(
        String::NewFromUtf8(isolate, "setFormat(width,height,channels) expects 3 numbers").ToLocalChecked())
    );
    return;
  }

  uint32_t w = static_cast<uint32_t>(args[0]->IntegerValue(isolate->GetCurrentContext()).FromJust());
  uint32_t h = static_cast<uint32_t>(args[1]->IntegerValue(isolate->GetCurrentContext()).FromJust());
  uint32_t c = static_cast<uint32_t>(args[2]->IntegerValue(isolate->GetCurrentContext()).FromJust());

  // Invalid format exception
  if (w == 0 ||
      h == 0 ||
      (c != 3 && c != 4)
  ) {
    isolate->ThrowException(
      Exception::Error(
        String::NewFromUtf8(isolate, "Invalid format").ToLocalChecked()
      )
    );
    return;
  }

  SharedHeader* hdr = headerPtr();

  // seqlock write - changing format
  InterlockedIncrement(&hdr->seq); // odd (32-bit)
  MemoryBarrier();
  hdr->width = w;
  hdr->height = h;
  hdr->channels = c;
  // frame_size left as is, it's been set at publishFrame()
  MemoryBarrier();
  InterlockedIncrement(&hdr->seq); // even

  if (g_hEvent)
    SetEvent(g_hEvent);

  args.GetReturnValue().Set(
    Boolean::New(isolate, true)
  );
}


/*
Return a zero-copy Node Buffer pointing to data area
*/
void GetFrameBuffer(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = args.GetIsolate();

  if (g_base == nullptr) {
    isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Shared memory not created").ToLocalChecked()));
    return;
  }

  size_t cap = dataCapacity();
  if (cap == 0) {
    isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "No capacity").ToLocalChecked()));
    return;
  }

  char* ptr = static_cast<char*>(dataPtr());

  // Create external Buffer that points directly into mapping.
  Local<v8::Object> buf = node::Buffer::New(isolate, ptr, cap, noop_free, nullptr).ToLocalChecked();

  args.GetReturnValue().Set(buf);
}

void GetCapacity(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = args.GetIsolate();
  if (g_base == nullptr) {
    isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Shared memory not created").ToLocalChecked()));
    return;
  }
  args.GetReturnValue().Set(
    Number::New(isolate, static_cast<double>(dataCapacity()))
  );
}

// Writer: publish frame metadata and signal event
// publishFrame(frameBytes)
void PublishFrame(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = args.GetIsolate();
  if (g_base == nullptr) {
    isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Shared memory not created").ToLocalChecked()));
    return;
  }
  if (args.Length() < 1 || !args[0]->IsNumber()) {
    isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "Expected frame size").ToLocalChecked()));
    return;
  }

  uint32_t frameBytes = static_cast<uint32_t>(args[0]->IntegerValue(isolate->GetCurrentContext()).FromJust());
  if (frameBytes > dataCapacity()) {
    isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Frame too large").ToLocalChecked()));
    return;
  }

  SharedHeader* hdr = headerPtr();

  // seqlock write:
  InterlockedIncrement(&hdr->seq); // become odd (writer in-progress)
  MemoryBarrier();

  hdr->frame_size = frameBytes;
  // bump 64-bit index correctly:
  AtomicIncrement64(reinterpret_cast<volatile LONG64*>(&hdr->frame_index));

  MemoryBarrier();
  InterlockedIncrement(&hdr->seq); // become even (writer done)

  // signal event so readers can wake up
  if (g_hEvent != nullptr) SetEvent(g_hEvent);

  args.GetReturnValue().Set(Boolean::New(isolate, true));
}

/*
Node test reader: read latest frame into a newly allocated Buffer (copy)
readFrame([timeoutMs]) -> Buffer or null if timeout
*/
void ReadFrame(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = args.GetIsolate();

  if (g_base == nullptr) {
    isolate->ThrowException(
      Exception::Error(
        String::NewFromUtf8(isolate, "Shared memory not created").ToLocalChecked()
      )
    );
    return;
  }

  DWORD timeout = 0;
  if (args.Length() >= 1 && args[0]->IsNumber()) {
    timeout = static_cast<DWORD>(args[0]->IntegerValue(isolate->GetCurrentContext()).FromJust());
  } else {
    timeout = INFINITE;
  }

  SharedHeader* hdr = headerPtr();

  // Wait on event if available
  if (g_hEvent != nullptr) {
    DWORD w = WaitForSingleObject(g_hEvent, timeout);
    if (w == WAIT_TIMEOUT) {
      args.GetReturnValue().Set(v8::Null(isolate));
      return;
    }
  } else {
    // no event: fall through (busy-check)
  }

  // seqlock read: try until consistent
  uint32_t start, end;
  uint32_t frameBytes = 0;
  uint64_t frameIndex = 0;
  const int maxAttempts = 10;
  int attempts = 0;
  do {
    if (++attempts > maxAttempts) {
      isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Failed to read stable frame (too many retries)").ToLocalChecked()));
      return;
    }
    start = hdr->seq;
    if (start & 1) { // writer in progress
      Sleep(0);
      continue;
    }

    MemoryBarrier();
    frameBytes = hdr->frame_size;
    frameIndex = hdr->frame_index;

    if (frameBytes > dataCapacity()) {
      isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Frame size invalid").ToLocalChecked()));
      return;
    }

    // copy data to new Buffer
    char* src = static_cast<char*>(dataPtr());
    Local<v8::Object> outbuf = node::Buffer::Copy(isolate, src, frameBytes).ToLocalChecked();
    MemoryBarrier();
    end = hdr->seq;
    if (start != end) {
      // inconsistent; try again
      continue;
    }

    // attach meta? we'll return Buffer only.
    args.GetReturnValue().Set(outbuf);
    return;
  } while (true);
}

/*
Close/unmap
*/
void CloseSharedMemory(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = args.GetIsolate();
  if (g_base != nullptr) {
    UnmapViewOfFile(g_base);
    g_base = nullptr;
  }
  if (g_hMap != nullptr) {
    CloseHandle(g_hMap);
    g_hMap = nullptr;
  }
  if (g_hEvent != nullptr) {
    CloseHandle(g_hEvent);
    g_hEvent = nullptr;
  }
  g_mapSize = 0;
  args.GetReturnValue().Set(Boolean::New(isolate, true));
}

/*
Get metadata
*/
void GetMetadata(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = args.GetIsolate();

  if (g_base == nullptr) {
    isolate->ThrowException(
      Exception::Error(
        String::NewFromUtf8(isolate, "Shared memory not created").ToLocalChecked()
      )
    );
    return;
  }

  SharedHeader* hdr = headerPtr();
  Local<Object> out = Object::New(isolate);
  Local<v8::Context> ctx = isolate->GetCurrentContext();

  out->Set(ctx, String::NewFromUtf8(isolate, "width").ToLocalChecked(), Integer::New(isolate, hdr->width)).Check();
  out->Set(ctx, String::NewFromUtf8(isolate, "height").ToLocalChecked(), Integer::New(isolate, hdr->height)).Check();
  out->Set(ctx, String::NewFromUtf8(isolate, "channels").ToLocalChecked(), Integer::New(isolate, hdr->channels)).Check();
  out->Set(ctx, String::NewFromUtf8(isolate, "frame_size").ToLocalChecked(), Integer::New(isolate, hdr->frame_size)).Check();

  out->Set(ctx,
           String::NewFromUtf8(isolate, "frame_index").ToLocalChecked(),
           v8::BigInt::NewFromUnsigned(isolate, hdr->frame_index)).Check();

  args.GetReturnValue().Set(out);
}

void Initialize(Local<Object> exports) {
  NODE_SET_METHOD(exports, "create", CreateSharedMemory);
  NODE_SET_METHOD(exports, "setFormat", SetFormat);
  NODE_SET_METHOD(exports, "getCapacity", GetCapacity);
  NODE_SET_METHOD(exports, "getFrameBuffer", GetFrameBuffer);
  NODE_SET_METHOD(exports, "publishFrame", PublishFrame);
  NODE_SET_METHOD(exports, "readFrame", ReadFrame);
  NODE_SET_METHOD(exports, "close", CloseSharedMemory);
  NODE_SET_METHOD(exports, "getMetadata", GetMetadata);
}

NODE_MODULE(NODE_GYP_MODULE_NAME, Initialize)