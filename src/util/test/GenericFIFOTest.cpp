#include <iostream>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#include "../GenericFIFO.h"

using namespace std::chrono_literals;

namespace
{

bool testBasicWriteRead()
{
    std::cout << "Test 1 (Basic write/read): ";

    GenericFIFO<int> fifo(8);
    int in[3] = { 1, 2, 3 };
    int out[3] = { 0, 0, 0 };

    bool result = (fifo.write(in, 3) == 0);
    result &= (fifo.numUsed() == 3);
    result &= (fifo.read(out, 3) == 0);
    result &= (out[0] == 1 && out[1] == 2 && out[2] == 3);
    result &= (fifo.numUsed() == 0);

    std::cout << (result ? "PASS" : "FAIL") << "\n";
    return result;
}

bool testFullAndEmptyBoundaries()
{
    std::cout << "Test 2 (Full/empty boundaries): ";

    // capacity() is nelem - 1 (one slot always kept empty to disambiguate
    // full from empty).
    GenericFIFO<int> fifo(8);
    int cap = fifo.capacity();
    std::vector<int> in(cap, 42);
    std::vector<int> out(cap, 0);

    bool result = (fifo.write(in.data(), cap) == 0);
    result &= (fifo.numFree() == 0);

    // One more element than free space must fail outright, and must not
    // have disturbed the FIFO's contents.
    int extra = 99;
    result &= (fifo.write(&extra, 1) != 0);
    result &= (fifo.numUsed() == cap);

    result &= (fifo.read(out.data(), cap) == 0);
    bool allMatch = true;
    for (int v : out) allMatch &= (v == 42);
    result &= allMatch;
    result &= (fifo.numUsed() == 0);

    // Reading from an empty FIFO must fail cleanly.
    int dummy = 0;
    result &= (fifo.read(&dummy, 1) != 0);

    std::cout << (result ? "PASS" : "FAIL") << "\n";
    return result;
}

bool testWraparound()
{
    std::cout << "Test 3 (Ring wraparound correctness): ";

    GenericFIFO<int> fifo(8);
    bool result = true;
    int nextExpected = 0;
    int nextWrite = 0;

    // Push far more elements through than the buffer's capacity, in small
    // write/read bursts, so the ring wraps around many times. Every value
    // read back must match what was written, in order.
    for (int round = 0; round < 1000 && result; round++)
    {
        int chunk = 3;
        std::vector<int> in(chunk);
        for (int i = 0; i < chunk; i++) in[i] = nextWrite++;

        if (fifo.write(in.data(), chunk) != 0)
        {
            result = false;
            break;
        }

        std::vector<int> out(chunk, -1);
        if (fifo.read(out.data(), chunk) != 0)
        {
            result = false;
            break;
        }

        for (int i = 0; i < chunk; i++)
        {
            if (out[i] != nextExpected++)
            {
                result = false;
                break;
            }
        }
    }

    std::cout << (result ? "PASS" : "FAIL") << "\n";
    return result;
}

bool testResetClearsData()
{
    std::cout << "Test 4 (reset() clears pending data): ";

    GenericFIFO<int> fifo(8);
    int in[4] = { 1, 2, 3, 4 };
    bool result = (fifo.write(in, 4) == 0);
    result &= (fifo.numUsed() == 4);

    fifo.reset();
    result &= (fifo.numUsed() == 0);
    result &= (fifo.numFree() == fifo.capacity());

    // Fresh data written after reset() must read back correctly and must
    // not be affected by whatever was pending beforehand.
    int in2[2] = { 100, 200 };
    int out2[2] = { 0, 0 };
    result &= (fifo.write(in2, 2) == 0);
    result &= (fifo.read(out2, 2) == 0);
    result &= (out2[0] == 100 && out2[1] == 200);

    std::cout << (result ? "PASS" : "FAIL") << "\n";
    return result;
}

// Regression test for a race between reset() and a concurrent write()/
// read() on the other side of the FIFO. GenericFIFO is a lock-free SPSC
// ring buffer; reset() is called by whichever thread owns a FIFO endpoint
// while the other endpoint's thread can be mid-write()/read() concurrently
// (e.g. clearFifos_() running on the TX thread while the audio device
// callback thread is still writing/reading the same FIFO).
//
// A single writer thread pushes a strictly increasing uint64_t counter. A
// single reader thread reads it back and checks two invariants that must
// hold regardless of how aggressively reset() races them:
//   1) no element read is ever the SENTINEL value pre-filled into every
//      ring slot before the test starts (seeing it means a ring slot that
//      no real commit ever wrote was read as if it had been).
//   2) every element read is strictly greater than the previous one. A
//      single writer can only ever produce a strictly increasing stream,
//      and even a reset() concurrent with real traffic can only *skip*
//      ranges (lost writes) -- never repeat or reorder them. A duplicate
//      or smaller value means stale/torn ring contents were read.
// A third thread calls reset() in a tight loop for the whole test.
bool testConcurrentResetRace()
{
    std::cout << "Test 5 (Concurrent reset() race stress test): ";

    constexpr int NELEM = 256;
    constexpr int CHUNK = 64;
    constexpr std::uint64_t SENTINEL = 0xFFFFFFFFFFFFFFFFULL;
    constexpr double TEST_SECONDS = 2.0;

    std::vector<std::uint64_t> storage(NELEM, SENTINEL);
    GenericFIFO<std::uint64_t> fifo(NELEM, storage.data());

    std::atomic<bool> stop{ false };
    std::atomic<std::uint64_t> corruptionEvents{ 0 };
    std::atomic<std::uint64_t> orderingViolations{ 0 };
    std::atomic<std::uint64_t> resetCount{ 0 };
    std::atomic<std::uint64_t> writeSuccesses{ 0 };
    std::atomic<std::uint64_t> readSuccesses{ 0 };

    std::thread writer([&]() {
        std::uint64_t buf[CHUNK];
        std::uint64_t next = 1;
        while (!stop.load(std::memory_order_relaxed))
        {
            for (int i = 0; i < CHUNK; i++) buf[i] = next + (std::uint64_t)i;
            if (fifo.write(buf, CHUNK) == 0)
            {
                next += CHUNK;
                writeSuccesses.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    std::thread reader([&]() {
        std::uint64_t buf[CHUNK];
        std::uint64_t last = 0;
        while (!stop.load(std::memory_order_relaxed))
        {
            if (fifo.read(buf, CHUNK) == 0)
            {
                readSuccesses.fetch_add(1, std::memory_order_relaxed);
                for (int i = 0; i < CHUNK; i++)
                {
                    std::uint64_t v = buf[i];
                    if (v == SENTINEL)
                    {
                        corruptionEvents.fetch_add(1, std::memory_order_relaxed);
                        continue;
                    }
                    if (v <= last)
                    {
                        orderingViolations.fetch_add(1, std::memory_order_relaxed);
                    }
                    last = v;
                }
            }
        }
    });

    std::thread resetter([&]() {
        while (!stop.load(std::memory_order_relaxed))
        {
            fifo.reset();
            resetCount.fetch_add(1, std::memory_order_relaxed);
        }
    });

    std::this_thread::sleep_for(std::chrono::duration<double>(TEST_SECONDS));
    stop.store(true, std::memory_order_relaxed);

    writer.join();
    reader.join();
    resetter.join();

    bool result = (corruptionEvents.load() == 0) && (orderingViolations.load() == 0);

    std::cout << (result ? "PASS" : "FAIL")
              << " (resets=" << resetCount.load()
              << " writeOK=" << writeSuccesses.load()
              << " readOK=" << readSuccesses.load()
              << " corruption=" << corruptionEvents.load()
              << " orderingViolations=" << orderingViolations.load() << ")\n";
    return result;
}

} // namespace

int main(int argc, char** argv)
{
    bool result = true;

    result &= testBasicWriteRead();
    result &= testFullAndEmptyBoundaries();
    result &= testWraparound();
    result &= testResetClearsData();
    result &= testConcurrentResetRace();

    return result ? 0 : -1;
}
