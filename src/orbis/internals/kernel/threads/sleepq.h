// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <iterator>
#include <list>

namespace Libraries::Kernel {

struct Pthread;
struct SleepQueue;

struct ListBaseHook {
    ListBaseHook* prev = nullptr;
    ListBaseHook* next = nullptr;

    ListBaseHook() = default;
    ListBaseHook(const ListBaseHook&) = delete;
    ListBaseHook& operator=(const ListBaseHook&) = delete;

    ~ListBaseHook() {
        unlink();
    }

    void unlink() noexcept {
        if (next != nullptr) {
            prev->next = next;
            next->prev = prev;
            prev = nullptr;
            next = nullptr;
        }
    }
};

class SleepqList {
public:
    struct iterator {
        using iterator_category = std::bidirectional_iterator_tag;
        using value_type = SleepQueue;
        using difference_type = std::ptrdiff_t;
        using pointer = SleepQueue*;
        using reference = SleepQueue&;

        ListBaseHook* node = nullptr;

        reference operator*() const noexcept;
        pointer operator->() const noexcept;
        iterator& operator++() noexcept {
            node = node->next;
            return *this;
        }
        iterator operator++(int) noexcept {
            iterator tmp = *this;
            node = node->next;
            return tmp;
        }
        bool operator==(const iterator& other) const noexcept = default;
    };

    SleepqList() noexcept {
        root_.prev = &root_;
        root_.next = &root_;
    }

    SleepqList(const SleepqList&) = delete;
    SleepqList& operator=(const SleepqList&) = delete;

    ~SleepqList() {
        clear();
    }

    [[nodiscard]] bool empty() const noexcept {
        return root_.next == &root_;
    }

    void push_front(SleepQueue& item) noexcept;
    [[nodiscard]] SleepQueue& front() noexcept;

    void pop_front() noexcept {
        root_.next->unlink();
    }

    void clear() noexcept {
        while (!empty()) {
            pop_front();
        }
    }

    [[nodiscard]] iterator begin() noexcept {
        return iterator{root_.next};
    }
    [[nodiscard]] iterator end() noexcept {
        return iterator{&root_};
    }

private:
    ListBaseHook root_;
};

struct SleepQueue : public ListBaseHook {
    std::list<Pthread*> sq_blocked;
    SleepqList sq_freeq;
    void* sq_wchan;
    int sq_type;
};

inline SleepQueue& SleepqList::iterator::operator*() const noexcept {
    return *static_cast<SleepQueue*>(node);
}

inline SleepQueue* SleepqList::iterator::operator->() const noexcept {
    return static_cast<SleepQueue*>(node);
}

inline void SleepqList::push_front(SleepQueue& item) noexcept {
    item.unlink();
    ListBaseHook* first = root_.next;
    item.prev = &root_;
    item.next = first;
    first->prev = &item;
    root_.next = &item;
}

inline SleepQueue& SleepqList::front() noexcept {
    return *static_cast<SleepQueue*>(root_.next);
}

void SleepqLock(void* wchan);

void SleepqUnlock(void* wchan);

SleepQueue* SleepqLookup(void* wchan);

void SleepqAdd(void* wchan, Pthread* td);

bool SleepqRemove(SleepQueue* sq, Pthread* td);

void SleepqDrop(SleepQueue* sq, void (*callback)(Pthread*, void*), void* arg);

} // namespace Libraries::Kernel
