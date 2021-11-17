cd /tmp/ &&
wget -c  https://kernel.ubuntu.com/~kernel-ppa/mainline/v5.10.73/amd64/linux-headers-5.10.73-051073-generic_5.10.73-051073.202110131013_amd64.deb &&
wget -c  https://kernel.ubuntu.com/~kernel-ppa/mainline/v5.10.73/amd64/linux-headers-5.10.73-051073_5.10.73-051073.202110131013_all.deb &&
wget -c  https://kernel.ubuntu.com/~kernel-ppa/mainline/v5.10.73/amd64/linux-image-unsigned-5.10.73-051073-generic_5.10.73-051073.202110131013_amd64.deb &&
wget -c  https://kernel.ubuntu.com/~kernel-ppa/mainline/v5.10.73/amd64/linux-modules-5.10.73-051073-generic_5.10.73-051073.202110131013_amd64.deb &&
sudo dpkg -i *.deb
