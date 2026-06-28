#pragma once

#include "memory_model.hpp"
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <atomic>
#include <unistd.h>   // getpid — per-process COLD-file namespace
#if defined(MATRIX_USE_CUDA)
#include <cuda_runtime.h>
#endif

// A column's bytes resident in exactly ONE tier (HOST/DEVICE/COLD). migrate_to() moves
// them by always staging through HOST: read the bytes to a host buffer, free the source
// tier, push to the destination. This collapses the 3x3 transition matrix into two halves
// and routes DEVICE<->COLD via HOST for free. The integrity invariant: checksum() is the
// same regardless of which tier holds the bytes. DEVICE requires a CUDA build.
class TieredColumn {
public:
    TieredColumn(uint64_t id, const unsigned char* bytes, size_t n)
        : id_(id), size_(n), tier_(MemorySpace::HOST), host_(bytes, bytes + n) {}

    ~TieredColumn() { free_current(); }

    TieredColumn(const TieredColumn&) = delete;
    TieredColumn& operator=(const TieredColumn&) = delete;

    MemorySpace tier() const { return tier_; }
    size_t size_bytes() const { return size_; }
    uint64_t id() const { return id_; }

    // Pointer to the resident HOST bytes for in-place reads (e.g. a scan). Valid only while
    // tier()==HOST; nullptr otherwise (the bytes live on SSD/VRAM — migrate to HOST first).
    // Zero-copy: returns the live buffer, no allocation.
    const unsigned char* host_ptr() const {
        return tier_ == MemorySpace::HOST ? host_.data() : nullptr;
    }

    // Move the column's bytes to `dest` (no-op if already there). Always stages through HOST.
    void migrate_to(MemorySpace dest) {
        if (dest == tier_) return;
        std::vector<unsigned char> staged = read_to_host(); // pull bytes from wherever we are
        free_current();                                     // release the source tier's resource
        tier_ = MemorySpace::HOST;                          // logically in HOST now (staged holds bytes)
        host_ = std::move(staged);
        if (dest == MemorySpace::HOST) return;
        if (dest == MemorySpace::COLD) {
            write_cold(host_);
            host_.clear(); host_.shrink_to_fit();
            tier_ = MemorySpace::COLD;
            return;
        }
        if (dest == MemorySpace::DEVICE) {
#if defined(MATRIX_USE_CUDA)
            push_device(host_);
            host_.clear(); host_.shrink_to_fit();
            tier_ = MemorySpace::DEVICE;
            return;
#else
            std::fprintf(stderr, "TieredColumn: DEVICE tier requires a CUDA build\n");
            std::abort();
#endif
        }
        std::fprintf(stderr, "TieredColumn: unsupported destination tier\n");
        std::abort();
    }

    // TieredColumn (public): append bytes to a HOST-resident column (grow in place). Caller ensures the
    // column is HOST (the engine borrows COLD->HOST first). size_ grows so checksum()/host_ptr() span it.
    void append_bytes(const unsigned char* b, size_t n) {
        if (tier_ != MemorySpace::HOST) {
            std::fprintf(stderr, "TieredColumn::append_bytes requires HOST tier\n"); std::abort();
        }
        host_.insert(host_.end(), b, b + n);
        size_ += n;
    }

    // Byte checksum wherever the column lives (DEVICE copies back to host first).
    uint64_t checksum() const {
        const std::vector<unsigned char> b = read_to_host();
        uint64_t sum = 0;
        for (unsigned char c : b) sum += c;
        return sum;
    }

#if defined(MATRIX_USE_CUDA)
    const void* device_ptr() const { return device_; } // valid only while tier()==DEVICE
#endif

private:
    // Process-unique serial so two columns (even with the same logical id, even in two engine
    // instances) never share a COLD file. Assigned once at construction, stable for the object's
    // life (so HOST<->COLD round-trips hit the same path).
    static uint64_t next_serial() {
        static std::atomic<uint64_t> counter{0};
        return counter.fetch_add(1, std::memory_order_relaxed);
    }

    std::string cold_path() const {
        // pid + per-object serial: unique within and across engine instances/processes.
        return std::string("/tmp/matrixdb_tcol_")
             + std::to_string(static_cast<long long>(getpid())) + "_"
             + std::to_string(serial_) + ".bin";
    }

    std::vector<unsigned char> read_to_host() const {
        if (tier_ == MemorySpace::HOST) return host_;
        if (tier_ == MemorySpace::COLD) {
            std::vector<unsigned char> b(size_);
            FILE* f = std::fopen(cold_path().c_str(), "rb");
            if (!f) { std::fprintf(stderr, "TieredColumn: cold read failed %s\n", cold_path().c_str()); std::abort(); }
            const size_t got = std::fread(b.data(), 1, size_, f);
            std::fclose(f);
            if (got != size_) { std::fprintf(stderr, "TieredColumn: short cold read\n"); std::abort(); }
            return b;
        }
#if defined(MATRIX_USE_CUDA)
        if (tier_ == MemorySpace::DEVICE) {
            std::vector<unsigned char> b(size_);
            cudaMemcpy(b.data(), device_, size_, cudaMemcpyDeviceToHost);
            return b;
        }
#endif
        std::fprintf(stderr, "TieredColumn: read from unsupported tier\n");
        std::abort();
    }

    void write_cold(const std::vector<unsigned char>& b) {
        FILE* f = std::fopen(cold_path().c_str(), "wb");
        if (!f) { std::fprintf(stderr, "TieredColumn: cold write failed %s\n", cold_path().c_str()); std::abort(); }
        const size_t wrote = std::fwrite(b.data(), 1, b.size(), f);
        std::fclose(f);
        // Fail loud on a short write (e.g. disk full) rather than leave a truncated cold
        // copy that only surfaces as corruption on read-back — symmetric with read_to_host.
        if (wrote != b.size()) {
            std::fprintf(stderr, "TieredColumn: short cold write (%zu/%zu)\n", wrote, b.size());
            std::abort();
        }
    }

    void free_current() {
        if (tier_ == MemorySpace::COLD) std::remove(cold_path().c_str());
#if defined(MATRIX_USE_CUDA)
        if (tier_ == MemorySpace::DEVICE && device_) { cudaFree(device_); device_ = nullptr; }
#endif
        // HOST: host_ vector frees itself / is overwritten by the caller.
    }

#if defined(MATRIX_USE_CUDA)
    void push_device(const std::vector<unsigned char>& b) {
        cudaMalloc(&device_, size_);
        cudaMemcpy(device_, b.data(), size_, cudaMemcpyHostToDevice);
    }
    void* device_ = nullptr;
#endif

    uint64_t id_;
    size_t size_;
    MemorySpace tier_;
    const uint64_t serial_ = next_serial();
    std::vector<unsigned char> host_;
};
