
/* UVM: Tiny Virtual Machine capable of time-slice multitasking */
/* By: Wouter Houweling, 2015 */

//#pragma output CLIB_MALLOC_HEAP_SIZE = 5000 

#include "config.h"


#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

//#include <malloc.h>

#ifdef posix
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netdb.h>
#include <termios.h>
#endif

typedef enum { false, true } bool;

#define DEBUG_STEP 1

/* Need to make this dynamic, this will do for now */
#define STACK_SIZE 512
#define MEM_SIZE 512
#define TSLICE 200


/* Opcodes */
#define PUSH 1
#define POP 14

#define DUP 8

#define ADD 2
#define OUT 3
#define IN 15
#define HALT 4
#define JUMP 5
#define JUMPC 9
#define SKZ 18

#define STORE 6
#define LOAD 7

#define ISTORE 23
#define ILOAD 24

#define FSTORE 28
#define FLOAD 29

#define SWAP 13
#define EQ 16
#define NOT 20

#define CALL 10
#define RET 11

#define DUMP 17
#define TRACE 19

#define SYS 21
#define ASSERT 25

#define FP 26

#define DSP 30
#define ISP 31

/* Syscalls */
#define SYS_TIME 101
#define SYS_SPAWN 102
#define SYS_WAIT 103
#define SYS_ABORT 104
#define SYS_TASK_COUNT 110
#define SYS_TASK_ID 111
#define SYS_TASK_NAME 112
#define SYS_TASK_STATUS 113
#define SYS_MEM_USE 120
#define SYS_STOI 201
#define SYS_ATOI 202


int mem = 0;

struct instr {
   int op;
   int par;
   int src;
   char * line;
};

struct task {

   int id;
   char * name;
   struct instr * prg;
   struct instr * entry;
   struct instr * pc; /* Program counter */
   struct label * labels;

   char * mem;
   int mc; /* Alloc counter */

   char * stack;
   int * sc; /* Stack counter */
   int * fp; /* Frame pointer */

   int * cstack;
   int * cs;

   int length;

   int tracing;
   void * next;
   void * prev;

   int ready;
   int wait_for;

};

struct label {
   char * name;
   int address;
   void * next;
};

//struct label * labels;
struct task * tasks;

//extern long heap(6000);

int task_id = 100;

void panic(char * msg, int line) {
  printf("panic: %s (line %d)\n", msg, line);
  exit(1);
}

void * allocate(int size, int line) {
  void * block = malloc(size);
  if(block == NULL) panic("out of memory", line);
  mem=mem+size;
//  printf("mem: %d (%d)\n", mem, line);
  return block;
}

int lbl_lookup(struct task * t, char * name, int line) {
   struct label * s = t->labels;
   while(s) {
      printf(" lbl %s == %s\n", s->name, name);
      if(strcmp(s->name, name) == 0) {
         return s->address;
      }
      s = s->next;
   }
   return -1;

}

void lbl_register(struct task * t, char * name, int address) {

    struct label * lbl = allocate(sizeof(struct label), 28);

    lbl->name = allocate(sizeof(char)*strlen(name), 1);

    lbl->address = address;

    strcpy(lbl->name, name);

    if(t->labels == NULL) {
      t->labels = lbl;
      lbl->next = NULL;
    } else {
      lbl->next = t->labels;
      t->labels = lbl;
    }
}

void zero(char * data, int size) {
   for(int i=0; i<size; i++) {
      *(data+i) = '\0';
   }
}

struct task * spawn(char * name) {

   struct task * t = allocate(sizeof(struct task), 2);

   if(t == NULL) {
     panic("spawn: out of memory", __LINE__);
   }

   t->prg = allocate(500*sizeof(struct instr), 3);

   t->stack = (char *) allocate(STACK_SIZE, 4);
   t->cstack = (int *) allocate(STACK_SIZE, sizeof(int));
   t->mem = (char *) allocate(MEM_SIZE, 5);
   
   t->pc = t->prg;
   t->tracing = 0;
   t->length = 0;
   t->sc = t->stack + 2 * sizeof(int *); /* The extra space is the stack underflow protection */
   t->cs = t->cstack;
   t->mc = 0;
   t->ready = 1;
   t->labels = NULL;
   t->fp = 0;

   task_id++;
   t->id = task_id;

