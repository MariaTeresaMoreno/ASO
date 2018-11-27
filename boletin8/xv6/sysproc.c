#include "types.h"
#include "x86.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"

int
sys_fork(void)
{
  return fork();
}

int
sys_exit(void)
{
  exit();
  return 0;  // not reached
}

int
sys_wait(void)
{
  return wait();
}

int
sys_kill(void)
{
  int pid;

  if(argint(0, &pid) < 0)
    return -1;
  return kill(pid);
}

int
sys_getpid(void)
{
  return myproc()->pid;
}

//Devuelve el puntero anterior a donde empieza el segmento.
int
sys_sbrk(void)
{
  int addr;
  int n;

  if(argint(0, &n) < 0) //obtiene el primer argumento n
    return -1;
  
  addr = myproc()->sz;
  
  if(n <= 0){ //arg. negativo
    if(myproc()->sz+n < 0) //tam pag es menor q cero
      return -1;
    growproc(n);
  }
  myproc()->sz += n; //modificar tam.

  return addr;
}

int
sys_sleep(void)
{
  int n;
  uint ticks0;

  if(argint(0, &n) < 0)
    return -1;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(myproc()->killed){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

// return how many clock tick interrupts have occurred
// since start.
int
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

//implementación de la llamada al sistema date
int
sys_date(void){ //(void*)&r puntero que se pasa como param
    struct rtcdate *r;
    if(argptr(0, (void*)&r,sizeof(*r))<0) //comprueba que el puntero tenga algo
      return -1;
    cmostime(r);
    return 0;
}