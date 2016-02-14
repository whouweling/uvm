# uvm

Experiment to find out how to to create a very compact VM.

This code is not very robust.

Example output:

```
u/vm 0.1
spawned new task 'shell' as 101
load 'shell' into 'shell' (101)
load 'stdlib' into 'shell' (101)
READY
> help
commands: help, halt, time, run
> count
spawned new task 'count' as 102
load 'count' into 'count' (102)
task 'count' (102) completed
> time
1455451168
> halt
Bye!
task 'shell' (101) completed
```
