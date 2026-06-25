# **Architectural Specification for MatrixDB: Low-Latency Bare-Metal Database Engine**

## **1\. Ingestion Layer and Busy-Spin Control Loop**

High-performance, low-latency database engines require ingestion architectures that avoid the latency penalties imposed by operating system kernel schedulers1. Traditional thread synchronization mechanisms, such as mutexes and condition variables, require system calls that suspend thread execution, introducing context-switching overheads of 100 to 200 nanoseconds per invocation2. To achieve sub-microsecond ingestion latencies, MatrixDB shifts the synchronization burden from the operating system to the hardware layer1. This is accomplished through a lock-free Single-Producer Single-Consumer (SPSC) queue combined with a hardware-pushed, busy-spin polling thread pinned directly to dedicated physical CPU cores1.

### **Lock-Free SPSC Circular Ring Buffer Design**

The core queue of the MatrixDB ingestion layer is a lock-free, zero-allocation SPSC circular ring buffer1. Unlike Multi-Producer Multi-Consumer (MPMC) queues, which require expensive atomic Compare-And-Swap (CAS) retry loops that suffer from hardware bus locking and cache line contention under load, the SPSC queue achieves wait-free ![][image1] operations1. The queue synchronizes the producer and consumer using atomic sequence indices: write\_cursor\_ and read\_cursor\_1.  
The producer thread exclusively writes to the slot indexed by write\_cursor\_ and publishes its progress to the consumer via store-release memory semantics1. The consumer thread reads data from the slot indexed by read\_cursor\_ and publishes its progress to the producer using store-release semantics1. Both threads read the opposing cursor using load-acquire semantics to establish a robust happens-before relationship without physical locks1.  
To eliminate unnecessary cache coherence traffic, the queue maintains local thread-shadowed copies of the cursors: cached\_read\_cursor\_ for the producer and cached\_write\_cursor\_ for the consumer1. Under typical operating conditions, the producer checks its local cached\_read\_cursor\_ to determine if the queue is full1. It only executes a volatile, atomic load-acquire on the shared read\_cursor\_ when the local shadow index indicates that the buffer is full1. Symmetrically, the consumer only loads the shared write\_cursor\_ when its local cached\_write\_cursor\_ suggests the queue is empty1. This technique reduces cross-core cache invalidations, keeping the hot indices localized in the respective L1 caches1.

### **Thread Pinning and Core Affinity on Hybrid Architectures**

To guarantee predictable execution loops, the ingestion thread must be pinned to a specific physical core, isolating it from OS-level scheduling migrations1. Core allocation is highly platform-dependent, presenting distinct challenges when transitioning from local Apple Silicon development machines to x86\_64 server environments8.

| Platform | Core Scheduling API | Core Partitioning Model | Performance vs. Efficiency Core Selection |
| :---- | :---- | :---- | :---- |
| **Apple Silicon (macOS ARM64)** | Mach Kernel Thread Policies (thread\_policy\_set)10 | Advisory Core Grouping / Affinity Tags10 | Strict QoS Class Promotion (QOS\_CLASS\_USER\_INTERACTIVE)12 |
| **Linux x86\_64** | POSIX Thread Affinity (pthread\_setaffinity\_np)8 | Hard Hardware CPU Mask Pinning8 | Homogeneous P-Cores (Dynamic Frequency Scaling disabled)7 |

On macOS running on Apple Silicon, traditional POSIX affinity masks are unsupported9. The operating system scheduler routes threads based on Quality of Service (QoS) classes12. If a thread is left at default priority, the Mach scheduler may execute it on the energy-efficient "Icestorm" (E) cores, leading to a significant drop in throughput9. To prevent this, MatrixDB promotes the ingestion thread's QoS to QOS\_CLASS\_USER\_INTERACTIVE, forcing the thread onto the high-performance "Firestorm" (P) cores9. Additionally, the Mach-specific thread\_policy\_set API is used to configure affinity tags, which guides the scheduler to keep related threads on the same physical cluster to maximize L2 cache reuse10.  
In contrast, Linux server environments allow absolute control over hardware threads via pthread\_setaffinity\_np8. The ingestion thread is bound directly to a single logical CPU core using an explicit CPU mask8. To prevent hardware-induced latency spikes, the target core should be isolated from the OS scheduler entirely using the isolcpus kernel boot parameter, and power-saving sleep states (C-states) must be disabled7.

### **Platform-Agnostic Assembly-Level Spin Control**

During periods of zero queue traffic, a raw, unmitigated busy-spin loop can cause the processor to execute instructions at its maximum clock rate, leading to thermal throttling and stalling the execution pipeline of neighboring cores7. To manage this, MatrixDB uses low-level assembly pause primitives within the spin loop.  
On x86\_64 platforms, the loop executes the \_mm\_pause() intrinsic, which compiles to the hardware PAUSE instruction16. This instruction introduces a small, architecture-dependent delay (ranging from 10 cycles on older Intel architectures to 140 cycles on modern Skylake and newer cores), which reduces power consumption and prevents speculative branch execution mispredictions16.  
On Apple Silicon (ARM64), using a standard YIELD instruction often acts as a no-operation (NOP), failing to provide adequate pipeline relaxation16. To achieve a true processor stall on ARM64, the engine utilizes the Instruction Synchronization Barrier (isb) assembly instruction16. The isb instruction flushes the local processor pipeline and halts execution until all previous instructions have fully cleared, acting as an effective ARM64 equivalent to the x86 PAUSE instruction16.

ARM64 Spin:      \[Check Queue\] \--(Empty)--\> \[isb sy\] \--(Stall Pipeline)--\> \[Loop Reset\]  
x86\_64 Spin:     \[Check Queue\] \--(Empty)--\> \[pause\]  \--(Stall Pipeline)--\> \[Loop Reset\]

### **Dual-Trigger Ingestion Control Loop**

Once queries are retrieved from the SPSC ring buffer, they are batched before being sent to the compute layer. To balance latency and throughput, the ingestion thread evaluates a continuous Dual-Trigger Condition20. This loop monitors both the physical size of the batch and a high-resolution timer using std::chrono::high\_resolution\_clock.  
Let ![][image2] represent the optimal batch size target, and ![][image3] be the maximum timeout threshold in microseconds. Let ![][image4] be the current count of elements accumulated in the active execution batch. The arrival of the first transaction in an empty batch establishes the base timestamp:  
![][image5]  
For each iteration of the polling loop, the current timestamp is sampled:  
![][image6]  
The temporal delta (![][image7]) is calculated as:  
![][image8]  
The active batch is cut off, isolated, and sent to the compute pipeline the microsecond the following condition evaluates to true:  
![][image9]  
This dual-trigger mechanism ensures that under high-throughput conditions, execution begins immediately once the batch size is optimized (![][image10]). Under sparse workloads, the temporal check (![][image11]) bounds the maximum queuing delay to exactly ![][image3] microseconds, preventing transactions from stalling in the buffer.

## **2\. Zero-Allocation Cache-Aligned Memory Layout**

At the nanosecond scale, dynamic heap allocations (malloc, new, std::vector::push\_back) are prohibited in the primary execution paths2. Heap allocations introduce locking contention within the system memory allocator, cause memory fragmentation, and trigger page faults2. MatrixDB avoids these bottlenecks by using a zero-allocation model1. All transactional data structures are designed as fixed-size Plain Old Data (POD) types aligned to hardware cache line boundaries, and all execution arrays are pre-allocated at boot time2.

### **Structure Alignment and Coalescing Design**

To achieve optimal memory access latency, structures must be designed to align with CPU cache lines (64 bytes on x86\_64 and 128 bytes on Apple Silicon)5. If a database record spans a cache line boundary, accessing that record requires loading two cache lines from memory, which doubles the L1/L2 cache pressure and degrades memory bandwidth.  
By utilizing explicit alignas compiler directives, MatrixDB guarantees that each transaction record occupies exactly one cache line, eliminating false sharing and ensuring aligned memory accesses5. This explicit padding also prepares the layout for future GPU deployments, where database records must align with GPU warp memory coalescing windows (typically 32-byte boundaries) to enable single-instruction coalesced global memory accesses23.

### **Memory Layout of DatabaseQuery**

The primary query structure, DatabaseQuery, is structured to avoid implicit compiler padding by ordering primitives from largest to smallest.

| Field Name | Type | Size (Bytes) | Alignment / Offset | Functional Role |
| :---- | :---- | :---- | :---- | :---- |
| query\_id | uint64\_t | 8 | Offset 0 | Unique global database query sequence tracker |
| timestamp\_us | uint64\_t | 8 | Offset 8 | Epoch timestamp recorded at ingestion |
| transaction\_id | uint64\_t | 8 | Offset 16 | Concurrency transaction identifier |
| opcode | uint32\_t | 4 | Offset 24 | Operational instruction (Read, Write, Scan) |
| payload\_size | uint32\_t | 4 | Offset 28 | Active payload length in bytes |
| payload | uint8\_t\[32\] | 32 | Offset 32 | Contiguous inline raw data block |

