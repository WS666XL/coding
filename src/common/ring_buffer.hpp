#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace lowlat {

// 有界无锁单生产者/单消费者队列。
//
// 线程约束：同一时刻只允许一个线程 push、一个线程 pop。
// 满/空通过返回值表达，由调用方决定丢帧还是重试——这是流水线的背压信号。
//
// 索引为单调递增计数，容量必须是 2 的幂，用掩码取模。
// head_ 与 tail_ 各自按 cache line 对齐，避免伪共享。
template <typename T>
class SpscRingBuffer {
    static_assert(std::is_trivially_copyable<T>::value,
                  "T 必须是可平凡拷贝类型，否则无法安全地无锁搬运");
    static_assert(std::is_copy_assignable<T>::value, "T 必须可拷贝赋值");
    static_assert(std::is_default_constructible<T>::value, "T 必须可默认构造");

public:
    // capacity 会被向上取整到 2 的幂；capacity == 0 时抛出 std::invalid_argument。
    explicit SpscRingBuffer(std::size_t capacity)
        : capacity_(round_up_pow2(capacity)),
          mask_(capacity_ - 1),
          slots_(capacity_)
    {
    }

    SpscRingBuffer(const SpscRingBuffer&) = delete;
    SpscRingBuffer& operator=(const SpscRingBuffer&) = delete;

    // 生产者调用。队列满时返回 false，不覆盖已有数据。
    bool try_push(const T& value) noexcept
    {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        if (head - tail >= capacity_) {
            push_failures_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        slots_[head & mask_] = value;
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    // 消费者调用。队列空时返回 false。
    bool try_pop(T* out) noexcept
    {
        if (out == nullptr) {
            return false;
        }
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t head = head_.load(std::memory_order_acquire);
        if (tail == head) {
            pop_failures_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        *out = slots_[tail & mask_];
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    std::size_t size() const noexcept
    {
        const std::size_t head = head_.load(std::memory_order_acquire);
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        return head - tail;
    }

    std::size_t capacity() const noexcept { return capacity_; }
    bool empty() const noexcept { return size() == 0; }
    bool full() const noexcept { return size() >= capacity_; }

    // 供压测观察背压用，不做同步语义保证。
    std::uint64_t push_failures() const noexcept
    {
        return push_failures_.load(std::memory_order_relaxed);
    }

    std::uint64_t pop_failures() const noexcept
    {
        return pop_failures_.load(std::memory_order_relaxed);
    }

    static std::size_t round_up_pow2(std::size_t value)
    {
        if (value == 0) {
            throw std::invalid_argument("SpscRingBuffer capacity 不能为 0");
        }
        std::size_t result = 1;
        while (result < value) {
            if (result > (static_cast<std::size_t>(-1) / 2)) {
                throw std::invalid_argument("SpscRingBuffer capacity 过大");
            }
            result <<= 1;
        }
        return result;
    }

private:
    const std::size_t capacity_;
    const std::size_t mask_;
    std::vector<T> slots_;

    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
    alignas(64) std::atomic<std::uint64_t> push_failures_{0};
    alignas(64) std::atomic<std::uint64_t> pop_failures_{0};
};

}  // namespace lowlat
