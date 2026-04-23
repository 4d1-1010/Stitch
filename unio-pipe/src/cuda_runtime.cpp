// Shared CUDA runtime loader (Linux). See cuda_runtime.h for the
// public contract. Delegates all the dlopen + dlsym work to
// ffnvcodec's dynlink_loader.h (MIT, already in the build tree),
// which ships inline helpers cuda_load_functions /
// cuda_free_functions. We just hold the resulting CudaFunctions*
// plus a primary CUcontext so NVDEC (#21) and NVENC-Linux (#27)
// share the same GPU device address space.
//
// Failure modes surfaced via ready()=false + reason():
//   libcuda.so.1 not loadable      → "libcuda.so.1 not available"
//   cuInit(0) fails                → "cuInit failed (HRESULT=...)"
//   cuDeviceGetCount returns 0     → "no CUDA devices visible"
//   cuCtxCreate fails              → "cuCtxCreate failed"
// Non-ready state leaves cuda_fns() / primary_ctx() as whatever
// partial progress got made — callers must guard on ready()
// first.

#include "cuda_runtime.h"

#if defined(__linux__)

#include <cstdio>
#include <cstring>

#include <ffnvcodec/dynlink_loader.h>

namespace unio {

CudaRuntime& CudaRuntime::Instance() {
    static CudaRuntime inst;
    inst.EnsureLoaded();
    return inst;
}

void CudaRuntime::EnsureLoaded() {
    std::call_once(load_flag_, [this]() { Load(); });
}

CudaRuntime::CudaRuntime() = default;

CudaRuntime::~CudaRuntime() {
    if (ctx_ && cuda_ && cuda_->cuCtxDestroy) {
        cuda_->cuCtxDestroy(ctx_);
        ctx_ = nullptr;
    }
    if (cuda_) {
        cuda_free_functions(&cuda_);
    }
}

void CudaRuntime::Load() {
    // Step 1 — dlopen + symbol table via ffnvcodec's loader.
    // cuda_load_functions returns 0 on success; negative on
    // dlopen failure or any mandatory symbol missing.
    const int cuda_ret = cuda_load_functions(&cuda_, nullptr);
    if (cuda_ret != 0 || !cuda_) {
        reason_ = "libcuda.so.1 not loadable or too old "
                  "(cuda_load_functions returned non-zero)";
        return;
    }

    // Step 2 — cuInit(0). Triggers kernel-module side init.
    // Failure here is usually a misconfigured driver (uvm not
    // loaded, version skew) — same class of false-positive the
    // PR #33 review flagged; we catch it here because Load()
    // actually exercises the runtime.
    const int init_ret = cuda_->cuInit(0);
    if (init_ret != 0) {
        char buf[96];
        std::snprintf(buf, sizeof(buf),
                      "cuInit(0) failed (CUresult=%d) — check "
                      "nvidia_uvm module + driver version",
                      init_ret);
        reason_ = buf;
        cuda_free_functions(&cuda_);
        return;
    }

    int device_count = 0;
    if (cuda_->cuDeviceGetCount(&device_count) != 0
        || device_count == 0) {
        reason_ = "no CUDA devices visible (cuDeviceGetCount=0)";
        cuda_free_functions(&cuda_);
        return;
    }

    // Step 3 — pick device 0 + fetch its name. Multi-GPU
    // selection (Optimus iGPU vs dGPU) is the job of the
    // capability probe + path negotiation (#44). Device 0 on
    // adi-pc is the NVIDIA dGPU per nvidia-smi; the iGPU doesn't
    // expose a CUDA device at all.
    int dev = 0;
    if (cuda_->cuDeviceGet(&dev, 0) != 0) {
        reason_ = "cuDeviceGet(0) failed";
        cuda_free_functions(&cuda_);
        return;
    }
    char name_buf[128] = {};
    if (cuda_->cuDeviceGetName(name_buf, sizeof(name_buf) - 1,
                                dev) == 0) {
        device_name_ = name_buf;
    }

    // Step 4 — primary context. CU_CTX_SCHED_BLOCKING_SYNC keeps
    // the host thread from busy-spinning while the GPU is busy
    // — we're a latency-sensitive app, not throughput, and
    // blocking-sync gives the scheduler a hint to yield. NVENC's
    // recommended flag per the SDK programming guide.
    if (cuda_->cuCtxCreate(&ctx_, CU_CTX_SCHED_BLOCKING_SYNC, dev)
        != 0) {
        reason_ = "cuCtxCreate failed";
        cuda_free_functions(&cuda_);
        return;
    }

    // cuCtxCreate pushes the new context onto the calling thread's
    // stack. Pop it so the library-load step doesn't leave the
    // current thread with our context current — consumers do
    // their own Push/Pop around each CUDA call.
    CUcontext popped = nullptr;
    cuda_->cuCtxPopCurrent(&popped);

    ready_ = true;
    std::fprintf(stderr,
        "unio-pipe: CUDA runtime ready — device \"%s\", "
        "ctx=%p\n",
        device_name_.c_str(),
        static_cast<void*>(ctx_));
}

}  // namespace unio

#else  // !__linux__

namespace unio {

CudaRuntime& CudaRuntime::Instance() {
    static CudaRuntime inst;
    return inst;
}

CudaRuntime::CudaRuntime() {
    reason_ = "CudaRuntime is Linux-only; Windows NVENC uses D3D11";
}

CudaRuntime::~CudaRuntime() = default;

void CudaRuntime::Load() {}

}  // namespace unio

#endif  // __linux__
