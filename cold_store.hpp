#pragma once

#include "types.hpp"   // OP_WRITE
#include <cstdio>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <utility>
#include <unistd.h>    // fsync, fileno

// SSD-backed write-ahead log (gap DU-1/2/3 / three-tier cold tier). Append-only: a record
// is [u32 length][u32 crc32(payload)][payload]. Synchronous (fsync per policy) so a
// returned append_put is durable. replay() rebuilds state front-to-back and stops at the
// first torn or corrupt record — never replays corruption. "SSD" is a file here; the
// interface is identical on real flash.

enum class SyncPolicy {
    SYNC_EACH,  // fsync after every append — committed write survives a crash (default)
    SYNC_OFF,   // no fsync — tests / throughput; crash loses unflushed tail
};

// On-disk payload layout (serialized explicitly, NOT a C struct — avoids padding/ABI
// dependence): key (8 bytes) + value (8 bytes) + opcode (4 bytes) = 20 bytes.
constexpr size_t MATRIX_WAL_PAYLOAD_BYTES = 20;
constexpr uint32_t MATRIX_WAL_MAX_RECORD = 4096;    // sane upper bound for the length field
constexpr size_t   MATRIX_WAL_COMMIT_BYTES = 4;          // commit-marker record length
constexpr uint32_t MATRIX_WAL_COMMIT       = 0x434F4D4Du; // 'COMM' — commit-marker payload

// Standard CRC32 (reflected, poly 0xEDB88320). Inline, no dependency.
inline uint32_t matrix_crc32(const unsigned char* data, size_t n) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        crc ^= data[i];
        for (int k = 0; k < 8; ++k)
            crc = (crc & 1u) ? (crc >> 1) ^ 0xEDB88320u : (crc >> 1);
    }
    return crc ^ 0xFFFFFFFFu;
}

class ColdStore {
public:
    explicit ColdStore(std::string path, SyncPolicy policy = SyncPolicy::SYNC_EACH)
        : path_(std::move(path)), policy_(policy) {
        // "ab" creates if missing and positions writes at end (append).
        fp_ = std::fopen(path_.c_str(), "ab");
        if (!fp_) {
            // A WAL that can't open its file cannot guarantee durability — fail loudly
            // rather than null-deref on the first append.
            std::fprintf(stderr, "ColdStore: cannot open WAL '%s'\n", path_.c_str());
            std::abort();
        }
    }

    ~ColdStore() {
        if (fp_) std::fclose(fp_);
    }

    ColdStore(const ColdStore&) = delete;
    ColdStore& operator=(const ColdStore&) = delete;

    // Append one auto-commit put durably (applied immediately on replay). Behavior unchanged.
    void append_put(uint64_t key, uint64_t value) { append_record(OP_WRITE, key, value); }

    // Append a transactional put — buffered on replay until a commit marker; discarded if a crash
    // leaves it without one. Written as part of the engine's commit() group.
    void append_txn_put(uint64_t key, uint64_t value) { append_record(OP_TXN_WRITE, key, value); }

    // Append a commit marker durably — replay applies all txn-puts buffered since the last commit.
    void append_commit() {
        const uint32_t magic = MATRIX_WAL_COMMIT;
        const uint32_t length = static_cast<uint32_t>(MATRIX_WAL_COMMIT_BYTES);
        const uint32_t crc = matrix_crc32(reinterpret_cast<const unsigned char*>(&magic), MATRIX_WAL_COMMIT_BYTES);
        std::fwrite(&length, sizeof(length), 1, fp_);
        std::fwrite(&crc,    sizeof(crc),    1, fp_);
        std::fwrite(&magic,  1, MATRIX_WAL_COMMIT_BYTES, fp_);
        std::fflush(fp_);
        if (policy_ == SyncPolicy::SYNC_EACH) ::fsync(::fileno(fp_));
        ++records_written_;
    }

