//
// Created by gamerpuppy on 7/4/2021.
//

#ifndef STS_LIGHTSPEED_ACTIONQUEUE_H
#define STS_LIGHTSPEED_ACTIONQUEUE_H

#include "sts_common.h"

#include <bitset>
#include <deque>
#include <functional>
#include <optional>
#include <algorithm>
#include <cassert>


namespace sts {

    class BattleContext;
    typedef std::function<void (BattleContext &)> ActionFunction;

    struct Action {
        ActionFunction actFunc;
        bool clearOnCombatVictory = true;

        Action() = default;
        Action(ActionFunction a) : actFunc(std::move(a)) {}
        Action(ActionFunction a, bool b) : actFunc(std::move(a)), clearOnCombatVictory(b) {}
    };


    // Simple deque
    template<int capacity>
    struct ActionQueue {
        friend BattleContext;
        int front = 0;
        int back = 0;
        int size = 0;
        std::bitset<capacity> bits; // for the shouldClear field
        std::optional<std::deque<Action>> expanded;

#ifdef sts_action_queue_use_raw_array
        ActionFunction arr[capacity];
        ActionQueue() = default;
        ActionQueue(const ActionQueue &rhs) : front(rhs.front), back(rhs.back),
                size(rhs.size), bits(rhs.bits), expanded(rhs.expanded) {
            for (int i = 0; i < capacity; ++i) {
                arr[i] = rhs.arr[i];
            }
        }
#else
        std::array<ActionFunction,capacity> arr;
#endif

        void clear();
        void pushFront(Action a);
        void pushBack(Action a);
        bool isEmpty();
        ActionFunction popFront();
        void clearOnCombatVictory();
        [[nodiscard]] int getCapacity() const;

    private:
        void expand();
    };

    template<int capacity>
    void ActionQueue<capacity>::clear() {
        size = 0;
        back = 0;
        front = 0;
        bits.reset();
        expanded.reset();
    }

    template<int capacity>
    void ActionQueue<capacity>::expand() {
        expanded.emplace();
        int idx = front;
        for (int i = 0; i < size; ++i) {
            expanded->emplace_back(std::move(arr[idx]), bits[idx]);
            idx = (idx + 1) % capacity;
        }
    }

    template <int capacity>
    void ActionQueue<capacity>::pushFront(Action a) {
        if (expanded) {
            expanded->push_front(std::move(a));
            ++size;
            return;
        }
        if (size == capacity) {
            expand();
            expanded->push_front(std::move(a));
            ++size;
            return;
        }
        --front;
        ++size;
        if (front < 0) {
            front = capacity-1;
        }
        bits.set(front, a.clearOnCombatVictory);
        arr[front] = std::move(a.actFunc);
    }

    template<int capacity>
    void ActionQueue<capacity>::pushBack(Action a) {
        if (expanded) {
            expanded->push_back(std::move(a));
            ++size;
            return;
        }
        if (size == capacity) {
            expand();
            expanded->push_back(std::move(a));
            ++size;
            return;
        }
        if (back >= capacity) {
            back = 0;
        }
        bits.set(back, a.clearOnCombatVictory);
        arr[back] = std::move(a.actFunc);
        ++back;
        ++size;
    }

    template<int capacity>
    bool ActionQueue<capacity>::isEmpty() {
        return size == 0;
    }

    template<int capacity>
    ActionFunction ActionQueue<capacity>::popFront() {
#ifdef sts_asserts
        assert(size > 0 );
#endif
        if (expanded) {
            auto action = std::move(expanded->front().actFunc);
            expanded->pop_front();
            --size;
            if (expanded->empty()) {
                clear();
            }
            return action;
        }
        ActionFunction a = arr[front];
        ++front;
        --size;
        if (front >= capacity) {
            front = 0;
        }
        return a;
    }

    template<int capacity>
    void ActionQueue<capacity>::clearOnCombatVictory() {
        if (expanded) {
            auto &queue = *expanded;
            queue.erase(std::remove_if(queue.begin(), queue.end(),
                [] (const Action &a) { return a.clearOnCombatVictory; }), queue.end());
            size = static_cast<int>(queue.size());
            if (queue.empty()) {
                clear();
            }
            return;
        }

        int read = front;
        int write = front;
        int kept = 0;
        const int oldSize = size;
        for (int i = 0; i < oldSize; ++i) {
            if (!bits[read]) {
                if (write != read) {
                    arr[write] = std::move(arr[read]);
                }
                bits[write] = false;
                write = (write + 1) % capacity;
                ++kept;
            }
            read = (read + 1) % capacity;
        }
        size = kept;
        back = write;
    }

    template<int capacity>
    int ActionQueue<capacity>::getCapacity() const {
        return capacity;
    }

}


#endif //STS_LIGHTSPEED_ACTIONQUEUE_H
