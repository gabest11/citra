// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>

#include "common/assert.h"
#include "common/logging/log.h"

#include "core/arm/arm_interface.h"
#include "core/core.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/process.h"
#include "core/hle/kernel/thread.h"
#include "core/hle/kernel/timer.h"

namespace Kernel {

unsigned int Object::next_object_id;
HandleTable g_handle_table;

void WaitObject::AddWaitingThread(SharedPtr<Thread> thread) {
    auto itr = std::find(waiting_threads.begin(), waiting_threads.end(), thread);
    if (itr == waiting_threads.end())
        waiting_threads.push_back(std::move(thread));
}

void WaitObject::RemoveWaitingThread(Thread* thread) {
    auto itr = std::find(waiting_threads.begin(), waiting_threads.end(), thread);
    if (itr != waiting_threads.end())
        waiting_threads.erase(itr);
}

SharedPtr<Thread> WaitObject::WakeupNextThread() {
    if (waiting_threads.empty())
        return nullptr;

    auto next_thread = std::move(waiting_threads.front());
    waiting_threads.erase(waiting_threads.begin());

    next_thread->ReleaseWaitObject(this);

    return next_thread;
}

void WaitObject::WakeupAllWaitingThreads() {
    auto waiting_threads_copy = waiting_threads;

    // We use a copy because ReleaseWaitObject will remove the thread from this object's
    // waiting_threads list
    for (auto thread : waiting_threads_copy)
        thread->ReleaseWaitObject(this);

    ASSERT_MSG(waiting_threads.empty(), "failed to awaken all waiting threads!");
}

HandleTable::HandleTable() {
    next_generation = 1;
    Clear();
}

ResultVal<Handle> HandleTable::Create(SharedPtr<Object> obj) {
    DEBUG_ASSERT(obj != nullptr);

    u16 slot = next_free_slot;
    if (slot >= generations.size()) {
        LOG_ERROR(Kernel, "Unable to allocate Handle, too many slots in use.");
        return ERR_OUT_OF_HANDLES;
    }
    next_free_slot = generations[slot];

    u16 generation = next_generation++;

    // Overflow count so it fits in the 15 bits dedicated to the generation in the handle.
    // CTR-OS doesn't use generation 0, so skip straight to 1.
    if (next_generation >= (1 << 15)) next_generation = 1;

    generations[slot] = generation;
    objects[slot] = std::move(obj);

    Handle handle = generation | (slot << 15);
    return MakeResult<Handle>(handle);
}

ResultVal<Handle> HandleTable::Duplicate(Handle handle) {
    SharedPtr<Object> object = GetGeneric(handle);
    if (object == nullptr) {
        LOG_ERROR(Kernel, "Tried to duplicate invalid handle: %08X", handle);
        return ERR_INVALID_HANDLE;
    }
    return Create(std::move(object));
}

ResultCode HandleTable::Close(Handle handle) {
    if (!IsValid(handle))
        return ERR_INVALID_HANDLE;

    u16 slot = GetSlot(handle);

    objects[slot] = nullptr;

    generations[slot] = next_free_slot;
    next_free_slot = slot;
    return RESULT_SUCCESS;
}

bool HandleTable::IsValid(Handle handle) const {
    size_t slot = GetSlot(handle);
    u16 generation = GetGeneration(handle);

    return slot < MAX_COUNT && objects[slot] != nullptr && generations[slot] == generation;
}

SharedPtr<Object> HandleTable::GetGeneric(Handle handle) const {
    if (handle == CurrentThread) {
        return GetCurrentThread();
    } else if (handle == CurrentProcess) {
        return g_current_process;
    }

    if (!IsValid(handle)) {
        return nullptr;
    }
    return objects[GetSlot(handle)];
}

void HandleTable::Clear() {
    for (u16 i = 0; i < MAX_COUNT; ++i) {
        generations[i] = i + 1;
        objects[i] = nullptr;
    }
    next_free_slot = 0;
}

/// Initialize the kernel
void Init() {
    Kernel::ThreadingInit();
    Kernel::TimersInit();

    Object::next_object_id = 0;
    // TODO(Subv): Start the process ids from 10 for now, as lower PIDs are
    // reserved for low-level services
    Process::next_process_id = 10;
}

/// Shutdown the kernel
void Shutdown() {
    Kernel::ThreadingShutdown();
    Kernel::TimersShutdown();
    g_handle_table.Clear(); // Free all kernel objects
    g_current_process = nullptr;
}

} // namespace
