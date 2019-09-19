# NaOS  

## Nano Operating system
A 64bit (x86-64) Simple Operating System Writing By C++.  (:sunglasses:)  
NaOS targets Intel / AMD modern processors.  

![build passing](https://img.shields.io/badge/build-passing-green) ![BSD-3-Clause](https://img.shields.io/badge/License-BSD--3--Clause-green) ![development on linux](https://img.shields.io/badge/build--platform-linux-lightgrey)  


## Features  
* - [x] Four-level Paging
* - [x] VGA display
* - [x] Buddy and Slab system
* - [x] Task switch
* - [ ] Process scheduling
* - [ ] Thread support
* - [x] VFS
* - [x] API calling
* - [ ] APIC
* - [ ] Multicore support (SMP)
* - [ ] Kernel module
* - [ ] Disk driver
* - [ ] Native display driver
* - [ ] POSIX layer
* - [ ] ELF part support
  

## Quick Start  

### **Requirement**  
* A **\*nix** system
* **GNU Binutils** *(objcopy, objdump, version 2.32 tested)*
* **GCC** or **Clang** supports *C++17* version  *(GCC 9.1.0 & Clang 8.0.1 tested on [Manjaro](https://manjaro.org/) 18.0.4)*
* **CMake 3.3** or later
* **Python 3** *(For running utility)*
* An emulator, virtual machine such as **Bochs**, **QEMU**, **Virtual Box** and **VMware Workstation** *(For running OS)*
* **Grub**, **gdisk** *(Often exist, for making runnable raw disk file)*
 
Other recommended tools

* **clang-format**: Code formatter 
* **cpp-check**: Code static analyzer 
* [**VSCode**](https://code.visualstudio.com/): Development environment.

### 1. Download code
Clone this repo ```git clone https://url/path/to/repo``` 

### 2. Before the first compilation
Run these commands before the first compilation
```Bash
# Firstly going into NaOS root directory
cd /path/to/repo

# Create directories
python util/first_run.py
# Make a raw disk file for running
python util/make_disk.py
```
### 3. Compile
```Bash
# Firstly going into NaOS root directory
cd /path/to/repo

cd build
# CMAKE_BUILD_TYPE: Debug; Release
# USE_CLANG: OFF (Use GCC); ON (Use Clang)
cmake -DCMAKE_BUILD_TYPE=Debug -DUSE_CLANG=OFF ..
# Then make
make
```
### 4. Run
After make success, your will get these files
```
build
├── bin # Binary executable files without debug info
│   ├── loader
│   │   └── loader-multiboot
│   └── system
│       └── kernel # kernel file
└── debug # Binary executable files with debug info which can be used by debugger gdb/lldb
    ├── loader
    │   └── loader-multiboot.dbg
    └── system
        └── kernel.dbg
```

**make sure you mount disk by ``` sudo python util/mount_disk.py ``` before run emulator**. then
```Bash
# Run emulator
# qemu: python util/run.py q
# bochs: python util/run.py b
sudo python util/run.py q
```
### 5. Debug
Use command ```python util/gen_debug_asm.py kernel``` to generate kernel disassembly if needed. (e.g. Debug in Bochs)

Easily debugging with **VSCode** when running NaOS on QEMU. Just configure in *.vscode/launch.json*
```Json
"MIMode": "gdb", # your debugger: gdb or lldb
"miDebuggerServerAddress": "localhost:1234", # your host:port
```
, start QEMU and press F5.


## Repo Tree
```
NaOS
├── build # code generate directory
├── naos
│   ├── includes
│   │   ├── kernel
│   │   └── loader
│   └── src
│       ├── kernel
│       │   ├── arch # arch x86_64 specification source code
│       │   ├── common # kernel data
│       │   ├── mm # memory system
│       │   └── util # util functions: memcpy, strcpy, cxxlib, formatter
│       └── loader
│           ├── common # common loader liberies such as disk reader, ScreenPrinter
│           └── multiboot # loader that conforms to the multiboot spec.
├── run
│   ├── cfg # include emulator config file: bochsrc
│   └── image
│       ├── mnt
│       │   ├── boot # mount_disk.py mount first partition. grub partition
│       │   └── disk # mount_disk.py mount second partition. system partition
│       └── disk.img # make_disk.py created
└── util # Python tools
```

## Maintainers 
[@Kadds](https://github.com/Kadds).

## Reference 
* [OSDev](https://forum.osdev.org/)
* [Linux](https://www.kernel.org/)
* [Intel SDM](https://software.intel.com/en-us/articles/intel-sdm)
* [Minix3](http://www.minix3.org/)

## License
[BSD-3-Clause](./LICENSE) © Kadds

