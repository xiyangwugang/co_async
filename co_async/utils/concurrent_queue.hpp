#pragma once
#include <co_async/std.hpp>
#include <co_async/utils/cacheline.hpp>
#include <co_async/utils/ring_queue.hpp>

namespace co_async {
template <class T, std::size_t Capacity = 0>
struct alignas(hardware_destructive_interference_size) ConcurrentRingQueue {
    static constexpr std::size_t Shift = std::bit_width(Capacity);
    using Stamp = std::conditional_t<
        Shift <= 4, std::uint8_t,
        std::conditional_t<
            Shift <= 8, std::uint16_t,
            std::conditional_t<Shift <= 16, std::uint32_t, std::uint64_t>>>;
    static_assert(Shift * 2 <= sizeof(Stamp) * 8);
    static_assert(Capacity < (1 << Shift));
    static constexpr Stamp kSize = 1 << Shift;

    [[nodiscard]] std::optional<T> pop() {
        auto s = mStamp.load(std::memory_order_acquire);
        if (!canRead(s)) {
            /* mStamp.compare_exchange_weak(s, Stamp(0)); */
            return std::nullopt;
        }
        while (!mStamp.compare_exchange_weak(s, advectRead(s),
                                             std::memory_order_acq_rel)) {
            if (!canRead(s)) {
                return std::nullopt;
            }
        }
        return std::move(mHead[offsetRead(s)]);
    }

    [[nodiscard]] bool push(T &&value) {
        auto s = mStamp.load(std::memory_order_acquire);
        if (!canWrite(s)) [[unlikely]] {
            return false;
        }
        while (!mStamp.compare_exchange_weak(s, advectWrite(s),
                                             std::memory_order_acq_rel)) {
            if (!canWrite(s)) [[unlikely]] {
                return false;
            }
        }
        mHead[offsetWrite(s)] = std::move(value);
        return true;
    }

    ConcurrentRingQueue() = default;
    ConcurrentRingQueue(ConcurrentRingQueue &&) = delete;

private:
    inline Stamp offsetRead(Stamp s) const {
        return s >> Shift;
    }

    inline Stamp offsetWrite(Stamp s) const {
        return s & (kSize - 1);
    }

    inline bool canRead(Stamp s) const {
        return offsetRead(s) != offsetWrite(s);
    }

    inline bool canWrite(Stamp s) const {
        return (offsetRead(s) & (Stamp)(kSize - 1)) !=
               ((offsetWrite(s) + (Stamp)(kSize - Capacity)) &
                (Stamp)(kSize - 1));
    }

    inline Stamp advectRead(Stamp s) const {
        return (Stamp)((((Stamp)(s >> Shift) + (Stamp)1) & (Stamp)(kSize - 1))
                       << Shift) |
               (s & (Stamp)(kSize - 1));
    }

    inline Stamp advectWrite(Stamp s) const {
        return (((s & (Stamp)(kSize - 1)) + (Stamp)1) & (Stamp)(kSize - 1)) |
               (Stamp)(s & ((Stamp)(kSize - 1) << Shift));
    }

    std::unique_ptr<T[]> const mHead = std::make_unique<T[]>(kSize);
    std::atomic<Stamp> mStamp{0};
};

template <class T>
struct alignas(hardware_destructive_interference_size)
    ConcurrentRingQueue<T, 0> {
    std::optional<T> pop() {
        std::lock_guard lck(mMutex);
        if (mQueue.empty()) {
            return std::nullopt;
        }
        T p = std::move(mQueue.front());
        mQueue.pop_front();
        return p;
    }

    bool push(T &&value) {
        std::lock_guard lck(mMutex);
        mQueue.push_back(std::move(value));
        return true;
    }

private:
    std::deque<T> mQueue;
    std::mutex mMutex;
};
} // namespace co_async
