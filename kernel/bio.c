// Buffer cache.
//
// Modified for MIT 6.S081 Lock Lab.
// Use multiple bucket locks to reduce contention.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"


#define NBUCKET 13


struct bucket {
  struct spinlock lock;
  struct buf head;
};


struct bucket buckets[NBUCKET];


struct buf buf[NBUF];



static int
hash(uint blockno)
{
  return blockno % NBUCKET;
}



// Initialize buffer cache.
void
binit(void)
{
  struct buf *b;


  for(int i = 0; i < NBUCKET; i++){

    initlock(&buckets[i].lock, "bcache");


    buckets[i].head.prev = &buckets[i].head;
    buckets[i].head.next = &buckets[i].head;
  }



  for(b = buf; b < buf + NBUF; b++){

    initsleeplock(&b->lock, "buffer");


    // Initially put all buffers in bucket 0.

    b->next = buckets[0].head.next;
    b->prev = &buckets[0].head;


    buckets[0].head.next->prev = b;
    buckets[0].head.next = b;
  }
}



// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;


  int id = hash(blockno);



  /*
   * First check target bucket
   */

  acquire(&buckets[id].lock);



  for(b = buckets[id].head.next;
      b != &buckets[id].head;
      b = b->next){


    if(b->dev == dev &&
       b->blockno == blockno){


      b->refcnt++;


      release(&buckets[id].lock);


      acquiresleep(&b->lock);


      return b;
    }
  }



  /*
   * Find unused buffer in target bucket
   */

  for(b = buckets[id].head.prev;
      b != &buckets[id].head;
      b = b->prev){


    if(b->refcnt == 0){


      b->dev = dev;
      b->blockno = blockno;
      b->valid = 0;
      b->refcnt = 1;


      release(&buckets[id].lock);


      acquiresleep(&b->lock);


      return b;
    }
  }



  release(&buckets[id].lock);



  /*
   * Steal buffer from other buckets
   */

  for(int i = 0; i < NBUCKET; i++){


    if(i == id)
      continue;


    acquire(&buckets[i].lock);



    for(b = buckets[i].head.prev;
        b != &buckets[i].head;
        b = b->prev){


      if(b->refcnt == 0){


        /*
         * Remove from old bucket
         */

        b->prev->next = b->next;
        b->next->prev = b->prev;



        /*
         * Insert into target bucket
         */

        b->next = buckets[id].head.next;
        b->prev = &buckets[id].head;


        buckets[id].head.next->prev = b;
        buckets[id].head.next = b;



        b->dev = dev;
        b->blockno = blockno;
        b->valid = 0;
        b->refcnt = 1;



        release(&buckets[i].lock);


        acquiresleep(&b->lock);


        return b;
      }
    }


    release(&buckets[i].lock);
  }


  panic("bget: no buffers");
}



// Return buffer with contents.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;


  b = bget(dev, blockno);


  if(!b->valid){

    virtio_disk_rw(b, 0);

    b->valid = 1;
  }


  return b;
}



// Write buffer.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");


  virtio_disk_rw(b, 1);
}



// Release buffer.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");


  releasesleep(&b->lock);


  int id = hash(b->blockno);


  acquire(&buckets[id].lock);



  b->refcnt--;


  if(b->refcnt == 0){


    /*
     * Move to MRU head
     */


    b->next->prev = b->prev;
    b->prev->next = b->next;


    b->next = buckets[id].head.next;
    b->prev = &buckets[id].head;


    buckets[id].head.next->prev = b;
    buckets[id].head.next = b;
  }



  release(&buckets[id].lock);
}



void
bpin(struct buf *b)
{
  int id = hash(b->blockno);


  acquire(&buckets[id].lock);


  b->refcnt++;


  release(&buckets[id].lock);
}



void
bunpin(struct buf *b)
{
  int id = hash(b->blockno);


  acquire(&buckets[id].lock);


  b->refcnt--;


  release(&buckets[id].lock);
}
