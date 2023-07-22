#include "common.hpp"
#include "kernel/arch/acpi/acpi.hpp"
#include "kernel/arch/idt.hpp"
#include "kernel/arch/io.hpp"
#include "kernel/arch/io_apic.hpp"
#include "kernel/handle.hpp"
#include "kernel/irq.hpp"
#include "kernel/kernel.hpp"
#include "kernel/kobject.hpp"
#include "kernel/lock.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/mm/slab.hpp"
#include "kernel/semaphore.hpp"
#include "kernel/task.hpp"
#include "kernel/trace.hpp"
#include "kernel/types.hpp"
#include <cstdarg>

ExportC
{
#include "acoutput.h"
#include "acpi.h"
#include "acpiosxf.h"
#include "acpixf.h"
#include "actypes.h"

    int vsnprintf(char *String, ACPI_SIZE Size, const char *Format, va_list Args);
}

struct ACPIInterInfo
{
    ACPI_OSD_HANDLER handler;
    void *context;
    uint32_t index;
};

irq::request_result _ctx_interrupt_ acpi_interrupt(const irq::interrupt_info *inter, u64 extra_data, u64 user_data)
{
    auto info = reinterpret_cast<ACPIInterInfo *>(user_data);
    auto ret = info->handler(info->context);
    if (ret == ACPI_INTERRUPT_NOT_HANDLED)
    {
        return irq::request_result::no_handled;
    }

    return irq::request_result::ok;
}

