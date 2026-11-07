/*
MIT No Attribution

Copyright 2026, BogDan Vatra <bogdan@kde.org>

Permission is hereby granted, free of charge, to any person obtaining a copy of this
software and associated documentation files (the "Software"), to deal in the Software
without restriction, including without limitation the rights to use, copy, modify,
merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#pragma once

#include <algorithm>
#include <atomic>
#include <cassert>
#include <compare>
#include <concepts>
#include <cstddef>
#include <memory>
#include <new>
#include <shared_mutex>
#include <tuple>
#include <type_traits>
#include <typeinfo>
#include <utility>

namespace safe {

// The lock type used by the pointers. std::shared_mutex satisfies it, std::mutex
// does not, and saying so here keeps the diagnostic at the point of use.
template<typename M>
concept SharedMutex = requires(M &mutex) {
    mutex.lock();
    mutex.unlock();
    mutex.lock_shared();
    mutex.unlock_shared();
};

template<typename T, SharedMutex M = std::shared_mutex>
class SharedPtr;

template<typename T, SharedMutex M = std::shared_mutex>
class WeakPtr;

template<typename T, SharedMutex M = std::shared_mutex>
class CopyOnWritePtr;

template<typename T, SharedMutex M = std::shared_mutex>
class EnableSharedFromThis;

template<typename T, SharedMutex M = std::shared_mutex, typename... Args>
SharedPtr<T, M> makeShared(Args &&...args);

template<typename T, SharedMutex M = std::shared_mutex, typename Allocator, typename... Args>
SharedPtr<T, M> allocateShared(const Allocator &allocator, Args &&...args);

template<typename Deleter, typename T, SharedMutex M>
constexpr Deleter *getDeleter(const SharedPtr<T, M> &pointer) noexcept;

template<typename T, SharedMutex M = std::shared_mutex, typename... Args>
CopyOnWritePtr<T, M> makeCopyOnWrite(Args &&...args);

namespace detail {
template<typename Owner, typename Object, SharedMutex M>
void armSharedFromThis(const SharedPtr<Owner, M> &owner, Object *object) noexcept;

template<typename T>
struct DefaultDelete
{
    void operator()(T *pointer) const noexcept { delete pointer; }
};

// Tags selecting the cast performed by the pointer converting constructors
struct StaticCastTag
{};
struct DynamicCastTag
{};
struct ConstCastTag
{};

template<SharedMutex M>
struct ControlBlockBase
{
    // The mutex lives in the erased base, so the pointers only need to remember
    // the block and the (possibly adjusted) object address
    mutable M mutex;

    std::atomic<std::size_t> sharedRefs{1};
    std::atomic<std::size_t> weakRefs{1}; // 1 implicit count held collectively by shared_refs

    virtual ~ControlBlockBase() = default;
    virtual void destroyData() noexcept = 0;
    virtual void destroySelf() noexcept = 0;
    virtual void *getDeleter(const std::type_info &) noexcept { return nullptr; }

    void addShared() noexcept { sharedRefs.fetch_add(1, std::memory_order_relaxed); }

    void releaseShared() noexcept
    {
        // If we are the last shared owner
        if (sharedRefs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            destroyData(); // Destroy T immediately
            releaseWeak(); // Decrement the implicit weak ref
        }
    }

    void addWeak() noexcept { weakRefs.fetch_add(1, std::memory_order_relaxed); }

    void releaseWeak() noexcept
    {
        // If we are the absolute last observer (no shared, no weak)
        if (weakRefs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            destroySelf(); // Free the Control Block memory itself
        }
    }

    bool tryAddShared() noexcept
    {
        std::size_t count = sharedRefs.load(std::memory_order_relaxed);
        while (count != 0) {
            if (sharedRefs.compare_exchange_weak(count,
                                                 count + 1,
                                                 std::memory_order_acquire,
                                                 std::memory_order_relaxed)) {
                return true;
            }
        }
        return false;
    }
};

template<typename T>
class ObjectStorage
{
    alignas(T) std::byte m_storage[sizeof(T)];
    bool m_engaged{false};

    T *raw() noexcept { return reinterpret_cast<T *>(m_storage); }

public:
    T *get() noexcept { return std::launder(raw()); }

    const T *get() const noexcept
    {
        return std::launder(reinterpret_cast<const T *>(m_storage));
    }

    template<typename Allocator, typename... Args>
    void construct(Allocator &allocator, Args &&...args)
    {
        std::allocator_traits<Allocator>::construct(allocator,
                                                    raw(),
                                                    std::forward<Args>(args)...);
        m_engaged = true;
    }

    template<typename Allocator>
    void destroy(Allocator &allocator) noexcept
    {
        if (m_engaged) {
            std::allocator_traits<Allocator>::destroy(allocator, get());
            m_engaged = false;
        }
    }
};

template<typename T, SharedMutex M, typename Allocator>
struct InplaceControlBlock final : ControlBlockBase<M>
{
    using BlockAllocator =
        typename std::allocator_traits<Allocator>::template rebind_alloc<InplaceControlBlock>;
    using BlockTraits = std::allocator_traits<BlockAllocator>;
    using BlockPointer = typename BlockTraits::pointer;

    [[no_unique_address]] Allocator allocator;
    BlockPointer allocation;
    ObjectStorage<T> data;

    template<typename... Args>
    explicit InplaceControlBlock(const Allocator &sourceAllocator,
                                 BlockPointer sourceAllocation,
                                 Args &&...args)
        : allocator(sourceAllocator)
        , allocation(sourceAllocation)
    {
        data.construct(allocator, std::forward<Args>(args)...);
    }

    void destroyData() noexcept override { data.destroy(allocator); }

    void destroySelf() noexcept override
    {
        BlockAllocator blockAllocator(allocator);
        BlockPointer blockAllocation = allocation;
        BlockTraits::destroy(blockAllocator, this);
        BlockTraits::deallocate(blockAllocator, blockAllocation, 1);
    }
};

template<typename Block, typename Allocator, typename... Args>
Block *allocateControlBlock(const Allocator &allocator, Args &&...args)
{
    using BlockAllocator = typename Block::BlockAllocator;
    using BlockTraits = typename Block::BlockTraits;
    using BlockPointer = typename Block::BlockPointer;

    BlockAllocator blockAllocator(allocator);
    BlockPointer allocation = BlockTraits::allocate(blockAllocator, 1);
    struct DeallocationGuard
    {
        BlockAllocator &allocator;
        BlockPointer allocation;
        bool active{true};

        ~DeallocationGuard()
        {
            if (active)
                BlockTraits::deallocate(allocator, allocation, 1);
        }
    } guard{blockAllocator, allocation};

    Block *block = std::to_address(allocation);
    BlockTraits::construct(blockAllocator,
                           block,
                           allocator,
                           allocation,
                           std::forward<Args>(args)...);
    guard.active = false;
    return block;
}

template<typename T, SharedMutex M, typename Allocator, typename... Args>
InplaceControlBlock<T, M, Allocator> *
allocateInplaceControlBlock(const Allocator &allocator, Args &&...args)
{
    using Block = InplaceControlBlock<T, M, Allocator>;
    return allocateControlBlock<Block>(allocator, std::forward<Args>(args)...);
}

template<typename Y, SharedMutex M, typename Deleter, typename Allocator>
struct ExternalControlBlock final : ControlBlockBase<M>
{
    using BlockAllocator =
        typename std::allocator_traits<Allocator>::template rebind_alloc<ExternalControlBlock>;
    using BlockTraits = std::allocator_traits<BlockAllocator>;
    using BlockPointer = typename BlockTraits::pointer;

    [[no_unique_address]] Allocator allocator;
    BlockPointer allocation;
    Y *pointer;
    [[no_unique_address]] Deleter deleter;

    ExternalControlBlock(const Allocator &sourceAllocator,
                         BlockPointer sourceAllocation,
                         Y *sourcePointer,
                         const Deleter &sourceDeleter)
        : allocator(sourceAllocator)
        , allocation(sourceAllocation)
        , pointer(sourcePointer)
        , deleter(sourceDeleter)
    {}

    void destroyData() noexcept override
    {
        deleter(pointer);
        pointer = nullptr;
    }

    void destroySelf() noexcept override
    {
        BlockAllocator blockAllocator(allocator);
        BlockPointer blockAllocation = allocation;
        BlockTraits::destroy(blockAllocator, this);
        BlockTraits::deallocate(blockAllocator, blockAllocation, 1);
    }

    void *getDeleter(const std::type_info &type) noexcept override
    {
        return type == typeid(Deleter) ? std::addressof(deleter) : nullptr;
    }
};

template<typename Y, SharedMutex M, typename Deleter, typename Allocator>
ExternalControlBlock<Y, M, Deleter, Allocator> *
allocateExternalControlBlock(const Allocator &allocator, Y *pointer, const Deleter &deleter)
{
    using Block = ExternalControlBlock<Y, M, Deleter, Allocator>;
    return allocateControlBlock<Block>(allocator, pointer, deleter);
}
} // namespace detail

// Both guards keep the control block alive for as long as they exist, so a guard
// stays valid even if the pointer it came from is reassigned or destroyed while
// the lock is held. They can only be created by the pointer classes.
template<typename T, SharedMutex M>
class [[nodiscard]] SharedLockGuard
{
    template<typename, SharedMutex>
    friend class SharedPtr;

    const T *m_data;
    detail::ControlBlockBase<M> *m_cb;

    SharedLockGuard(detail::ControlBlockBase<M> *cb, const T *data)
        : m_data(data)
        , m_cb(cb)
    {
        // Lock first: if it throws the destructor never runs, so the reference
        // must not be taken yet. The creating pointer keeps the block alive here.
        m_cb->mutex.lock_shared();
        m_cb->addShared();
    }

public:
    ~SharedLockGuard()
    {
        // Unlock before dropping the reference: releasing the last one destroys
        // the control block, and with it the mutex
        if (m_cb) {
            m_cb->mutex.unlock_shared();
            m_cb->releaseShared();
        }
    }

    SharedLockGuard(SharedLockGuard &&other) noexcept
        : m_data(std::exchange(other.m_data, nullptr))
        , m_cb(std::exchange(other.m_cb, nullptr))
    {}

    SharedLockGuard(const SharedLockGuard &) = delete;
    SharedLockGuard &operator=(const SharedLockGuard &) = delete;

    const T *operator->() const { return m_data; }
    const T &operator*() const { return *m_data; }
};

template<typename T, SharedMutex M>
class [[nodiscard]] UniqueLockGuard
{
    template<typename, SharedMutex>
    friend class SharedPtr;

    T *m_data;
    detail::ControlBlockBase<M> *m_cb;

    UniqueLockGuard(detail::ControlBlockBase<M> *cb, T *data)
        : m_data(data)
        , m_cb(cb)
    {
        // Lock first: if it throws the destructor never runs, so the reference
        // must not be taken yet. The creating pointer keeps the block alive here.
        m_cb->mutex.lock();
        m_cb->addShared();
    }

public:
    ~UniqueLockGuard()
    {
        // Unlock before dropping the reference: releasing the last one destroys
        // the control block, and with it the mutex
        if (m_cb) {
            m_cb->mutex.unlock();
            m_cb->releaseShared();
        }
    }

    UniqueLockGuard(UniqueLockGuard &&other) noexcept
        : m_data(std::exchange(other.m_data, nullptr))
        , m_cb(std::exchange(other.m_cb, nullptr))
    {}

    UniqueLockGuard(const UniqueLockGuard &) = delete;
    UniqueLockGuard &operator=(const UniqueLockGuard &) = delete;

    const T *operator->() const { return m_data; }
    const T &operator*() const { return *m_data; }

    T *operator->() { return m_data; }
    T &operator*() { return *m_data; }
};


template<typename T, SharedMutex M>
class SharedPtr
{
    template<typename, SharedMutex>
    friend class SharedPtr;

    template<typename, SharedMutex>
    friend class WeakPtr;

    template<typename, SharedMutex>
    friend class CopyOnWritePtr;

    template<typename U, SharedMutex Mx, typename... Args>
    friend SharedPtr<U, Mx> makeShared(Args &&...args);

    template<typename U, SharedMutex Mx, typename Allocator, typename... Args>
    friend SharedPtr<U, Mx> allocateShared(const Allocator &allocator, Args &&...args);

    template<typename Deleter, typename U, SharedMutex Mx>
    friend constexpr Deleter *getDeleter(const SharedPtr<U, Mx> &pointer) noexcept;

    template<typename...>
    friend class ScopedLock;

    // Private, not protected: a protected control block would let any derived class
    // forge a pointer or drop a reference. The friends above are the only holders.
private:
    detail::ControlBlockBase<M> *m_cb{nullptr};
    T *m_data{nullptr};

    // Adopts an existing reference, it does not take one. Only the factories,
    // WeakPtr::lock() and CopyOnWritePtr can hand out a control block.
    SharedPtr(detail::ControlBlockBase<M> *cb, T *data) noexcept
        : m_cb(cb)
        , m_data(data)
    {}

    constexpr void release() noexcept
    {
        if (m_cb) {
            m_cb->releaseShared();
            m_cb = nullptr;
            m_data = nullptr;
        }
    }

public:
    using ElementType = T;
    using MutexType = M;

    constexpr SharedPtr() = default;

    constexpr SharedPtr(std::nullptr_t) noexcept {}

    template<typename Y>
        requires std::is_convertible_v<Y *, T *>
    explicit SharedPtr(Y *pointer)
        : SharedPtr(pointer, detail::DefaultDelete<Y>{})
    {}

    template<typename Y, typename Deleter>
        requires(std::is_convertible_v<Y *, T *>
                 && std::copy_constructible<std::decay_t<Deleter>>
                 && std::is_nothrow_invocable_v<std::decay_t<Deleter> &, Y *>)
    SharedPtr(Y *pointer, Deleter deleter)
        : SharedPtr(pointer, std::move(deleter), std::allocator<std::byte>{})
    {}

    template<typename Y, typename Deleter, typename Allocator>
        requires(std::is_convertible_v<Y *, T *>
                 && std::copy_constructible<std::decay_t<Deleter>>
                 && std::copy_constructible<std::decay_t<Allocator>>
                 && std::is_nothrow_invocable_v<std::decay_t<Deleter> &, Y *>)
    SharedPtr(Y *pointer, Deleter deleter, Allocator allocator)
    {
        using StoredDeleter = std::decay_t<Deleter>;
        using StoredAllocator = std::decay_t<Allocator>;

        assert(pointer != nullptr && "SharedPtr cannot own a null pointer!");

        struct PointerGuard
        {
            Y *pointer;
            StoredDeleter &deleter;
            bool active{true};

            ~PointerGuard()
            {
                if (active)
                    deleter(pointer);
            }
        } guard{pointer, deleter};

        auto *cb = detail::allocateExternalControlBlock<Y, M, StoredDeleter, StoredAllocator>(
            allocator, pointer, deleter);
        m_cb = cb;
        m_data = static_cast<T *>(pointer);
        guard.active = false;
        detail::armSharedFromThis(*this, pointer);
    }

    constexpr SharedPtr(const SharedPtr &other) noexcept
        : m_cb(other.m_cb)
        , m_data(other.m_data)
    {
        if (m_cb)
            m_cb->addShared();
    }

    constexpr SharedPtr(SharedPtr &&other) noexcept
        : m_cb(std::exchange(other.m_cb, nullptr))
        , m_data(std::exchange(other.m_data, nullptr))
    {}

    // Shares ownership with owner while exposing an address protected by the
    // owner's mutex. The caller must keep alias within the owner's lifetime.
    template<typename U>
    SharedPtr(const SharedPtr<U, M> &owner, T *alias) noexcept
        : m_cb(owner.m_cb)
        , m_data(alias)
    {
        assert(m_cb != nullptr && "SharedPtr alias owner is null!");
        assert(m_data != nullptr && "SharedPtr alias is null!");
        m_cb->addShared();
    }

    template<typename U>
    SharedPtr(SharedPtr<U, M> &&owner, T *alias) noexcept
        : m_cb(std::exchange(owner.m_cb, nullptr))
        , m_data(alias)
    {
        assert(m_cb != nullptr && "SharedPtr alias owner is null!");
        assert(m_data != nullptr && "SharedPtr alias is null!");
        owner.m_data = nullptr;
    }

    // Implicit conversion to a compatible pointer type (e.g. Derived -> Base)
    template<typename U>
        requires(!std::is_same_v<U, T> && std::is_convertible_v<U *, T *>)
    constexpr SharedPtr(const SharedPtr<U, M> &other) noexcept
        : m_cb(other.m_cb)
        , m_data(static_cast<T *>(other.m_data))
    {
        if (m_cb)
            m_cb->addShared();
    }

    template<typename U>
        requires(!std::is_same_v<U, T> && std::is_convertible_v<U *, T *>)
    constexpr SharedPtr(SharedPtr<U, M> &&other) noexcept
        : m_cb(std::exchange(other.m_cb, nullptr))
        , m_data(static_cast<T *>(std::exchange(other.m_data, nullptr)))
    {}

    // Used by safe::staticCast(), shares ownership with other
    template<typename U>
    constexpr SharedPtr(const SharedPtr<U, M> &other, detail::StaticCastTag) noexcept
        : m_cb(other.m_cb)
        , m_data(static_cast<T *>(other.m_data))
    {
        if (m_cb)
            m_cb->addShared();
    }

    // Used by safe::constCast()
    template<typename U>
    constexpr SharedPtr(const SharedPtr<U, M> &other, detail::ConstCastTag) noexcept
        : m_cb(other.m_cb)
        , m_data(const_cast<T *>(other.m_data))
    {
        if (m_cb)
            m_cb->addShared();
    }

    // Used by safe::dynamicCast(), empty if the cast fails
    template<typename U>
    constexpr SharedPtr(const SharedPtr<U, M> &other, detail::DynamicCastTag) noexcept
        : m_data(dynamic_cast<T *>(other.m_data))
    {
        if (m_data) {
            m_cb = other.m_cb;
            if (m_cb)
                m_cb->addShared();
        }
    }

    virtual constexpr ~SharedPtr() { release(); }

    constexpr SharedPtr &operator=(const SharedPtr &other) noexcept
    {
        if (this != &other) {
            release();
            m_cb = other.m_cb;
            m_data = other.m_data;
            if (m_cb)
                m_cb->addShared();
        }
        return *this;
    }

    constexpr SharedPtr &operator=(SharedPtr &&other) noexcept
    {
        if (this != &other) {
            release();
            m_cb = std::exchange(other.m_cb, nullptr);
            m_data = std::exchange(other.m_data, nullptr);
        }
        return *this;
    }

    template<typename U>
        requires(!std::is_same_v<U, T> && std::is_convertible_v<U *, T *>)
    constexpr SharedPtr &operator=(const SharedPtr<U, M> &other) noexcept
    {
        release();
        m_cb = other.m_cb;
        m_data = static_cast<T *>(other.m_data);
        if (m_cb)
            m_cb->addShared();
        return *this;
    }

    template<typename U>
        requires(!std::is_same_v<U, T> && std::is_convertible_v<U *, T *>)
    constexpr SharedPtr &operator=(SharedPtr<U, M> &&other) noexcept
    {
        release();
        m_cb = std::exchange(other.m_cb, nullptr);
        m_data = static_cast<T *>(std::exchange(other.m_data, nullptr));
        return *this;
    }

    constexpr SharedPtr &operator=(std::nullptr_t) noexcept
    {
        release();
        return *this;
    }

    constexpr void reset() noexcept { release(); }

    template<typename Y>
        requires std::is_convertible_v<Y *, T *>
    void reset(Y *pointer)
    {
        SharedPtr(pointer).swap(*this);
    }

    template<typename Y, typename Deleter>
        requires(std::is_convertible_v<Y *, T *>
                 && std::copy_constructible<std::decay_t<Deleter>>
                 && std::is_nothrow_invocable_v<std::decay_t<Deleter> &, Y *>)
    void reset(Y *pointer, Deleter deleter)
    {
        SharedPtr(pointer, std::move(deleter)).swap(*this);
    }

    template<typename Y, typename Deleter, typename Allocator>
        requires(std::is_convertible_v<Y *, T *>
                 && std::copy_constructible<std::decay_t<Deleter>>
                 && std::copy_constructible<std::decay_t<Allocator>>
                 && std::is_nothrow_invocable_v<std::decay_t<Deleter> &, Y *>)
    void reset(Y *pointer, Deleter deleter, Allocator allocator)
    {
        SharedPtr(pointer, std::move(deleter), std::move(allocator)).swap(*this);
    }

    constexpr void swap(SharedPtr &other) noexcept
    {
        std::swap(m_cb, other.m_cb);
        std::swap(m_data, other.m_data);
    }

    // Ownership comparison, like std::shared_ptr::owner_equal. Two pointers to the
    // same object through different base classes still compare equal.
    template<typename U>
    constexpr bool operator==(const SharedPtr<U, M> &other) const noexcept
    {
        return m_cb == other.m_cb;
    }

    template<typename U>
    constexpr std::strong_ordering operator<=>(const SharedPtr<U, M> &other) const noexcept
    {
        return std::compare_three_way{}(m_cb, other.m_cb);
    }

    constexpr bool operator==(std::nullptr_t) const noexcept { return m_cb == nullptr; }

    // Identity of the owned object, for comparing pointers. Deliberately opaque:
    // handing out the control block would let callers corrupt the reference counts.
    constexpr const void *owner() const noexcept { return m_cb; }

    // Number of shared owners, including any live lock guard
    constexpr std::size_t useCount() const noexcept
    {
        return m_cb ? m_cb->sharedRefs.load(std::memory_order_relaxed) : 0;
    }

    // Number of observers, plus one collectively held by the shared owners
    constexpr std::size_t weakCount() const noexcept
    {
        return m_cb ? m_cb->weakRefs.load(std::memory_order_relaxed) : 0;
    }

    // Explicit Bool Operator to check if valid
    constexpr explicit operator bool() const noexcept { return m_cb != nullptr; }

    virtual UniqueLockGuard<T, M> writeLock()
    {
        assert(m_data != nullptr && "SharedPtr is null!");
        return UniqueLockGuard<T, M>(m_cb, m_data);
    }

    virtual UniqueLockGuard<T, M> writeLock() const
    {
        assert(m_data != nullptr && "SharedPtr is null!");
        return UniqueLockGuard<T, M>(m_cb, m_data);
    }

    SharedLockGuard<T, M> readLock() const
    {
        assert(m_data != nullptr && "SharedPtr is null!");
        return SharedLockGuard<T, M>(m_cb, m_data);
    }
};

// std::static_pointer_cast() equivalent
template<typename T, typename U, SharedMutex M>
[[nodiscard]] constexpr SharedPtr<T, M> staticCast(const SharedPtr<U, M> &other) noexcept
{
    return SharedPtr<T, M>(other, detail::StaticCastTag{});
}

// std::dynamic_pointer_cast() equivalent, returns an empty pointer if the cast fails
template<typename T, typename U, SharedMutex M>
[[nodiscard]] constexpr SharedPtr<T, M> dynamicCast(const SharedPtr<U, M> &other) noexcept
{
    return SharedPtr<T, M>(other, detail::DynamicCastTag{});
}

// std::const_pointer_cast() equivalent
template<typename T, typename U, SharedMutex M>
[[nodiscard]] constexpr SharedPtr<T, M> constCast(const SharedPtr<U, M> &other) noexcept
{
    return SharedPtr<T, M>(other, detail::ConstCastTag{});
}

template<typename Deleter, typename T, SharedMutex M>
constexpr Deleter *getDeleter(const SharedPtr<T, M> &pointer) noexcept
{
    if (!pointer.m_cb)
        return nullptr;
    return static_cast<Deleter *>(pointer.m_cb->getDeleter(typeid(std::remove_cv_t<Deleter>)));
}

template<typename T, SharedMutex M>
constexpr void swap(SharedPtr<T, M> &first, SharedPtr<T, M> &second) noexcept
{
    first.swap(second);
}

template<typename T, SharedMutex M>
constexpr void swap(WeakPtr<T, M> &first, WeakPtr<T, M> &second) noexcept
{
    first.swap(second);
}

template<typename T, SharedMutex M>
class WeakPtr
{
    template<typename, SharedMutex>
    friend class WeakPtr;

private:
    detail::ControlBlockBase<M> *m_cb{nullptr};
    T *m_data{nullptr};

    constexpr void release() noexcept
    {
        if (m_cb) {
            m_cb->releaseWeak();
            m_cb = nullptr;
            m_data = nullptr;
        }
    }

public:
    using ElementType = T;
    using MutexType = M;

    constexpr WeakPtr() = default;

    template<typename U>
        requires std::is_convertible_v<U *, T *>
    constexpr WeakPtr(const SharedPtr<U, M> &shared) noexcept
        : m_cb(shared.m_cb)
        , m_data(static_cast<T *>(shared.m_data))
    {
        if (m_cb)
            m_cb->addWeak();
    }

    constexpr WeakPtr(const WeakPtr &other) noexcept
        : m_cb(other.m_cb)
        , m_data(other.m_data)
    {
        if (m_cb)
            m_cb->addWeak();
    }

    // Implicit conversion to a compatible pointer type (e.g. Derived -> Base)
    template<typename U>
        requires(!std::is_same_v<U, T> && std::is_convertible_v<U *, T *>)
    constexpr WeakPtr(const WeakPtr<U, M> &other) noexcept
        : m_cb(other.m_cb)
        , m_data(static_cast<T *>(other.m_data))
    {
        if (m_cb)
            m_cb->addWeak();
    }

    constexpr WeakPtr(WeakPtr &&other) noexcept
        : m_cb(std::exchange(other.m_cb, nullptr))
        , m_data(std::exchange(other.m_data, nullptr))
    {}

    constexpr ~WeakPtr() { release(); }

    constexpr WeakPtr &operator=(const WeakPtr &other) noexcept
    {
        if (this != &other) {
            release();
            m_cb = other.m_cb;
            m_data = other.m_data;
            if (m_cb)
                m_cb->addWeak();
        }
        return *this;
    }

    constexpr WeakPtr &operator=(WeakPtr &&other) noexcept
    {
        if (this != &other) {
            release();
            m_cb = std::exchange(other.m_cb, nullptr);
            m_data = std::exchange(other.m_data, nullptr);
        }
        return *this;
    }

    template<typename U>
        requires(!std::is_same_v<U, T> && std::is_convertible_v<U *, T *>)
    constexpr WeakPtr &operator=(const WeakPtr<U, M> &other) noexcept
    {
        release();
        m_cb = other.m_cb;
        m_data = static_cast<T *>(other.m_data);
        if (m_cb)
            m_cb->addWeak();
        return *this;
    }

    template<typename U>
        requires std::is_convertible_v<U *, T *>
    constexpr WeakPtr &operator=(const SharedPtr<U, M> &shared) noexcept
    {
        release();
        m_cb = shared.m_cb;
        m_data = static_cast<T *>(shared.m_data);
        if (m_cb)
            m_cb->addWeak();
        return *this;
    }

    constexpr void reset() noexcept { release(); }

    constexpr void swap(WeakPtr &other) noexcept
    {
        std::swap(m_cb, other.m_cb);
        std::swap(m_data, other.m_data);
    }

    // Ownership comparison, like SharedPtr. Two observers of the same object
    // compare equal even if they watch it through different base classes, and
    // an expired observer keeps comparing equal to its peers.
    template<typename U>
    constexpr bool operator==(const WeakPtr<U, M> &other) const noexcept
    {
        return m_cb == other.m_cb;
    }

    template<typename U>
    constexpr std::strong_ordering operator<=>(const WeakPtr<U, M> &other) const noexcept
    {
        return std::compare_three_way{}(m_cb, other.m_cb);
    }

    // Identity of the observed object, comparable with SharedPtr::owner().
    // Stays the same once the object is gone, so an expired observer is still
    // findable in a container.
    constexpr const void *owner() const noexcept { return m_cb; }

    constexpr std::size_t useCount() const noexcept
    {
        return m_cb ? m_cb->sharedRefs.load(std::memory_order_relaxed) : 0;
    }

    constexpr bool expired() const noexcept
    {
        return !m_cb || m_cb->sharedRefs.load(std::memory_order_relaxed) == 0;
    }

    constexpr SharedPtr<T, M> lock() const noexcept
    {
        // The data pointer stays valid as long as a shared owner exists, and
        // tryAddShared() only succeeds while T is still alive.
        if (m_cb && m_cb->tryAddShared())
            return SharedPtr<T, M>(m_cb, m_data);
        return SharedPtr<T, M>{};
    }
};

namespace detail {
template<SharedMutex M, typename U>
EnableSharedFromThis<U, M> *esftBaseOf(const EnableSharedFromThis<U, M> *) noexcept;

template<typename T, typename M>
concept HasSharedFromThis = SharedMutex<M> && requires(T *object) { esftBaseOf<M>(object); };

template<typename P>
struct IsPlainSharedPtr : std::false_type
{};

template<typename T, SharedMutex M>
struct IsPlainSharedPtr<SharedPtr<T, M>> : std::true_type
{};
} // namespace detail

// Locks several pointers at once. The mutexes are always acquired in the same
// global order (by control block address) and repeated pointers are locked once,
// so two threads locking the same objects in opposite argument order cannot
// deadlock and passing the same pointer twice cannot self deadlock.
template<typename... Ptrs>
class [[nodiscard]] ScopedLock
{
    static_assert(sizeof...(Ptrs) > 0, "ScopedLock needs at least one pointer");

    using MutexType = typename std::tuple_element_t<0, std::tuple<Ptrs...>>::MutexType;
    static_assert((std::is_same_v<MutexType, typename Ptrs::MutexType> && ...),
                  "all pointers must use the same mutex type");

    using Block = detail::ControlBlockBase<MutexType>;
    static constexpr std::size_t Count = sizeof...(Ptrs);

    std::tuple<typename Ptrs::ElementType *...> m_data;
    std::array<Block *, Count> m_locked{};
    std::size_t m_lockedCount{0};

public:
    explicit ScopedLock(Ptrs &...pointers)
        : m_data{pointers.m_data...}
        , m_locked{pointers.m_cb...}
    {
        assert(((pointers.m_cb != nullptr) && ...) && "ScopedLock on a null pointer!");

        std::ranges::sort(m_locked);
        m_lockedCount = std::size_t(std::ranges::unique(m_locked).begin() - m_locked.begin());

        // Take every lock first, then the references: if a lock throws, the
        // destructor never runs and must not have anything to undo
        for (std::size_t i = 0; i < m_lockedCount; ++i)
            m_locked[i]->mutex.lock();
        for (std::size_t i = 0; i < m_lockedCount; ++i)
            m_locked[i]->addShared();
    }

    ~ScopedLock()
    {
        for (std::size_t i = m_lockedCount; i-- > 0;) {
            m_locked[i]->mutex.unlock();
            m_locked[i]->releaseShared();
        }
    }

    ScopedLock(const ScopedLock &) = delete;
    ScopedLock &operator=(const ScopedLock &) = delete;

    template<std::size_t I>
    [[nodiscard]] auto &get() const noexcept
    {
        return *std::get<I>(m_data);
    }
};

// CopyOnWritePtr is deliberately not accepted: locking it here would not detach.
// Convert it to a SharedPtr first if a plain shared view is what you want.
template<typename... Ptrs>
    requires(... && detail::IsPlainSharedPtr<std::remove_cv_t<Ptrs>>::value)
[[nodiscard]] ScopedLock<Ptrs...> lockAll(Ptrs &...pointers)
{
    return ScopedLock<Ptrs...>(pointers...);
}

// Lets an object hand out owning pointers to itself. Inside a critical section all
// you have is a T&, and this is the way back to a pointer.
// The returned pointer is *not* locked, and the mutex is not recursive: locking it
// again while still inside the critical section deadlocks.
template<typename T, SharedMutex M>
class EnableSharedFromThis
{
    template<typename Owner, typename Object, SharedMutex Mx>
    friend void detail::armSharedFromThis(const SharedPtr<Owner, Mx> &, Object *) noexcept;

    WeakPtr<T, M> m_weakThis;

protected:
    constexpr EnableSharedFromThis() = default;

    // A copy is a different object, so it must not inherit the back reference.
    // Whoever takes ownership of the copy arms it.
    constexpr EnableSharedFromThis(const EnableSharedFromThis &) noexcept {}
    constexpr EnableSharedFromThis &operator=(const EnableSharedFromThis &) noexcept
    {
        return *this;
    }
    constexpr ~EnableSharedFromThis() = default;

public:
    using ElementType = T;
    using MutexType = M;

    [[nodiscard]] constexpr SharedPtr<T, M> sharedFromThis() const noexcept
    {
        return m_weakThis.lock();
    }
    [[nodiscard]] constexpr WeakPtr<T, M> weakFromThis() const noexcept { return m_weakThis; }
    [[nodiscard]] constexpr SharedPtr<T, M> sharedFromThis() noexcept
    {
        return m_weakThis.lock();
    }
    [[nodiscard]] constexpr WeakPtr<T, M> weakFromThis() noexcept { return m_weakThis; }
};

namespace detail {
template<typename Owner, typename Object, SharedMutex M>
void armSharedFromThis(const SharedPtr<Owner, M> &owner, Object *object) noexcept
{
    if constexpr (!std::is_const_v<Object> && HasSharedFromThis<Object, M>) {
        // EnableSharedFromThis<U, M>, with U deduced from the base rather than
        // from the concrete object type.
        using EsftBase = std::remove_pointer_t<decltype(esftBaseOf<M>(object))>;
        using SharedType = typename EsftBase::ElementType;
        auto *base = static_cast<EsftBase *>(object);
        if (base->m_weakThis.expired()) {
            SharedPtr<SharedType, M> self(owner, static_cast<SharedType *>(object));
            base->m_weakThis = self;
        }
    }
}
} // namespace detail

template<typename T, SharedMutex M, typename... Args>
SharedPtr<T, M> makeShared(Args &&...args)
{
    return allocateShared<T, M>(std::allocator<std::byte>{}, std::forward<Args>(args)...);
}

template<typename T, SharedMutex M, typename Allocator, typename... Args>
SharedPtr<T, M> allocateShared(const Allocator &allocator, Args &&...args)
{
    auto *cb =
        detail::allocateInplaceControlBlock<T, M>(allocator, std::forward<Args>(args)...);
    T *data = cb->data.get();
    SharedPtr<T, M> pointer(cb, data);
    detail::armSharedFromThis(pointer, data);
    return pointer;
}

// A shared pointer that clones the object the first time it is written to while
// other owners exist.
//
// Interaction with WeakPtr: a WeakPtr observes a *control block*, not a pointer.
// Once this pointer detaches it moves to a fresh block, so observers taken before
// the detach keep watching the original object and never see the clone. Take the
// WeakPtr from the pointer you actually want to follow, or use a plain SharedPtr
// if observers must track every write.
template<typename T, SharedMutex M>
class CopyOnWritePtr : public SharedPtr<T, M>
{
    using Base = SharedPtr<T, M>;

    template<typename U, SharedMutex Mx, typename... Args>
    friend CopyOnWritePtr<U, Mx> makeCopyOnWrite(Args &&...args);

    CopyOnWritePtr(detail::ControlBlockBase<M> *cb, T *data) noexcept
        : Base(cb, data)
    {}

    // Note: T is what gets cloned, so holding a base pointer to a derived object
    // (e.g. after an up-cast) slices the object on the first detach().
    void detach()
    {
        if (!this->m_cb)
            return;

        if (this->m_cb->sharedRefs.load(std::memory_order_acquire) > 1) {
            detail::InplaceControlBlock<T, M, std::allocator<std::byte>> *newCb = nullptr;
            {
                // Read-Lock old data to protect copy constructor execution. The guard
                // must be released before release(), which may destroy the old block
                // (and with it the mutex) if the other owners vanish meanwhile.
                auto oldDataGuard = this->readLock();

                // Create cloned block
                newCb = detail::allocateInplaceControlBlock<T, M>(
                    std::allocator<std::byte>{}, *oldDataGuard);
            }

            this->release();

            this->m_cb = newCb;
            this->m_data = newCb->data.get();
            detail::armSharedFromThis(static_cast<const Base &>(*this), this->m_data);
        }
    }

public:
    constexpr CopyOnWritePtr() = default;

    // Adopts a shared object. Deliberately not `using Base::Base`: inheriting the
    // converting constructors would let SharedPtr<Derived> become CopyOnWritePtr<Base>
    // implicitly, which slices on the first detach().
    constexpr CopyOnWritePtr(const Base &other) noexcept
        : Base(other)
    {}

    UniqueLockGuard<T, M> writeLock() override
    {
        assert(this->m_data != nullptr && "CopyOnWritePtr is null!");
        detach();
        return Base::writeLock();
    }
};

template<typename T, SharedMutex M, typename... Args>
CopyOnWritePtr<T, M> makeCopyOnWrite(Args &&...args)
{
    auto *cb = detail::allocateInplaceControlBlock<T, M>(
        std::allocator<std::byte>{}, std::forward<Args>(args)...);
    T *data = cb->data.get();
    CopyOnWritePtr<T, M> pointer(cb, data);
    detail::armSharedFromThis(static_cast<const SharedPtr<T, M> &>(pointer), data);
    return pointer;
}

} // namespace safe

// Hashing is by owner, exactly like ==, so the pointers work as keys in the
// unordered containers. A SharedPtr and a WeakPtr to the same object hash the
// same, and an expired WeakPtr keeps the hash it had.
//
// CopyOnWritePtr is deliberately left unhashable: detaching moves it to a fresh
// control block, so a key already sitting in a container would silently stop
// matching the pointer it was made from. Convert it to a SharedPtr, and mean it,
// if you want to key on the object it owns right now.
namespace std {

template<typename T, safe::SharedMutex M>
struct hash<safe::SharedPtr<T, M>>
{
    std::size_t operator()(const safe::SharedPtr<T, M> &pointer) const noexcept
    {
        return std::hash<const void *>{}(pointer.owner());
    }
};

template<typename T, safe::SharedMutex M>
struct hash<safe::WeakPtr<T, M>>
{
    std::size_t operator()(const safe::WeakPtr<T, M> &pointer) const noexcept
    {
        return std::hash<const void *>{}(pointer.owner());
    }
};

} // namespace std
