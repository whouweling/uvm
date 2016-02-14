all:
	gcc -g uvm.c -o uvm

debug: 
	gdb -ex run --args uvm test

test: 
	./uvm test
