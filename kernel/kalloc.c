// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// pipe buffers, and 2 MB superpages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);
static void superfreerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

// 普通 4 KB 页的链表节点。
struct run {
  struct run *next;
};

// 2 MB superpage 的链表节点。
// 链表指针只占用 superpage 开头的一小部分空间。
struct superrun {
  struct superrun *next;
};

// 预留 20 个 2 MB 区域，共 40 MB。
// 父进程与子进程可以同时拥有多张 superpage。
#define NSUPER 20

// PHYSTOP 是物理内存顶端。
// 将最上面的 40 MB 留给 superpage 分配器。
#define SUPERBASE (PHYSTOP - NSUPER * SUPERPGSIZE)

// 普通 4 KB 页面分配器。
struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

// 2 MB superpage 分配器。
struct {
  struct spinlock lock;
  struct superrun *freelist;
} supermem;

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  initlock(&supermem.lock, "supermem");

  /*
   * 普通页和 superpage 使用两个互不重叠的区域。
   *
   * end ~ SUPERBASE:
   *   作为普通 4 KB 页。
   *
   * SUPERBASE ~ PHYSTOP:
   *   作为 2 MB superpage。
   */
  freerange(end, (void *)SUPERBASE);
  superfreerange((void *)SUPERBASE, (void *)PHYSTOP);
}

// 把一段物理内存切分为普通 4 KB 页。
void
freerange(void *pa_start, void *pa_end)
{
  char *p;

  p = (char *)PGROUNDUP((uint64)pa_start);

  for(; p + PGSIZE <= (char *)pa_end; p += PGSIZE){
    kfree(p);
  }
}

// 把一段物理内存切分为 2 MB superpage。
static void
superfreerange(void *pa_start, void *pa_end)
{
  char *p;

  /*
   * SUPERBASE 本身已经按 2 MB 对齐。
   * 为稳妥起见，仍使用 SUPERPGROUNDUP。
   */
  p = (char *)SUPERPGROUNDUP((uint64)pa_start);

  for(; p + SUPERPGSIZE <= (char *)pa_end;
      p += SUPERPGSIZE){
    superfree(p);
  }
}

// 释放普通 4 KB 物理页。
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 ||
     (char *)pa < end ||
     (uint64)pa >= SUPERBASE){
    panic("kfree");
  }

  // Fill with junk to catch dangling references.
  memset(pa, 1, PGSIZE);

  r = (struct run *)pa;

  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

// 分配普通 4 KB 物理页。
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;

  if(r){
    kmem.freelist = r->next;
  }

  release(&kmem.lock);

  if(r){
    memset((char *)r, 5, PGSIZE);
  }

  return (void *)r;
}

// 释放一张 2 MB superpage。
void
superfree(void *pa)
{
  struct superrun *r;

  /*
   * superpage 必须满足：
   * 1. 物理地址按 2 MB 对齐；
   * 2. 位于专门预留的 superpage 区域；
   * 3. 没有超过物理内存顶端。
   */
  if(((uint64)pa % SUPERPGSIZE) != 0 ||
     (uint64)pa < SUPERBASE ||
     (uint64)pa >= PHYSTOP){
    panic("superfree");
  }

  // 覆盖旧内容，帮助发现释放后继续使用的问题。
  memset(pa, 1, SUPERPGSIZE);

  r = (struct superrun *)pa;

  acquire(&supermem.lock);
  r->next = supermem.freelist;
  supermem.freelist = r;
  release(&supermem.lock);
}

// 分配一张 2 MB superpage。
void *
superalloc(void)
{
  struct superrun *r;

  acquire(&supermem.lock);
  r = supermem.freelist;

  if(r){
    supermem.freelist = r->next;
  }

  release(&supermem.lock);

  if(r){
    memset((char *)r, 5, SUPERPGSIZE);
  }

  return (void *)r;
}
