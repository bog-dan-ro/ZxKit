/*
    Copyright (c) 2026, BogDan Vatra <bogdan@kde.org>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include <atomic>
#include <cstdint>
#include <thread>

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#include <immintrin.h>
#endif

namespace zxkit {

/// Tells the CPU we are busy waiting (cheaper for the sibling hyper-thread, less power).
inline void cpuRelax() noexcept
{
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
    _mm_pause();
#elif defined(__aarch64__) || defined(__arm__)
    asm volatile("yield" ::: "memory");
#else
    std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}

/// Busy wait strategy: spin on the CPU, and only after a long wait (e.g. the
/// lock owner got preempted) let the scheduler run something else.
class SpinBackoff
{
public:
    void pause() noexcept
    {
        if (m_spins < kRelaxSpins) {
            cpuRelax();
        } else if (m_spins < kRelaxSpins + kBurstSpins) {
            for (int i = 0; i < 16; ++i)
                cpuRelax();
        } else {
            std::this_thread::yield();
        }
        ++m_spins;
    }

private:
    static constexpr uint32_t kRelaxSpins = 64;
    static constexpr uint32_t kBurstSpins = 4096;
    uint32_t m_spins = 0;
};

/// A reader / writer spin lock satisfying `safe::SharedMutex`.
///
/// ZxKit holds its locks for very short times (the emulation thread locks the
/// emulator one scanline at a time), so waiting threads spin instead of being
/// suspended by the kernel, which keeps the emulation thread's latency low.
///
/// Writers are served in FIFO order (ticket lock), and once a writer is waiting
/// no new readers get in, so neither side can starve: a thread re-locking in a
/// tight loop (the emulation thread) always lets a waiting thread in first.
class SpinSharedMutex
{
public:
    SpinSharedMutex() = default;
    SpinSharedMutex(const SpinSharedMutex &) = delete;
    SpinSharedMutex &operator=(const SpinSharedMutex &) = delete;

    void lock() noexcept
    {
        const uint32_t ticket = m_nextTicket.fetch_add(1, std::memory_order_relaxed);
        SpinBackoff backoff;
        while (m_nowServing.load(std::memory_order_acquire) != ticket)
            backoff.pause();
        // Our turn among the writers: wait for the readers to leave
        uint32_t expected = 0;
        while (!m_state.compare_exchange_weak(expected, kWriter, std::memory_order_acquire,
                                              std::memory_order_relaxed)) {
            expected = 0;
            backoff.pause();
        }
    }

    bool try_lock() noexcept
    {
        uint32_t ticket = m_nowServing.load(std::memory_order_acquire);
        if (m_state.load(std::memory_order_relaxed) != 0
            || !m_nextTicket.compare_exchange_strong(ticket, ticket + 1, std::memory_order_acquire,
                                                     std::memory_order_relaxed))
            return false;
        // We hold the next ticket now, the readers in may still have to leave
        uint32_t expected = 0;
        SpinBackoff backoff;
        while (!m_state.compare_exchange_weak(expected, kWriter, std::memory_order_acquire,
                                              std::memory_order_relaxed)) {
            expected = 0;
            backoff.pause();
        }
        return true;
    }

    void unlock() noexcept
    {
        m_state.store(0, std::memory_order_release);
        m_nowServing.fetch_add(1, std::memory_order_release);
    }

    void lock_shared() noexcept
    {
        SpinBackoff backoff;
        for (;;) {
            // Waiting writers go first
            if (m_nextTicket.load(std::memory_order_acquire)
                == m_nowServing.load(std::memory_order_acquire)) {
                uint32_t state = m_state.load(std::memory_order_relaxed);
                if (!(state & kWriter)
                    && m_state.compare_exchange_weak(state, state + 1, std::memory_order_acquire,
                                                     std::memory_order_relaxed))
                    return;
            }
            backoff.pause();
        }
    }

    bool try_lock_shared() noexcept
    {
        if (m_nextTicket.load(std::memory_order_acquire)
            != m_nowServing.load(std::memory_order_acquire))
            return false;
        uint32_t state = m_state.load(std::memory_order_relaxed);
        return !(state & kWriter)
                && m_state.compare_exchange_strong(state, state + 1, std::memory_order_acquire,
                                                   std::memory_order_relaxed);
    }

    void unlock_shared() noexcept
    {
        m_state.fetch_sub(1, std::memory_order_release);
    }

private:
    static constexpr uint32_t kWriter = 0x80000000u;

    std::atomic<uint32_t> m_state{ 0 }; // kWriter, or the number of readers
    std::atomic<uint32_t> m_nextTicket{ 0 }; // writers queue
    std::atomic<uint32_t> m_nowServing{ 0 };
};

} // namespace zxkit