    // Empty the log after its contents are captured in a checkpoint. Durable per SyncPolicy.
    void truncate() {
        std::fclose(fp_);
        fp_ = std::fopen(path_.c_str(), "wb");   // "wb" truncates to zero length
        if (!fp_) { std::fprintf(stderr, "ColdStore::truncate: reopen failed %s\n", path_.c_str()); std::abort(); }
        if (policy_ == SyncPolicy::SYNC_EACH) ::fsync(::fileno(fp_));
        std::fclose(fp_);
        fp_ = std::fopen(path_.c_str(), "ab");   // back to append mode
        if (!fp_) { std::fprintf(stderr, "ColdStore::truncate: reopen-append failed %s\n", path_.c_str()); std::abort(); }
        records_written_ = 0;
    }

    // Replay every intact record in append order, calling apply(key, value). Stops at the
    // first short read or CRC mismatch (torn/corrupt tail). Missing/empty file → nothing.
    template <typename Apply>
    void replay(Apply&& apply) const {
        FILE* r = std::fopen(path_.c_str(), "rb");
        if (!r) return;
        std::vector<std::pair<uint64_t, uint64_t>> txn; // txn-puts buffered since the last commit
        for (;;) {
            uint32_t length = 0;
            if (std::fread(&length, sizeof(length), 1, r) != 1) break;     // clean EOF
            if (length == 0 || length > MATRIX_WAL_MAX_RECORD) break;      // torn tail
            uint32_t crc = 0;
            if (std::fread(&crc, sizeof(crc), 1, r) != 1) break;           // torn tail
            unsigned char buf[MATRIX_WAL_MAX_RECORD];
            if (std::fread(buf, 1, length, r) != length) break;            // torn tail
            if (matrix_crc32(buf, length) != crc) break;                   // corruption
            if (length == MATRIX_WAL_PAYLOAD_BYTES) {                      // 20-byte put record
                uint32_t opcode = 0; std::memcpy(&opcode, buf + 16, 4);
                uint64_t key = 0, value = 0;
                std::memcpy(&key,   buf + 0, 8);
                std::memcpy(&value, buf + 8, 8);
                if (opcode == OP_WRITE)          apply(key, value);        // auto-commit (unchanged)
                else if (opcode == OP_TXN_WRITE) txn.emplace_back(key, value); // buffer until commit
                // other opcode at length 20: skip (forward-compat)
            } else if (length == MATRIX_WAL_COMMIT_BYTES) {               // 4-byte marker
                uint32_t magic = 0; std::memcpy(&magic, buf, 4);
                if (magic == MATRIX_WAL_COMMIT) { for (auto& kv : txn) apply(kv.first, kv.second); txn.clear(); }
                // other 4-byte record: skip (forward-compat)
            }
            // other lengths: skip (forward-compat)
        }
        std::fclose(r);   // EOF: any still-buffered txn was uncommitted -> discarded
    }

    uint64_t records_written() const { return records_written_; }
    SyncPolicy policy() const { return policy_; }   // the durability level in force (DU-5)

private:
    // Write one length-20 [key8][value8][opcode4] record durably (the put/txn-put wire form).
    void append_record(uint32_t opcode, uint64_t key, uint64_t value) {
        unsigned char payload[MATRIX_WAL_PAYLOAD_BYTES];
        std::memcpy(payload + 0,  &key,    8);
        std::memcpy(payload + 8,  &value,  8);
        std::memcpy(payload + 16, &opcode, 4);
        const uint32_t length = MATRIX_WAL_PAYLOAD_BYTES;
        const uint32_t crc = matrix_crc32(payload, MATRIX_WAL_PAYLOAD_BYTES);
        std::fwrite(&length, sizeof(length), 1, fp_);
        std::fwrite(&crc,    sizeof(crc),    1, fp_);
        std::fwrite(payload, 1, MATRIX_WAL_PAYLOAD_BYTES, fp_);
        std::fflush(fp_);
        if (policy_ == SyncPolicy::SYNC_EACH) ::fsync(::fileno(fp_));
        ++records_written_;
    }

    std::string path_;
    SyncPolicy policy_;
    FILE* fp_ = nullptr;
    uint64_t records_written_ = 0;
};
