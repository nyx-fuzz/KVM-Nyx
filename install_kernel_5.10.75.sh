cd /tmp/ &&
wget -c https://kernel.ubuntu.com/~kernel-ppa/mainline/v5.10.75/amd64/linux-headers-5.10.75-051075-generic_5.10.75-051075.202110201038_amd64.deb &&
wget -c https://kernel.ubuntu.com/~kernel-ppa/mainline/v5.10.75/amd64/linux-headers-5.10.75-051075_5.10.75-051075.202110201038_all.deb &&
wget -c https://kernel.ubuntu.com/~kernel-ppa/mainline/v5.10.75/amd64/linux-image-unsigned-5.10.75-051075-generic_5.10.75-051075.202110201038_amd64.deb &&
wget -c https://kernel.ubuntu.com/~kernel-ppa/mainline/v5.10.75/amd64/linux-modules-5.10.75-051075-generic_5.10.75-051075.202110201038_amd64.deb &&
sudo dpkg -i *.deb
