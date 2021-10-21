yes | make oldconfig &&
make modules_prepare &&
cp /lib/modules/`uname -r`/build/scripts/module.lds scripts/ &&
cp /lib/modules/`uname -r`/build/Module.symvers . &&
cp /lib/modules/`uname -r`/build/include/config/kernel.release include/config/kernel.release &&
cp /lib/modules/`uname -r`/build/include/generated/utsrelease.h include/generated/utsrelease.h &&
make  M=arch/x86/kvm/ -j &&
echo "[!] kvm-nyx successfully compiled"
