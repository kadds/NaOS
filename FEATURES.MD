Features / TODO
====

* - [x] X86-64 Architecture
    - [x] Four-level Paging
    - [x] VGA display
    - [x] System call
    - [x] Context switch
    - [x] multiboot2 loader
* - [x] Memory subsystem
    - [x] Buddy frame allocator
    - [x] Slab cache pool
        - [ ] Cache line coloring
* - [ ] Process subsystem
    - [ ] Job/Group Control
    - [x] Scheduler
        - [x] Round Robin
        - [x] CFS
        - [x] Load balancing
    - [ ] Thread
        - [x] Schedule
        - [ ] Synchronize
            - [x] Spinlock
            - [x] Read/Write lock
            - [x] Mutex
            - [x] Semaphore
            - [ ] Condition wait
        - [x] Sleep
    - [ ] IPC
        - [ ] PIPE
        - [ ] Memory shared
        - [x] Signal
    - [x] Memory map
        - [x] Virtual address allocator
    - [ ] Elf loader
        - [X] Static binary loader
        - [ ] Dynamic link elf loader
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
    - [ ] Socket 
    - [ ] TCP/IP 
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
    - [ ] Kernel preempt
* - [ ] Extra
    - [ ] Kernel modules loader
    - [x] Kernel symbols query
    - [ ] User / Group
        - [ ] Root 
    - [ ] C++ lib integration  
    - [ ] Rust integration  
    - [ ] Graphics draw
    - [x] Kernel logger


