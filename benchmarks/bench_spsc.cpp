#include <benchmark/benchmark.h>

#include "exchange/memory_pool.h"
#include "exchange/order.h"
#include "exchange/order_book.h"
#include "exchange/order_command.h"
#include "exchange/spsc_queue.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
#include <immintrin.h>
#endif

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

using namespace exchange;

namespace {

// Two physical cores (not hyperthread siblings). On an i5-12600K logical
// CPUs 0-11 are the six P-cores, two threads each.
constexpr int kConsumerCpu = 2;
constexpr int kProducerCpu = 4;

inline void cpu_relax() {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
    _mm_pause();
#endif
}

void pin_this_thread(int cpu) {
#if defined(_WIN32)
    SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR{1} << cpu);
#elif defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
#else
    (void)cpu;
#endif
}

// Baseline: the same ring buffer guarded by a std::mutex.
template <typename T, std::size_t Capacity>
class MutexQueue {
public:
    MutexQueue() : slots_(std::make_unique<T[]>(Capacity)) {}

    bool try_push(const T& v) {
        std::lock_guard lock(m_);
        if (tail_ - head_ == Capacity) return false;
        slots_[tail_++ % Capacity] = v;
        return true;
    }
    bool try_pop(T& out) {
        std::lock_guard lock(m_);
        if (head_ == tail_) return false;
        out = slots_[head_++ % Capacity];
        return true;
    }

private:
    std::mutex           m_;
    std::unique_ptr<T[]> slots_;
    std::size_t          head_ = 0, tail_ = 0;
};

constexpr std::size_t kQueueSize = 4096;
constexpr uint64_t    kMessages  = 1 << 20;
constexpr uint64_t    kStop      = ~uint64_t{0};

// ---- Throughput: stream 1M integers from one core to another --------------

template <typename Q>
void throughput(benchmark::State& state) {
    pin_this_thread(kConsumerCpu);
    auto q = std::make_unique<Q>();

    for (auto _ : state) {
        std::thread producer([&] {
            pin_this_thread(kProducerCpu);
            for (uint64_t i = 0; i < kMessages; ++i)
                while (!q->try_push(i)) cpu_relax();
        });
        uint64_t sum = 0, v = 0;
        for (uint64_t n = 0; n < kMessages;)
            if (q->try_pop(v)) { sum += v; ++n; }
        producer.join();
        benchmark::DoNotOptimize(sum);
    }
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations() * kMessages));
}

void BM_Throughput_Spsc(benchmark::State& s)  { throughput<SpscQueue<uint64_t, kQueueSize>>(s); }
void BM_Throughput_Mutex(benchmark::State& s) { throughput<MutexQueue<uint64_t, kQueueSize>>(s); }
BENCHMARK(BM_Throughput_Spsc)->UseRealTime()->Unit(benchmark::kMillisecond);
BENCHMARK(BM_Throughput_Mutex)->UseRealTime()->Unit(benchmark::kMillisecond);

// ---- Latency: ping-pong one message between two cores ----------------------
// Each iteration is one round trip; one-way latency is about half.

template <typename Q>
void ping_pong(benchmark::State& state) {
    pin_this_thread(kConsumerCpu);
    auto req  = std::make_unique<Q>();
    auto resp = std::make_unique<Q>();

    std::thread echo([&] {
        pin_this_thread(kProducerCpu);
        uint64_t v = 0;
        for (;;) {
            if (!req->try_pop(v)) continue;
            if (v == kStop) break;
            while (!resp->try_push(v)) cpu_relax();
        }
    });

    uint64_t i = 0, v = 0;
    for (auto _ : state) {
        while (!req->try_push(i)) cpu_relax();
        while (!resp->try_pop(v)) {}
        ++i;
    }
    while (!req->try_push(kStop)) cpu_relax();
    echo.join();
    benchmark::DoNotOptimize(v);
}