   t->name = (char *) allocate(strlen(name), 5);
   strcpy(t->name, name);

   zero((void *) t->mem, MEM_SIZE);

   /* Add to the tasks list: */
   if(! tasks) {
      tasks = t;
      t->next = NULL;
      t->prev = NULL;
   } else {
      t->next = tasks;
      t->prev = NULL;
      tasks->prev = t;
      tasks = t;
   }

   //printf("spawned new task '%s' as %d\n", t->name, t->id);

   return t;

}

void halt(struct task * t) {

   t->ready = 0;

   /* Unlink the task */
   if(tasks == t) {
      tasks = t->next;
      if(tasks) {
         tasks->prev = NULL;
      }
   } else {
      struct task * next = t->next;
      struct task * prev = t->prev;
      if(next) { next->prev = prev; }
      if(prev) { prev->next = next; }
   }

   /* Ready tasks waiting for this task */
   struct task * i = tasks;
   while(i) {
     if(! i->ready && i->wait_for == t->id) {
        i->ready = 1;
     }
     i = i->next;
   }

   free(t->stack);
   free(t->mem);
   free(t->name);
   free(t->prg);
   free(t);

}

void task_abort(struct task * t, int code) {

   printf("*** abort %s (%d): %d\n", t->name, t->id, code);
   halt(t);

}


