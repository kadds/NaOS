Features / TODO
====

* - [ ] X86-64 Architecture
    - [x] Four-level Paging
    - [x] VGA display
    - [ ] Graphics draw
    - [x] System call
    - [x] Context switch
    - [x] multiboot2 loader
* - [x] Memory subsystem
    - [x] Buddy frame allocator
    - [x] Slab cache pool
    - [x] Virtual address allocator
* - [ ] Process subsystem
    - [ ] Scheduler
        - [x] Time span
        - [ ] CFS
        - [ ] Load balancing
    - [ ] Thread
        - [x] Schedule
        - [ ] Synchronize
            - [ ] Condition wait
            - [x] Sleep
            - [ ] Locks
    - [ ] IPC
    - [ ] Signal
    - [x] Memory map
        - [ ] Memory shared
    - [ ] Elf loader
        - [X] Static binary loader
        - [ ] Dynamic link
* - [ ] File subsystem
    - [ ] Virtual File system
        - [x] Read, Write, Seek
        - [x] Hard link, Symbolic link
        - [ ] Authority identification
    - [ ] NFS
    - [ ] Ext2
    - [ ] FAT
* - [x] Interrupt subsystem
    - [x] Hard IRQ
    - [x] Soft IRQ
        - [x] Kernel softirq thread
    - [x] APIC (Local/APIC, IO/APIC)
* - [ ] Network subsystem
    - [ ] socket
* - [ ] Drivers
    - [ ] ATA Disk driver
        - [ ] DMA
        - [ ] POL
    - [ ] Native display driver
    - [ ] Network interface card driver
    - [ ] Mouse & Keyboard driver
* - [ ] Compatibility layer
    - [ ] UEFI boot loader
    - [ ] POSIX API
* - [ ] Multi-core support (SMP)
    - [x] AP Startup
    - [ ] Kernel preempt
* - [ ] Extra
    - [ ] Kernel modules loader
    - [x] Kernel symbols query
    - [ ] User / Group
        - [ ] Root 
    - [ ] C++ lib integration  
    - [ ] Rust integration
