CC=gcc



uvm: uvm.c
	$(CC) -o uvm uvm.c

debug:
	$(CC) -g -o uvm uvm.c
	gdb -ex run uvm

run:
	$(CC) -o uvm uvm.c
	./uvm