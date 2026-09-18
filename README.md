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
* **GNU EFI lib** pacman -S gnu-efi
* **GCC** or **Clang** *(GCC 12.1.1 tested)*
* **CMake 3.3+**
* **ninja** and **meson** are required to build libc   
* **Python 3** *(For running utilities and the NaoIDL compiler)*
* **Lark 1.3.1** (`python3 -m pip install -r idl/requirements.txt`)
* An emulator or virtual machine such as **Bochs**, **QEMU**, **Virtual Box**, **hyper-v** and **VMware Workstation** *(For running OS)*
* **Grub2**, **fdisk / gdisk**, **qemu-img**, **fstool** *(For creating and editing raw disk images without host mount privileges)*

### **Optional**
* **OVMF** (UEFI firmware) for QEMU if boot from UEFI environment
  
### 1. Download source code
Clone this repo 
```bash
git clone https://url/path/to/repo
git submodule update --init --recursive
python3 -m pip install -r idl/requirements.txt
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

meson setup build --cross-file cross_file.txt -Ddefault_library=static
cd build && ninja

cd /path/to/repo
# build kernel and userland binaries (BusyBox + legacy nanobox)
mkdir build-debug
cd build-debug
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
meson setup build --cross-file cross_file.txt -Ddefault_library=static
cd build && ninja 

cd /path/to/repo
# build kernel and userland binaries (BusyBox + legacy nanobox)
mkdir build-release
cd build-release

# CMAKE_BUILD_TYPE: Debug\Release
cmake -DCMAKE_BUILD_TYPE=Release -DFREELIBCXX_TEST=OFF  -DCMAKE_CXX_COMPILER="/usr/bin/clang++" -DCMAKE_C_COMPILER="/usr/bin/clang" -DCMAKE_CXX_FLAGS="-fuse-ld=lld" -DCMAKE_C_FLAGS="-fuse-ld=lld" ..

make -j
```

### Linux host service smoke (no QEMU)

The Linux `std` versions of `ramdiskd`, `exfatd`, and `vfsd` can be tested as
three independent processes over the fixed local service directory and UDS:

```bash
python3 util/host_services.py --cargo cargo
```

The runner uses the in-memory `ramdiskd` backend and validates the generated
IDL round trip, URI prefix discovery, block lease/resource checks, bulk I/O,
and the `/data` filesystem path. It does not alter the NaOS boot image.

### 3. Creating artifact

