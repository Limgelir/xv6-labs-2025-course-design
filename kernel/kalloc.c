// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"


void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.


struct run {
  struct run *next;
};


// 每个 CPU 一个空闲链表
struct kmem_cpu {
  struct spinlock lock;
  struct run *freelist;
};

struct kmem_cpu kmems[NCPU];



void
kinit()
{
  for(int i = 0; i < NCPU; i++){
    char name[16];

    snprintf(name, sizeof(name), "kmem%d", i);

    initlock(&kmems[i].lock, name);

    kmems[i].freelist = 0;
  }

  freerange(end, (void*)PHYSTOP);
}



void
freerange(void *pa_start, void *pa_end)
{
  char *p;

  p = (char*)PGROUNDUP((uint64)pa_start);

  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}



// Free one physical page.
void
kfree(void *pa)
{
  struct run *r;


  if(((uint64)pa % PGSIZE) != 0 ||
     (char*)pa < end ||
     (uint64)pa >= PHYSTOP)
    panic("kfree");


  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);


  r = (struct run*)pa;


  push_off();

  int id = cpuid();

  acquire(&kmems[id].lock);

  r->next = kmems[id].freelist;

  kmems[id].freelist = r;

  release(&kmems[id].lock);

  pop_off();
}



// Allocate one 4096-byte page.
void *
kalloc(void)
{
  struct run *r;


  push_off();

  int id = cpuid();

  pop_off();


  /*
   * 1. Try current CPU freelist
   */

  acquire(&kmems[id].lock);


  r = kmems[id].freelist;


  if(r){

    kmems[id].freelist = r->next;

    release(&kmems[id].lock);


    memset((char*)r, 5, PGSIZE);

    return (void*)r;
  }


  release(&kmems[id].lock);



  /*
   * 2. Steal from other CPU
   */

  for(int i = 0; i < NCPU; i++){

    if(i == id)
      continue;


    acquire(&kmems[i].lock);


    r = kmems[i].freelist;


    if(r){

      kmems[i].freelist = r->next;


      release(&kmems[i].lock);


      memset((char*)r, 5, PGSIZE);

      return (void*)r;
    }


    release(&kmems[i].lock);
  }



  return 0;
}
