
TOOLCHAIN_PATH=$(realpath ../arm-eabi-4.7/bin)
DEFCONFIG=uz801_defconfig
MODVERS=modvers/Module-uz801-v3.2.symvers

if ! echo "$PATH" | grep -q -E "(^|:)$TOOLCHAIN_PATH(:|$)"; then
    export PATH="$TOOLCHAIN_PATH:$PATH"
fi

mkdir -p build
cp $MODVERS build/Module.symvers

make -j $(nproc) ARCH=arm CROSS_COMPILE=arm-eabi- O=build $DEFCONFIG
make -j $(nproc) ARCH=arm CROSS_COMPILE=arm-eabi- O=build zImage
make -j $(nproc) M=net ARCH=arm CROSS_COMPILE=arm-eabi- O=build modules

mkdir -p build1
cp -r build/arch/ build1
cp -r build/scripts build1
cp -r build/include build1
cp build/Module.symvers build1
ln -s $(realpath .) build1/source

make -j $(nproc) M=net ARCH=arm CROSS_COMPILE=arm-eabi- O=build1 modules
make -j $(nproc) M=net ARCH=arm CROSS_COMPILE=arm-eabi- O=build1 modules


rm -r system/lib/modules/*
mkdir -p system/lib/modules

find build1 -name "*.ko" -exec cp {} system/lib/modules/ \;
find system/lib/modules/ -name "*.ko" -exec arm-eabi-strip --strip-unneeded {} \;

