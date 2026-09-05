#pragma once

#include "FreeRTOS.h"
#include <cstring>
#include <deque>
#include <vector>

// The view runs on one thread in this harness. Model the byte-copy and capacity
// semantics of its FreeRTOS queue without emulating the firmware scheduler.
struct QaQueue {
    size_t capacity;
    size_t itemSize;
    std::deque<std::vector<unsigned char>> items;
};
using QueueHandle_t = QaQueue*;

inline QueueHandle_t xQueueCreate(size_t capacity, size_t itemSize)
{
    return new QaQueue{capacity, itemSize, {}};
}

inline BaseType_t xQueueSend(QueueHandle_t queue, const void* source, int)
{
    if (queue->items.size() >= queue->capacity) {
        return pdFALSE;
    }
    const auto* bytes = static_cast<const unsigned char*>(source);
    queue->items.emplace_back(bytes, bytes + queue->itemSize);
    return pdTRUE;
}

inline BaseType_t xQueueReceive(QueueHandle_t queue, void* destination, int)
{
    if (queue->items.empty()) {
        return pdFALSE;
    }
    std::memcpy(destination, queue->items.front().data(), queue->itemSize);
    queue->items.pop_front();
    return pdTRUE;
}

inline void xQueueReset(QueueHandle_t queue) { queue->items.clear(); }
inline void vQueueDelete(QueueHandle_t queue) { delete queue; }
