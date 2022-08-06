# NaOS  

## Nano Operating system
A 64bit (arch x86-64) Simple Operating System Writing By C++.  (:sunglasses:)  
NaOS runs on Intel / AMD modern processors.  

![BSD-3-Clause](https://img.shields.io/badge/License-BSD--3--Clause-green) ![development on linux](https://img.shields.io/badge/build--platform-linux-lightgrey)  


## Features  
View [Features](./FEATURES.MD) .

## Preview
![preview1](https://github.com/kadds/images/raw/master/naos/view1.apng)

## Quick Start  

### **Requirement**  
* **GNU Binutils** *(version 2.38 tested)*
* **GCC** or **Clang** supports *C++20* version  *(GCC 12.1.0 & Clang 13.0.1 tested on archlinux)*
* **CMake 3.3** or later
* **ninja** and **meson** is needed for build libc   
* **Python 3** *(For running utility)*
* An emulator or virtual machine such as **Bochs**, **QEMU**, **Virtual Box**, **hyper-v** and **VMware Workstation** *(For running OS)*
* **Grub2**, **fdisk / gdisk**, **udisks2** *(For making runnable raw disk file and mounting raw disk without root privilege)*

### **Optional**
* **OVMF** (UEFI firmware) for QEMU if boot from UEFI environment
  
### **Recommend**  
* **clang-format**: Code formatter 
* **cpp-check**: Code static analyzer 

### 1. Download source code
Clone this repo 
```bash
git clone https://url/path/to/repo
git submodule update --init
``` 

### 2. Compile
```bash
cd /path/to/repo

# build libc first
cd naos/libc/mlibc/

# gcc
cat > cross_file.txt << EOF
[host_machine]
system = 'naos'
cpu_family = 'x86_64'
cpu = 'i686'
endian = 'little'

[binaries]
c = '/usr/bin/x86_64-pc-linux-gnu-gcc' 
cpp = '/usr/bin/x86_64-pc-linux-gnu-g++'
ar = '/usr/bin/ar'
strip = '/usr/bin/strip'

[properties]
needs_exe_wrapper = true
EOF

# or clang
cat > cross_file.txt << EOF
[host_machine]
system = 'naos'
cpu_family = 'x86_64'
cpu = 'i686'
endian = 'little'

[binaries]
c = '/usr/bin/clang'
c_ld = '/usr/bin/lld'
cpp = '/usr/bin/clang++'
cpp_ld = '/usr/bin/lld'
ar = '/usr/bin/ar'
strip = '/usr/bin/strip'

[properties]
needs_exe_wrapper = true
EOF

meson setup build --cross-file cross_file.txt
cd build
ninja 

cd ../../../

# build kernel & nanobox binary
mkdir build
cd build
# CMAKE_BUILD_TYPE: Debug\Release
# 
# clang: cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=1 -DFREELIBCXX_TEST=OFF  -DCMAKE_CXX_COMPILER="/usr/bin/clang++" -DCMAKE_C_COMPILER="/usr/bin/clang" -DCMAKE_CXX_FLAGS="-fuse-ld=lld" -DCMAKE_C_FLAGS="-fuse-ld=lld" ..

# or gcc
cmake -DCMAKE_BUILD_TYPE=Debug -DUSE_CLANG=OFF ..
make -j
```

### 3. Make a raw disk

> NaOS requires to boot from multiboot2, which is GRUB supported. [multiboot2 (spec)](https://www.gnu.org/software/grub/manual/multiboot2/multiboot.html).   
#### A. ISO target
To get kernel iso image:
```sh
cur=`pwd`
cp ${cur}/build/bin/system/kernel ${cur}/run/iso/kernel
cp ${cur}/build/bin/system/rfsimg ${cur}/run/iso/rfsimg
grub-mkrescue -o ./run/image/naos.iso ./run/iso
```

#### B. Disk target
##### Option1. **Legacy mode**

Example of disk partition (MBR):    
| Partition number | Type  | (Gdisk) Code |  FS   |  Content  |  Size  |
| :--------------: | :---: | :----------: | :---: | :-------: | :----: |
|        1         | Grub  |     8300     | FAT32 | Grub data | 50MiB  |
|        2         | Root  |     8300     | FAT32 | NaOS data | 100MiB |

Installing command (MBR):
```bash
sudo grub-install --boot-directory=/mnt/Root/boot  --targe=i386-pc run/image/disk.img
```

*grub.cfg* (MBR):
```
root=(hd0,msdos1)
set default=0
set timeout=1
menuentry "NaOS multiboot2" {
    insmod part_msdos
    insmod fat
    multiboot2 /boot/kernel
    module2 /boot/rfsimg
    boot
}
```

##### Option2. **UEFI mode**
Installing [OVMF](https://sourceforge.net/projects/tianocore/) and configuring OVMF_CODE.fd path at *util/run.py*.  
  

Example of disk partition (UEFI):  
| Partition number | Type  | (Gdisk) Code |  FS   |      Content       |  Size  |
| :--------------: | :---: | :----------: | :---: | :----------------: | :----: |
|        1         |  ESP  |     EF00     | FAT32 |  Grub EFI loader   | 50MiB  |
|        2         | Root  |     8300     | FAT32 | Grub and NaOS data | 100MiB |


Installing command (UEFI):
```bash
sudo grub-install --boot-directory=/mnt/Root/boot  --efi-directory=/mnt/ESP --targe=x86_64-efi run/image/disk.img
```

*grub.cfg* (UEFI):
```
root=(hd0,gpt2)
set default=0
set timeout=1
menuentry "NaOS multiboot2" {
    insmod efi_gop
    insmod efi_uga
    insmod ieee1275_fb
    insmod part_gpt
    insmod part_msdos
    insmod fat
    insmod ext2
    multiboot2 /boot/kernel
    module2 /boot/rfsimg
    boot
}
```
---

Move the raw disk file to *run/image/disk.img*.   
  
Reference:  
> [Make a disk](https://wiki.archlinux.org/index.php/Fdisk)  
> [Install the grub](https://wiki.archlinux.org/index.php/GRUB). (archlinux wiki)  
> [OSDev](https://wiki.osdev.org/Bootable_Disk)  
### 4. Run
After ```make``` success, you will get these files
```
build
├── bin # Binary executable files without debug info
│   ├── rfsroot # root file system image files (the root folder when kernel running)
│   └── system
│       ├── rfsimg # root file system image
│       └── kernel # kernel binary file
└── debug # Binary executable files with debug info which can be used by debugger gdb/lldb ...
```

**make sure you mount disk by ``` python util/disk.py mount``` before run emulator**. then
```Bash
# Run emulator
python util/run.py q
```

The kernel log will be generated in *run/kernel_out.log*, just ```tail -f run/kernel_out.log```.

### 5. Debug
Use command ```python util/gen_debug_asm.py kernel``` to generate kernel disassembly if needed. (e.g. Debug in bochs)

Easily debugging with **VSCode** when running NaOS on QEMU. Just configure in *.vscode/launch.json*: 
```Json
"MIMode": "gdb", # your debugger: gdb or lldb
"miDebuggerServerAddress": "localhost:1234", # your host:port
```


## Repo Tree
```
NaOS
├── build # build target directory
├── naos
│   ├── includes
│   │   └── kernel
│   └── src
│       ├── kernel
│       │   ├── arch # arch x86_64 specification source code
│       │   ├── common # kernel data
│       │   ├── dev # driver interface & drivers
│       │   ├── fs # file subsystem 
│       │   ├── io # io subsystem
│       │   ├── mm # memory subsystem
│       │   ├── module # module support code
│       │   ├── schedulers # round robin scheduler and completely fair scheduler
│       │   ├── syscall # syscall entries
│       │   ├── task # ELF loader and built-in task
│       │   └── util # util functions: memcpy, strcpy, cxxlib, formatter, containers
│       └── usr
│           ├── init # the userland init program 
│           └── bin # nanobox program, like busybox
├── run
│   ├── cfg # include emulator config file: bochsrc
│   └── image
│       └── disk.img # raw disk image to startup OS
└── util # Python tools
```

## Maintainers 
[@Kadds](https://github.com/Kadds).

## Reference 
* [OSDev](https://forum.osdev.org/)
* [Linux](https://www.kernel.org/)
* [Intel SDM](https://software.intel.com/en-us/articles/intel-sdm)
* [Minix3](http://www.minix3.org/)
* [VX6](https://github.com/mit-pdos/xv6-public)

## License
[BSD-3-Clause](./LICENSE) © Kadds

----

```
          _____                    _____                   _______                   _____          
         /\    \                  /\    \                 /::\    \                 /\    \         
        /::\____\                /::\    \               /::::\    \               /::\    \        
       /::::|   |               /::::\    \             /::::::\    \             /::::\    \       
      /:::::|   |              /::::::\    \           /::::::::\    \           /::::::\    \      
     /::::::|   |             /:::/\:::\    \         /:::/~~\:::\    \         /:::/\:::\    \     
    /:::/|::|   |            /:::/__\:::\    \       /:::/    \:::\    \       /:::/__\:::\    \    
   /:::/ |::|   |           /::::\   \:::\    \     /:::/    / \:::\    \      \:::\   \:::\    \   
  /:::/  |::|   | _____    /::::::\   \:::\    \   /:::/____/   \:::\____\   ___\:::\   \:::\    \  
 /:::/   |::|   |/\    \  /:::/\:::\   \:::\    \ |:::|    |     |:::|    | /\   \:::\   \:::\    \ 
/:: /    |::|   /::\____\/:::/  \:::\   \:::\____\|:::|____|     |:::|    |/::\   \:::\   \:::\____\
\::/    /|::|  /:::/    /\::/    \:::\  /:::/    / \:::\    \   /:::/    / \:::\   \:::\   \::/    /
 \/____/ |::| /:::/    /  \/____/ \:::\/:::/    /   \:::\    \ /:::/    /   \:::\   \:::\   \/____/ 
         |::|/:::/    /            \::::::/    /     \:::\    /:::/    /     \:::\   \:::\    \     
         |::::::/    /              \::::/    /       \:::\__/:::/    /       \:::\   \:::\____\    
         |:::::/    /               /:::/    /         \::::::::/    /         \:::\  /:::/    /    
         |::::/    /               /:::/    /           \::::::/    /           \:::\/:::/    /     
         /:::/    /               /:::/    /             \::::/    /             \::::::/    /      
        /:::/    /               /:::/    /               \::/____/               \::::/    /       
        \::/    /                \::/    /                 ~~                      \::/    /        
         \/____/                  \/____/                                           \/____/         
                                                                                                    
```