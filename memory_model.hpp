#pragma once

// Where a dataset physically lives / which processor addresses it.
enum class MemorySpace {
    HOST,    // CPU RAM
    DEVICE,  // GPU VRAM (discrete)
    COLD,    // SSD — cold columns + durability log (append-only, high latency)
    UNIFIED, // shared CPU+GPU pool (DGX Spark / Grace-Hopper) — placement is zero-copy
};

// Boot-time description of the machine's memory architecture. The CostModel reads this
// so the data-transfer term is included (discrete) or zero (unified).
//   DISCRETE: HOST and DEVICE are distinct; placing data in DEVICE costs a transfer.
//   UNIFIED : one physical pool; "placement" only chooses the executor, never moves data.
struct MemoryModel {
    enum Kind { DISCRETE, UNIFIED } kind = DISCRETE;

    // ponytail: unified-memory hardware isn't available to test on, so detection
    // defaults to DISCRETE. When a unified box (DGX Spark / GH) is in hand, set this
    // from cudaDeviceGetAttribute(cudaDevAttrPageableMemoryAccess / Managed) and
    // implement the UNIFIED cost branch. Seam only for now.
    static MemoryModel detect(bool gpu_available) {
        MemoryModel m;
        m.kind = DISCRETE; // discrete-only until validated on real unified hardware
        (void)gpu_available;
        return m;
    }

    bool is_unified() const { return kind == UNIFIED; }
};