bool load(struct task * t, char * source, struct label * labels) {


    //struct session * fp = vfs_open(source);

    FILE * fp = fopen(source, "r");

    if(fp == NULL) {
      printf("*** error: unable to read '%s' pid %d\n", source, t->id);
      task_abort(t, 0);
      return false;
    }

    //printf("load '%s' into '%s' (%d)\n", source, t->name, t->id);

   int line = 0;
   int top = 1;

   if(! labels) {
      top = 0;
   }

   //printf("appending on %d\n", t->length);


   char ch;
   char * cmd = allocate(128, 5);
   char * par = allocate(128, 16);
   char * c = cmd;
   char * p = par;
   int length = 0;

   int tmode = 0;
   int cmd_mode = 0;

   //while((ch=vfs_fgetc(fp))!=EOF) {
   while((ch=fgetc(fp))!=EOF) {

      //printf("got %c\n", ch);

      if(ch == '#') {
        cmd_mode = 1;
      }

      if(cmd_mode && ch != '\n') {
        continue;
      } else {
        cmd_mode = 0;
      }

      if(ch == '\n') {

         line ++;

         printf("Got: '%s'\n", cmd);

         *c='\0';
         *p='\0';

         if(strlen(cmd) && cmd[0] != '#') {

            if(cmd[0] == ':') {
                lbl_register(t, cmd, t->pc - t->prg);
                if(par) {
                   printf("reserve %d on stack ..\n", atoi(par));

                }
            } else {

               if(cmd[0] == '.') {

                 lbl_register(t, cmd, t->mc);

                 /* Data string handling */
                 char * pt = par;
                 p = t->mem + t->mc;
                 bool esc = false;
                 while(*pt) {
                    if(*pt == '\\') {
                       esc = true;
                    } else {
                       if(esc) {
                          if(*pt == 'n') *p = '\n';
                       } else {
                          *p = *pt;
                       }
                       t->mc++;
                       p++;
                       *p = '\0';
                    }
                    pt++;
                 }

                 t->mc++;

               } else if (cmd[0] == '%') {

                  /* Reserve memory */
                  int s_length = atoi(par);
                  lbl_register(t, cmd, t->mc);
                  t->mc = t->mc + s_length;

               } else if (cmd[0] == '$') {
                  printf("local var %s %d\n", cmd, atoi(par));
                  lbl_register(t, cmd, atoi(par));
               } else {

                  if(strcmp(cmd, "include") == 0) {
                     load(t, par, labels);

                  } else {

                     if(strcmp(cmd, "push") == 0) t->pc->op = PUSH;
                     if(strcmp(cmd, "pop") == 0) t->pc->op = POP;
                     if(strcmp(cmd, "dup") == 0) t->pc->op = DUP;
                     if(strcmp(cmd, "add") == 0) t->pc->op = ADD;
                     if(strcmp(cmd, "out") == 0) t->pc->op = OUT;
                     if(strcmp(cmd, "in") == 0) t->pc->op = IN;
                     if(strcmp(cmd, "halt") == 0) t->pc->op = HALT;
                     if(strcmp(cmd, "ret") == 0) t->pc->op = RET;
                     if(strcmp(cmd, "swap") == 0) t->pc->op = SWAP;
                     if(strcmp(cmd, "eq") == 0) t->pc->op = EQ;
                     if(strcmp(cmd, "dump") == 0) t->pc->op = DUMP;
                     if(strcmp(cmd, "skz") == 0) t->pc->op = SKZ;

                     if(strcmp(cmd, "store") == 0) t->pc->op = STORE;
                     if(strcmp(cmd, "load") == 0) t->pc->op = LOAD;

                     if(strcmp(cmd, "fstore") == 0) t->pc->op = FSTORE;
                     if(strcmp(cmd, "fload") == 0) t->pc->op = FLOAD;

                     if(strcmp(cmd, "istore") == 0) t->pc->op = ISTORE;
                     if(strcmp(cmd, "iload") == 0) t->pc->op = ILOAD;

                     if(strcmp(cmd, "dsp") == 0) t->pc->op = DSP;
                     if(strcmp(cmd, "isp") == 0) t->pc->op = ISP;

                     if(strcmp(cmd, "trace") == 0) t->pc->op = TRACE;
                     if(strcmp(cmd, "not") == 0) t->pc->op = NOT;
                     if(strcmp(cmd, "sys") == 0) t->pc->op = SYS;
                     if(strcmp(cmd, "assert") == 0) t->pc->op = ASSERT;

                     if(par[0] == '.' || par[0] == '%' || par[0] == '$') {
                       t->pc->par = lbl_lookup(t, par, line);
                       if(t->pc->par == -1) {
                            printf("error: label '%s' not found (line %d)\n", par, line);
                            task_abort(t, 0);
                            return;
                       }
                     } else {
                        t->pc->par = atoi(par);
                     }

                     if(strcmp(cmd, "jump") == 0) {
                        t->pc->op = JUMP;
                        t->pc->par = lbl_lookup(t, par, line);
                     }

                     if(strcmp(cmd, "jumpc") == 0) {
                        t->pc->op = JUMPC;
                        t->pc->par = lbl_lookup(t, par, line);
                     }

                     if(strcmp(cmd, "call") == 0) {
                        t->pc->op = CALL;
                        t->pc->par = lbl_lookup(t, par, line);
                     }

                     if((t->pc->op == CALL || t->pc->op == JUMP || t->pc->op == JUMPC)
                            && t->pc->par == -1) {
                            printf("error: label '%s' not found (line %d)\n", par, line);
                            task_abort(t, 0);
                            return;
                       }

                     if(! t->pc->op) {
                        printf("error: unkown op '%s' on line %d\n", cmd, line);
                        task_abort(t, 0);
                        return;
                     }

                     t->pc->src = line;
                     #ifndef CPM
                       t->pc->line = allocate(sizeof(char)*100, 8);
                       sprintf(t->pc->line, "%s %s", cmd, par);
                     #endif
                     t->length++;
                     t->pc++;
                  }

               }

            }
         }
         c=cmd; *c = '\0';
         p=par;
         tmode=0;
      } else {
         if(ch == ' ' && tmode == 0) {

            /* Remove first spaces */
            if(strlen(cmd)) {
               tmode++;
            }

         } else {
            switch(tmode) {

               case 0: /* OPCODE */
                  *c = ch;
                  c++;
                  break;

               case 1: /* Parameter */
                  *p = ch;
                  p++;
                  break;
            }
         }

      }

   }

   /* We always start execution at :main */
   if(lbl_lookup(t, ":main", 0) != -1) {
      t->pc = t->prg + lbl_lookup(t, ":main", 0);
   }

   if(top) {
      /* Free memory */
      struct label * l = labels;
      while(l) {
         if(l->name) free(l->name);
         free(l);
         l = l->next;
      }
   }


   return true;
}

void debug_stack(struct task * t, int * s) {
   int * ds = t->stack;
   while(ds != s) {
      printf("[%d]", *ds);
      ds++;
   }
   printf("\n");
}

