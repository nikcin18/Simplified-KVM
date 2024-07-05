guest.img: guest.o
	ld -T guest.ld guest.o -o guest.img

guest.o: guest.c
	$(CC) -m64 -ffreestanding -fno-pic -c -o $@ $^

mini_hypervisor: mini_hypervisor.c
	gcc -lpthread mini_hypervisor.c -o mini_hypervisor

delete-local-files:
	find . -name "*.local*" -type f -delete

all:
	make delete-local-files
	make guest.o
	make guest.img
	make mini_hypervisor