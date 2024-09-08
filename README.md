# Building kernel modules for UZ801 4G modem stick  
  This is Linux kernel modules build system repository for UZ801 4G modem stick.
## Quick start  
  
1. First, clone the toolchain by running the following command in the parent directory of this repository:  
  
```bash  
git clone https://android.googlesource.com/platform/prebuilts/gcc/linux-x86/arm/arm-eabi-4.7 \
-b android-4.4.4_r2  
```  
  
2. Build modules  
```bash  
bash ./build.sh  
```  
  
Note: The build script might throw some errors (e.g., "selected processor does not support ARM mode"), just ignore them, the build process should restart and build modules successfully.
  
The built modules will be copied to system/lib/modules in the current directory.

## Support for other devices
To generate Module.modvers for your device, follow the manual https://glandium.org/blog/?p=2664
```bash
python extract-symvers.py -B 0xc0008000 kernel-filename > Module.symvers
```
Copy Module.symvers to modvers folder and adjust build.sh script as necessary.
