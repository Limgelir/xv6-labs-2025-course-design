#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"

/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[];  // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S

// Make a direct-map page table for the kernel.
pagetable_t
kvmmake(void)
{
  pagetable_t kpgtbl;

  kpgtbl = (pagetable_t) kalloc();
  memset(kpgtbl, 0, PGSIZE);

  // uart registers
  kvmmap(kpgtbl, UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap(kpgtbl, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

#ifdef LAB_NET
  // PCI-E ECAM (configuration space), for pci.c
  kvmmap(kpgtbl, 0x30000000L, 0x30000000L, 0x10000000, PTE_R | PTE_W);

  // pci.c maps the e1000's registers here.
  kvmmap(kpgtbl, 0x40000000L, 0x40000000L, 0x20000, PTE_R | PTE_W);
#endif  

  // PLIC
  kvmmap(kpgtbl, PLIC, PLIC, 0x4000000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(kpgtbl, KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap(kpgtbl, (uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(kpgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

  // allocate and map a kernel stack for each process.
  proc_mapstacks(kpgtbl);
  
  return kpgtbl;
}

// Initialize the kernel_pagetable, shared by all CPUs.
void
kvminit(void)
{
  kernel_pagetable = kvmmake();
}

// Switch the current CPU's h/w page table register to
// the kernel's page table, and enable paging.
void
kvminithart()
{
  // wait for any previous writes to the page table memory to finish.
  sfence_vma();

  w_satp(MAKE_SATP(kernel_pagetable));

  // flush stale entries from the TLB.
  sfence_vma();
}

/*
 * walkinternal() 与原 walk() 类似，但会通过 level_out
 * 返回找到的叶子 PTE 位于哪一级。
 *
 * level = 0：普通 4 KB 页
 * level = 1：2 MB superpage
 */
static pte_t *
walkinternal(pagetable_t pagetable, uint64 va, int alloc,
             int *level_out)
{
  if(va >= MAXVA){
    panic("walk");
  }

  for(int level = 2; level > 0; level--){
    pte_t *pte = &pagetable[PX(level, va)];

    if(*pte & PTE_V){
#ifdef LAB_PGTBL
      /*
       * 若当前 PTE 已经是叶子项，则不能继续把物理页
       * 当作下一级页表。
       *
       * Level-1 叶子就是 2 MB superpage。
       */
      if(PTE_LEAF(*pte)){
        if(level_out != 0){
          *level_out = level;
        }

        return pte;
      }
#endif

      pagetable = (pagetable_t)PTE2PA(*pte);
    } else {
      if(!alloc ||
         (pagetable = (pagetable_t)kalloc()) == 0){
        return 0;
      }

      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }

  if(level_out != 0){
    *level_out = 0;
  }

  return &pagetable[PX(0, va)];
}

/*
 * 保留原来的 walk() 接口，避免修改所有调用者。
 */
pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
{
  return walkinternal(pagetable, va, alloc, 0);
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  uint64 pa;
  int level;

  if(va >= MAXVA){
    return 0;
  }

  pte = walkinternal(pagetable, va, 0, &level);

  if(pte == 0){
    return 0;
  }

  if((*pte & PTE_V) == 0){
    return 0;
  }

  if((*pte & PTE_U) == 0){
    return 0;
  }

  pa = PTE2PA(*pte);

  /*
   * 普通 Level-0 PTE：
   * PTE2PA() 已经是当前 4 KB 页起始地址。
   *
   * Level-1 superpage：
   * PTE2PA() 是整张 2 MB 页的起始地址，
   * 还必须加入 va 在 superpage 内部的偏移。
   */
  if(level == 1){
    pa += va & (SUPERPGSIZE - 1);

    /*
     * copyin/copyout 后面还会加入页内偏移，
     * 因此这里返回当前 4 KB 子页的物理起点。
     */
    pa = PGROUNDDOWN(pa);
  }

  return pa;
}


#if defined(LAB_PGTBL) || defined(SOL_MMAP) || defined(SOL_COW)

static void
vmprint_recursive(pagetable_t pagetable, int level, uint64 base_va)
{
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];

    if((pte & PTE_V) == 0){
      continue;
    }

    uint64 pa = PTE2PA(pte);
    uint64 va = base_va | ((uint64)i << PXSHIFT(level));

    for(int depth = 0; depth < 3 - level; depth++){
      printf(" ..");
    }

    printf("%p: pte %p pa %p\n",
           (void *)va,
           (void *)pte,
           (void *)pa);

    if(level > 0 &&
       (pte & (PTE_R | PTE_W | PTE_X)) == 0){
      vmprint_recursive((pagetable_t)pa, level - 1, va);
    }
  }
}

void
vmprint(pagetable_t pagetable)
{
  printf("page table %p\n", (void *)pagetable);
  vmprint_recursive(pagetable, 2, 0);
}

#endif

/*
 * 返回虚拟地址 va 对应的 Level-1 PTE。
 *
 * 普通 walk() 会一直走到 Level-0；
 * superpage 需要直接在 Level-1 建立叶子 PTE。
 */
static pte_t *
walksuper(pagetable_t pagetable, uint64 va, int alloc)
{
  pte_t *pte;

  if(va >= MAXVA){
    panic("walksuper");
  }

  /*
   * 先查看 Level-2 PTE。
   * Level-2 索引对应 Sv39 地址中的 VPN[2]。
   */
  pte = &pagetable[PX(2, va)];

  if(*pte & PTE_V){
    /*
     * 如果 Level-2 本身已经是叶子项，则不能在它下面
     * 建立 Level-1 页表。
     */
    if(PTE_LEAF(*pte)){
      return 0;
    }

    pagetable = (pagetable_t)PTE2PA(*pte);
  } else {
    if(!alloc ||
       (pagetable = (pagetable_t)kalloc()) == 0){
      return 0;
    }

    memset(pagetable, 0, PGSIZE);
    *pte = PA2PTE(pagetable) | PTE_V;
  }

  /*
   * 当前 pagetable 已经是 Level-1 页表页。
   */
  return &pagetable[PX(1, va)];
}

/*
 * 在 Level-1 建立一张 2 MB superpage 映射。
 */
static int
supermappage(pagetable_t pagetable, uint64 va,
             uint64 pa, int perm)
{
  pte_t *pte;

  if((va % SUPERPGSIZE) != 0){
    panic("supermappage: va");
  }

  if((pa % SUPERPGSIZE) != 0){
    panic("supermappage: pa");
  }

  pte = walksuper(pagetable, va, 1);

  if(pte == 0){
    return -1;
  }

  if(*pte & PTE_V){
    panic("supermappage: remap");
  }

  /*
   * 只要 Level-1 PTE 同时具有 PTE_V 和至少一个 R/W/X
   * 权限，硬件就会把它视为 2 MB 叶子映射。
   */
  *pte = PA2PTE(pa) | perm | PTE_V;

  return 0;
}

// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
void
kvmmap(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, int perm)
{
  if(mappages(kpgtbl, va, sz, pa, perm) != 0)
    panic("kvmmap");
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa.
// va and size MUST be page-aligned.
// Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
int
mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
  uint64 a, last;
  pte_t *pte;

  if((va % PGSIZE) != 0)
    panic("mappages: va not aligned");

  if((size % PGSIZE) != 0)
    panic("mappages: size not aligned");

  if(size == 0)
    panic("mappages: size");
  
  a = va;
  last = va + size - PGSIZE;
  for(;;){
    if((pte = walk(pagetable, a, 1)) == 0)
      return -1;
    if(*pte & PTE_V)
      panic("mappages: remap");
    *pte = PA2PTE(pa) | perm | PTE_V;
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// create an empty user page table.
// returns 0 if out of memory.
pagetable_t
uvmcreate()
{
  pagetable_t pagetable;
  pagetable = (pagetable_t) kalloc();
  if(pagetable == 0)
    return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

/*
 * 将 Level-1 superpage 降级为 512 张普通 4 KB 页面。
 *
 * 转换前：
 *   Level-1 叶子 PTE → 一张 2 MB 物理页
 *
 * 转换后：
 *   Level-1 非叶子 PTE → Level-0 页表
 *   Level-0 页表中有 512 个普通叶子 PTE
 */
static int
demote_superpage(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  pte_t oldpte;
  uint64 oldpa;
  uint flags;
  pagetable_t newtable;
  int level;

  pte = walkinternal(pagetable, va, 0, &level);

  if(pte == 0 ||
     level != 1 ||
     !PTE_LEAF(*pte)){
    return -1;
  }

  oldpte = *pte;
  oldpa = PTE2PA(oldpte);
  flags = PTE_FLAGS(oldpte);

  /*
   * 分配一张新的 Level-0 页表。
   */
  newtable = (pagetable_t)kalloc();

  if(newtable == 0){
    return -1;
  }

  memset(newtable, 0, PGSIZE);

  /*
   * 为原 superpage 的每个 4 KB 区域分配普通物理页，
   * 并复制原有内容。
   */
  for(int i = 0; i < 512; i++){
    char *mem = kalloc();

    if(mem == 0){
      /*
       * 分配失败时，释放已经创建的普通页，
       * 原 superpage 映射暂时保持不变。
       */
      for(int j = 0; j < i; j++){
        if(newtable[j] & PTE_V){
          kfree((void *)PTE2PA(newtable[j]));
          newtable[j] = 0;
        }
      }

      kfree(newtable);
      return -1;
    }

    memmove(mem,
            (void *)(oldpa + (uint64)i * PGSIZE),
            PGSIZE);

    /*
     * 保留原 superpage 的用户、读写等权限。
     * flags 中已经包含 PTE_V。
     */
    newtable[i] = PA2PTE(mem) | flags;
  }

  /*
   * 将原来的 Level-1 叶子 PTE 改成非叶子 PTE，
   * 指向刚刚创建的 Level-0 页表。
   */
  *pte = PA2PTE(newtable) | PTE_V;

  /*
   * 新普通页已经保存全部内容，因此可以释放旧 superpage。
   */
  superfree((void *)oldpa);

  // 页表结构改变后刷新 TLB。
  sfence_vma();

  return 0;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. It's OK if the mappings don't exist.
// Optionally free the physical memory.
void
uvmunmap(pagetable_t pagetable, uint64 va,
         uint64 npages, int do_free)
{
  uint64 a;
  uint64 endva;

  if((va % PGSIZE) != 0){
    panic("uvmunmap: not aligned");
  }

  a = va;
  endva = va + npages * PGSIZE;

  while(a < endva){
    pte_t *pte;
    int level;

    pte = walkinternal(pagetable, a, 0, &level);

    /*
     * 保留当前实验分支允许“未映射页”的行为。
     */
    if(pte == 0 || (*pte & PTE_V) == 0){
      a += PGSIZE;
      continue;
    }

    if(!PTE_LEAF(*pte)){
      panic("uvmunmap: not a leaf");
    }

    /*
     * 当前地址位于 Level-1 superpage。
     */
    if(level == 1){
      uint64 superbase;

      superbase =
          a & ~((uint64)SUPERPGSIZE - 1);

      /*
       * 如果当前释放从 superpage 起点开始，
       * 且后续范围完整覆盖 2 MB，就直接释放整张页。
       */
      if(a == superbase &&
         endva - a >= SUPERPGSIZE){
        if(do_free){
          superfree((void *)PTE2PA(*pte));
        }

        *pte = 0;
        a += SUPERPGSIZE;
        continue;
      }

      /*
       * 仅释放 superpage 的一部分：
       * 先降级，再重新处理同一个虚拟地址。
       */
      if(demote_superpage(pagetable, a) < 0){
        panic("uvmunmap: demote");
      }

      /*
       * 不改变 a。
       * 下一轮重新 walk 后，会得到普通 Level-0 PTE。
       */
      continue;
    }

    /*
     * 普通 4 KB 页面。
     */
    if(do_free){
      uint64 pa = PTE2PA(*pte);
      kfree((void *)pa);
    }

    *pte = 0;
    a += PGSIZE;
  }

  sfence_vma();
}


// Allocate PTEs and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz,
         uint64 newsz, int xperm)
{
  char *mem;
  uint64 a;
  int sz;

  if(newsz < oldsz){
    return oldsz;
  }

  oldsz = PGROUNDUP(oldsz);

  /*
   * 每轮可能增加：
   * - 4 KB 普通页面；
   * - 2 MB superpage。
   */
  for(a = oldsz; a < newsz; a += sz){
    /*
     * 使用 superpage 必须同时满足：
     *
     * 1. 当前虚拟地址按 2 MB 对齐；
     * 2. 剩余待分配范围至少还有 2 MB。
     */
    if((a % SUPERPGSIZE) == 0 &&
       newsz - a >= SUPERPGSIZE){
      sz = SUPERPGSIZE;

      mem = superalloc();

      if(mem == 0){
        /*
         * 如果 superpage 池已经用完，也可以退回普通页。
         * 为简化和保证当前测试稳定，这里直接清理并失败。
         */
        uvmdealloc(pagetable, a, oldsz);
        return 0;
      }

      memset(mem, 0, SUPERPGSIZE);

      if(supermappage(pagetable,
                      a,
                      (uint64)mem,
                      PTE_R | PTE_U | xperm) != 0){
        superfree(mem);
        uvmdealloc(pagetable, a, oldsz);
        return 0;
      }
    } else {
      sz = PGSIZE;

      mem = kalloc();

      if(mem == 0){
        uvmdealloc(pagetable, a, oldsz);
        return 0;
      }

      memset(mem, 0, PGSIZE);

      if(mappages(pagetable,
                  a,
                  PGSIZE,
                  (uint64)mem,
                  PTE_R | PTE_U | xperm) != 0){
        kfree(mem);
        uvmdealloc(pagetable, a, oldsz);
        return 0;
      }
    }
  }

  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if(newsz >= oldsz)
    return oldsz;

  if(PGROUNDUP(newsz) < PGROUNDUP(oldsz)){
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
void
freewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    if((pte & PTE_V) && (pte & (PTE_R|PTE_W|PTE_X)) == 0){
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    } else if(pte & PTE_V){
      // backtrace();
      panic("freewalk: leaf");
    }
  }
  kfree((void*)pagetable);
}

// Free user memory pages,
// then free page-table pages.
void
uvmfree(pagetable_t pagetable, uint64 sz)
{
  if(sz > 0)
    uvmunmap(pagetable, 0, PGROUNDUP(sz)/PGSIZE, 1);
  freewalk(pagetable);
}

// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  pte_t *pte;
  uint64 pa;
  uint64 i;
  uint flags;
  char *mem;
  int level;

  for(i = 0; i < sz; ){
    /*
     * walkinternal() 不仅返回 PTE，
     * 还通过 level 告诉我们它是：
     *
     * level == 0：普通 4 KB 页面；
     * level == 1：2 MB superpage。
     */
    pte = walkinternal(old, i, 0, &level);

    /*
     * 当前 xv6 允许地址空间中存在未映射区域，
     * 因此直接跳过。
     */
    if(pte == 0 || (*pte & PTE_V) == 0){
      i += PGSIZE;
      continue;
    }

    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);

    if(level == 1){
      /*
       * Level-1 叶子代表一张 2 MB superpage。
       * 起始虚拟地址应当按 2 MB 对齐。
       */
      if((i % SUPERPGSIZE) != 0){
        panic("uvmcopy: super alignment");
      }

      /*
       * 正常情况下，进程大小应包含完整的 superpage。
       */
      if(i + SUPERPGSIZE > sz){
        panic("uvmcopy: partial superpage");
      }

      /*
       * 为子进程重新分配一张独立的 2 MB 物理页。
       */
      mem = superalloc();
      if(mem == 0){
        goto err;
      }

      /*
       * 复制完整的 2 MB 内容。
       */
      memmove(mem, (void *)pa, SUPERPGSIZE);

      /*
       * 在子进程页表中同样创建 Level-1 叶子 PTE。
       *
       * supermappage() 会自己添加 PTE_V，
       * 所以这里先去除已有的 PTE_V。
       */
      if(supermappage(new,
                      i,
                      (uint64)mem,
                      flags & ~PTE_V) != 0){
        superfree(mem);
        goto err;
      }

      /*
       * 一次跳过完整的 2 MB。
       */
      i += SUPERPGSIZE;
    } else {
      /*
       * 普通 4 KB 页面保持 xv6 原来的复制方式。
       */
      mem = kalloc();
      if(mem == 0){
        goto err;
      }

      memmove(mem, (void *)pa, PGSIZE);

      if(mappages(new,
                  i,
                  PGSIZE,
                  (uint64)mem,
                  flags) != 0){
        kfree(mem);
        goto err;
      }

      i += PGSIZE;
    }
  }

  return 0;

err:
  /*
   * 你的 uvmunmap() 已经支持普通页和 superpage，
   * 因此这里可以统一清理。
   */
  uvmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
void
uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  
  pte = walk(pagetable, va, 0);
  if(pte == 0)
    panic("uvmclear");
  *pte &= ~PTE_U;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int
copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;
  pte_t *pte;

  while(len > 0){
    va0 = PGROUNDDOWN(dstva);
    if (va0 >= MAXVA)
      return -1;

    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0) {
      if((pa0 = vmfault(pagetable, va0, 0)) == 0) {
        return -1;
      }
    }

    if((pte = walk(pagetable, va0, 0)) == 0) {
      // printf("copyout: pte should exist %lx %ld\n", dstva, len);
      return -1;
    }


    // forbid copyout over read-only user text pages.
    if((*pte & PTE_W) == 0)
      return -1;
    
    n = PGSIZE - (dstva - va0);
    if(n > len)
      n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int
copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
  uint64 n, va0, pa0;
  
  while(len > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0) {
      if((pa0 = vmfault(pagetable, va0, 0)) == 0) {
        return -1;
      }
    }
    n = PGSIZE - (srcva - va0);
    if(n > len)
      n = len;
    memmove(dst, (void *)(pa0 + (srcva - va0)), n);

    len -= n;
    dst += n;
    srcva = va0 + PGSIZE;
  }
  return 0;
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int
copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
  uint64 n, va0, pa0;
  int got_null = 0;

  while(got_null == 0 && max > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if(n > max)
      n = max;

    char *p = (char *) (pa0 + (srcva - va0));
    while(n > 0){
      if(*p == '\0'){
        *dst = '\0';
        got_null = 1;
        break;
      } else {
        *dst = *p;
      }
      --n;
      --max;
      p++;
      dst++;
    }

    srcva = va0 + PGSIZE;
  }
  if(got_null){
    return 0;
  } else {
    return -1;
  }
}




// allocate and map user memory if process is referencing a page
// that was lazily allocated in sys_sbrk().
// returns 0 if va is invalid or already mapped, or if
// out of physical memory, and physical address if successful.
uint64
vmfault(pagetable_t pagetable, uint64 va, int read)
{
  uint64 mem;
  struct proc *p = myproc();
  

  if (va >= p->sz)
    return 0;
  va = PGROUNDDOWN(va);
  if(ismapped(pagetable, va)) {
    return 0;
  }
  mem = (uint64) kalloc();
  if(mem == 0)
    return 0;
  memset((void *) mem, 0, PGSIZE);
  if (mappages(p->pagetable, va, PGSIZE, mem, PTE_W|PTE_U|PTE_R) != 0) {
    kfree((void *)mem);
    return 0;
  }
  return mem;
}

int
ismapped(pagetable_t pagetable, uint64 va) {
  pte_t *pte = walk(pagetable, va, 0);
  if (pte == 0) {
    return 0;
  }
  if (*pte & PTE_V){
    return 1;
  }
  return 0;
}



#ifdef LAB_PGTBL
pte_t*
pgpte(pagetable_t pagetable, uint64 va) {
  return walk(pagetable, va, 0);
}
#endif
