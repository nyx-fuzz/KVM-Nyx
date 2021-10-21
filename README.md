# KVM-Nyx

KVM-Nyx is a fork of KVM and enables Intel-PT tracing for vCPUs and nested hypercalls which are required for hypervisor fuzzing. KVM-Nyx enables fuzzing any x86 target by using one of Nyx's fuzzer frontends. 
Our patches are based on kernel version 5.10.73.

You can find more detailed information in our main repository.

<p>
<img align="right" width="200"  src="logo.png">
</p>

## Setup (Out-of-Tree)

You can compile and install KVM-Nyx by using one of Ubuntu`s pre-compiled mainline kernels. To do so, you need to install the following packages (Ubuntu 21-04 only):

```
sh install_kernel_5.10.73.sh
```

To compile and load KVM-Nyx you can use the following scripts: 

```
sh compile_kvm_nyx_standalone.sh
sh load_kvm_nyx.sh
```

## Bug Reports and Contributions

If you found and fixed a bug on your own: We are very open to patches, please create a pull request!  

### License

Our patches are provided under **GPLv2 license**. 

**Free Software Hell Yeah!** 

Proudly provided by: 
* [Sergej Schumilo](http://schumilo.de) - sergej@schumilo.de / [@ms_s3c](https://twitter.com/ms_s3c)
* [Cornelius Aschermann](https://hexgolems.com) - cornelius@hexgolems.com / [@is_eqv](https://twitter.com/is_eqv)
