# NaOS  

## Nano Operating system
A 64bit (arch x86-64) simple operating system writing by c++.  (:sunglasses:)  
NaOS runs on Intel / AMD modern processors.  

![BSD-3-Clause](https://img.shields.io/badge/License-BSD--3--Clause-green) ![development on linux](https://img.shields.io/badge/build--platform-linux-lightgrey)  


## Features  
View [Features](./FEATURES.MD) .

## Preview
![preview1](https://github.com/kadds/images/raw/master/naos/view1.apng)

## Quick Start  

### **Requirements**  
* **GNU Binutils** *(version 2.38 tested)*
* **GCC** or **Clang** *(GCC 12.1.1 tested)*
* **CMake 3.3+**
* **ninja** and **meson** are required to build libc   
* **Python 3** *(For running utility)*
* An emulator or virtual machine such as **Bochs**, **QEMU**, **Virtual Box**, **hyper-v** and **VMware Workstation** *(For running OS)*
* **Grub2**, **fdisk / gdisk**, **udisks2** *(For making runnable raw disk file and mounting raw disk without root privilege)*

### **Optional**
* **OVMF** (UEFI firmware) for QEMU if boot from UEFI environment
  
### 1. Download source code
Clone this repo 
```bash
git clone https://url/path/to/repo
git submodule update --init
``` 

### 2. Compile 
* GCC
```bash
cd /path/to/repo

# build libc first
cd naos/libc/mlibc/
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

meson setup build --cross-file cross_file.txt
cd build && ninja

cd /path/to/repo
# build kernel & nanobox binary
mkdir build
cd build
# CMAKE_BUILD_TYPE: Debug\Release
cmake -DCMAKE_BUILD_TYPE=Debug -DFREELIBCXX_TEST=OFF ..
make -j
```

* Or Clang/LLVM 
(Not tested yet)
```bash
cd /path/to/repo

# build libc first
cd naos/libc/mlibc/
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
cd build && ninja 

cd /path/to/repo
# build kernel & nanobox binary
mkdir build
cd build

# CMAKE_BUILD_TYPE: Debug\Release
cmake -DCMAKE_BUILD_TYPE=Release -DFREELIBCXX_TEST=OFF  -DCMAKE_CXX_COMPILER="/usr/bin/clang++" -DCMAKE_C_COMPILER="/usr/bin/clang" -DCMAKE_CXX_FLAGS="-fuse-ld=lld" -DCMAKE_C_FLAGS="-fuse-ld=lld" ..

make -j
```

### 3. Creating artifact

> NaOS requires multiboot2 loader, which is supported by GRUB. [multiboot2 (spec)](https://www.gnu.org/software/grub/manual/multiboot2/multiboot.html).   
#### A. ISO image target
To get kernel iso image:
```sh
cur=`pwd`
cp ${cur}/build/bin/system/kernel ${cur}/run/iso/kernel
cp ${cur}/build/bin/system/rfsimg ${cur}/run/iso/rfsimg
grub-mkrescue -o ./run/image/naos.iso ./run/iso
```

#### B. Disk target
##### Option1. **Legacy mode**
Make partitions by `gdisk`.   
Example of disk partition (MBR):    
| Partition number | Type  | (Gdisk) Code |  FS   |  Content  |  Size  |
| :--------------: | :---: | :----------: | :---: | :-------: | :----: |
|        1         | Grub  |     8300     | FAT32 | Grub data | 50MiB  |
|        2         | Root  |     8300     | FAT32 | NaOS data | 100MiB |

Install (MBR):
```bash
sudo grub-install --boot-directory=/mnt/Root/boot  --targe=i386-pc run/image/disk.img
```

*grub.cfg* (MBR):
```
root=(hd0,msdos1)
set default=0
set timeout=1
menuentry "NaOS multiboot2" {
    insmod all_video
    insmod part_msdos
    insmod fat
    multiboot2 /boot/kernel
    module2 /boot/rfsimg
    boot
}
```

##### Option2. **UEFI mode**
Install [OVMF](https://sourceforge.net/projects/tianocore/) and configure `OVMF_CODE.fd` in *util/run.py*.  
Make partitions by `gdisk`.   
Example of disk partitions (UEFI):  
| Partition number | Type  | (Gdisk) Code |  FS   |      Content       |  Size  |
| :--------------: | :---: | :----------: | :---: | :----------------: | :----: |
|        1         |  ESP  |     EF00     | FAT32 |  Grub EFI loader   | 50MiB  |
|        2         | Root  |     8300     | FAT32 | Grub and NaOS data | 100MiB |


Install (UEFI):
```bash
sudo grub-install --boot-directory=/mnt/Root/boot  --efi-directory=/mnt/ESP --targe=x86_64-efi run/image/disk.img
```

*grub.cfg* (UEFI):
```
root=(hd0,gpt2)
set default=0
set timeout=1
menuentry "NaOS multiboot2" {
    insmod all_video
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

The raw disk file should be in the path *run/image/disk.img*.   
  
References:  
> [Make a disk](https://wiki.archlinux.org/index.php/Fdisk)  
> [Install the grub](https://wiki.archlinux.org/index.php/GRUB). (archlinux wiki)  
> [OSDev](https://wiki.osdev.org/Bootable_Disk)  

### 4. Run
After ```make``` success, the following files will be generated
```
build
├── bin # Binary executable files without debug info
│   ├── rfsroot # root file system image files (the root folder when kernel loading)
│   └── system
│       ├── rfsimg # root file system image
│       └── kernel # kernel binary file
└── debug # Binary executable files with debug info which can be used by debugger like gdb/lldb ...
```

**make sure to mount disk by ``` python util/disk.py mount``` before running the emulator**. then
```Bash
# Run emulator
python util/run.py q
```

The kernel log will be generated in *run/kernel_out.log*, 

```Bash
tail -f run/kernel_out.log
```

### 5. Debug
The command ```python util/gen_debug_asm.py kernel``` will generate kernel disassembly if needed. (e.g. Debug in bochs)

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
│   ├── fakeroot # the files in fake root path will be overwritten to the real root path
│   ├── cfg # include emulator config file: bochsrc
│   └── image
│       └── disk.img # raw disk image to startup OS
└── util # Python tools
```

## References 
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