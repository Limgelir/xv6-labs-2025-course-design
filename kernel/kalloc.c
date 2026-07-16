// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

#define PA2INDEX(pa) (((uint64)(pa) - KERNBASE) / PGSIZE)
#define NPAGE ((PHYSTOP - KERNBASE) / PGSIZE)

struct {
  struct spinlock lock;
  int count[NPAGE];
} ref;

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  initlock(&ref.lock, "ref");
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);

  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE){
    acquire(&ref.lock);
    ref.count[PA2INDEX(p)] = 1;
    release(&ref.lock);

    kfree(p);
  }
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;
  int count;

  if(((uint64)pa % PGSIZE) != 0 ||
     (char*)pa < end ||
     (uint64)pa >= PHYSTOP)
    panic("kfree");

  acquire(&ref.lock);

  ref.count[PA2INDEX(pa)]--;
  count = ref.count[PA2INDEX(pa)];

  if(count < 0){
    release(&ref.lock);
    panic("kfree ref");
  }

  release(&ref.lock);

  /*
   * 仍有其他页表引用该物理页，
   * 此时不能真正释放。
   */
  if(count > 0)
    return;

  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  if(r){
    memset((char*)r, 5, PGSIZE);

    acquire(&ref.lock);
    ref.count[PA2INDEX(r)] = 1;
    release(&ref.lock);
  }

  return (void*)r;
}

void
kaddref(void *pa)
{
  if(((uint64)pa % PGSIZE) != 0 ||
     (uint64)pa < KERNBASE ||
     (uint64)pa >= PHYSTOP)
    panic("kaddref");

  acquire(&ref.lock);
  ref.count[PA2INDEX(pa)]++;
  release(&ref.lock);
}

int
kgetref(void *pa)
{
  int count;

  acquire(&ref.lock);
  count = ref.count[PA2INDEX(pa)];
  release(&ref.lock);

  return count;
}
