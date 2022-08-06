Features / TODO
====

* - [x] X86-64 Architecture
    - [x] Four-level Paging
        - [x] Copy on write
    - [x] VGA display
      - [x] Kernel Console
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
        - [x] Thread local storage
    - [ ] IPC
        - [x] PIPE
        - [x] FIFO
        - [ ] Memory shared
        - [x] Message queue
        - [x] Signal
    - [x] Memory map
        - [x] Virtual address allocator
        - [x] File-based mapping
        - [x] Anon-based mapping
        - [x] Copy on write
    - [ ] Elf loader
        - [X] Static binary loader
        - [ ] Dynamic link elf loader
    - [ ] Resource limit
        - [ ] Control-Group
* - [ ] File subsystem
    - [ ] Virtual File system
        - [x] Read, Write, Seek
        - [x] Hard link, Symbolic link
        - [ ] Time 
        - [ ] Authority identification
            - [x] Read, Write, Exec
    - [ ] NFS
    - [ ] Ext2
    - [ ] FAT
    - [ ] IO Model
        - [x] Blocked IO
        - [x] NoBlocked IO
        - [ ] IO Multiplexing
            - [ ] Select
            - [ ] Epoll
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
        - [x] fork/exec/waitpid
        - [ ] pthread
        - [x] mmap
        - [x] file
* - [ ] Multi-core support (SMP)
    - [x] AP Startup
    - [x] Load balancing
    - [ ] Kernel preempt
* - [ ] Extra
    - [ ] Kernel modules loader
    - [x] Kernel symbols query
        - [x] Kernel stack trace
    - [ ] libc integration
        - [x] mlibc
    - [ ] C++ lib integration  
        - [x] freelibcxx
    - [ ] Graphics user interface
        - [ ] Window manager
    - [x] Kernel logger
    - [ ] Basic commands
      - [ ] nanobox