ExportC
{
    ACPI_STATUS AcpiOsInitialize()
    {
        trace::debug("ACPI impl init");
        return 0;
    }
    ACPI_STATUS AcpiOsTerminate()
    {
        trace::debug("ACPI impl uninit");
        return 0;
    }

    ACPI_PHYSICAL_ADDRESS AcpiOsGetRootPointer()
    {
        if (kernel_args->rsdp == 0)
        {
            return kernel_args->rsdp_old;
        }
        return kernel_args->rsdp;
    }

    ACPI_STATUS AcpiOsPredefinedOverride(const ACPI_PREDEFINED_NAMES *PredefinedObject, ACPI_STRING *NewValue)
    {
        *NewValue = NULL;
        return 0;
    }
    ACPI_STATUS AcpiOsTableOverride(ACPI_TABLE_HEADER * ExistingTable, ACPI_TABLE_HEADER * *NewTable)
    {
        *NewTable = NULL;
        return 0;
    }
    ACPI_STATUS
    AcpiOsPhysicalTableOverride(ACPI_TABLE_HEADER * ExistingTable, ACPI_PHYSICAL_ADDRESS * NewAddress,
                                UINT32 * NewTableLength)
    {
        *NewAddress = 0;
        return 0;
    }

    ACPI_STATUS
    AcpiOsCreateLock(ACPI_SPINLOCK * OutHandle)
    {
        auto spin = memory::KernelCommonAllocatorV->New<lock::spinlock_t>();
        *OutHandle = (void *)spin;
        return 0;
    }

    void AcpiOsDeleteLock(ACPI_SPINLOCK Handle)
    {
        auto spin = (lock::spinlock_t *)Handle;
        memory::KernelCommonAllocatorV->Delete(spin);
    }

    ACPI_CPU_FLAGS
    AcpiOsAcquireLock(ACPI_SPINLOCK Handle)
    {
        auto spin = (lock::spinlock_t *)Handle;
        bool IF = arch::idt::save_and_disable();
        spin->lock();
        return IF;
    }

    void AcpiOsReleaseLock(ACPI_SPINLOCK Handle, ACPI_CPU_FLAGS Flags)
    {
        auto spin = (lock::spinlock_t *)Handle;
        spin->unlock();
        if (Flags)
        {
            arch::idt::enable();
        }
    }

    ACPI_STATUS
    AcpiOsCreateSemaphore(UINT32 MaxUnits, UINT32 InitialUnits, ACPI_SEMAPHORE * OutHandle)
    {
        auto ptr = memory::KernelCommonAllocatorV->New<lock::semaphore_t>(MaxUnits);
        *OutHandle = (void *)ptr;
        return 0;
    }

    ACPI_STATUS
    AcpiOsDeleteSemaphore(ACPI_SEMAPHORE Handle)
    {
        auto semp = (lock::semaphore_t *)Handle;
        memory::KernelCommonAllocatorV->Delete(semp);
        return 0;
    }

    ACPI_STATUS
    AcpiOsWaitSemaphore(ACPI_SEMAPHORE Handle, UINT32 Units, UINT16 Timeout)
    {
        auto semp = (lock::semaphore_t *)Handle;
        semp->down(Units);
        return 0;
    }

    ACPI_STATUS
    AcpiOsSignalSemaphore(ACPI_SEMAPHORE Handle, UINT32 Units)
    {
        auto semp = (lock::semaphore_t *)Handle;
        semp->up(Units);
        return 0;
    }

    void *AcpiOsAllocate(ACPI_SIZE Size) { return memory::kmalloc(Size, 1); }
    // void *AcpiOsAllocateZeroed(ACPI_SIZE Size)
    // {
    //     auto ptr = memory::kmalloc(Size, 1);
    //     memset(ptr, 0, Size);
    //     return ptr;
    // }

    void AcpiOsFree(void *Memory) { memory::kfree(Memory); }

    void *AcpiOsMapMemory(ACPI_PHYSICAL_ADDRESS PhysicalAddress, ACPI_SIZE Length)
    {
        return memory::pa2va(phy_addr_t::from(PhysicalAddress));
    }

    void AcpiOsUnmapMemory(void *where, ACPI_SIZE length) {}

    ACPI_STATUS AcpiOsGetPhysicalAddress(void *LogicalAddress, ACPI_PHYSICAL_ADDRESS *PhysicalAddress)
    {
        auto ptr = memory::kernel_vm_info->paging().get_map(LogicalAddress);
        if (ptr.has_value())
        {
            *PhysicalAddress = (uintptr_t)(ptr.value()());
        }
        return 0;
    }

    // Memory/Object Cache
    ACPI_STATUS
    AcpiOsCreateCache(char *CacheName, UINT16 ObjectSize, UINT16 MaxDepth, ACPI_CACHE_T **ReturnCache)
    {
        auto group = memory::global_acpi_slab_domain->create_new_slab_group(
            ObjectSize, freelibcxx::string(memory::KernelCommonAllocatorV, CacheName), 1, 0);

        *ReturnCache = (void **)group;
        return 0;
    }

    ACPI_STATUS
    AcpiOsDeleteCache(ACPI_CACHE_T * Cache)
    {
        auto group = (memory::slab_group *)Cache;
        memory::global_acpi_slab_domain->remove_slab_group(group);
        return 0;
    }

    ACPI_STATUS
    AcpiOsPurgeCache(ACPI_CACHE_T * Cache) { return 0; }

    void *AcpiOsAcquireObject(ACPI_CACHE_T * Cache)
    {
        auto group = (memory::slab_group *)Cache;

        auto ptr = group->alloc();
        memset(ptr, 0, group->get_size());
        return ptr;
    }

    ACPI_STATUS
    AcpiOsReleaseObject(ACPI_CACHE_T * Cache, void *Object)
    {
        auto group = (memory::slab_group *)Cache;
        group->free(Object);
        return 0;
    }

    // Interrupt
    ACPI_STATUS
    AcpiOsInstallInterruptHandler(UINT32 InterruptNumber, ACPI_OSD_HANDLER ServiceRoutine, void *Context)
    {
        trace::debug("ACPI install gsi ", InterruptNumber);
        auto info = memory::KernelCommonAllocatorV->New<ACPIInterInfo>();
        info->handler = ServiceRoutine;
        info->context = Context;
        info->index = InterruptNumber;

        irq::register_request_func(InterruptNumber + 0x20, acpi_interrupt, reinterpret_cast<u64>(info));
        return 0;
    }

    ACPI_STATUS
    AcpiOsRemoveInterruptHandler(UINT32 InterruptNumber, ACPI_OSD_HANDLER ServiceRoutine)
    {
        auto info_address = irq::get_register_request_func(InterruptNumber, acpi_interrupt);
        if (info_address.has_value())
        {
            auto info = reinterpret_cast<ACPIInterInfo *>(info_address.value());
            memory::KernelCommonAllocatorV->Delete(info);

            irq::unregister_request_func(InterruptNumber + 0x20, acpi_interrupt);
            return 0;
        }
        return -1;
    }

    // thread
    ACPI_THREAD_ID
    AcpiOsGetThreadId(void)
    {
        auto thd = task::current();
        if (thd == nullptr)
        {
            return 0;
        }
        return thd->tid;
    }

    ACPI_STATUS
    AcpiOsExecute(ACPI_EXECUTE_TYPE Type, ACPI_OSD_EXEC_CALLBACK Function, void *Context) { return -1; }

    void AcpiOsWaitEventsComplete(void) { trace::info("wait events"); }
    void AcpiOsSleep(UINT64 Milliseconds) { trace::info("sleep ", Milliseconds); }
    void AcpiOsStall(UINT32 Microseconds) { trace::info("stall", Microseconds); }

    // platform interface

    ACPI_STATUS
    AcpiOsReadPort(ACPI_IO_ADDRESS Address, UINT32 * Value, UINT32 Width)
    {
        if (Width == 8)
        {
            auto val = io_in8(Address);
            *Value = val;
        }
        else if (Width == 16)
        {
            auto val = io_in16(Address);
            *Value = val;
        }
        else if (Width == 32)
        {
            auto val = io_in32(Address);
            *Value = val;
        }
        else
        {
            trace::panic("invalid Width ", Width);
        }
        return 0;
    }

    ACPI_STATUS
    AcpiOsWritePort(ACPI_IO_ADDRESS Address, UINT32 Value, UINT32 Width)
    {
        if (Width == 8)
        {
            io_out8(Address, Value);
        }
        else if (Width == 16)
        {
            io_out16(Address, Value);
        }
        else if (Width == 32)
        {
            io_out32(Address, Value);
        }
        else
        {
            trace::panic("invalid Width ", Width);
        }
        return 0;
    }

    // memory interface
    ACPI_STATUS
    AcpiOsReadMemory(ACPI_PHYSICAL_ADDRESS Address, UINT64 * Value, UINT32 Width) { return -1; }
    ACPI_STATUS
    AcpiOsWriteMemory(ACPI_PHYSICAL_ADDRESS Address, UINT64 Value, UINT32 Width) { return -1; }

    // platform pci
    ACPI_STATUS
    AcpiOsReadPciConfiguration(ACPI_PCI_ID * PciId, UINT32 Reg, UINT64 * Value, UINT32 Width) { return -1; }

    ACPI_STATUS
    AcpiOsWritePciConfiguration(ACPI_PCI_ID * PciId, UINT32 Reg, UINT64 Value, UINT32 Width) { return -1; }

    // miscellaneous

    BOOLEAN AcpiOsReadable(void *Memory, ACPI_SIZE Length) { return TRUE; }
    BOOLEAN AcpiOsWritable(void *Memory, ACPI_SIZE Length) { return TRUE; }
    UINT64
    AcpiOsGetTimer(void) { return 0; }

    ACPI_STATUS
    AcpiOsSignal(UINT32 Function, void *Info) { return -1; }

    ACPI_STATUS
    AcpiOsEnterSleep(UINT8 SleepState, UINT32 RegaValue, UINT32 RegbValue) { return -1; }

    // debug
    void ACPI_INTERNAL_VAR_XFACE AcpiOsPrintf(const char *Format, ...)
    {
        va_list st;
        va_start(st, Format);
        AcpiOsVprintf(Format, st);
    }

    void AcpiOsVprintf(const char *Format, va_list Args)
    {
        char *buf = (char *)memory::kmalloc(4096, 1);
        vsnprintf(buf, 4095, Format, Args);
        trace::print(buf);
        memory::kfree(buf);
    }

    void AcpiOsRedirectOutput(void *Destination) { trace::info("redirect to ", trace::hex(Destination)); }

    ACPI_STATUS
    AcpiOsGetTableByName(char *Signature, UINT32 Instance, ACPI_TABLE_HEADER **Table, ACPI_PHYSICAL_ADDRESS *Address)
    {
        return -1;
    }
    ACPI_STATUS
    AcpiOsGetTableByIndex(UINT32 Index, ACPI_TABLE_HEADER * *Table, UINT32 * Instance, ACPI_PHYSICAL_ADDRESS * Address)
    {
        return -1;
    }
    ACPI_STATUS
    AcpiOsGetTableByAddress(ACPI_PHYSICAL_ADDRESS Address, ACPI_TABLE_HEADER * *Table) { return -1; }
}