int * sys_call(struct task * t, int * s) {

   int call = *(--s);
   struct task * x;
   char * c;

   switch(call) {

      case SYS_TIME: /* Get unix time */
         sprintf(t->mem + *(--s), "%d", time( NULL ));
         break;

      case SYS_SPAWN: /* Spawn task */
         s--;
         struct task * c = spawn(t->mem + *(s));
         if(load(c, t->mem + *(s), NULL)) {
            *s = c->id;
         } else {
            *s = -1;
         }
         s++;
         break;

      case SYS_WAIT: /* Wait for task */
         s--;
         t->wait_for = *s;
         t->ready = 0;
         break;


      case SYS_ABORT: /* Abort task */
         s--;
         x = tasks;
         while(x) {
            if(x->id == *s) {
              task_abort(x, 6);
              break;
            }
            x = x->next;
         }
         break;

      case SYS_TASK_COUNT: /* Task count */
        *s = 0;
        x = tasks;
        while(x) {
            x = x->next;
            (*s) ++;
        }
        s++;
        break;

      case SYS_TASK_ID: /* Get task id */
        s--;
        x = tasks; while(x && *s) { x = x->next; (*s) --;}
        *s = x->id;
        s++;
        break;

      case SYS_TASK_NAME: /* Get task name */
        s--;
        x = tasks; while(x && *s) { x = x->next; (*s) --;}
        sprintf(t->mem + *(--s), "%s", x->name);
        break;

      case SYS_TASK_STATUS: /* Get task status */
        s--;
        x = tasks; while(x && *s) { x = x->next; (*s) --;}
        if(x->ready) { *s = 'r'; } else { *s = 'w'; }
        s++;
        break;

      case SYS_MEM_USE: /* Mem used */
        *s = mem; s++;
        break;

      case SYS_STOI: /* Convert int to str */
        sprintf(t->mem + *(--s), "%d", *(--s));
        break;

      case SYS_ATOI: /* Convert str to int */
        s--; *s = atoi(t->mem + *(s)); s++;
        break;

      default:
        task_abort(t, 4);
        break;
   }

   return s;

}

char * nbgetc() {

  char c;

  #ifdef cpm
    if(! kbhit()) { return NULL; }
    c = getch();
    if(c == 13) { c=10; }
    return c;
  #endif

  #ifdef posix
    fd_set readset;
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 0;

    FD_ZERO(&readset);
    FD_SET(fileno(stdin), &readset);

    select(fileno(stdin)+1, &readset, NULL, NULL, &tv);

    if(FD_ISSET(fileno(stdin), &readset))
    {
        return(fgetc(stdin));
    }
  #endif
}


int pop(struct task * t) {
  int * s = t->sc;
  s--;
  return *s;
}

void push(int * s, int val) {
  *s = val;
  s++;
}


