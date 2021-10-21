cd /tmp/ &&
wget -c https://kernel.ubuntu.com/~kernel-ppa/mainline/v5.10.31/amd64/linux-headers-5.10.31-051031_5.10.31-051031.202104160635_all.deb &&
wget -c https://kernel.ubuntu.com/~kernel-ppa/mainline/v5.10.31/amd64/linux-headers-5.10.31-051031-generic_5.10.31-051031.202104160635_amd64.deb &&
wget -c https://kernel.ubuntu.com/~kernel-ppa/mainline/v5.10.31/amd64/linux-image-unsigned-5.10.31-051031-generic_5.10.31-051031.202104160635_amd64.deb &&
wget -c https://kernel.ubuntu.com/~kernel-ppa/mainline/v5.10.31/amd64/linux-modules-5.10.31-051031-generic_5.10.31-051031.202104160635_amd64.deb &&
sudo dpkg -i *.deb
