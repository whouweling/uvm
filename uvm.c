
/* UVM: Tiny Virtual Machine capable of time-slice multitasking */
/* By: Wouter Houweling, 2015 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>

#include <netdb.h>
#include <unistd.h>
#include <stdio.h>

typedef enum { false, true } bool;

#define DEBUG_STEP 0

/* Need to make this dynamic, this will do for now */
#define STACK_SIZE 1024
#define MEM_SIZE 1024
#define DATA_SIZE 128
#define TSLICE 1000


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

#define SWAP 13
#define EQ 16
#define NOT 20

#define CALL 10
#define RET 11

#define DUMP 17
#define TRACE 19

#define SYS 21
#define ASSERT 25


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

   int length;

   int tracing;
   void * next;
   void * prev;

   int ready;
};

struct label {
   char * name;
   int address;
   void * next;
};

//struct label * labels;
struct task * tasks;

int task_id = 100;

int lbl_lookup(struct task * t, char * name, int line) {
   struct label * s = t->labels;
   while(s) {
      if(strcmp(s->name, name) == 0) {
         return s->address;
      }
      s = s->next;
   }
   return -1;

}

void lbl_register(struct task * t, char * name, int address) {

    struct label * lbl = malloc(sizeof(struct label));

    lbl->name = malloc(sizeof(char)*strlen(name));
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

   struct task * t = malloc(sizeof(struct task));

   t->prg = malloc(4024*sizeof(struct instr));

   t->stack = (char *) malloc(STACK_SIZE);
   t->mem = (char *) malloc(MEM_SIZE);
   t->pc = t->prg;
   t->tracing = 0;
   t->length = 0;
   t->sc = t->stack;
   t->mc = 0;
   t->ready = 1;
   t->labels = NULL;

   task_id++;
   t->id = task_id;

   t->name = (char *) malloc(strlen(name));
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

   printf("spawned new task '%s' as %d\n", t->name, t->id);

   return t;

}

void halt(struct task * t) {

   t->ready = 0;
   printf("task '%s' (%d) completed\n", t->name, t->id);

   return;

   if(tasks == t) {
      tasks = t->next;
      if(tasks) {
         tasks->prev = NULL;
      }
   } else {
      struct task * next = t->next;
      struct task * prev = t->prev;

      next->prev = prev;
      prev->next = next;
   }

  // free(t->stack);
   free(t->mem);
   free(t->name);
   free(t);


}

void task_abort(struct task * t, int code) {

   printf("abort %s (%d): %d\n", t->name, t->id, code);
   halt(t);

}


void load(struct task * t, char * source, struct label * labels) {

    FILE * fp = fopen(source, "r");

    if(fp == NULL) {
      printf("error: unable to read '%s'\n", source);
      task_abort(t, 0);
      return;
    }

    printf("load '%s' into '%s' (%d)\n", source, t->name, t->id);

   int line = 0;
   int top = 1;

   if(! labels) {
      top = 0;
   }

   printf("appending on %d\n", t->length);


   char ch;
   char * cmd = malloc(128);
   char * par = malloc(128);
   char * c = cmd;
   char * p = par;
   int length = 0;


   int tmode = 0;
   int cmd_mode = 0;
   while((ch=fgetc(fp))!=EOF) {


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

         *c='\0';
         *p='\0';

         if(strlen(cmd) && cmd[0] != '#') {

            if(cmd[0] == ':') {

                lbl_register(t, cmd, t->pc - t->prg);

            } else {

               if(cmd[0] == '.') {

                 lbl_register(t, cmd, t->mc);

                 /* Data string handling */
                 char * pt = par;
                 char * p = t->mem + t->mc;
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
                 int length = atoi(par);
                 lbl_register(t, cmd, t->mc);
                 t->mc = t->mc + length;

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

                     if(strcmp(cmd, "istore") == 0) t->pc->op = ISTORE;
                     if(strcmp(cmd, "iload") == 0) t->pc->op = ILOAD;

                     if(strcmp(cmd, "trace") == 0) t->pc->op = TRACE;
                     if(strcmp(cmd, "not") == 0) t->pc->op = NOT;
                     if(strcmp(cmd, "sys") == 0) t->pc->op = SYS;
                     if(strcmp(cmd, "assert") == 0) t->pc->op = ASSERT;

                     if(par[0] == '.' || par[0] == '%') {
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
                     t->pc->line = malloc(sizeof(char)*100);
                     t->length++;
                     sprintf(t->pc->line, "%s %s", cmd, par);
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

   switch(call) {

      case 101: /* Get unix time */
         sprintf(t->mem + *(--s), "%d", time( NULL ));
         break;


      case 102: /* Spawn task */
         s--;
         struct task * c = spawn(t->mem + *(s));
         load(c, t->mem + *(s), NULL);
         break;

      case 110: /* Task count */

        *s = 0;
        x = tasks;
        while(x) {
            x = x->next;
            (*s) ++;
        }
        s++;
        break;

      case 111: /* Get task id */
        s--;
        *s = 0;
        x = tasks;
        while(t && (*s) > 0) {
            x = x->next;
            (*s) --;
        }
        *s = x->id;
        s++;
        break;

   }

   return s;

}

char * nbgetc() {
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
  return NULL;
}


int pop(int * s) {
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

   struct instr * i = t->pc;

   int r;

   int ax;
   int bx;

   int tslice = TSLICE;


   while(tslice) {

      tslice--;

      if(t->tracing) {
        printf("[op %d %d (line %d, '%s')\n", i->op, i->par, i->src, i->line);
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

         case OUT:
            s--;
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

         case CALL:
            *s = i - t->prg; s++;
            i = t->prg + i->par - 1;
            break;

         case RET:
            s--;
            i = t->prg + *(s);
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

      if(s < stack) {
         printf("error: task %s (%d): stack underflow (line %d)\n", t->name, t->id, i->src);
         task_abort(t, 1);
         return;
      }

      if((i > t->prg + t->length) || (i+1) < t->prg) {
         printf("error: task %s (%d): segfault\n", t->name, t->id);
         task_abort(t, 2);
         return;
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
}

void scheduler() {

   struct task * t = tasks;
   while(tasks) {

      if(t->ready) {
        run(t);
      }

      if(t->next) {
         t = t->next;
      } else {
         t = tasks;
      }
   }

   printf("scheduler: no more tasks, exiting\n");
}


int main(int argc, char *argv[]) {

    printf("u/vm 0.1\n");

    setbuf(stdout, NULL);

    struct termios ttystate, ttysave;
    unsigned char c;

    tcgetattr(STDIN_FILENO,&ttystate);
    ttysave = ttystate;
    ttystate.c_lflag &=(~ICANON & ~ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW,&ttystate);

    if(argc != 2) {
      printf("usage: <program>\n");
      exit(1);
    }

    char * source = argv[1];

    struct task * t = spawn(source);
    load(t, source, NULL);
    scheduler();

    ttystate.c_lflag |= ICANON | ECHO;
    tcsetattr(STDIN_FILENO, TCSANOW, &ttysave);
}
