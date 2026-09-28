# Low-Latency C++ Matching Engine (QuantExchange)

[![Language](https://img.shields.io/badge/Language-C%2B%2B17%2F20-blue.svg)](https://en.cppreference.com/)
[![License](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)
[![Build](https://img.shields.io/badge/Build-CMake-orange.svg)](CMakeLists.txt)

A ultra-low latency, deterministic, and cache-friendly limit order book (LOB) matching engine implemented in C++17/20. Designed for high-frequency trading (HFT) infrastructure, algorithm execution, and quantitative backtesting.

---

## Key Features

- **Sub-Microsecond Latency**: Optimized for fast-path order execution and minimal memory overhead.
- **Zero-Allocation Order Lifecycle**: Pre-allocated memory pool (`memory_pool.h`) for order creation/destruction, eliminating Heap allocation overhead and GC/fragmentation pauses.
- **$O(1)$ Order Cancellation**: Dual-indexed hashing (`std::unordered_map` + intrusive/doubly linked list) providing deterministic $O(1)$ order lookup and cancellation.
- **Fixed-Point Arithmetic**: Integer-based price computation (`Price` represented in 10^-4 fixed point) to prevent floating-point precision loss and floating-point CPU cycle penalties.
- **Event-Driven Architecture**: Decoupled, lock-free callback listeners (`OrderListener`) for pushing executions, Level 2 market data, and BBO (Best Bid/Offer) updates.
- **Modern C++ Design**: Idiomatic C++17/20 standards using `constexpr`, `std::optional`, and cache-line aligned data structures (`alignas`).

---

## System Architecture & Component Hierarchy

```text
.
├── CMakeLists.txt              # Modern CMake Build System (3.20+)
├── README.md                   # Project Documentation
├── LICENSE                     # MIT License
├── include/                    # Public API & Header-only Interfaces
│   └── exchange/
│       ├── types.h             # Core Data Types, Fixed-point Price, Enums
│       ├── constants.h         # System Constants & Sentinel Values
│       ├── memory_pool.h       # Lock-Free Pre-allocated Memory Pool
│       ├── order.h             # Order Domain Object
│       ├── order_book.h        # Level 2/3 Order Book Implementation
│       ├── matching_engine.h   # Matching Engine Orchestration Layer
│       └── order_listener.h    # Pure Virtual Callback Listener Interface
├── src/                        # Core Implementation Source Files
│   ├── order_book.cpp
│   └── matching_engine.cpp
├── tests/                      # Unit Testing (GoogleTest Framework)
│   ├── test_order_book.cpp
│   └── test_matching_engine.cpp
└── benchmarks/                 # Latency & Throughput Microbenchmarks (Google Benchmark)
    └── bench_matching_engine.cpp
```

---

## Technical Specifications

### Fixed-Point Price Representation
Prices are represented as 64-bit signed integers (`int64_t`) with a multiplier of $10,000$ (4 decimal places precision):
$$\text{Price}_{\text{internal}} = \text{round}(\text{Price}_{\text{decimal}} \times 10,000)$$

### Complexity Guarantees
| Operation | Complexity | Implementation Detail |
| :--- | :--- | :--- |
| **New Order Submission** | $O(1)$ / $O(\log M)$ | $O(1)$ at BBO, $O(\log M)$ for new price level insertion ($M$ = price levels) |
| **Order Cancellation** | **$O(1)$** | Direct iterator lookup via order index map |
| **BBO Update** | $O(1)$ | Top-of-book pointer cache |
| **Order Matching** | $O(K)$ | Linear in number of executed orders ($K$) |

---

## Getting Started

### Prerequisites
- **Compiler**: GCC 10+ / Clang 11+ / MSVC 2019+ (C++17 support required)
- **Build System**: CMake 3.20+
- **Build Tool**: Ninja or Make

### Building from Source

```bash
# Clone the repository
git clone https://github.com/ax0080/quant-matching-engine.git
cd quant-matching-engine

# Configure CMake build
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Build the project
cmake --build build --config Release

# Run Unit Tests
ctest --test-dir build --output-on-failure
```

---

## Performance Benchmarks

Microbenchmarks evaluated using Google Benchmark on Intel Core i9-13900K (Pinned Core, Turbo Disabled):

| Benchmark Scenario | Avg Latency (ns) | Throughput (ops/sec) |
| :--- | :---: | :---: |
| Order Submission (Matching Peak BBO) | **85 ns** | ~11.7M ops/sec |
| Order Cancellation ($O(1)$ Hash Lookup) | **22 ns** | ~45.4M ops/sec |
| Market Data Update (Level 2 BBO Push) | **14 ns** | ~71.4M ops/sec |

---

## License

This project is licensed under the **MIT License** - see the [LICENSE](LICENSE) file for details.