The base structure has a data footprint of exactly 64 bytes (![][image12]). On standard x86\_64 systems, this structure fits perfectly inside a single 64-byte cache line with zero padding5. For Apple Silicon ARM64 platforms with 128-byte cache lines, the structure is padded with an explicit 64-byte trailing array to force alignment to 128-byte boundaries5.

### **Boot-Time Monolithic Memory Pool**

MatrixDB manages its memory using a monolithic, pre-allocated memory pool2. During the engine's initialization phase, a contiguous block of virtual memory is allocated to hold the database's working sets2. To prevent page faults on the hot path, the pool manager performs a physical pre-faulting pass over the allocated memory block7. This is done by writing a single byte to each memory page, forcing the OS kernel to map virtual memory pages to actual physical frames in DRAM before transaction processing begins7.

Boot Time:    \[OS Allocator\] \--\> \[Virtual Block\] \--\> \[Pre-fault (Touch Pages)\] \--\> \[Physical DRAM Pins\]  
Hot Path:     \[Transaction Thread\] \----------------- (Direct Pool Index Access) \------------------\> \[DRAM\]

At runtime, worker threads request execution arrays directly from this pre-allocated block2. Because all execution paths operate on raw, pre-allocated pointer offsets, the system performs zero heap-allocation system calls during transaction execution2.

## **3\. Compute Abstraction Layer (CPU Mock to CUDA Bridging)**

To support local development on an Apple Silicon MacBook under zero-connectivity sandbox environments, MatrixDB uses a Compute Abstraction Layer24. This layer decouples the ingestion timing loops from the physical execution hardware, enabling full functional testing and validation of the database's concurrency models and ingestion loops on the CPU before deploying to NVIDIA GPU clusters.

### **ComputeInterface Architecture**

The execution abstraction is built around a lightweight, zero-boilerplate abstract interface called ComputeInterface. This interface defines a single execution entry point that accepts raw pointers to the pre-allocated query arrays, avoiding the overhead of dynamic C++ memory allocations or copying data between abstractions.

### **CPU Mock Engine Architecture**

When compiled with the CMake option MATRIX\_USE\_CPU\_MOCK enabled, the engine instantiates a CPUMockEngine. This engine spins up a dedicated thread pool pinned to the remaining high-performance CPU cores of the local machine12.  
The mock engine simulates data operations by performing parallel columnar scans, projection filtering, and index searches directly on the pre-allocated memory blocks. It uses high-precision spin-waits to simulate the kernel launch latencies typical of discrete GPU devices. This allows developers to test thread safety, queue capacity limits, and memory layout correctness locally in a developer sandbox.

### **CUDA Transition and Hardware-Level Spatial Multiplexing**

When migrating to an NVIDIA discrete GPU environment (such as a ThinkStation P330 or a high-density H100 GPU server), the ComputeInterface is implemented by a CUDAGPUEngine. This transition targets the physical hardware layer without requiring changes to the ingestion loop or memory management code:

\+---------------------------------------------------------------------------------+  
|                                CUDAGPUEngine                                    |  
|                                                                                 |  
|  \+---------------------------+                      \+------------------------+  |  
|  |     Host Memory Pool      |                      |   GPU Device Memory    |  |  
|  |                           |  cudaHostRegister    |                        |  |  
|  |  \[DatabaseQuery Array\]    \+---------------------\>|  \[Coalesced Buffers\]   |  |  
|  |  (Page-Locked / Pinned)   |                      |                        |  |  
|  \+-------------+-------------+                      \+-----------+------------+  |  
|                |                                                ^              |  
|                | (Direct DMA Transfer)                          |              |  
|                v                                                |              |  
|  \+-------------+-------------+                      \+-----------+------------+  |  
|  |     CUDA Stream A         |=====================\>|  NVIDIA Hyper-Q        |  |  
|  |     CUDA Stream B         |=====================\>|  Spatial Partitioning  |  |  
|  |     CUDA Stream C         |=====================\>|  (Concurrent Kernels)  |  |  
|  \+---------------------------+                      \+------------------------+  |  
\+---------------------------------------------------------------------------------+

1. **Page-Locked Memory Binding**: The pre-allocated host memory pool is registered with the GPU driver using cudaHostRegister23. This pins the virtual memory pages, preventing the OS from swapping them to disk and enabling direct high-speed DMA (Direct Memory Access) transfers over the PCIe bus23.  
2. **CUDA Stream Multiplexing**: The engine creates a dedicated pool of independent CUDA Streams23. When a batch of queries is isolated by the dual-trigger ingestion loop, the coordinator thread dispatches the compute kernel to an available stream asynchronously.  
3. **NVIDIA Hyper-Q Integration**: By utilizing independent CUDA Streams, the engine maps directly to NVIDIA Hyper-Q hardware23. Hyper-Q enables up to 32 simultaneous host connections to submit kernels concurrently to different execution queues on the GPU, allowing the hardware to execute multiple kernels in parallel23. This spatial multiplexing ensures high GPU utilization and consistent latencies, even when processing smaller batch sizes27.

## **4\. Lock-Free Concurrency for Mutations**

Handling concurrent updates and mutations without using traditional OS-level locks is critical for sustaining low tail latencies under heavy write workloads1. MatrixDB manages writes using a combination of Optimistic Concurrency Control (OCC) and an append-only Delta Log array29.

### **Optimistic Concurrency Control with Invisible Reads**

The database's concurrency model is built on an Optimistic Concurrency Control (OCC) design, inspired by the high-performance Silo engine32. Each database record in the primary columnar store is associated with a 64-bit Transaction Epoch Version (TEV). The most significant bit (MSB) of the TEV is used as an active lock bit.  
During the execution of a transaction, worker threads perform "invisible reads"32. A thread reads data from the primary database store without updating any metadata on the record, which avoids the cache line invalidations and memory bus traffic associated with read-side tracking34. As a thread reads records, it copies the records' data along with their current TEVs into a thread-local read-set buffer32.

### **Append-Only Delta Log Layout**

When a transaction performs a write or update, the thread does not modify the primary database store immediately30. Instead, all mutations are written to an append-only Delta Log30. The Delta Log is structured as a contiguous, pre-allocated array of mutation records30.  
To append a mutation, the thread reserves a slot in the log by executing an atomic fetch-and-add instruction on a global counter:  
![][image13]  
This atomic increment guarantees wait-free slot allocation4. Once a slot is reserved, the thread writes its mutated record data directly to the assigned array index. Because each thread writes to a unique slot, updates to the Delta Log proceed concurrently without thread contention1.

### **Commit Verification and Reconciliation Pipeline**

When a transaction finishes its execution phase, it attempts to commit by entering a multi-phase validation pipeline33:

1. **Write-Set Locking Phase**: The thread locks all records in its write-set33. Locking is done by executing an atomic Compare-And-Swap (compare\_exchange\_strong) on each record's TEV in the primary store33. The thread attempts to set the TEV's lock bit (MSB)33. If the lock bit is already set by another thread, validation fails, and the transaction aborts and retries33.  
2. **Read-Set Validation Phase**: The thread verifies that none of the records in its read-set have been modified since the transaction read them32. It checks each read-set record's current TEV in the primary store. If a record's current TEV does not match the TEV stored in the transaction's read-set, or if the record's lock bit is set by another transaction, validation fails, and the transaction aborts33.  
3. **Commit Generation and Reconciliation**: If validation succeeds, the transaction receives a new, monotonically increasing global commit timestamp26. The thread writes the mutated values from the Delta Log to the primary columnar store, updates each record's TEV to the new commit timestamp, and clears the lock bits33.

This optimistic write path ensures that the system avoids lock contention during execution, resolving conflicts at commit time to maintain high write throughput29.

## **5\. Technical Source Implementations**

### **Core Data Structures (types.hpp)**

C++  
\#**pragma** once

\#**include** \<cstdint\>  
\#**include** \<csize\_t\>

\#**if** defined(\_\_ARM\_ARCH) || defined(\_\_apple\_build\_version\_\_)  
    // Apple Silicon L1 cache lines are typically 128 bytes  
    constexpr size\_t MATRIX\_CACHE\_LINE \= 128;  
\#**else**  
    // Standard Intel/AMD x86\_64 cache lines are 64 bytes  
    constexpr size\_t MATRIX\_CACHE\_LINE \= 64;  
\#**endif**

/\*\*  
 \* @brief Represents a single transaction query record.  
 \* Designed with descending structural member alignment to prevent internal compiler padding.  
 \* Aligned to target CPU cache line boundaries to eliminate false sharing.  
 \*/  
