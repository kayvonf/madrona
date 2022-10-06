/*
 * Copyright 2021-2022 Brennan Shacklett and contributors
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */
#pragma once

#include <madrona/sync.hpp>

#include <atomic>
#include <cstdint>

namespace madrona {

template <typename K, typename V, template <typename> typename StoreT>
class IDMap {
public:
    class Cache {
    public:
        Cache();
        Cache(const Cache &) = delete;

    private:
        uint32_t free_head_;
        uint32_t num_free_ids_;
        uint32_t overflow_head_;
        uint32_t num_overflow_ids_;

    friend class IDMap;
    };

    struct FreeNode {
        uint32_t subNext;
        uint32_t globalNext;
    };

    struct Node {
        union {
            V val;
            FreeNode freeNode;
        };
        std::atomic_uint32_t gen;
    };

    IDMap(uint32_t init_capacity);

    inline K acquireID(Cache &cache);

    inline void releaseID(Cache &cache, uint32_t id);
    inline void releaseID(Cache &cache, K k)
    {
        releaseID(cache, k.id);
    }

    inline void bulkRelease(Cache &cache, K *keys, uint32_t num_keys);

    inline V lookup(K k) const
    {
        const Node &node = store_[k.id];

        if (node.gen.load(std::memory_order_relaxed) != k.gen) {
            return V::none();
        }

        return node.val;
    }

    inline bool present(K k) const
    {
        const Node &node = store_[k.id];
        return node.gen.load(std::memory_order_relaxed) == k.gen;
    }

    inline V & getRef(K k)
    {
        Node &node = store_[k.id];
        assert(node.gen.load(std::memory_order_relaxed) == k.gen);

        return node.val;
    }

    inline V & getRef(uint32_t id)
    {
        return store_[id].val;
    }

#ifdef TSAN_ENABLED
    // These helpers are needed when TSAN is enabled to allow explicitly
    // marking Node::gen as acquired and released, which is currently done
    // with atomic_thread_fence by outside code after updating / reading
    // multiple IDs.

    inline void acquireGen(uint32_t id)
    {
        TSAN_ACQUIRE(&store_[id].gen);
    }

    inline void releaseGen(uint32_t id)
    {
        TSAN_RELEASE(&store_[id].gen);
    }
#endif

private:
    using Store = StoreT<Node>;
   
    static_assert(sizeof(FreeNode) <= sizeof(V));

    struct alignas(std::atomic_uint64_t) FreeHead {
        uint32_t gen;
        uint32_t head;
    };

    std::atomic<FreeHead> free_head_;
    [[no_unique_address]] Store store_;

    static_assert(decltype(free_head_)::is_always_lock_free);

    static constexpr uint32_t ids_per_cache_ = 64;

    friend Store;
};

}
