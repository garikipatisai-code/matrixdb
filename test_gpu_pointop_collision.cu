// GPU DM-1b collision-fix verification (Colab/nvcc only, no GPU-runnable equivalent locally):
// matrix_page_kernel's point-op store is a flat MATRIX_STORE_SLOTS array indexed by `key & MASK`,
// no probing -- two different keys landing on the same slot silently overwrite each other. This
// used to be GUARANTEED for the standard benchmark workload, because MATRIX_STORE_SLOTS (4096) was
// smaller than the workload's key range (BATCH_MAX=65536) while KVStore's own capacity was
// independently 65536 -- see types.hpp's own note and PRODUCTION_READINESS.md's DM-1b entry for the
// full history. Fixed by matching MATRIX_STORE_SLOTS to BATCH_MAX/KVStore's capacity. Two checks:
//   1. A full-capacity batch of unique sequential keys (the actual benchmark's shape) must now be
//      collision-free: the store checksum (mock projection: value == key, so checksum == sum of
//      keys written) must exactly equal the analytically-known sum. Before the fix this was
//      guaranteed to come up short; this is the hardware proof the fix closes that gap.
//   2. A genuinely over-capacity KEY RANGE (more distinct keys, over time, than slots) still
//      collides -- this is the documented, intentionally-not-solved residual (a general GPU hash
//      table was judged disproportionate for a kernel confirmed unreachable from anything else in
//      this repo). Note this can't be constructed as a single oversized execute_batch() call:
//      execute_batch clamps count to MATRIX_BATCH_MAX, which now equals MATRIX_STORE_SLOTS (the
//      same fix that closed check 1), so a single call can never exceed capacity anymore. Instead:
//      two separate full-capacity calls whose key ranges ALIAS on the same slots (key and
//      key+MATRIX_STORE_SLOTS always collide). This check exists so that claim is verified, not
//      just asserted in a comment: confirms no crash/hang, and confirms the final store state is
//      exactly the second call's values (a deterministic cross-call overwrite, since execute_batch
//      synchronizes before returning) -- proving the collision genuinely still happens for this
//      case, so the documentation isn't overclaiming a fix that only covers part of the problem.
//
// Build (Colab T4):
//   nvcc -std=c++17 -O3 -x cu -D_GNU_SOURCE -Xcompiler -pthread -DMATRIX_USE_CUDA \
//       test_gpu_pointop_collision.cu -o test_gpu_pointop_collision && ./test_gpu_pointop_collision
#include "compute_cuda.cuh"
#include <cstdio>
#include <cassert>
#include <cstdint>
#include <vector>

static DatabaseQuery make_write(uint64_t key) {
    DatabaseQuery q{};
    q.query_id = key;
    q.opcode = OP_WRITE;
    return q;
}

int main() {
    // Check 1: full-capacity, unique-key batch -- the actual benchmark's shape. Must be
    // collision-free now that MATRIX_STORE_SLOTS matches BATCH_MAX.
    {
        CUDAGPUEngine eng(4);
        const size_t N = MATRIX_STORE_SLOTS;
        std::vector<DatabaseQuery> batch(N);
        uint64_t expected_sum = 0;
        for (size_t i = 0; i < N; ++i) {
            batch[i] = make_write(static_cast<uint64_t>(i));
            expected_sum += i;   // mock projection: value == key
        }
        eng.execute_batch(batch.data(), N);
        const uint64_t got = eng.store_checksum();
        std::printf("Check 1 (full-capacity, unique keys): store_checksum=%llu expected=%llu  %s\n",
                    (unsigned long long)got, (unsigned long long)expected_sum,
                    got == expected_sum ? "OK" : "*** MISMATCH -- a collision silently dropped a write ***");
        assert(got == expected_sum &&
               "DM-1b fix: a full-capacity unique-key batch must be collision-free");
        assert(eng.writes() == N && "every write in the batch was dispatched");
    }

    // Check 2: two SEPARATE full-capacity batches whose key ranges ALIAS on the same slots (key
    // and key+MATRIX_STORE_SLOTS always collide, since the slot index is key & MASK) -- this is how
    // a real over-capacity workload manifests once execute_batch's own per-call cap (MATRIX_BATCH_MAX,
    // which now equals MATRIX_STORE_SLOTS) prevents constructing a single over-sized call. The
    // second call's writes strictly follow the first (execute_batch synchronizes its stream before
    // returning), so every slot deterministically ends up holding the SECOND call's value -- an
    // exact, assertable oracle for "collision still loses data when the true key range exceeds
    // capacity," confirming the documented residual limitation is real without relying on an
    // unpredictable intra-batch race.
    {
        CUDAGPUEngine eng(4);
        const size_t N = MATRIX_STORE_SLOTS;
        std::vector<DatabaseQuery> batch1(N), batch2(N);
        uint64_t sum2 = 0;
        for (size_t i = 0; i < N; ++i) {
            batch1[i] = make_write(static_cast<uint64_t>(i));
            batch2[i] = make_write(static_cast<uint64_t>(i) + N);   // aliases batch1's slots exactly
            sum2 += static_cast<uint64_t>(i) + N;
        }
        eng.execute_batch(batch1.data(), N);
        eng.execute_batch(batch2.data(), N);   // deterministically overwrites every slot batch1 touched
        const uint64_t got = eng.store_checksum();
        std::printf("Check 2 (cross-call aliasing, key and key+STORE_SLOTS collide): "
                    "store_checksum=%llu expected=%llu (the second call's values, since they overwrite the first)  %s\n",
                    (unsigned long long)got, (unsigned long long)sum2,
                    got == sum2 ? "OK (collision confirmed, as documented, and fully deterministic here)" : "*** UNEXPECTED ***");
        assert(got == sum2 &&
               "sanity: two full-capacity batches with aliasing key ranges must leave the SECOND "
               "batch's values in the store (a deterministic cross-call overwrite) -- confirms the "
               "documented residual limitation (more distinct keys over time than slots) is real. "
               "If this fails, either MATRIX_STORE_SLOTS changed or something else did; update this "
               "test and the DM-1b documentation together.");
        assert(eng.writes() == 2 * N && "every write in both batches was dispatched (no hang, no crash)");
    }

    std::printf("ALL GPU-POINTOP-COLLISION TESTS PASSED\n");
    return 0;
}