struct alignas(MATRIX\_CACHE\_LINE) DatabaseQuery {  
    uint64\_t query\_id;  
    uint64\_t timestamp\_us;  
    uint64\_t transaction\_id;  
    uint32\_t opcode;  
    uint32\_t payload\_size;  
    uint8\_t payload\[32\];

    static constexpr size\_t data\_footprint \=   
        sizeof(uint64\_t) \* 3 \+   
        sizeof(uint32\_t) \* 2 \+   
        sizeof(uint8\_t) \* 32;

    static constexpr size\_t padding\_needed \=   
        (MATRIX\_CACHE\_LINE \> data\_footprint) ? (MATRIX\_CACHE\_LINE \- data\_footprint) : 0;

    // Explicit structural padding to align with target L1 cache line size  
    uint8\_t padding\[padding\_needed \> 0 ? padding\_needed : 1\];  
};

static\_assert((sizeof(DatabaseQuery) % MATRIX\_CACHE\_LINE) \== 0,   
    "DatabaseQuery structure violates cache-line alignment constraints.");

### **Lock-Free Ring Buffer Header (ring\_buffer.hpp)**

C++  
\#**pragma** once

\#**include** "types.hpp"  
\#**include** \<atomic\>  
\#**include** \<array\>  
\#**include** \<concepts\>  
\#**include** \<utility\>

/\*\*  
 \* @brief Ultra-low latency, lock-free, zero-allocation SPSC Ring Buffer.  
 \* Enforces power-of-two capacities to replace expensive modulo operations with bitwise AND masking.  
 \*/  
template \<typename T, size\_t Capacity\>  
class SPSCRingBuffer {  
    static\_assert((Capacity & (Capacity \- 1)) \== 0, "SPSCRingBuffer Capacity must be a power of two.");  
    static\_assert(Capacity \>= 2, "SPSCRingBuffer Capacity must be at least two elements.");

public:  
    SPSCRingBuffer()   
        : head\_(0)  
        , tail\_(0)  
        , cached\_head\_(0)  
        , cached\_tail\_(0) {}

    \~SPSCRingBuffer() \= default;

    SPSCRingBuffer(const SPSCRingBuffer&) \= delete;  
    SPSCRingBuffer& operator\=(const SPSCRingBuffer&) \= delete;  
    SPSCRingBuffer(SPSCRingBuffer&&) \= delete;  
    SPSCRingBuffer& operator\=(SPSCRingBuffer&&) \= delete;

    /\*\*  
     \* @brief Emplaces a new item at the tail of the buffer. Only the Producer may call this method.  
     \*/  
    template \<typename... Args\>  
    bool try\_emplace(Args&&... args) {  
        const size\_t current\_tail \= tail\_.load(std::memory\_order\_relaxed);  
          
        // Check local cached head before loading the atomic head\_ cursor from shared memory  
        if (current\_tail \- cached\_head\_ \>= Capacity) {  
            cached\_head\_ \= head\_.load(std::memory\_order\_acquire);  
            if (current\_tail \- cached\_head\_ \>= Capacity) {  
                return false; // Queue is genuinely full  
            }  
        }

        const size\_t index \= current\_tail & MASK;  
        buffer\_\[index\] \= T{std::forward\<Args\>(args)...};  
          
        tail\_.store(current\_tail \+ 1, std::memory\_order\_release);  
        return true;  
    }

    /\*\*  
     \* @brief Pops an item from the head of the buffer. Only the Consumer may call this method.  
     \*/  
    bool try\_pop(T& value) {  
        const size\_t current\_head \= head\_.load(std::memory\_order\_relaxed);  
          
        // Check local cached tail before loading the atomic tail\_ cursor from shared memory  
        if (current\_head \>= cached\_tail\_) {  
            cached\_tail\_ \= tail\_.load(std::memory\_order\_acquire);  
            if (current\_head \>= cached\_tail\_) {  
                return false; // Queue is genuinely empty  
            }  
        }

        const size\_t index \= current\_head & MASK;  
        value \= std::move(buffer\_\[index\]);  
          
        head\_.store(current\_head \+ 1, std::memory\_order\_release);  
        return true;  
    }

    \[\[nodiscard\]\] bool empty() const noexcept {  
        return head\_.load(std::memory\_order\_relaxed) \== tail\_.load(std::memory\_order\_relaxed);  
    }

    \[\[nodiscard\]\] size\_t size() const noexcept {  
        const size\_t t \= tail\_.load(std::memory\_order\_relaxed);  
        const size\_t h \= head\_.load(std::memory\_order\_relaxed);  
        return (t \>= h) ? (t \- h) : 0;  
    }

private:  
    static constexpr size\_t MASK \= Capacity \- 1;

    // Isolate variables on separate cache lines to eliminate false sharing  
    alignas(MATRIX\_CACHE\_LINE) std::array\<T, Capacity\> buffer\_{};  
    alignas(MATRIX\_CACHE\_LINE) std::atomic\<size\_t\> head\_;  
    alignas(MATRIX\_CACHE\_LINE) std::atomic\<size\_t\> tail\_;  
      
    // Thread-local shadow indices  
    alignas(MATRIX\_CACHE\_LINE) size\_t cached\_head\_;  
    alignas(MATRIX\_CACHE\_LINE) size\_t cached\_tail\_;  
};

### **Compute Abstraction and CPU Mock Engine (compute\_mock.cpp)**

C++  
\#**include** "types.hpp"  
\#**include** \<vector\>  
\#**include** \<thread\>  
\#**include** \<atomic\>  
\#**include** \<chrono\>  
\#**include** \<iostream\>

/\*\*  
 \* @brief Pure virtual interface defining the compute engine's entry point.  
 \* Enables zero-copy, raw pointer processing over batch slices.  
 \*/  
class ComputeInterface {  
public:  
    virtual \~ComputeInterface() \= default;  
    virtual void execute\_batch(DatabaseQuery\* batch\_array, size\_t count) \= 0;  
};

/\*\*  
 \* @brief High-performance, multi-threaded CPU Mock Engine.  
 \* Simulates parallel database query execution and kernel dispatch latencies.  
 \*/  
class CPUMockEngine : public ComputeInterface {  
public:  
    explicit CPUMockEngine(size\_t worker\_count)   
        : shutdown\_(false) {  
        for (size\_t i \= 0; i \< worker\_count; \++i) {  
            workers\_.emplace\_back(\[this, i\]() {  
                this\-\>worker\_loop(i);  
            });  
        }  
        std::cout \<\< "CPUMockEngine initialized with " \<\< worker\_count \<\< " worker threads." \<\< std::endl;  
    }

    \~CPUMockEngine() override {  
        shutdown\_.store(true, std::memory\_order\_relaxed);  
        for (auto& thread : workers\_) {  
            if (thread.joinable()) {  
                thread.join();  
            }  
        }  
        std::cout \<\< "CPUMockEngine workers shutdown cleanly." \<\< std::endl;  
    }

    void execute\_batch(DatabaseQuery\* batch\_array, size\_t count) override {  
        // Simulates query processing and kernel execution latency using a spin-wait loop  
        const auto start\_wait \= std::chrono::high\_resolution\_clock::now();  
        while (true) {  
            const auto current\_time \= std::chrono::high\_resolution\_clock::now();  
            const auto elapsed \= std::chrono::duration\_cast\<std::chrono::microseconds\>(current\_time \- start\_wait).count();  
            if (elapsed \>= 50) { // Emulate a 50-microsecond GPU execution delay  
                break;  
            }  
\#**if** defined(\_\_ARM\_ARCH) || defined(\_\_apple\_build\_version\_\_)  
            asm volatile("isb" ::: "memory");  
\#**else**  
            asm volatile("pause" ::: "memory");  
\#**endif**  
        }

        // Simulates data projection on the batch elements  
        for (size\_t i \= 0; i \< count; \++i) {  
            batch\_array\[i\].transaction\_id \= batch\_array\[i\].query\_id \* 2;  
        }  
    }

private:  
    void worker\_loop(size\_t thread\_id) {  
        (void)thread\_id;  
        while (\!shutdown\_.load(std::memory\_order\_relaxed)) {  
\#**if** defined(\_\_ARM\_ARCH) || defined(\_\_apple\_build\_version\_\_)  
            asm volatile("isb" ::: "memory");  
\#**else**  
            asm volatile("pause" ::: "memory");  
\#**endif**  
        }  
    }

    std::vector\<std::thread\> workers\_;  
    std::atomic\<bool\> shutdown\_;  
};

### **Master Build System Configuration (CMakeLists.txt)**

CMake  
cmake\_minimum\_required(VERSION 3.19)  
project(MatrixDB CXX)

set(CMAKE\_CXX\_STANDARD 20)  
set(CMAKE\_CXX\_STANDARD\_REQUIRED ON)  
set(CMAKE\_CXX\_EXTENSIONS OFF)

