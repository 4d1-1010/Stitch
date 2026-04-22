#pragma once

#include <cstdint>
#include <mutex>
#include <string>

// Shared CUDA runtime — loaded once per process. Used by both
// NVDEC (decoder_nvdec.cpp, #21) and NVENC-Linux
// (encoder_nvenc_linux.cpp, #27). The CUDA context holds GPU
// memory allocations that both the decoder output pool and the
// encoder input pool reference, so a single shared context keeps
// the working-set on one device.
//
// Loading strategy: lazy. CudaRuntime::Instance() on first call
// dlopens libcuda.so.1 via ffnvcodec's dynlink_loader.h +
// cuda_load_functions, then cuInit(0) + cuDeviceGet(0) +
// cuCtxCreate. Any failure leaves ready()=false so the factories
// can decline cleanly without bringing down the helper.
//
// Platform scope: Linux only. Windows NVENC uses the D3D11
// input-surface path (encoder_nvenc.cpp) which has its own
// driver lifecycle via D3D11CreateDevice + NvEncRegisterResource,
// and Windows NVDEC isn't something we target (D3D11VA is the
// Windows decoder path). Header declares the type on every OS so
// the vendor decoder / encoder TUs can include it unconditionally;
// impl is Linux-only.

#if defined(__linux__)
// Include ffnvcodec's loader header — that's where CudaFunctions
// (the function-pointer table struct) lives, along with the
// cuda_load_functions / cuda_free_functions inline helpers we
// call from cuda_runtime.cpp. dynlink_loader.h transitively
// drags in dynlink_cuda.h + dynlink_nvcuvid.h + nvEncodeAPI.h;
// consumers that only want CudaRuntime (not the NVENC API)
// end up paying for those declarations too, but every real
// caller (decoder_nvdec.cpp, encoder_nvenc_linux.cpp) already
// needs those headers anyway, so the net cost is zero.
#include <ffnvcodec/dynlink_loader.h>
#endif

namespace unio {

class CudaRuntime {
public:
    // Process-wide singleton. First call does the dlopen +
    // cuInit + cuCtxCreate sequence. Subsequent calls are
    // thread-safe no-ops that return the cached handle.
    static CudaRuntime& Instance();

    // True iff the runtime loaded, cuInit(0) returned success,
    // and a primary context was created on device 0. When false,
    // the consumer's Make* factory should return nullptr.
    bool ready() const { return ready_; }

    // Human-readable reason the runtime isn't ready, empty when
    // ready()=true. Used by the capability probe (#33) to
    // surface the specific failure mode into helper_caps.
    const std::string& reason() const { return reason_; }

#if defined(__linux__)
    // Raw CUDA function table from ffnvcodec/dynlink_cuda.h.
    // Consumer TUs call cuda_fns()->cuCtxPushCurrent / cuMemAlloc
    // / etc directly. Never null when ready()=true.
    CudaFunctions* cuda_fns() const { return cuda_; }

    // Primary CUDA context bound to device 0. Single context
    // shared between NVENC and NVDEC so GPU memory they allocate
    // lives in the same address space (enables future
    // zero-copy paths via cuMemcpyDtoD).
    CUcontext primary_ctx() const { return ctx_; }

    // Name of the CUDA device behind primary_ctx_. Populated at
    // init from cuDeviceGetName.
    const std::string& device_name() const { return device_name_; }
#endif

    // Non-copyable + non-movable — singleton.
    CudaRuntime(const CudaRuntime&) = delete;
    CudaRuntime& operator=(const CudaRuntime&) = delete;

private:
    CudaRuntime();
    ~CudaRuntime();

    void EnsureLoaded();
    void Load();

    bool ready_ = false;
    std::string reason_;
    std::once_flag load_flag_;
#if defined(__linux__)
    CudaFunctions* cuda_ = nullptr;
    CUcontext ctx_ = nullptr;
    std::string device_name_;
#endif
};

}  // namespace unio
