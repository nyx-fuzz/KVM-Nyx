sudo rmmod kvm_intel kvm
sudo insmod arch/x86/kvm/kvm.ko;
sudo insmod arch/x86/kvm/kvm-intel.ko;
sudo chmod 777 /dev/kvm
