// spike/spike_server.cpp — throwaway network+GPU round-trip spike server (NOT production code).
// Answers one question: does a real TCP+serialization hop still leave the GPU's measured
// in-process scan win (FINDINGS.md 3.5, ~12-19x) multi-fold once a client talks to it over the
// network? Serves ONE connection at a time, reusing matrixdbd's own framing helpers
// (matrixsrv_detail::recv_all/send_all) but a minimal opcode protocol instead of the full
// MatrixRequest wire format — this file is never wired into matrixdbd or any tested path.
//
// Wire format:
//   request:  [u32 len][payload]   len==0 -> HEALTH (server echoes an empty response)
//                                  len==5 -> SCAN: payload[0]=opcode(1), payload[1..5)=u32 threshold (LE)
//   response: [u32 len][payload]   HEALTH -> len==0
//                                  SCAN    -> len==16: [0..8)=double seconds (LE), [8..16)=uint64 count (LE)
//
// Build (CPU):  clang++ -std=c++20 -O2 spike_server.cpp -o spike_server_cpu
// Build (GPU):  nvcc -std=c++17 -O3 -x cu -D_GNU_SOURCE -Xcompiler -pthread -DMATRIX_USE_CUDA \
//               spike_server.cpp -o spike_server_gpu
#include "server_tcp.hpp"      // matrixsrv_detail::recv_all/send_all, matrix_set_recv_timeout/send_timeout
#if defined(MATRIX_USE_CUDA)
    #include "compute_cuda.cuh"
#endif
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>

int main(int argc, char** argv) {
    if (argc < 2) { std::cerr << "usage: spike_server <port>\n"; return 2; }
    const uint16_t port = static_cast<uint16_t>(std::strtoul(argv[1], nullptr, 10));
    std::signal(SIGPIPE, SIG_IGN);   // a peer hanging up mid-send must not kill the server

#if defined(MATRIX_USE_CUDA)
    std::unique_ptr<ComputeInterface> engine = std::make_unique<CUDAGPUEngine>(4);
    std::cerr << "spike_server: CUDA engine, port " << port << "\n";
#else
    std::unique_ptr<ComputeInterface> engine = std::make_unique<CPUMockEngine>(4);
    std::cerr << "spike_server: CPU engine, port " << port << "\n";
#endif

    const int srv = ::socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { std::cerr << "spike_server: socket() failed\n"; return 1; }
    int yes = 1; ::setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_addr.s_addr = INADDR_ANY; addr.sin_port = htons(port);
    if (::bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        std::cerr << "spike_server: bind() failed (note: blocked in a sandboxed build env — run on a real host)\n";
        return 1;
    }
    if (::listen(srv, 16) != 0) { std::cerr << "spike_server: listen() failed\n"; return 1; }
    std::cerr << "spike_server: listening\n";

    for (;;) {
        const int fd = ::accept(srv, nullptr, nullptr);
        if (fd < 0) continue;
        matrix_set_recv_timeout(fd, 30000);
        matrix_set_send_timeout(fd, 30000);
        for (;;) {
            uint32_t len = 0;
            if (!matrixsrv_detail::recv_all(fd, &len, sizeof len)) break;
            if (len == 0) {
                const uint32_t rlen = 0;
                if (!matrixsrv_detail::send_all(fd, &rlen, sizeof rlen)) break;
                continue;
            }
            if (len != 5) break;   // only a 5-byte SCAN request is supported
            uint8_t req[5];
            if (!matrixsrv_detail::recv_all(fd, req, sizeof req)) break;
            if (req[0] != 1) break;   // opcode 1 == SCAN
            uint32_t threshold;
            std::memcpy(&threshold, req + 1, sizeof threshold);

            DatabaseQuery q{};
            matrix_set_scan_threshold(q, threshold);
            const auto t0 = std::chrono::steady_clock::now();
            engine->execute_scan(q);
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            const uint64_t count = q.transaction_id;

            uint8_t resp[16];
            std::memcpy(resp, &seconds, sizeof seconds);
            std::memcpy(resp + 8, &count, sizeof count);
            const uint32_t rlen = sizeof resp;
            if (!matrixsrv_detail::send_all(fd, &rlen, sizeof rlen)) break;
            if (!matrixsrv_detail::send_all(fd, resp, sizeof resp)) break;
        }
        ::close(fd);
    }
}
