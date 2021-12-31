# KVM-Nyx

KVM-Nyx is a fork of KVM and enables Intel-PT tracing for vCPUs and nested hypercalls which are required for hypervisor fuzzing. It enables fuzzing of any x86 / x86-64 target by using one of Nyx's fuzzer frontends. 
Our patches are based on kernel version 5.10.73. You can find more detailed information in our main repository.

Warning: Our patches are only supported on Intel CPUs (Skylake or later).

<p>
<img align="right" width="200"  src="logo.png">
</p>


## Setup 

There are 3 different ways to install and setup KVM-Nyx. The first approach is to install an Ubuntu pre-compiled mainline kernel and then install our prebuilt KVM-Nyx binaries. If you want to build the modules yourself, you install the Ubuntu kernel (including the headers) and then compile the KVM-Nyx modules. The last option is to compile the entire kernel including KVM-Nyx (this repository is a copy of a full kernel source tree).

We recommend the first approach, as it is by far the easiest to set up KVM-Nyx. 

## Setup (KVM-Nyx binaries)

First, install the Ubuntu kernel image and its modules (headers are not necessarily required):

```
cd /tmp/ &&
wget -c  https://kernel.ubuntu.com/~kernel-ppa/mainline/v5.10.73/amd64/linux-image-unsigned-5.10.73-051073-generic_5.10.73-051073.202110131013_amd64.deb &&
wget -c  https://kernel.ubuntu.com/~kernel-ppa/mainline/v5.10.73/amd64/linux-modules-5.10.73-051073-generic_5.10.73-051073.202110131013_amd64.deb &&
sudo dpkg -i *.deb
sudo reboot
```

Next, make sure you have booted the Ubuntu kernel (run `uname -a` and check that the kernel version is `5.10.73-051073-generic`). Download our KVM-Nyx module binaries (check the [release section](https://github.com/nyx-fuzz/KVM-Nyx/releases)), extract the modules and move into the output directory. Finally, run the following command to load the modules:

```
unzip kvm-nyx-5.10.73-1.0.zip
cd kvm-nyx-5.10.73-1.0
sudo rmmod kvm_intel kvm
sudo insmod ./kvm.ko;
sudo insmod ./kvm-intel.ko;
sudo chmod 777 /dev/kvm
```

At this point your system is now ready for fuzzing targets in Intel-PT mode. Please keep in mind that you need to re-execute the last commands (which load the modules) to use KVM-Nyx after reboot. To  install KVM-Nyx permanently, you can replace the KVM modules in the `/lib/modules/$(uname -r)` directory by the KVM-Nyx modules. 


## Setup (compile KVM-Nyx modules out-of-tree)

First, install the Ubuntu kernel image, its modules and all headers:

```
cd /tmp/ &&
wget -c  https://kernel.ubuntu.com/~kernel-ppa/mainline/v5.10.73/amd64/linux-headers-5.10.73-051073-generic_5.10.73-051073.202110131013_amd64.deb &&
wget -c  https://kernel.ubuntu.com/~kernel-ppa/mainline/v5.10.73/amd64/linux-headers-5.10.73-051073_5.10.73-051073.202110131013_all.deb &&
wget -c  https://kernel.ubuntu.com/~kernel-ppa/mainline/v5.10.73/amd64/linux-image-unsigned-5.10.73-051073-generic_5.10.73-051073.202110131013_amd64.deb &&
wget -c  https://kernel.ubuntu.com/~kernel-ppa/mainline/v5.10.73/amd64/linux-modules-5.10.73-051073-generic_5.10.73-051073.202110131013_amd64.deb &&
sudo dpkg -i *.deb
sudo reboot
```

Make sure you have booted the Ubuntu kernel (run `uname -a` and check that the kernel version is `5.10.73-051073-generic`). Next, check out this repository by running the following command:

```
git clone --depth 1 --branch kvm-nyx-5.10.73 git@github.com:nyx-fuzz/kvm-nyx.git
cd kvm-nyx
```

To compile KVM-Nyx (out-of-three) you can use the following scripts: 

```
sh compile_kvm_nyx_standalone.sh
```

To load the modules you can simply run the following command:

```
sh load_kvm_nyx.sh
```

Please keep in mind that you need to re-execute the last commands (which load the modules) to use KVM-Nyx after reboot. To  install KVM-Nyx permanently, you can replace the KVM modules in the `/lib/modules/$(uname -r)` directory by the KVM-Nyx modules. 

## Setup (compile entire kernel including KVM-Nyx modules)

Check out this repository by running the following command:

```
git clone --depth 1 --branch kvm-nyx-5.10.73 git@github.com:nyx-fuzz/kvm-nyx.git
cd kvm-nyx
```

Prepare a kernel config (e.g. by running `make oldconfig`) and add the following option to the config (`.config`) before compile the kernel and its modules: 

```
CONFIG_KVM_NYX=y
```

## Bug Reports and Contributions

If you found and fixed a bug on your own: We are very open to patches, please create a pull request!  

### License

Our patches are provided under **GPLv2 license**. 

**Free Software Hell Yeah!** 

Proudly provided by: 

* [Sergej Schumilo](http://schumilo.de) - sergej@schumilo.de / [@ms_s3c](https://twitter.com/ms_s3c)
* [Cornelius Aschermann](https://hexgolems.com) - cornelius@hexgolems.com / [@is_eqv](https://twitter.com/is_eqv)
