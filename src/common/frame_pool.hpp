#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace lowlat {

// 固定尺寸缓冲池，用于消除热路径上的每帧 malloc。
//
// 返回的 shared_ptr 携带自定义 deleter，最后一个引用释放时把缓冲归还池中，
// 因此帧对象可以安全地跨线程传递（采集线程 -> 编码线程 -> 发送线程）。
//
// 生命周期：deleter 持有 Core 的 shared_ptr，即使 FramePool 先被销毁，
// 已借出的缓冲依然有效，归还时也不会访问悬空对象。
//
// 线程安全：acquire/release 走互斥量。调用频率约为每秒数十到数百次，
// 不属于每字节级热路径，用锁换实现确定性是划算的。
class FramePool {
public:
    static std::shared_ptr<FramePool> create(std::size_t buffer_size, std::size_t capacity)
    {
        if (buffer_size == 0 || capacity == 0) {
            return nullptr;
        }
        auto core = std::make_shared<Core>(buffer_size, capacity);
        return std::shared_ptr<FramePool>(new FramePool(std::move(core)));
    }

    FramePool(const FramePool&) = delete;
    FramePool& operator=(const FramePool&) = delete;

    // 池耗尽时返回空指针，同时累加 exhausted_count()。
    std::shared_ptr<std::uint8_t> acquire()
    {
        std::uint8_t* slot = nullptr;
        {
            std::lock_guard<std::mutex> lock(core_->mutex);
            if (core_->free_list.empty()) {
                core_->exhausted.fetch_add(1, std::memory_order_relaxed);
                return nullptr;
            }
            slot = core_->free_list.back();
            core_->free_list.pop_back();
        }
        return std::shared_ptr<std::uint8_t>(slot, Releaser{core_});
    }

    std::size_t buffer_size() const noexcept { return core_->buffer_size; }
    std::size_t capacity() const noexcept { return core_->free_list.capacity(); }

    std::size_t available() const
    {
        std::lock_guard<std::mutex> lock(core_->mutex);
        return core_->free_list.size();
    }

    // 池耗尽次数，直接作为"背压不足"的现场证据。
    std::uint64_t exhausted_count() const noexcept
    {
        return core_->exhausted.load(std::memory_order_relaxed);
    }

private:
    struct Core {
        Core(std::size_t size, std::size_t count)
            : buffer_size(size), storage(size * count, 0), capacity(count)
        {
            free_list.reserve(count);
            for (std::size_t i = 0; i < count; ++i) {
                free_list.push_back(storage.data() + i * buffer_size);
            }
        }

        void release(std::uint8_t* slot) noexcept
        {
            std::lock_guard<std::mutex> lock(mutex);
            free_list.push_back(slot);
        }

        std::size_t buffer_size;
        std::vector<std::uint8_t> storage;
        std::size_t capacity;
        std::vector<std::uint8_t*> free_list;
        mutable std::mutex mutex;
        std::atomic<std::uint64_t> exhausted{0};
    };

    struct Releaser {
        std::shared_ptr<Core> core;

        void operator()(std::uint8_t* slot) const noexcept
        {
            if (core && slot != nullptr) {
                core->release(slot);
            }
        }
    };

    explicit FramePool(std::shared_ptr<Core> core) noexcept : core_(std::move(core)) {}

    std::shared_ptr<Core> core_;
};

}  // namespace lowlat
