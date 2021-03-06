// Copyright 2015 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/common_paths.h"
#include "common/file_util.h"
#include "common/logging/log.h"

#include "core/hle/service/service.h"
#include "core/hle/service/apt/apt.h"
#include "core/hle/service/apt/apt_a.h"
#include "core/hle/service/apt/apt_s.h"
#include "core/hle/service/apt/apt_u.h"

#include "core/hle/hle.h"
#include "core/hle/kernel/event.h"
#include "core/hle/kernel/mutex.h"
#include "core/hle/kernel/shared_memory.h"
#include "core/hle/kernel/thread.h"

namespace Service {
namespace APT {

// Address used for shared font (as observed on HW)
// TODO(bunnei): This is the hard-coded address where we currently dump the shared font from via
// https://github.com/citra-emu/3dsutils. This is technically a hack, and will not work at any
// address other than 0x18000000 due to internal pointers in the shared font dump that would need to
// be relocated. This might be fixed by dumping the shared font @ address 0x00000000 and then
// correctly mapping it in Citra, however we still do not understand how the mapping is determined.
static const VAddr SHARED_FONT_VADDR = 0x18000000;

/// Handle to shared memory region designated to for shared system font
static Kernel::SharedPtr<Kernel::SharedMemory> shared_font_mem;

static Kernel::SharedPtr<Kernel::Mutex> lock;
static Kernel::SharedPtr<Kernel::Event> notification_event; ///< APT notification event
static Kernel::SharedPtr<Kernel::Event> start_event; ///< APT start event

static std::vector<u8> shared_font;

static u32 cpu_percent; ///< CPU time available to the running application

void Initialize(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();
    u32 app_id = cmd_buff[1];
    u32 flags  = cmd_buff[2];

    cmd_buff[2] = 0x04000000; // According to 3dbrew, this value should be 0x04000000
    cmd_buff[3] = Kernel::g_handle_table.Create(notification_event).MoveFrom();
    cmd_buff[4] = Kernel::g_handle_table.Create(start_event).MoveFrom();

    // TODO(bunnei): Check if these events are cleared every time Initialize is called.
    notification_event->Clear();
    start_event->Clear();

    ASSERT_MSG((nullptr != lock), "Cannot initialize without lock");
    lock->Release();

    cmd_buff[1] = RESULT_SUCCESS.raw; // No error

    LOG_TRACE(Service_APT, "called app_id=0x%08X, flags=0x%08X", app_id, flags);
}

void GetSharedFont(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();

    if (!shared_font.empty()) {
        // TODO(bunnei): This function shouldn't copy the shared font every time it's called.
        // Instead, it should probably map the shared font as RO memory. We don't currently have
        // an easy way to do this, but the copy should be sufficient for now.
        memcpy(Memory::GetPointer(SHARED_FONT_VADDR), shared_font.data(), shared_font.size());

        cmd_buff[0] = 0x00440082;
        cmd_buff[1] = RESULT_SUCCESS.raw; // No error
        cmd_buff[2] = SHARED_FONT_VADDR;
        cmd_buff[4] = Kernel::g_handle_table.Create(shared_font_mem).MoveFrom();
    } else {
        cmd_buff[1] = -1; // Generic error (not really possible to verify this on hardware)
        LOG_ERROR(Kernel_SVC, "called, but %s has not been loaded!", SHARED_FONT);
    }
}

void NotifyToWait(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();
    u32 app_id = cmd_buff[1];
    // TODO(Subv): Verify this, it seems to get SWKBD and Home Menu further.
    start_event->Signal();

    cmd_buff[1] = RESULT_SUCCESS.raw; // No error
    LOG_WARNING(Service_APT, "(STUBBED) app_id=%u", app_id);
}

void GetLockHandle(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();
    u32 flags = cmd_buff[1]; // TODO(bunnei): Figure out the purpose of the flag field

    cmd_buff[1] = RESULT_SUCCESS.raw; // No error

    // Not sure what these parameters are used for, but retail apps check that they are 0 after
    // GetLockHandle has been called.
    cmd_buff[2] = 0;
    cmd_buff[3] = 0;
    cmd_buff[4] = 0;

    cmd_buff[5] = Kernel::g_handle_table.Create(lock).MoveFrom();
    LOG_TRACE(Service_APT, "called handle=0x%08X", cmd_buff[5]);
}

void Enable(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();
    u32 unk = cmd_buff[1]; // TODO(bunnei): What is this field used for?
    cmd_buff[1] = RESULT_SUCCESS.raw; // No error
    LOG_WARNING(Service_APT, "(STUBBED) called unk=0x%08X", unk);
}

void GetAppletManInfo(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();
    u32 unk = cmd_buff[1];
    cmd_buff[1] = RESULT_SUCCESS.raw; // No error
    cmd_buff[2] = 0;
    cmd_buff[3] = 0;
    cmd_buff[4] = static_cast<u32>(AppID::HomeMenu); // Home menu AppID
    cmd_buff[5] = static_cast<u32>(AppID::Application); // TODO(purpasmart96): Do this correctly

    LOG_WARNING(Service_APT, "(STUBBED) called unk=0x%08X", unk);
}

void IsRegistered(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();
    u32 app_id = cmd_buff[1];
    cmd_buff[1] = RESULT_SUCCESS.raw; // No error
    cmd_buff[2] = 1; // Set to registered
    LOG_WARNING(Service_APT, "(STUBBED) called app_id=0x%08X", app_id);
}

void InquireNotification(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();
    u32 app_id = cmd_buff[1];
    cmd_buff[1] = RESULT_SUCCESS.raw; // No error
    cmd_buff[2] = static_cast<u32>(SignalType::None); // Signal type
    LOG_WARNING(Service_APT, "(STUBBED) called app_id=0x%08X", app_id);
}

void SendParameter(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();
    u32 src_app_id          = cmd_buff[1];
    u32 dst_app_id          = cmd_buff[2];
    u32 signal_type         = cmd_buff[3];
    u32 buffer_size         = cmd_buff[4];
    u32 value               = cmd_buff[5];
    u32 handle              = cmd_buff[6];
    u32 size                = cmd_buff[7];
    u32 in_param_buffer_ptr = cmd_buff[8];
    
    cmd_buff[1] = RESULT_SUCCESS.raw; // No error

    LOG_WARNING(Service_APT, "(STUBBED) called src_app_id=0x%08X, dst_app_id=0x%08X, signal_type=0x%08X,"
               "buffer_size=0x%08X, value=0x%08X, handle=0x%08X, size=0x%08X, in_param_buffer_ptr=0x%08X",
               src_app_id, dst_app_id, signal_type, buffer_size, value, handle, size, in_param_buffer_ptr);
}

void ReceiveParameter(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();
    u32 app_id = cmd_buff[1];
    u32 buffer_size = cmd_buff[2];
    cmd_buff[1] = RESULT_SUCCESS.raw; // No error
    cmd_buff[2] = 0;
    cmd_buff[3] = static_cast<u32>(SignalType::AppJustStarted); // Signal type
    cmd_buff[4] = 0x10; // Parameter buffer size (16)
    cmd_buff[5] = 0;
    cmd_buff[6] = 0;
    cmd_buff[7] = 0;
    LOG_WARNING(Service_APT, "(STUBBED) called app_id=0x%08X, buffer_size=0x%08X", app_id, buffer_size);
}

void GlanceParameter(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();
    u32 app_id = cmd_buff[1];
    u32 buffer_size = cmd_buff[2];

    cmd_buff[1] = RESULT_SUCCESS.raw; // No error
    cmd_buff[2] = 0;
    cmd_buff[3] = static_cast<u32>(SignalType::AppJustStarted); // Signal type
    cmd_buff[4] = 0x10; // Parameter buffer size (16)
    cmd_buff[5] = 0;
    cmd_buff[6] = 0;
    cmd_buff[7] = 0;

    LOG_WARNING(Service_APT, "(STUBBED) called app_id=0x%08X, buffer_size=0x%08X", app_id, buffer_size);
}

void CancelParameter(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();
    u32 flag1  = cmd_buff[1];
    u32 unk    = cmd_buff[2];
    u32 flag2  = cmd_buff[3];
    u32 app_id = cmd_buff[4];

    cmd_buff[1] = RESULT_SUCCESS.raw; // No error
    cmd_buff[2] = 1; // Set to Success

    LOG_WARNING(Service_APT, "(STUBBED) called flag1=0x%08X, unk=0x%08X, flag2=0x%08X, app_id=0x%08X",
                flag1, unk, flag2, app_id);
}

void PrepareToStartApplication(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();
    u32 title_info1  = cmd_buff[1];
    u32 title_info2  = cmd_buff[2];
    u32 title_info3  = cmd_buff[3];
    u32 title_info4  = cmd_buff[4];
    u32 flags        = cmd_buff[5];

    cmd_buff[1] = RESULT_SUCCESS.raw; // No error

    LOG_WARNING(Service_APT, "(STUBBED) called title_info1=0x%08X, title_info2=0x%08X, title_info3=0x%08X,"
               "title_info4=0x%08X, flags=0x%08X", title_info1, title_info2, title_info3, title_info4, flags);
}

void StartApplication(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();
    u32 buffer1_size = cmd_buff[1];
    u32 buffer2_size = cmd_buff[2];
    u32 flag         = cmd_buff[3];
    u32 size1        = cmd_buff[4];
    u32 buffer1_ptr  = cmd_buff[5];
    u32 size2        = cmd_buff[6];
    u32 buffer2_ptr  = cmd_buff[7];

    cmd_buff[1] = RESULT_SUCCESS.raw; // No error

    LOG_WARNING(Service_APT, "(STUBBED) called buffer1_size=0x%08X, buffer2_size=0x%08X, flag=0x%08X,"
               "size1=0x%08X, buffer1_ptr=0x%08X, size2=0x%08X, buffer2_ptr=0x%08X",
               buffer1_size, buffer2_size, flag, size1, buffer1_ptr, size2, buffer2_ptr);
}

void AppletUtility(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();

    // These are from 3dbrew - I'm not really sure what they're used for.
    u32 unk = cmd_buff[1];
    u32 buffer1_size = cmd_buff[2];
    u32 buffer2_size = cmd_buff[3];
    u32 buffer1_addr = cmd_buff[5];
    u32 buffer2_addr = cmd_buff[65];

    cmd_buff[1] = RESULT_SUCCESS.raw; // No error

    LOG_WARNING(Service_APT, "(STUBBED) called unk=0x%08X, buffer1_size=0x%08X, buffer2_size=0x%08X, "
             "buffer1_addr=0x%08X, buffer2_addr=0x%08X", unk, buffer1_size, buffer2_size,
             buffer1_addr, buffer2_addr);
}

void SetAppCpuTimeLimit(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();
    u32 value   = cmd_buff[1];
    cpu_percent = cmd_buff[2];

    if (value != 1) {
        LOG_ERROR(Service_APT, "This value should be one, but is actually %u!", value);
    }

    cmd_buff[1] = RESULT_SUCCESS.raw; // No error

    LOG_WARNING(Service_APT, "(STUBBED) called cpu_percent=%u, value=%u", cpu_percent, value);
}

void GetAppCpuTimeLimit(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();
    u32 value = cmd_buff[1];

    ASSERT(cpu_percent != 0);

    if (value != 1) {
        LOG_ERROR(Service_APT, "This value should be one, but is actually %u!", value);
    }

    cmd_buff[1] = RESULT_SUCCESS.raw; // No error
    cmd_buff[2] = cpu_percent;

    LOG_WARNING(Service_APT, "(STUBBED) called value=%u", value);
}

void Init() {
    AddService(new APT_A_Interface);
    AddService(new APT_S_Interface);
    AddService(new APT_U_Interface);
    
    // Load the shared system font (if available).
    // The expected format is a decrypted, uncompressed BCFNT file with the 0x80 byte header
    // generated by the APT:U service. The best way to get is by dumping it from RAM. We've provided
    // a homebrew app to do this: https://github.com/citra-emu/3dsutils. Put the resulting file
    // "shared_font.bin" in the Citra "sysdata" directory.

    shared_font.clear();
    std::string filepath = FileUtil::GetUserPath(D_SYSDATA_IDX) + SHARED_FONT;

    FileUtil::CreateFullPath(filepath); // Create path if not already created
    FileUtil::IOFile file(filepath, "rb");

    if (file.IsOpen()) {
        // Read shared font data
        shared_font.resize((size_t)file.GetSize());
        file.ReadBytes(shared_font.data(), (size_t)file.GetSize());

        // Create shared font memory object
        using Kernel::MemoryPermission;
        shared_font_mem = Kernel::SharedMemory::Create(3 * 1024 * 1024, // 3MB
                MemoryPermission::ReadWrite, MemoryPermission::Read, "APT_U:shared_font_mem");
    } else {
        LOG_WARNING(Service_APT, "Unable to load shared font: %s", filepath.c_str());
        shared_font_mem = nullptr;
    }

    lock = Kernel::Mutex::Create(false, "APT_U:Lock");

    cpu_percent = 0;

    // TODO(bunnei): Check if these are created in Initialize or on APT process startup.
    notification_event = Kernel::Event::Create(RESETTYPE_ONESHOT, "APT_U:Notification");
    start_event = Kernel::Event::Create(RESETTYPE_ONESHOT, "APT_U:Start");
}

void Shutdown() {
    shared_font.clear();
    shared_font_mem = nullptr;
    lock = nullptr;
    notification_event = nullptr;
    start_event = nullptr;
}

} // namespace APT
} // namespace Service
