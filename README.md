# NaOS  

## Nano Operating system
A 64bit (x86-64) Simple Operating System Writing By C++.  (:sunglasses:)  
NaOS running at Intel / AMD modern processors.  

![build passing](https://img.shields.io/badge/build-passing-green) ![BSD-3-Clause](https://img.shields.io/badge/License-BSD--3--Clause-green) ![development on linux](https://img.shields.io/badge/build--platform-linux-lightgrey)  


## Features  
View [Features](./FEATURES.MD) .

## Quick Start  

### **Requirement**  
* A linux system
* **GNU Binutils** *(version 2.32 tested)*
* **GCC** or **Clang** supports *C++17* version  *(GCC 9.2.0 & Clang 9.0.0 tested on [Manjaro](https://manjaro.org/) 18.0.4)*
* **CMake 3.3** or later
* **Python 3** *(For running utility)*
* An emulator, virtual machine such as **Bochs**, **QEMU**, **Virtual Box** and **VMware Workstation** *(For running OS)*
* **Grub**, **gdisk** *(Often exist, for making runnable raw disk file)*
 
### **Recommend**  

* **clang-format**: Code formatter 
* **cpp-check**: Code static analyzer 
* [**VSCode**](https://code.visualstudio.com/): Development environment.

### 1. Download code
Clone this repo ```git clone https://url/path/to/repo``` 

### 2. Compile
```Bash
cd /path/to/repo

mkdir build
cd build
# CMAKE_BUILD_TYPE: Debug; Release
# USE_CLANG: OFF (Use GCC); ON (Use Clang)
cmake -DCMAKE_BUILD_TYPE=Debug -DUSE_CLANG=OFF ..
# Then make
make -j4
```

### 3. Make a raw disk
```Bash
cd /path/to/repo
# Make a raw disk file for running
python util/make_disk.py
```

### 4. Run
After ```make``` success, you will get these files
```
build
├── bin # Binary executable files without debug info
│   ├── loader
│   │   └── loader-multiboot
│   ├── rfsimage # root file system image files (root folder)
│   └── system
│       ├── rfsimg # root file system image
│       └── kernel # kernel file
└── debug # Binary executable files with debug info which can be used by debugger gdb/lldb ...
```

**make sure you mount disk by ``` sudo python util/mount_disk.py ``` before run emulator**. then
```Bash
# Run emulator
# qemu: python util/run.py q
# qemu headless: python util/run.py qw
# bochs: python util/run.py b
sudo python util/run.py q
```

The kernel log will be generated in *util/kernel_out.log*, just ```cat util/kernel_out.log```.

### 5. Debug
Use command ```python util/gen_debug_asm.py kernel``` to generate kernel disassembly if needed. (e.g. Debug in bochs)

Easily debugging with **VSCode** when running NaOS on QEMU. Just configure in *.vscode/launch.json*
```Json
"MIMode": "gdb", # your debugger: gdb or lldb
"miDebuggerServerAddress": "localhost:1234", # your host:port
```
, start QEMU and press F5.


## Repo Tree
```
NaOS
├── build # generate directory
├── naos
│   ├── includes
│   │   ├── kernel
│   │   └── loader
│   └── src
│       ├── kernel
│       │   ├── arch # arch x86_64 specification source code
│       │   ├── common # kernel data
│       │   ├── fs # file system 
│       │   ├── mm # memory subsystem
│       │   ├── module # module support code
│       │   ├── schedulers # time span scheduler and completely fair scheduler
│       │   ├── task # elf file loader and built-in task
│       │   └── util # util functions: memcpy, strcpy, cxxlib, formatter, containers
│       └── loader
│           ├── common # common loader libraries such as disk reader, ScreenPrinter
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

