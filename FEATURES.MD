Features / TODO
====

* - [x] X86-64 Architecture
    - [x] Four-level Paging
    - [x] VGA display
    - [x] System call
    - [x] Context switch
    - [x] multiboot2 loader
    - [ ] ACPI
* - [x] Memory subsystem
    - [x] Buddy frame allocator
    - [x] Slab cache pool
        - [ ] Cache line coloring
    - [ ] Swap
* - [ ] Process subsystem
    - [ ] Job/Group Control
    - [x] Scheduler
        - [x] Round Robin scheduler (Realtime)
        - [x] Completely fair scheduler (Normal)
    - [ ] Thread
        - [x] Schedule
            - [x] CPU affinity schedule
        - [ ] Synchronize
            - [x] Spinlock
            - [x] Read/Write lock
            - [x] Mutex
            - [x] Semaphore
            - [ ] Condition wait
        - [x] Sleep
        - [ ] Thread local storage
    - [ ] IPC
        - [x] PIPE
        - [ ] FIFO
        - [ ] Memory shared
        - [ ] Message queue
        - [x] Signal
            - [ ] Mask
    - [x] Memory map
        - [x] Virtual address allocator
    - [ ] Elf loader
        - [X] Static binary loader
        - [ ] Dynamic link elf loader
    - [ ] Resource limit
* - [ ] File subsystem
    - [ ] Virtual File system
        - [x] Read, Write, Seek
        - [x] Hard link, Symbolic link
        - [ ] Time
        - [ ] Authority identification
    - [ ] NFS
    - [ ] Ext2
    - [ ] FAT
    - [ ] IO Model
        - [x] Blocked IO
        - [ ] IO Multiplexing
        - [ ] Asynchronous IO
* - [x] Interrupt subsystem
    - [x] Hard IRQ
    - [x] Soft IRQ
        - [x] Kernel softirq thread
        - [x] Tasklet
    - [x] APIC (Local/APIC, IO/APIC)
* - [ ] Network subsystem
    - [ ] Socket 
    - [ ] TCP/IP 
    - [ ] DNS
* - [ ] Drivers
    - [ ] ATA Disk driver
        - [ ] DMA
        - [ ] POL
    - [ ] Native display driver
    - [ ] Network interface card driver
    - [X] Mouse & Keyboard driver
* - [ ] Compatibility layer
    - [x] UEFI boot
    - [ ] POSIX API
* - [ ] Multi-core support (SMP)
    - [x] AP Startup
    - [x] Load balancing
    - [ ] Kernel preempt
* - [ ] Extra
    - [ ] Kernel modules loader
    - [x] Kernel symbols query
        - [x] Kernel stack trace
    - [ ] User/Group
    - [ ] C++ lib integration  
    - [ ] Rust integration  
    - [ ] Graphics draw
    - [x] Kernel logger