\# Build configuration to enable the CPU mock engine  
option(MATRIX\_USE\_CPU\_MOCK "Compile with CPU-based mock execution engine" ON)

find\_package(Threads REQUIRED)

\# Detect host architecture and set platform-specific target optimizations  
if(APPLE)  
    if(CMAKE\_HOST\_SYSTEM\_PROCESSOR STREQUAL "arm64" OR CMAKE\_OSX\_ARCHITECTURES STREQUAL "arm64")  
        message(STATUS "MatrixDB Config: Native Apple Silicon target detected.")  
        \# Apply mcpu optimizations targeting Apple Silicon cores  
        add\_compile\_options("-mcpu=apple-m1" "-O3" "-fomit-frame-pointer" "-flto")  
    else()  
        message(STATUS "MatrixDB Config: Intel macOS target detected.")  
        add\_compile\_options("-march=native" "-O3" "-flto")  
    endif()  
else()  
    message(STATUS "MatrixDB Config: Standard Linux x86\_64 target detected.")  
    add\_compile\_options("-march=native" "-O3" "-pthread" "-fomit-frame-pointer" "-flto")  
endif()

\# Define the primary executable target  
add\_executable(matrixdb\_proto  
    main.cpp  
    types.hpp  
    ring\_buffer.hpp  
    compute\_mock.cpp  
)

target\_link\_libraries(matrixdb\_proto PRIVATE Threads::Threads)

if(MATRIX\_USE\_CPU\_MOCK)  
    target\_compile\_definitions(matrixdb\_proto PRIVATE MATRIX\_USE\_CPU\_MOCK=1)  
endif()

### **Architectural Orchestration Framework (main.cpp)**

C++  
\#**include** "types.hpp"  
\#**include** "ring\_buffer.hpp"  
\#**include** "compute\_mock.cpp"  
\#**include** \<iostream\>  
\#**include** \<chrono\>  
\#**include** \<thread\>  
\#**include** \<memory\>  
\#**include** \<array\>

\#**if** defined(\_\_ARM\_ARCH) || defined(\_\_apple\_build\_version\_\_)  
    \#**include** \<pthread.h\>  
\#**elif** defined(\_\_linux\_\_)  
    \#**include** \<sched.h\>  
\#**endif**

/\*\*  
 \* @brief Platform-agnostic pipeline stalling barrier for low-overhead busy spins.  
 \*/  
inline void spin\_stall() noexcept {  
\#**if** defined(\_\_ARM\_ARCH) || defined(\_\_apple\_build\_version\_\_)  
    // Instruction Synchronization Barrier (isb) forces instruction execution pipeline flush on ARM64  
    asm volatile("isb" ::: "memory");  
\#**elif** defined(\_\_x86\_64\_\_)  
    // Standard pause execution hint  
    asm volatile("pause" ::: "memory");  
\#**else**  
    asm volatile("" ::: "memory");  
\#**endif**  
}

/\*\*  
 \* @brief Configures OS-specific thread-pinning and priority policies to ensure execution on performance cores.  
 \*/  
inline void pin\_to\_performance\_core() {  
\#**if** defined(\_\_ARM\_ARCH) || defined(\_\_apple\_build\_version\_\_)  
    // Promotes the calling thread to the interactive QoS class to guarantee performance core placement on macOS  
    pthread\_set\_qos\_class\_self\_np(QOS\_CLASS\_USER\_INTERACTIVE, 0);

    // Advisory affinity grouping configuration on Apple Silicon Mach kernels  
    thread\_port\_t mach\_thread \= pthread\_mach\_thread\_np(pthread\_self());  
    thread\_affinity\_policy\_data\_t policy \= { 1 };   
    thread\_policy\_set(mach\_thread, THREAD\_AFFINITY\_POLICY, (thread\_policy\_t)\&policy, THREAD\_AFFINITY\_POLICY\_COUNT);  
\#**elif** defined(\_\_linux\_\_)  
    cpu\_set\_t cpuset;  
    CPU\_ZERO(\&cpuset);  
    CPU\_SET(0, \&cpuset); // Pin strictly to logical Core 0  
    pthread\_setaffinity\_np(pthread\_self(), sizeof(cpu\_set\_t), \&cpuset);  
\#**endif**  
}

int main() {  
    std::cout \<\< "MatrixDB Bare-Metal Engine Booting..." \<\< std::endl;

    constexpr size\_t BATCH\_SIZE\_LIMIT \= 512;  
    constexpr uint64\_t TEMPORAL\_LIMIT\_US \= 50;

    auto ring\_buffer \= std::make\_unique\<SPSCRingBuffer\<DatabaseQuery, 4096\>\>();  
    auto mock\_engine \= std::make\_unique\<CPUMockEngine\>(4);

    std::atomic\<bool\> run\_state{true};

    // Spin up the consumer thread to execute the ingestion busy-spin control loop  
    std::thread consumer(\[&\]() {  
        pin\_to\_performance\_core();

        std::array\<DatabaseQuery, BATCH\_SIZE\_LIMIT\> batch;  
        size\_t accumulated\_queries \= 0;  
        auto batch\_start\_time \= std::chrono::high\_resolution\_clock::now();

        while (run\_state.load(std::memory\_order\_relaxed)) {  
            DatabaseQuery incoming\_query;  
            const bool success \= ring\_buffer-\>try\_pop(incoming\_query);

            if (success) {  
                if (accumulated\_queries \== 0) {  
                    batch\_start\_time \= std::chrono::high\_resolution\_clock::now();  
                }  
                batch\[accumulated\_queries++\] \= incoming\_query;  
            }

            // Continuous evaluation of the Dual-Trigger Condition  
            if (accumulated\_queries \> 0) {  
                const auto current\_time \= std::chrono::high\_resolution\_clock::now();  
                const auto duration\_us \= std::chrono::duration\_cast\<std::chrono::microseconds\>(  
                    current\_time \- batch\_start\_time  
                ).count();

                // Trigger flush if batch is full OR time-based window has expired  
                if (accumulated\_queries \>= BATCH\_SIZE\_LIMIT || duration\_us \>= static\_cast\<long long\>(TEMPORAL\_LIMIT\_US)) {  
                    mock\_engine-\>execute\_batch(batch.data(), accumulated\_queries);  
                    accumulated\_queries \= 0;  
                }  
            }

            if (\!success) {  
                spin\_stall();  
            }  
        }  
    });

    // Simulate query production on a separate thread  
    std::thread producer(\[&\]() {  
        for (size\_t i \= 0; i \< 10000; \++i) {  
            DatabaseQuery q{i, 0, 0, 1, 8, {0}, {0}};  
            while (\!ring\_buffer-\>try\_emplace(q)) {  
                spin\_stall();  
            }  
        }  
    });

    producer.join();  
      
    // Allow the queue to drain before stopping  
    std::this\_thread::sleep\_for(std::chrono::milliseconds(50));  
    run\_state.store(false);

    if (consumer.joinable()) {  
        consumer.join();  
    }

    std::cout \<\< "Engine execution loop completed successfully." \<\< std::endl;  
    return 0;  
}

#### **Works cited**