void run(struct task * t) {

   char * stack = t->stack;
   
   int * s = t->sc;
   int * c = t->cs;
   
   struct instr * i = t->pc;

   int r;

   int ax;
   int bx;

   int tslice = TSLICE;

   while(tslice && t->ready) {

      tslice--;

      if(t->tracing && i->op != 15) {
        #ifndef CPM
        printf("[op %d %d (line %d, '%s')\n", i->op, i->par, i->src, i->line);
        #else
        printf("[op %d %d (line %d)\n", i->op, i->par, i->src);
        #endif
         
      }

      switch(i->op) {

         case PUSH:
            *s = i->par; s++;
            break;

         case POP:
            s--;
            break;

         case SWAP:
            r = *(s-2);
            *(s-2) = *(s-1);
            *(s-1) = r;
            break;

         case DUP:
            *s = *(s-1); s++;
            break;

         case ADD:
            s--;
            *(s-1) = *(s-1) + *(s);
            break;

         case EQ:
	        s--;
            *(s-1) = *(s-1) == *(s);
            break;

         case NOT:
            *(s-1) = ! *(s-1);
            break;

         case FP:
            *s = t->fp;
            printf("fp %d\n", *s);
            s++;
            break;

         case OUT:
            s--;
            //printf("%d\n", *s);
            putc(*s, stdout);
            break;

         case IN:
            *s = nbgetc();
            if(*s == NULL) {
               tslice = 0;
               continue;
            }
            s++;
            break;

         case JUMP:
            i = t->prg + i->par - 1;
            break;

         case JUMPC:
            if(*(--s)) {
               i = t->prg + i->par - 1;
            }
            break;

         case SKZ:

            if(! *(--s)) {
                i = i + i->par;
            }

            break;

         case STORE:
            s--;
            *(t->mem + *(s)) = *(s-1);
            s--;
            break;

         case LOAD:
	        s--;
            *s = *(t->mem + *(s));
            s++;
            break;

         case ISTORE:
	        s--;
            *(((int *)t->mem) + *(s)) = *(s-1);
	        s--;
            break;

         case ILOAD:
	        s--;
            *s = *(((int *)t->mem) + *(s));
            s++;
            break;

         case FSTORE:
	        s--;
           *(((int *)t->fp) + *(s)) = *(s-1);
	        s--;
           break;

         case FLOAD:
	        s--;
           
           *s = *(t->fp + *(s));
           printf("fload %d (fp=%d)\n", *s);
           s++;
           break;            

         case CALL:
            *c = i - t->prg; c++;
            *c = t->fp - (int*) stack; printf("fp @ %d\n", *c); c++;
            *c = s - (int *) stack; printf("stack @ %d\n", *c); c++;
            i = t->prg + i->par - 1;
            t->fp = s;
            printf("set fp to: %d\n", t->fp);
            break;

         case RET:
            c--; s = (int *) stack + *(c);
            c--; t->fp = (int *) stack + *(c);
            c--; i = t->prg + *(c);
            break;

         case ISP:
            s = s + i->par;
            break;

         case DSP:
            s = s - i->par;
            break;

         case SYS:
            s = sys_call(t, s);
            break;

         case DUMP:
            printf(">>> line %d: ", i->src); debug_stack(t, s);
            printf("\n");
            break;

         case TRACE:
            t->tracing = i->par;
            break;

         case ASSERT:
            if(*(--s) != i->par) {
               printf("error: task %d: assert failed %d != %d (line %d)\n", t->id, *s, i->par, i->src);
               task_abort(t, 2);
               return;
            }
            break;

         case HALT:
            halt(t);
            return;

      }
      
      #ifdef cpm
      outp(0, i->op); 
      #endif

      //for(int d=0; d< 100;d++) {}

      if(s <= (stack + 2)) {
         //printf("error: task %s (%d): stack underflow (line %d)\n", t->name, t->id, i->src);
         return task_abort(t, 1);;
      }

      if((i > t->prg + t->length) || (i+1) < t->prg) {
         printf("error: task %s (%d): segfault\n", t->name, t->id);
         return task_abort(t, 2);;
      }

      i++;

      if(t->tracing) {
        debug_stack(t, s);
        printf("\n\n");
        getc(stdin);
      }

   }

   t->pc = i;
   t->sc = s;
   t->cs = c;
}

void scheduler() {

   printf("scheduler: started\n");

   struct task * t = tasks;
   int i_led = 0;
   int t_dir = 0;
   int cycle = 0;
   while(tasks) {
   
      if(t->ready) {
        run(t);
      }

      if(t->next) {
         t = t->next;
      } else {
         t = tasks;
      }
      
      cycle++;
      
      if(cycle % 200 == 0) {
        if(i_led>6) { t_dir = ! t_dir; }
        if(i_led<1) { t_dir = ! t_dir; }
        if(t_dir) { i_led++; } else { i_led--; }
        
        int l = 1 << i_led;
        #ifdef cpm
        outp(0, l);
        #endif
      }

      usleep(10);
   }

   printf("scheduler: no more tasks, exiting\n");
}


int main(int argc, char *argv[]) {


    printf("u/vm 0.1\n");
    // mallinit();              // heap cleared to empty
    
    if(allocate(1, __LINE__) == NULL) {
      panic("unable to allocate memory", __LINE__);
    }
    
    #ifdef posix
    setbuf(stdout, NULL);

    struct termios ttystate, ttysave;
    unsigned char c;

    tcgetattr(STDIN_FILENO, & ttystate);
    ttysave = ttystate;
    ttystate.c_lflag &=(~ICANON & ~ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &ttystate);

    /*if(argc != 2) {
      printf("usage: <program>\n");
      exit(1);
    }*/
    #endif

    char * source = "init";
    struct task * t = spawn(source);
    load(t, source, NULL);
    scheduler();

    #ifdef posix
    printf("execution stopped.\n");
    ttystate.c_lflag |= ICANON | ECHO;
    tcsetattr(STDIN_FILENO, TCSANOW, &ttysave);
    #endif
}
