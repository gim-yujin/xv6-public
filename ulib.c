#include "types.h"
#include "stat.h"
#include "fcntl.h"
#include "user.h"
#include "x86.h"
#include "mmu.h"
#include "param.h"

char*
strcpy(char *s, const char *t)
{
  char *os;

  os = s;
  while((*s++ = *t++) != 0)
    ;
  return os;
}

int
strcmp(const char *p, const char *q)
{
  while(*p && *p == *q)
    p++, q++;
  return (uchar)*p - (uchar)*q;
}

uint
strlen(const char *s)
{
  int n;

  for(n = 0; s[n]; n++)
    ;
  return n;
}

void*
memset(void *dst, int c, uint n)
{
  stosb(dst, c, n);
  return dst;
}

char*
strchr(const char *s, char c)
{
  for(; *s; s++)
    if(*s == c)
      return (char*)s;
  return 0;
}

char*
gets(char *buf, int max)
{
  int i;
  char c;

  for(i=0; i+1 < max; ){
    int cc = read(0, &c, 1);
    if(cc < 1)
      break;
    buf[i++] = c;
    if(c == '\n' || c == '\r')
      break;
  }
  buf[i] = '\0';
  return buf;
}

int
stat(const char *n, struct stat *st)
{
  int fd;
  int r;

  fd = open(n, O_RDONLY);
  if(fd < 0)
    return -1;
  r = fstat(fd, st);
  close(fd);
  return r;
}

int
atoi(const char *s)
{
  int n;

  n = 0;
  while('0' <= *s && *s <= '9')
    n = n*10 + *s++ - '0';
  return n;
}

void*
memmove(void *vdst, const void *vsrc, int n)
{
  char *dst;
  const char *src;

  dst = vdst;
  src = vsrc;
  while(n-- > 0)
    *dst++ = *src++;
  return vdst;
}


struct thread_stackent {
  int pid;
  char *stack;
};

static struct thread_stackent threadstacks[NPROC];

static int
thread_stackrecord(int pid, char *stack)
{
  int i;

  for(i = 0; i < NPROC; i++){
    if(threadstacks[i].pid == 0){
      threadstacks[i].pid = pid;
      threadstacks[i].stack = stack;
      return 0;
    }
  }
  return -1;
}

static char*
thread_stacktake(int pid)
{
  int i;
  char *stack;

  for(i = 0; i < NPROC; i++){
    if(threadstacks[i].pid == pid){
      stack = threadstacks[i].stack;
      threadstacks[i].pid = 0;
      threadstacks[i].stack = 0;
      return stack;
    }
  }
  return 0;
}

int
thread_create(void (*start_routine)(void*), void *arg)
{
  char *stack;
  char *aligned;

  stack = malloc(2 * PGSIZE);
  if(stack == 0)
    return -1;
  aligned = (char*)(((uint)stack + PGSIZE - 1) & ~(PGSIZE - 1));
  {
    int pid = clone(start_routine, arg, aligned + PGSIZE);
    if(pid < 0){
      free(stack);
      return -1;
    }
    if(thread_stackrecord(pid, stack) < 0){
      tkill(pid);
      return -1;
    }
    return pid;
  }
}

int
thread_join(void)
{
  void *stack;
  char *raw;

  int pid = join(&stack);

  if(pid < 0)
    return -1;
  raw = thread_stacktake(pid);
  if(raw == 0)
    return -1;
  free(raw);
  return pid;
}