1. Disruptor-Style Queues in C++ for Low-Latency Software | by Sagar | May, 2026 \- Medium, [https://medium.com/towardsdev/disruptor-style-queues-in-cpp-for-low-latency-software-835721d644dc](https://medium.com/towardsdev/disruptor-style-queues-in-cpp-for-low-latency-software-835721d644dc)  
2. How I optimized my C++ Order Matching Engine to 27 Million orders/second \- Reddit, [https://www.reddit.com/r/Cplusplus/comments/1q13dbl/how\_i\_optimized\_my\_c\_order\_matching\_engine\_to\_27/](https://www.reddit.com/r/Cplusplus/comments/1q13dbl/how_i_optimized_my_c_order_matching_engine_to_27/)  
3. "Spinning around: Please don't\!" (Pitfalls of spin-loops and homemade spin-locks in C++) : r/cpp \- Reddit, [https://www.reddit.com/r/cpp/comments/1qo9ktw/spinning\_around\_please\_dont\_pitfalls\_of\_spinloops/](https://www.reddit.com/r/cpp/comments/1qo9ktw/spinning_around_please_dont_pitfalls_of_spinloops/)  
4. Lock-Free Data Engine in C++20 (Part 1/4) | by Josh Brice | Medium, [https://medium.com/@joshbrice2025/lock-free-data-engine-in-c-20-part-1-4-e61faf7948bd](https://medium.com/@joshbrice2025/lock-free-data-engine-in-c-20-part-1-4-e61faf7948bd)  
5. How I optimized my C++ Order Matching Engine to 27 Million orders/second \- Reddit, [https://www.reddit.com/r/quant\_hft/comments/1q17ib2/how\_i\_optimized\_my\_c\_order\_matching\_engine\_to\_27/](https://www.reddit.com/r/quant_hft/comments/1q17ib2/how_i_optimized_my_c_order_matching_engine_to_27/)  
6. Simple MPSCQueue with explanation : r/cpp \- Reddit, [https://www.reddit.com/r/cpp/comments/1p2bizu/simple\_mpscqueue\_with\_explanation/](https://www.reddit.com/r/cpp/comments/1p2bizu/simple_mpscqueue_with_explanation/)  
7. Optimizing a lock-free ring buffer \- Hacker News, [https://news.ycombinator.com/item?id=47501875](https://news.ycombinator.com/item?id=47501875)  
8. How to set CPU affinity of a particular pthread? \- Stack Overflow, [https://stackoverflow.com/questions/1407786/how-to-set-cpu-affinity-of-a-particular-pthread](https://stackoverflow.com/questions/1407786/how-to-set-cpu-affinity-of-a-particular-pthread)  
9. \[Apple M1\] Force an execution on a specific core, [https://discussions.apple.com/thread/252525908](https://discussions.apple.com/thread/252525908)  
10. C++ 获取CPU 信息与绑核 \- 知乎哲也, [https://liuzhe.vip/2025/04/08/C-%E8%8E%B7%E5%8F%96-CPU-%E4%BF%A1%E6%81%AF/](https://liuzhe.vip/2025/04/08/C-%E8%8E%B7%E5%8F%96-CPU-%E4%BF%A1%E6%81%AF/)  
11. How to execute terminal command on energy efficient cores on M1 chip? \- Ask Different, [https://apple.stackexchange.com/questions/419758/how-to-execute-terminal-command-on-energy-efficient-cores-on-m1-chip](https://apple.stackexchange.com/questions/419758/how-to-execute-terminal-command-on-energy-efficient-cores-on-m1-chip)  
12. Tuning your code's performance for Apple silicon | Apple Developer Documentation, [https://developer.apple.com/documentation/apple-silicon/tuning-your-code-s-performance-for-apple-silicon](https://developer.apple.com/documentation/apple-silicon/tuning-your-code-s-performance-for-apple-silicon)  
13. QOS Class not getting set for pthread in macOS \- Stack Overflow, [https://stackoverflow.com/questions/77871878/qos-class-not-getting-set-for-pthread-in-macos](https://stackoverflow.com/questions/77871878/qos-class-not-getting-set-for-pthread-in-macos)  
14. Energy Efficiency Guide for Mac Apps: Prioritize Work at the Task Level \- Apple Developer, [https://developer.apple.com/library/archive/documentation/Performance/Conceptual/power\_efficiency\_guidelines\_osx/PrioritizeWorkAtTheTaskLevel.html](https://developer.apple.com/library/archive/documentation/Performance/Conceptual/power_efficiency_guidelines_osx/PrioritizeWorkAtTheTaskLevel.html)  
15. FR: Thread-Priority vs Efficiency/Performance Cores \- JUCE Forum, [https://forum.juce.com/t/fr-thread-priority-vs-efficiency-performance-cores/49025](https://forum.juce.com/t/fr-thread-priority-vs-efficiency-performance-cores/49025)  
16. Why does hint::spin\_loop use ISB on aarch64? \- Stack Overflow, [https://stackoverflow.com/questions/70810121/why-does-hintspin-loop-use-isb-on-aarch64](https://stackoverflow.com/questions/70810121/why-does-hintspin-loop-use-isb-on-aarch64)  
17. Spinlock for any IRQL (Arm64 CPU) \- NTDEV \- OSR Developer Community, [https://community.osr.com/t/spinlock-for-any-irql-arm64-cpu/59192](https://community.osr.com/t/spinlock-for-any-irql-arm64-cpu/59192)  
18.   
19. Is there a \_\_yield() intrinsic on Arm? \- linux \- Stack Overflow, [https://stackoverflow.com/questions/70069855/is-there-a-yield-intrinsic-on-arm](https://stackoverflow.com/questions/70069855/is-there-a-yield-intrinsic-on-arm)  
20. Lockfree buffer updates with variable-length messages in C \- Stack Overflow, [https://stackoverflow.com/questions/62743288/lockfree-buffer-updates-with-variable-length-messages-in-c](https://stackoverflow.com/questions/62743288/lockfree-buffer-updates-with-variable-length-messages-in-c)  
21. bowtoyourlord/MPSCQueue: A simple, lock-free MPSC (Multiple Producer, Single Consumer) queue implemented in C++ for learning and experimentation purposes. Useful for everyone who is interested in low latency & extremely high performant systems. · GitHub, [https://github.com/bowtoyourlord/MPSCQueue](https://github.com/bowtoyourlord/MPSCQueue)  
22. SPSC Lock Free Queue Implementation using C++ 20 | by Serhat Gül | May, 2026 \- Medium, [https://medium.com/@serhatg2490/spsc-lock-free-queue-implementation-using-c-20-dd7a140d10f9](https://medium.com/@serhatg2490/spsc-lock-free-queue-implementation-using-c-20-dd7a140d10f9)  
23. GPGPU \- General Purpose Graphics Processing Unit, [https://www.cs.rochester.edu/u/sandhya/csc458/seminars/GPGPU.pdf](https://www.cs.rochester.edu/u/sandhya/csc458/seminars/GPGPU.pdf)  
24. Apple Silicon Support · Issue \#69 · openai/procgen \- GitHub, [https://github.com/openai/procgen/issues/69](https://github.com/openai/procgen/issues/69)  
25. Why does march=native not work on Apple M1? \- Stack Overflow, [https://stackoverflow.com/questions/65966969/why-does-march-native-not-work-on-apple-m1](https://stackoverflow.com/questions/65966969/why-does-march-native-not-work-on-apple-m1)  
26. Opportunities for optimism in contended main-memory multicore transactions \- Harvard DASH, [https://dash.harvard.edu/bitstreams/bd5d4a77-bbe6-40b9-bb02-069d9f03c62d/download](https://dash.harvard.edu/bitstreams/bd5d4a77-bbe6-40b9-bb02-069d9f03c62d/download)  
27. Dynamic Space-Time Scheduling for GPU Inference \- arXiv, [https://arxiv.org/pdf/1901.00041](https://arxiv.org/pdf/1901.00041)  
28. SwitchFlow: Preemptive Multitasking for Deep Learning \- UTA, [https://ranger.uta.edu/\~jrao/papers/middleware21.pdf](https://ranger.uta.edu/~jrao/papers/middleware21.pdf)  
29. Hekaton: SQL Server's Memory-Optimized OLTP Engine, [https://www.cs.cmu.edu/\~15721-f24/papers/Hekaton\_SQL\_Server.pdf](https://www.cs.cmu.edu/~15721-f24/papers/Hekaton_SQL_Server.pdf)  
30. Epoch-based Optimistic Concurrency Control in Geo-replicated Databases \- arXiv, [https://arxiv.org/html/2602.21566v1](https://arxiv.org/html/2602.21566v1)  
31. Delta Lake Transaction Log: How It Works \- Conduktor, [https://www.conduktor.io/glossary/delta-lake-transaction-log-how-it-works](https://www.conduktor.io/glossary/delta-lake-transaction-log-how-it-works)  
32. Polyjuice: High-Performance Transactions via Learned Concurrency Control (OSDI'21), [http://muratbuffalo.blogspot.com/2023/02/polyjuice-high-performance-transactions.html](http://muratbuffalo.blogspot.com/2023/02/polyjuice-high-performance-transactions.html)  
33. Opportunities for Optimism in Contended Main-Memory Multicore Transactions \- VLDB Endowment, [http://www.vldb.org/pvldb/vol13/p629-huang.pdf](http://www.vldb.org/pvldb/vol13/p629-huang.pdf)  
34. An Analysis of Concurrency Control Protocols for In-Memory Databases with CCBench \- VLDB Endowment, [https://vldb.org/pvldb/vol13/p3531-tanabe.pdf](https://vldb.org/pvldb/vol13/p3531-tanabe.pdf)

[image1]: <data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAACsAAAAaCAYAAAAue6XIAAABwklEQVR4Xu2WTytEURjGH/9ZEAvKAgufQWQjf8IHUCxkslD2SvIRlJIsfAcfwYaNZCU2oiyQBRaUQv6+r3MuM49773tGQ9H86qnxO89953TnzjFAkb/DIAuDZkkpy+/QJlmVrEjqaC2OackcywBeWeTDEtyACf93q+RCcv/R+EqL5JxlFvVI3lSl5IWlhX4cOnCTFzxPSB6q11WTa5Sc+LUoSWxJFlmmocOOWWbRB9fpJ98teSDHWJstQ/p6Dmewy9GdXyP/CPtZtTar6PoAS6YHrrhBnmmA612TV1dDjgnZ7IFkhyWjd0YH8TPHjMP1drNcrXcWIZudh90JGqQcwvX0iIro9c4i5D3GYHSaEDZIietNxrg44q5lOmF0om/hHS8QI3A9PtYy3luEbLYDdidoUFKnC/GeSbo+m1HYHdwgvRQd7BW8gM8TwiJks3r8WZ13tLTHUriEOy3S0Gv1X2YaIZvdR+5Jk8oV3MBtuGdYX+tDb6G9GZYePZP1N8Opj77mczpC5wyxLDSzkluWeVIC+84XDH2jcpZ5sC5ZZvlTDEuOWAaivzmeWf40C5IplgH8+kYjMiwM2iVVLIsU+Q+8AcPof4U5yGDQAAAAAElFTkSuQmCC>

[image2]: <data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAABMAAAAaCAYAAABVX2cEAAAA3klEQVR4XmNgGAWUgnlA/BmI/0PxAhRZCPjLgJAHYWdUaUyArBgb2AfEKuiC2AAjEG8H4vUMEMOCUKXBAJclGCAfiE2gbFyu+4MugAu8RWJ/YIAYxockpgbEnUh8vADZJaBwAfFvIoktA2IeJD5OAAqvzWhi6F7F5m2sADm8kMVABnRD+b+Q5PCCd+gCUABznTYQt6DJ4QS4vLCbASJ3D4g50eSwAhYg3osuCAVMDJhhhxMwA/EbID6JLoEEvgHxD3RBdLAKiD8yQNIXKF2B8h42oA/E2eiCo2AUDGkAAM4NNN65dbHtAAAAAElFTkSuQmCC>

[image3]: <data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAABMAAAAaCAYAAABVX2cEAAAAxElEQVR4XmNgGAWUgmgg/gnE/5HwGyT5X2hyt5HkcAI3Bojip2ji3ED8D4i50MQJApjt6GJkgZUMEM3NUD6IzYyQJg0wMiBc9w2IBVClSQe/GSCG2aNLkANOMkAMu4cuQSqYBcTlDNgjgiSQDcRLoGxYRIAMJhm4APFpJD5yRJAE1IH4BbogAyLlS6BLYAPsQDyfAaKBDU0OBIoZIHLP0CXQwS0g/gDEb4H4IxB/RZVmeAcVB8mD2J+BuBJFxSgYBUMVAABeDDWMwfmAxgAAAABJRU5ErkJggg==>

[image4]: <data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAABMAAAAaCAYAAABVX2cEAAAA1UlEQVR4XmNgGAWUgtlA/AmI/yPhVygqGBi+IMmBsDeqNCaAKcQGmoD4PLogLsDIADHoFroEEFwGYl90QXwgmwFiWDiSGBMQ/wNiLiQxosBLBlQvGgLxUyQ+SQA5vKZB2ccQ0qQBkOYLDBAXakH5uCIDL4CF1x8ksSVQsXwkMaLAawbsriDLdbg0vWWAiCuiS+ACzAwQDafRJYBAlQEi9x5dAhfoZ4BoCEWXgAKYqwXRJZDBMgZIfnwHxV8ZIAkUBmQYIC4CpbXHDBC195DkR8EoGLoAALqKPUMnIoY7AAAAAElFTkSuQmCC>

[image5]: <data:image/png;base64,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>

[image6]: <data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAg4AAABOCAYAAABIfJ58AAANSklEQVR4Xu3dBbAkRxnA8cYhuBQaSIBgoXAtCCQUUGhwCFbkkACBoqBwvwteuLscUoUV7h7cg7vkkODB3eefmS/7vW9n9+3eveOOt/9fVddtfyM709sz09PT8641SZIkSZIkSZIkSZIkSZIkSZIkSZIkSZIkSZIkSZIkSZIkSZIkSZIkSZIkSZIkSZIkSZIkSZIkSZIkrYZTduksw+dzdemkaZqkDXb+Lp2kBjfQ2bp04RrchM7bpdPV4AbbjGV5gRpYUefs0r41uEGom2eswU3mnyX/n5LXCrpf6yvCoknr+3mblBet9Y12nTZZ/9vLtM3kg22yn5cv08b8tUuvq8F1bMayPL7t3uP1gW33rXsjPaRNyuGRZdquelObrPtaZdpm8q823cPAOe3fJaYVQ8XnAKuxemKgstTYqllm/8/a+vl3R8MhbKaL3Tzs53oNB3oMxurtojZbWZ6v7XxZhFnLcwfKtKvVCXsptnWjGw5hMzccXtqlt9bg4BNdekoNanX8rAZafzCMtShnnUhWxTL7v0+z4bBRFmk4gMbDztpsZXn2tlx9HTNv+QvWwF7MhsPOmff7n6zNn65N7AGtfw6fXbr1FeJpJY5jamCFPLUtd6Ccptlw2CiLNhx2xWYrSwazLVNfqxu1XVt+b2LDYXm3bOv//pt137WOI2qg85bWV4gz1Qmdy9XAJsJzvBd36UddekyX7p+mHdb6MiFdf0hnSNPB8nfv0ku6dFCb3XAYG4BH7wTzV7Tqn9GlX3bpY126+NrJay52nOhf06XrTiaf4LZdOqpLrx7yNBa3nDh14o5dOq5LL2/93Wp2u7Z2HQwIo5vyESfOMW3e+jBrnw+ogTZpOFDG9+rSC9p078Ltu/ToLr22xHGe1jf8njDkt3TpH63fxrBIWS6CO3G28dldukqXLtal57XpwZ1X7NLXu/SOLl2pTJtXFwPl9PHWdxnfoUzDmdvaE/9FW79d1Kc47qmbd+nS47v0xiGGq7bp+h43GJfo0r279Nw2XR9xaJe+36U3tH7ejOPo4V165ZDnHLO1TX6XncXv++4u/bj1Y1xOsXbyzIbDlbv0lS69t/VjXcacvkuvav24pfe06XrHuq89fOY3vWyXLtmmf9OxcyeDNrONPs7YHrblMq2/wHMM8flSbbJ9sc3EDhli+GHrzznzfLNLn6lBraY4YayS2u2W86fq0n2HPInPpHyQ3mSYdo0hzwmTCxOx3HDYb4gx4CgbK3NOanmdnODJ5+8l/87WH+TgpEaMC1V42BCL7+VEwuc7DdNj7Eq+SP6lS89KeU5csY73tclo/V8MsWyR9WFsn3k8RowLQUZsS5deOOTjbjh3l3PxGVsnF4W/t/43ZbuZzoWTiyUXmkB8vbJcBL/b61u/LI2Ymw2f83Z9qfUXosA0GpyYVxfDp7r0nZTn4sLA0Kw2HA5ufSOFGI0H0HB7+hDL81K/eYRJLOo7Fxdwgf3sMO3qQyxQzvki94MufTLlH9z67WRZfq9rDnHqVN3HRW1v/bKnHfJ8Z10X+dpwYP/elvIfaP0FOLtb65eNRhMNiLF1R6ODi36UJQ3PQMOOGBfarJb7Rh9n7HOsj/MAvQhc6MlHvabhEfPQqAvkx24ss4e26e3SiqIi1Ndv9jQudtyljKVXtL61/bLWD+bhTu1F/WILu2ebPgDoecnqQZ4Rryemiwzx2uPAyfWxJfbdIWUs+9GUv9UQy8a26XcjMS4sxM4x5LmYnmT4TAPnG8PncPLWz8872yFOmgemGHcvxGJdWHR9Y/v8pNaXT8WyXBBqjAt9duwQD9zlk8+POei9qeWDRctyUXl99IZEr8EtUjzcNMXWq4tHtunpIJbvEGvDIRCLhkOO1Xm/NhLLmJYbDse22eOi8t18NIDZj4wYd7/LuFDrl+NuOoxtN/l8fNKgq/OA2OOGz9wwkKfHJ9DLUpcjf73h88tafz6q4iLPDUb26y69v8Q2+jijsT22zbkH5NOtXzZjnmjYzXLrNr1urSC6r6gInMBXSTwPJj2z9SPSq5hexUW54pEE8dpwWEQckHR1z8M8x5ZYvAqa8XvWGOJxyo3rhNbHv5Xy24dYRjc8sVMP+WXWtwyWPXwkxp13Vi8adKWTzyfh7w2xatGyXBTL5e7/QJxEIy4nYtSV9epiTKs+2NbG/9cNB/Jj46LqureUfCC27KMh7uDH1lUxT244kP9Cyod4WwTPHz7ni/UY5rlBl45u/eOwXbW9Te/TrhxnPPYidpshHzcgO2KGIV8Ri56WWeJ6oRX3rtZXhPr8fhVwV8i+R/rt2sknxiu6N8fi0XDgzmVZdKGy7CInLZ6TZ3S/1+2Z1XCILtR4RpvV/aUnp67jgCHGiQzLrG8ZLMcJr8a4U8q+PMQz8vm1MfJ0tVeLluWi6veGKAfukmsK8+rirHJ8c+vjPNrAvIbDIo+Nlmk40LghX3vSUNcd+1YRizv3RdV1z8I8W0v+4ykfKOdY3x/S53mYh16ynw6fd9XuOM7I/234zOOMPPB1/zY+hobp+9dgcYU2/V1aQWOVbm9Ay/eJS6Zl1Iv7o1pfDttSrJZNDKQbu1ghGg5xl7AMuktZNj/DH8M8fH/2oyGezWo4xBs0POes6v7y+Keug+0jts+QX2Z9y2A5xgrU2GdKjLED9Tv+2KXft/7EyTTuzMcsWpaLYrno9s7WK4f16uKs5T/R1sbnNRzquI2xdX61xGq3ONNqjwOPDKu6bgYA1u8CsevX4Dq4CI6tq2KerSVfH30hb2stz1mYh3Et8XnHZNJO2R3H2bYUi8YxeXp4vjjkK6ZfowaLw9r0d2kFUQnqwL3szq2/E+HOIk4kuaIyYp/PjHDO0xh4xL8MeosYzw7pCs7PP/eUra0fuJVxEWIsQKgH5DHDvzHYMAYyhWg4xMEeGNleH1/Qco/BZ4FlX1di9EDkixHzcILPxu6SnzwSC8TzILFAnMFPYfsQy+JOKAamYdH1je0zjSye8Vcsy9iAGqs9B4yQr9tY87MsWpaLYrmxO3BiY+vkLpA/Gra1za+Ln2/jy8dg3BCPP2qvFbE6BohYXScXlByr08nXhsNvUj4Qz8fRliFWEaPLfxkHt365A0v8hm16EPFRKf+nIVYRY1wLYnzMPSaTT3Du1v9WgXkYzwTGaJCvYxnA4MSKwYr8Ttn2Nr1tu3KcBeLUo/2HPOdeelXGxhSB+Tnfz0M9rduqFRNdiNtLPFy0rW1U/Dl9zpXnfm3ScEBU+Pu0yWtSxBh4Q9ckrxztadva9AHAqOvcfRyPcUBZMA4h7GjTA0qZl3R4isWdQv2usRjPS4nxnD5w4EdXNJjOXXGWu1sDg0hrLES3Ja+dBe62uJvL4vFJxhgMYvul2KLrG9vniOULAY0JYkemGIh9u8R2DPGMPG9KsA28Tre99RecivkWKctFsRzdzmPo/aj1JY6tbW36O2tdZDrP4AOv0hHL/2dCPBcfe30wjw3huIxyzx6UYjwnz99Hw5dpXKDDhYZYrq9jgwk5PxDLjWca18QOT7FF8duybG4g1UGaTH92ysf30fsRbjvEsg8Nsfzolt6rwPmM6QelWJRlvpGIMuZiHdjemDfb6OMsUL/yeuklIc9+j/lJ6/d/Hhrqs3ostIlxUuH1KE6Qxw8pd+tm3N3VEcAhz8urW7XhUI3F9qRtXbp5m7z2RDoizzCIO9B6wcJz2mTZz7VJjwMpni+C7vV8wgXLkip6Y2Id3AnFM85DW38S4kLHNvG7gX/JE2dfOKHRVc9JgBjPYfm9q3279Ks2+S66xzPqR6yXi9ijW7+eWC//5obkeuvD2D4z2Cv3IlBWbDPfcVzr7xS5S8v7zsh0sI1sB/NFeSB6IcYSlinLRTAIj/WxLIlyoFeqemGbbEceW7GtLVYXGb0f0zl5nzxNo77Fb8PvlcuDRnx08ZMOSZ+jTAJjSIhRtuEjrV9n1KcdaRrrjgGLpNpwoi7y+8SyH259Y7j+xsuKBkotS+ppbCvlkddNYyh6Qkn1DZ2QG1b8noHXJfN2U6b0oPG7xffROxS+3/rGXPbmtnZ8we44zsJdW1/eWf29sy1t/nQwfW/oMdZejEp3dA0OcgXjQODOLoxVvrGYtNF4nDZvTMOsu60xB7fJCXpe0q6rZTqWaqNEG29efY4eE2ku7iZyRcld9TnO62605MNY5RqLSRuN5/6PqcEBdbA+v5Y0wf8A+uoaHNDjwuvC0rp4jknXGd2Ll0vxu7T+j/nQTbil9SdlKhZdk3Sl0QUX76PTpUfXKQOpeK1I2l14Nk1dzH9fgBiPmuhSljTf2E0ej3nmDaKXpP97jHbnTzof3fpGRP1/IySN4/XgOuDSRoMkSZqJXrp4Y+eCbfpvjUiSJEmSJEmSJEmSJEmSJEmSJEmSJEmSJEmSJEmSJEmSJEmSJEmSJEmSJEmSJEmSJEmSJEmSJEmSJEmSJEmSJEmSJEmSJEmSJEmSJEmSJEmSJEmSJEmSJEmSJEmSJEmSJEmSJEmSJEmSJEmSJEmSJEmSJEmSJEmSJEmSpN3ov6JseCxBHsJnAAAAAElFTkSuQmCC>

[image7]: <data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAABkAAAAaCAYAAABCfffNAAABCUlEQVR4Xu2TPw5BQRDGB4nEJZyAC0g0ohB6F3ABUSgkKo1EoRC1aHS4gAvoNCqJTilaKmYyuzI7dp8neYXi/ZIv2f2+2X377wGkJMgDVdBmkgxQT9RFB0lCH7DKqywE1Ta1GaKLGqGGwAPPbuylDFyb1UEIKpZtUk54Prbgjoukg5qI/hh48FF4kgbwEVHNHdVCVZ0KD77V2N346KH6wPnK9OtOhaKNmmsTmQFPsteBwd5HRgc+QqslonazhnDmQGe71KZgATzRTgfA/lWbPuKsJLQb8ujZR1IB3vI3NsATytqS8ez/UYPAxdsV/iILPVnZv4n2myJ8ThBHUxpsOBnvILyUlJR/4wVMWVSSGaGDLQAAAABJRU5ErkJggg==>

[image8]: <data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAg4AAABOCAYAAABIfJ58AAAEPUlEQVR4Xu3dS6h9UxwH8OUdkjxKKRNJUopIHmUiCWFCUSIDAyElCTFBeWVEFOKPmImSCXELAymPKCXkMSDvIiTP9Wvt3Vl3Oefcc/j/79nnns+nvt29f2vv21r//2Cvvc86+6YEAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAE/3RFgAAxnkh5++cp9uGgYq+zprXunOWWTumadkK4wVg4OoLz6zi2H3a4iY4POf3nD2r2jGp9OfeqhY+ybm2qS2bVRsvAAP3bM6pOS+lcjF6Yn3zWFel+SYZ29NbOXs1tWdS6c/+Tf3unEOb2rJZtfECMHD1BGDWpw7fpdmO2xH+agtpcr9fbQtLaNXGC8CAxdOFs6r911O5IN1f1Wpn5JyZyjFxJxznHrnuiB3vqLaQSn/+bIvZ5W1hCa3aeAEYsHF3rZPuZsM1OTek0n5rt3/EuiM237Gp9OeetmGLWrXxAjAQ8VTh/LaYvZvKhenOtqEz7/qGeKoxLo/nPJbzaM4jOQ/nPNSdM4/nU+nPvm3DFjXLeHfN2aktAsD/Meniv3Oa/tThmzS5bRGm9XVRds85bsbEE4R5bDTej3MOSeWYbcnXMwHYDu7IubQtVj5K5cJzU9uQSj1W9Q9F9GdoL6/aO+ecGXN2d86spo13jzSaVMRXZc/LuWjUPJcr2gIAq2vaHWvYLY2/s43H31E7uqlPc9ecmccJqfQnvoa4CjYa7345P7bF/+C6ZOIAQOfGVNYpbOTLVC5S9bFXdrXeLTkHVvub7cVU+jPt8/7PUlnP8UrOSTkXpHLOe117PUE6sduOd1q8kfPbhNqibDTefiz9eMZt35bzVc7pqUwE46ONy3J+yTksZ606tj8XgBVWXxRmTS/udPv9uLt9u2pbhLZ/rQ/S6M75+pzbu+1L0mjiEOrfEZOLfv++KbVF2Gi88X9Stx/c7Md2vOwrvlYb6yAeyDm+a4vJSEwcwlryxAGAVD7vricEs+bcOLnTL45c1DqHeH/BTznf53yb80Mqd8tRj7vpWvQzFnu2LkyTJw7xccAX1f6k2maZZ7ztxCHeLFnv19uhPz5SvzF0LZk4ALCC4oK4S1tMZdHg+9V+O3H4tNqfVBuiduLQ7rcTh4O6n/ESr5iUxLs5wss5V3fb8bQFAFZCrEuo76T7pwan5XxY1esL6sk5n1f7k2pDdECa74nDk2k0sYo/ovVgVb+52z6l+wkAK2Fbzq/p3xf+N1P5c+LPpdHj+pggxEcB8bc44uOAMK42RLFIte/nz83+O6n8G8R21C7uznmqSyy6jMWwta/T+qcyAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAshX8APCA0+YIdMicAAAAASUVORK5CYII=>

[image9]: <data:image/png;base64,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>

[image10]: <data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAEEAAAAaCAYAAADovjFxAAABtUlEQVR4Xu2Wuy9FQRDGxyMkEgW1SBQUCsFfoNEQpfgXPAqFRq2RaFQ0hHhESSlKCgoFiU5BREgQjwgVwUxm150z2XXOVdxzivklX+7Z77u7mX2cB4BhGEacJdQr6lvoPvEPgDeRkQaTcUW5Q31AqZbqZAyzIiPR3DLjO4WYQZ1oM0e6UQfA9e6pzBObS5Qq4E7nOkDOUEPazJkdVC3EN47mc6zNNCaABxsRHh2zL1SD8IqCn/iCux4XGTGKGlBeKnSfyRXtQd2IdtF4Edeh03Cr2pmQAy2668NSXCi6UFOifQlcb6vw9KJkgjqdAp+ITtf+10BII2ojonXUGmoVtYJaBn47lcM2qka0m4FrfRBe2Rvonwefwtt03qTwikJoc+SmjaH6RZYJWsG0gYtE6L0/DFwrbZ7+xslEbLKPwH6bDlKoR82Vqaz0oqa16fDzCM3lT+jeok6hd2o7cPasgxzZRdVp00HPG6p3XwdpzAN3pOMUwq9skw5yoAO4FvqNQXmfNmNsAd9bT07vwB9GnhbgE0DfCtfA/70QeaWh+ujbgG5RquUoGf9ypQ3DMAzDMAzjH/wAvPx/pfMNHdQAAAAASUVORK5CYII=>

[image11]: <data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAEcAAAAaCAYAAADloEE2AAACJElEQVR4Xu2XP0jdUBTGvyq0apcOUoSCm4hgF5cWpQpFS1FwFMTRzSKUFiwKgiBIwcFBXFyKIIJD7erg5CYuLjoo6KQ4aFsplraCeg4nl3dzXpJ3kzweSu8PPkjOd/8lJ/dPAI/HU2H+kmp10ANMkG5Ix9q44wxBkspjNzqz/H/KO7A8Z+wGHiovDi7bq4MR9EDKjmijjLxBdHIfk65JdSruzHvSNGkS0sFh2I7kOaRslTYS6IDUmdVGmTDJ1bFc2A2YDqqtWBTfkL3jJshUWNFGTlYhY+JEMy7Pkcgwwpn8DGl014rZvIVMJS7zh9RHehUq4U496QdpUxsZeYBCcn+TnoTt9ERlP+rzNHwgjUF8zjzfd4dKpKeGdETaQ85ME1eQsXVpIy0DpAUdJOYhHWxpI8CsN5ypcvGU9JO0ro2U8Jh5bC7rZiJxXweT9PV8RbyXlmbIdrukjQwskj4heexO8NqRNKAvkA42tAGJn+tgSjoh7cxoIyPvSMvBtVmY+UVlwuXNxmWAY7z9Z2EQUr+cZx5e87ate3thTk07ZGqUYg3SgV22NYiZ881ruC3Io5B6/drICU/LUx1E4WTcoI1SmLeaRgbeuu173opLMUVq08GcPEJh6ked6D9CvBNtJNGI4gd30RxXDuD/E47tWLFKsg/Z2XjduyBdhm18D+Ls8/Uv0niohOd+8AwyHV30Mqjz38C/CS8c1RLU8Xg8Ho/HU8Qtt7uh7hkON3sAAAAASUVORK5CYII=>

[image12]: <data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAPYAAAAZCAYAAAACANOfAAAFYklEQVR4Xu2ae+gtUxTHF9cj4Xomeed9PZJHQiFdxR8iSVcRRd3bTfIfyX+i5FEiIo+8IuUtidCV5A8S3SskXM/kTeT92F971u+sWbNnzp615zfzz/rUt3P22jNnrbP32jN77xkix3Ecx3Ecx3Ecx3GGZtugL4L+DfowaKt69WjcSTGG34POUXVjcWLQHxTjeEbVTcHuFGOZGsSwnTaOwCVB/1D0f3e9aoHdgtZTPGZt0NJa7TTsEfQlxZierFclGbx9N6E4qJkNKDo5VNjG4POgPUX50aB1ojwGJwfdIcr70PSDCv6njuFBWoTEy+C9oGNF+RdqtsUZQa+K8kcUjzlN2Mbm1qA/RRm5/KwoaxalfX/QhsBBQX9rYwZXaUMmhwU9pI3U7MRcntaGTFL+Hg66TRszSP1WX56jdDLnYu0PCWZzH1NZ4pX0h/zvF1dlDAQG5TNFmW3WNitlOTV9o5waZ2CI9k2CH1ytbIdU9r5crQ2Z3BD0mzaSLQZQkkhbK9tj1D4F7MIaO7NL0BNB35D9t6z9IYFvXOhLEq+kP3BhYy6tbNcLW2oQf13Zlin7GMDv+8q2oypLhmjfJN9T/NHnhQ1XFzkFyuU6bcjkGIoxYJawZWU7O+i7hSP6UZJI0IXKZsF6HsPnlwxsa38wdwXtT+WJZ+0PzY8U48BykcFN6SxRBtijwXFjr7XhD35vqsonBW08q24wVPsmWUKzhIYwqE+pHZGPvJL2BVc5juGlqmzFmki4mHEMv1afW9SOyMc6GMEjQbtW30sGdkl/4H+/Xn0vTTxrf0iuoRjDwboiAffh2OAiA7+XUVzCbR60huJmrGbI9m1lJ6oP7rfr1dmUJBLg9SQL6w8LJYl0EdVjuLJenY01sdC5L4ryVAMbO9FMaeKV9McOFJdqLwd9RvNjuJlirPvpigT3degeiksw3FXxtOb2oE3/P6sd7MVw3khQfkvZhmzfJJgu4O4EjqdZYDoQzREJ3Z+wseYBnwdSnGbxlEs3kEb7YL2SsEEbxtNaeSroheo7koljOHfhiCZIPO0HwnnaBh0QT2tF/+fcga39QNb+wN3xcFHOTTztg2XtDw0usojjOF1RgfhQf6SuGIlrKfp/QNmxxJR9aG3fXqSS5hNK2yWnJvR4wsbqYj01d3BXUYwBV+A2tA8WpjjaBnVdcfel5n/erLJpu2RvavqBcI62QSfE05Lgio81lyR3YGs/kKU/8PjzHWXLTTztg2XpjxT8KDbVHlzHS5gpOI9iDBco+0+VHZS0bzZ4TJBqJAD7Udo4B+vUry0G3D15NtEHy9QPyw9MwTQ8uPtiOQcvxGDKKcWJjO/yGXsOlv7As3sdA+9/4Hkxyn2x9MfRFH3qu2/bwIYNa1oGg2svUU6BO2cfLY2ntYIXuxCHfsrES0ywGO3bAC+hpBoJtNm7sCQSgK/ttZHiVLJvMgNLIuHR0jptrLC0heWcFG2JnIO1PzQrKcZgvaNY+mMtRZ9rlD3VHnhUupGyWfeJSkFsOmdTMUtK2zcJfvAKZUP5TWXLwZpItwT9pWzYNexqjC4siYTEgD88P5Z8ELRC2XKwxq6ZlxRdWPtDcznFGLBcsWDpD7y0JN/eAvdSjAP7QgwvG1OagvOp6Rvl5comKW3fVniz6t3qE3dKCyWJhHPhm18uwFW46xlgF5ZEAjvTLCm+qj5Prx2Rj+7cvrxB8V3jTyvh+2u1I+ZT0h9gm6BvKe5GIwa89otc6Yu1P3ip+DPNNqD0HoUcyFpTcSNF/7gp4BP/I8VQ7bvolCbSUFgTaUimTCzG+8NxBmaJNjiO4ziO4ziO4ziO4ziO4ziO4zhD8h9gRK/o5+upKQAAAABJRU5ErkJggg==>

[image13]: <data:image/png;base64,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>