> NaOS requires multiboot2 loader, which is supported by GRUB. [multiboot2 (spec)](https://www.gnu.org/software/grub/manual/multiboot2/multiboot.html).   
#### A. ISO image target
To get kernel iso image:
```sh
python3 util/run.py --build-dir build-debug q --iso -n --no-reboot
```

#### B. Disk target
```bash 
python3 util/disk.py --build-dir build-debug create
```

For WSL or any environment without loop/mount privileges, use the rootless
image path instead of the historical host-mounted flow below:

```bash
python3 util/disk.py --build-dir build-debug create
python3 util/disk.py --build-dir build-debug mkfat       # or mkexfat
python3 util/disk.py --build-dir build-debug add host-file.txt /guest-file.txt
python3 util/disk.py --build-dir build-debug cat /guest-file.txt
```

This path uses `fstool` directly on `build-debug/image/disk.img:1`; it never creates a
loop device. Install it with
`cargo install --git https://github.com/KarpelesLab/fstool --locked --force fstool`.

The current `util/disk.py` uses only regular-file I/O through `fstool`; it does
not require a host mount service. The GRUB install examples below are
historical native-Linux references and are separate from the supported
rootless image workflow.

##### Option1. **Legacy mode**
Make partitions by `gdisk ./build-debug/image/disk.img`.
Example of disk partition (MBR):    
| Partition number | Type  | (Gdisk) Code |  FS   |  Content  |  Size  |
| :--------------: | :---: | :----------: | :---: | :-------: | :----: |
|        1         | Grub  |     8300     | FAT32 | Grub data | 70MiB  |
|        2         | Root  |     8300     | FAT32 | NaOS data | 70MiB |

The partitioned GRUB layout above is a historical native-Linux workflow. It
requires a separate host mount and privileged `grub-install`; it is not used
by the rootless `fstool` example above.

Install (MBR):
```bash
sudo grub-install --boot-directory=/run/media/user/xxx --targe=i386-pc build-debug/image/disk.img
```

vim *grub.cfg* (MBR):
```
root=(hd0,msdos1)
set default=0
set timeout=1
menuentry "NaOS multiboot2" {
    insmod all_video
    insmod part_msdos
    insmod fat
    multiboot2 /boot/kernel
    module2 /boot/vfsd vfsd
    module2 /boot/ramdiskd blockd
    module2 /boot/rootfsd rootfsd
    module2 /boot/init init
    module2 /boot/root.img rootimage
    boot
}
```

##### Option2. **UEFI mode**
Install [OVMF](https://sourceforge.net/projects/tianocore/) and configure `OVMF_CODE.fd` in *util/run.py*.  

```bash 
python3 util/disk.py --build-dir build-debug create
```

Make partitions by `gdisk ./build-debug/image/disk.img`.
Example of disk partitions (UEFI):  
| Partition number | Type  | (Gdisk) Code |  FS   |      Content       |  Size  |
| :--------------: | :---: | :----------: | :---: | :----------------: | :----: |
|        1         |  ESP  |     EF00     | FAT32 |  Grub EFI loader   | 70MiB  |
|        2         | Root  |     8300     | FAT32 | Grub and NaOS data | 70MiB |


The UEFI partitioned layout is likewise a historical native-Linux workflow;
the rootless WSL path edits p1 directly with `fstool` and does not install
GRUB into a host-mounted directory.

Install (UEFI):
```bash
sudo grub-install --boot-directory=/run/media/user/root_xxx  --efi-directory=/run/media/user/esp_xxx  --targe=x86_64-efi build-debug/image/disk.img
```

vim */run/media/user/root_xxx/grub/grub.cfg* (UEFI):
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
    module2 /boot/vfsd vfsd
    module2 /boot/ramdiskd blockd
    module2 /boot/rootfsd rootfsd
    module2 /boot/init init
    module2 /boot/root.img rootimage
    boot
}
```

---

The raw disk file is stored in *<build-dir>/image/disk.img*.
  
References:  
> [Make a disk](https://wiki.archlinux.org/index.php/Fdisk)  
> [Install the grub](https://wiki.archlinux.org/index.php/GRUB). (archlinux wiki)  
> [OSDev](https://wiki.osdev.org/Bootable_Disk)  

### 4. Run
After ```make``` success, the following files will be generated
```
build-debug
├── bin # Binary executable files without debug info
│   ├── rfsroot # root file system image files (the root folder when kernel loading)
│   └── system
│       ├── root.img # prepared FAT16 root image (4 KiB clusters)
│       └── kernel # kernel binary file
└── debug # Binary executable files with debug info which can be used by debugger like gdb/lldb ...
```

The disk emulator flow refreshes `/boot` directly inside the selected build-local image
with `fstool`; no host mount or loop device is used. `run.py q` also installs a
rootless BIOS GRUB layout into the build-local image and creates a missing image
as an exFAT disk. For the normal ISO flow use `--iso`:
```Bash
# Run emulator
python3 util/run.py --build-dir build-debug q --iso
# or uefi
python3 util/run.py --build-dir build-debug q --iso --uefi
```

The kernel log will be generated in *build-debug/kernel_out.log*.

```Bash
tail -f build-debug/kernel_out.log
```

### 5. Debug
The command ```python util/gen_debug_asm.py kernel``` will generate kernel disassembly if needed. (e.g. Debug in bochs)

## Repo Tree
```
NaOS
├── build-debug # build target directory; use another build-* for parallel validation
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
│           ├── busybox # BusyBox 1.37.0 source and independent build input
│           └── bin # legacy nanobox program
├── run
│   ├── fakeroot # the files in fake root path will be overwritten to the real root path
│   └── cfg # emulator configuration files
├── build-debug
│   ├── image # generated ISO and disk images
│   ├── iso # per-build ISO staging directory
│   └── kernel_out.log # per-build QEMU serial output
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
