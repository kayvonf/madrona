#pragma once

#include <type_traits>
#include <memory>
#include <utility>

namespace madrona {

template <typename T>
class Optional {
public:
    static constexpr Optional<T> none()
    {
        return Optional<T>();
    }

    template <typename... Args>
    static constexpr Optional<T> make(Args && ...args)
    {
        return Optional<T>(std::in_place, std::forward<Args>(args)...);
    }

    template <typename U = T,
              typename = std::enable_if_t<std::is_copy_constructible_v<U>>>
    explicit (!std::is_same_v<U, T>)
    constexpr Optional(const U &v)
        : storage_(v)
    {}

    template <typename U = T,
              typename = std::enable_if_t<std::is_move_constructible_v<U>>>
    explicit (!std::is_same_v<U, T>)
    constexpr Optional(U &&v)
        : storage_(std::forward<U>(v))
    {}

    Optional(const Optional &) = default;
    Optional(Optional &&) = default;

    Optional & operator=(const Optional &) = default;
    Optional & operator=(Optional &&) = default;

    ~Optional() = default;

    template <typename... Args>
    constexpr T & emplace(Args && ...args)
    {
        storage_.destruct();

        std::construct_at(&storage_.value, std::forward<Args>(args)...);
        storage_.initialized = true;

        return storage_.value;
    }

    constexpr bool has_value() const { return storage_.initialized; }

    constexpr T * operator->() { return &storage_.value; }
    constexpr const T * operator->() const { return &storage_.value; }

    constexpr T & operator*() & { return storage_.value; }
    constexpr const T & operator*() const & { return storage_.value; }

    constexpr T && operator*() && { return std::move(storage_.value); }
    constexpr const T && operator*() const &&
    {
        return std::move(storage_.value);
    }

    constexpr void reset()
    {
        storage_.destruct();
        storage_.initialized = false;
    }

private:
    constexpr Optional()
        : storage_()
    {}

    template <typename... Args>
    constexpr Optional(std::in_place_t, Args && ...args)
        : storage_(std::forward<Args>(args)...)
    {}

    struct empty_type {};

    struct StorageImpl {
        union {
          empty_type empty;
          T value;
        };

        bool initialized;

        constexpr StorageImpl()
            : empty(),
              initialized(false)
        {}

        template <typename... Args>
        constexpr StorageImpl(Args&& ...args)
            : value(std::forward<Args>(args)...),
              initialized(true)
        {}

        constexpr void destruct() {
            if (initialized) {
                value.~T();
            }
        }
    };

    template <typename U, bool trivial =
        std::is_trivially_destructible_v<U>>
    struct Destruct : StorageImpl {
        using StorageImpl::StorageImpl;
    };

    template <typename U, bool trivial =
        std::is_trivially_copy_constructible_v<U>>
    struct CopyConstruct : Destruct<U> {
        using Destruct<U>::Destruct;
    };

    template <typename U, bool trivial =
        std::is_trivially_move_constructible_v<U>>
    struct MoveConstruct : CopyConstruct<U> {
        using CopyConstruct<U>::CopyConstruct;
    };

    template <typename U, bool trivial =
        std::is_trivially_destructible_v<U> &&
        std::is_trivially_copy_constructible_v<U> &&
        std::is_trivially_copy_assignable_v<U>>
    struct CopyAssign : MoveConstruct<U> {
        using MoveConstruct<U>::MoveConstruct;
    };

    template <typename U, bool trivial =
        std::is_trivially_destructible_v<U> &&
        std::is_trivially_move_constructible_v<U> &&
        std::is_trivially_move_assignable_v<U>>
    struct MoveAssign : CopyAssign<U> {
        using CopyAssign<U>::CopyAssign;
    };

    template <typename U> struct Destruct<U, false> : StorageImpl {
        using StorageImpl::StorageImpl;
    
        constexpr ~Destruct()
        {
            this->destruct();
        }
    };

    template <typename U> struct CopyConstruct<U, false> : Destruct<U> {
        using Destruct<U>::Destruct;
    
        constexpr CopyConstruct(const CopyConstruct &o)
        {
            this->initialized = o.initialized;
    
            if (this->initialized) {
                std::construct_at(&this->value, o.value);
            }
        }
    };

    template <typename U> struct MoveConstruct<U, false> : CopyConstruct<U> {
        using CopyConstruct<U>::CopyConstruct;
    
        constexpr MoveConstruct(MoveConstruct &&o)
        {
            this->initialized = o.initialized;

            if (this->initialized) {
                std::construct_at(&this->value, std::move(o.value));
                o.initialized = false;
            } 
        }
    };

    template <typename U> struct CopyAssign<U, false> : MoveConstruct<U> {
        using MoveConstruct<U>::MoveConstruct;
    
        constexpr CopyAssign & operator=(const CopyAssign &o) 
        {
            if (this->initialized) {
                if (o.initialized) {
                    this->value = o.value;
                } else {
                    this->destruct();
                    this->initialized = false;
                }
            } else if (o.initialized) {
                std::construct_at(&this->value, o.value);
                this->initialized = true;
            }
    
            return *this;
        }
    };

    template<typename U> struct MoveAssign<U, false> : CopyAssign<U> {
        using CopyAssign<U>::CopyAssign;

        MoveAssign & operator=(const MoveAssign &) = default;
    
        constexpr MoveAssign & operator=(MoveAssign &&o)
        {
            if (this->initialized) {
                if (o.initialized) {
                    this->value = std::move(o.value);
                    o.initialized = false;
                } else {
                    this->destruct();
                    this->initialized = false;
                }
            } else if (o.initialized) {
                std::construct_at(&this->value, std::move(o.value));
                o.initialized = false;
                this->initialized = true;
            }
    
            return *this;
        }
    };
    
    using Storage = MoveAssign<T>;
    Storage storage_;
};


}