clear 

make clean 

make

qemu-system-x86_64 -m 512M -cdrom out/edgeos.iso -d int,cpu_reset -no-reboot -no-shutdown -serial stdio