void BM_RoundTrip_Spsc(benchmark::State& s)  { ping_pong<SpscQueue<uint64_t, kQueueSize>>(s); }
void BM_RoundTrip_Mutex(benchmark::State& s) { ping_pong<MutexQueue<uint64_t, kQueueSize>>(s); }
BENCHMARK(BM_RoundTrip_Spsc)->UseRealTime();
BENCHMARK(BM_RoundTrip_Mutex)->UseRealTime();

// ---- Pipeline: gateway thread -> queue -> matching thread -------------------

struct NullListener : OrderListener {};

std::vector<OrderCommand> make_flow(std::size_t n) {
    constexpr int64_t kMid = 1'000'000, kTick = 100;
    std::mt19937 rng(42);
    std::vector<OrderCommand> cmds;
    cmds.reserve(n);
    std::vector<OrderId> live;
    OrderId id = 1;
    while (cmds.size() < n) {
        OrderCommand c;
        uint32_t r = rng() % 100;
        if (r >= 45 && r < 90) {
            if (live.empty()) continue;
            std::size_t k = rng() % live.size();
            c.kind  = OrderCommand::Kind::Cancel;
            c.id    = live[k];
            live[k] = live.back();
            live.pop_back();
        } else {
            bool ioc = r >= 90;
            c.id     = id++;
            c.side   = (rng() & 1) ? Side::Buy : Side::Sell;
            int64_t off = ioc ? -5 : 1 + static_cast<int64_t>(rng() % 20);
            c.price  = Price{c.side == Side::Buy ? kMid - off * kTick : kMid + off * kTick};
            c.qty    = 1 + rng() % 100;
            c.ts     = c.id;
            c.tif    = ioc ? TimeInForce::IOC : TimeInForce::GTC;
            if (!ioc) live.push_back(c.id);
        }
        cmds.push_back(c);
    }
    return cmds;
}

const std::vector<OrderCommand>& flow() {
    static const auto cmds = make_flow(kMessages);
    return cmds;
}

// Reference: same commands, matched on the calling thread.
void BM_Pipeline_SingleThread(benchmark::State& state) {
    pin_this_thread(kConsumerCpu);
    const auto& cmds = flow();
    for (auto _ : state) {
        NullListener      listener;
        MemoryPool<Order> pool;
        OrderBook         book("BENCH", pool, &listener);
        for (const auto& c : cmds) apply(book, c);
        benchmark::DoNotOptimize(book.order_count());
    }
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations() * cmds.size()));
}
BENCHMARK(BM_Pipeline_SingleThread)->UseRealTime()->Unit(benchmark::kMillisecond);

template <typename Q>
void pipeline(benchmark::State& state) {
    pin_this_thread(kConsumerCpu);
    const auto& cmds = flow();
    auto q = std::make_unique<Q>();

    for (auto _ : state) {
        NullListener      listener;
        MemoryPool<Order> pool;
        OrderBook         book("BENCH", pool, &listener);

        std::thread gateway([&] {
            pin_this_thread(kProducerCpu);
            for (const auto& c : cmds)
                while (!q->try_push(c)) cpu_relax();
        });
        OrderCommand c;
        for (std::size_t n = 0; n < cmds.size();)
            if (q->try_pop(c)) { apply(book, c); ++n; }
        gateway.join();
        benchmark::DoNotOptimize(book.order_count());
    }
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations() * cmds.size()));
}

void BM_Pipeline_Spsc(benchmark::State& s)  { pipeline<SpscQueue<OrderCommand, kQueueSize>>(s); }
void BM_Pipeline_Mutex(benchmark::State& s) { pipeline<MutexQueue<OrderCommand, kQueueSize>>(s); }
BENCHMARK(BM_Pipeline_Spsc)->UseRealTime()->Unit(benchmark::kMillisecond);
BENCHMARK(BM_Pipeline_Mutex)->UseRealTime()->Unit(benchmark::kMillisecond);

} // namespace

BENCHMARK_MAIN();
