#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "e1000_dev.h"

#define TX_RING_SIZE 16

static struct tx_desc tx_ring[TX_RING_SIZE]
  __attribute__((aligned(16)));

/*
 * 保存每个 TX descriptor 当前使用的发送缓冲区。
 *
 * E1000 通过 DMA 读取这些缓冲区，所以发送后不能立刻释放。
 * 必须等硬件设置 DD，说明发送完成后，才能在下一次复用
 * 该 descriptor 时释放旧缓冲区。
 */
static char *tx_bufs[TX_RING_SIZE];

#define RX_RING_SIZE 16

static struct rx_desc rx_ring[RX_RING_SIZE]
  __attribute__((aligned(16)));

// Remember where the e1000's registers live.
static volatile uint32 *regs;

/*
 * 当前阶段使用一把锁保护 E1000 发送描述符环。
 *
 * 后续实现接收函数时，可以继续使用这把锁，但调用 net_rx()
 * 前必须释放锁；也可以再拆分为 TX 锁和 RX 锁。
 */
struct spinlock e1000_lock;

// Called by pci_init().
// xregs is the memory address at which the
// e1000's registers are mapped.
// This code loosely follows the initialization directions
// in Chapter 14 of Intel's Software Developer's Manual.
void
e1000_init(uint32 *xregs)
{
  int i;

  initlock(&e1000_lock, "e1000");

  regs = xregs;

  // Reset the device.
  regs[E1000_IMS] = 0; // Disable interrupts.
  regs[E1000_CTL] |= E1000_CTL_RST;
  regs[E1000_IMS] = 0; // Redisable interrupts.
  __sync_synchronize();

  // [E1000 14.5] Transmit initialization.
  memset(tx_ring, 0, sizeof(tx_ring));
  memset(tx_bufs, 0, sizeof(tx_bufs));

  for(i = 0; i < TX_RING_SIZE; i++){
    /*
     * DD 表示这个 descriptor 当前可以被驱动使用。
     * 初始时所有 TX descriptor 都应当处于空闲状态。
     */
    tx_ring[i].status = E1000_TXD_STAT_DD;
    tx_ring[i].addr = 0;
    tx_bufs[i] = 0;
  }

  regs[E1000_TDBAL] = (uint64)tx_ring;

  if(sizeof(tx_ring) % 128 != 0)
    panic("e1000");

  regs[E1000_TDLEN] = sizeof(tx_ring);
  regs[E1000_TDH] = 0;
  regs[E1000_TDT] = 0;

  // [E1000 14.4] Receive initialization.
  memset(rx_ring, 0, sizeof(rx_ring));

  for(i = 0; i < RX_RING_SIZE; i++){
    rx_ring[i].addr = (uint64)kalloc();

    if(!rx_ring[i].addr)
      panic("e1000");
  }

  regs[E1000_RDBAL] = (uint64)rx_ring;

  if(sizeof(rx_ring) % 128 != 0)
    panic("e1000");

  regs[E1000_RDH] = 0;
  regs[E1000_RDT] = RX_RING_SIZE - 1;
  regs[E1000_RDLEN] = sizeof(rx_ring);

  // Filter by qemu's MAC address, 52:54:00:12:34:56.
  regs[E1000_RA] = 0x12005452;
  regs[E1000_RA + 1] = 0x5634 | (1 << 31);

  // Multicast table.
  for(int i = 0; i < 4096 / 32; i++)
    regs[E1000_MTA + i] = 0;

  // Transmitter control bits.
  regs[E1000_TCTL] =
    E1000_TCTL_EN |                    // Enable transmitter.
    E1000_TCTL_PSP |                   // Pad short packets.
    (0x10 << E1000_TCTL_CT_SHIFT) |   // Collision threshold.
    (0x40 << E1000_TCTL_COLD_SHIFT);  // Collision distance.

  regs[E1000_TIPG] =
    10 |
    (8 << 10) |
    (6 << 20); // Inter-packet gap.

  // Receiver control bits.
  regs[E1000_RCTL] =
    E1000_RCTL_EN |       // Enable receiver.
    E1000_RCTL_BAM |      // Enable broadcast.
    E1000_RCTL_SZ_2048 |  // 2048-byte RX buffers.
    E1000_RCTL_SECRC;     // Strip CRC.

  // Ask e1000 for receive interrupts.
  regs[E1000_RDTR] = 0;       // Interrupt after every received packet.
  regs[E1000_RADV] = 0;       // No absolute receive delay.
  regs[E1000_IMS] = (1 << 7); // RXDW: Receiver Descriptor Write Back.
}

int
e1000_transmit(char *buf, int len)
{
  uint32 index;
  struct tx_desc *desc;

  /*
   * 基本参数检查。
   *
   * buf 必须指向一个由网络协议栈准备好的数据包缓冲区。
   * 长度必须为正，并且不能超过一页。
   */
  if(buf == 0 || len <= 0 || len > PGSIZE){
    if(buf != 0)
      kfree(buf);

    return -1;
  }

  /*
   * 多个进程可能同时调用 e1000_transmit()，
   * 因此 TX ring 和 TDT 寄存器必须由锁保护。
   */
  acquire(&e1000_lock);

  /*
   * TDT 指向驱动下一次应该填写的 TX descriptor。
   */
  index = regs[E1000_TDT];

  if(index >= TX_RING_SIZE){
    release(&e1000_lock);
    panic("e1000 transmit TDT");
  }

  desc = &tx_ring[index];

  /*
   * DD：Descriptor Done。
   *
   * 如果 DD 没有设置，说明 E1000 还没有完成这个
   * descriptor 上一次对应的数据包发送。
   *
   * 此时 TX ring 在当前位置已满，不能覆盖 descriptor。
   */
  if((desc->status & E1000_TXD_STAT_DD) == 0){
    release(&e1000_lock);

    /*
     * 当前发送请求未被驱动接收，因此缓冲区仍由调用者负责。
     * 不在这里释放 buf。
     */
    return -1;
  }

  /*
   * descriptor 已经发送完成，因此可以释放其上一次使用的
   * 数据包缓冲区。
   *
   * 第一次使用时 tx_bufs[index] 为 0，不会释放。
   */
  if(tx_bufs[index] != 0){
    kfree(tx_bufs[index]);
    tx_bufs[index] = 0;
  }

  /*
   * 保存新缓冲区。
   *
   * 不能在这里立即释放，因为 E1000 接下来还要通过 DMA
   * 读取该缓冲区中的数据。
   */
  tx_bufs[index] = buf;

  /*
   * xv6 内核对物理内存使用直接映射。
   * 这些由 kalloc() 分配的地址可以直接作为 DMA 地址。
   */
  desc->addr = (uint64)buf;
  desc->length = len;

  /*
   * EOP：End Of Packet。
   * 当前实验一个数据包只使用一个 descriptor，因此这个
   * descriptor 同时也是数据包最后一个 descriptor。
   *
   * RS：Report Status。
   * 要求 E1000 在发送完成后更新 descriptor 的 status，
   * 从而重新设置 DD。
   */
  desc->cmd =
    E1000_TXD_CMD_EOP |
    E1000_TXD_CMD_RS;

  /*
   * 清除以前的完成状态。
   *
   * 硬件完成发送后会重新设置 DD。
   */
  desc->status = 0;

  /*
   * 确保 descriptor 的各字段在更新 TDT 前已经写入内存，
   * 避免硬件看到尚未完整填写的 descriptor。
   */
  __sync_synchronize();

  /*
   * 推进发送环 Tail，通知 E1000 新 descriptor 已准备好。
   *
   * TX ring 大小为 16，因此从索引 15 后回绕到 0。
   */
  regs[E1000_TDT] =
    (index + 1) % TX_RING_SIZE;

  release(&e1000_lock);

  return 0;
}

static void
e1000_recv(void)
{
  for(;;){
    uint32 index;
    struct rx_desc *desc;
    char *oldbuf;
    char *newbuf;
    int len;

    /*
     * RX ring 和 E1000_RDT 由网卡中断处理函数访问，
     * 使用锁保护 descriptor 状态和寄存器更新。
     */
    acquire(&e1000_lock);

    /*
     * RDT 指向驱动最后处理完并交还给硬件的 descriptor。
     * 因此下一个可能包含新数据包的位置是 RDT + 1。
     */
    index = (regs[E1000_RDT] + 1) % RX_RING_SIZE;

    if(index >= RX_RING_SIZE){
      release(&e1000_lock);
      panic("e1000 recv RDT");
    }

    desc = &rx_ring[index];

    /*
     * DD 表示 Descriptor Done。
     *
     * 如果 DD 没有设置，说明该位置当前没有新的数据包，
     * 本次接收处理结束。
     */
    if((desc->status & E1000_RXD_STAT_DD) == 0){
      release(&e1000_lock);
      break;
    }

    /*
     * 保存 E1000 已经填充完成的旧缓冲区及数据长度。
     */
    oldbuf = (char *)desc->addr;
    len = desc->length;

    /*
     * 为该 RX descriptor 分配新的缓冲区。
     *
     * 旧缓冲区马上要交给网络协议栈，不能继续留给 E1000。
     */
    newbuf = kalloc();

    if(newbuf == 0){
      /*
       * 无法补充接收缓冲区时，不能把旧缓冲区交出去。
       * 保留当前 descriptor，等待后续再次处理。
       */
      release(&e1000_lock);
      break;
    }

    /*
     * 将新缓冲区配置到 descriptor 中，
     * 供 E1000 下一次环回到该位置时继续 DMA。
     */
    desc->addr = (uint64)newbuf;
    desc->special = 0;

    /*
     * 清除 DD 等状态位，表示该 descriptor 已重新可供硬件使用。
     */
    desc->status = 0;

    /*
     * 确保 descriptor 的更新先写入内存，
     * 再通知 E1000。
     */
    __sync_synchronize();

    /*
     * 更新 RDT，告诉 E1000：
     * index 位置的 descriptor 已经重新准备完成。
     */
    regs[E1000_RDT] = index;

    /*
     * 调用 net_rx() 前必须释放锁。
     *
     * net_rx() 处理 ARP 请求时可能调用 e1000_transmit()
     * 发送 ARP Reply，而发送函数也需要 e1000_lock。
     */
    release(&e1000_lock);

    /*
     * 网络协议栈接管 oldbuf 的所有权。
     * 驱动之后不能再访问或释放 oldbuf。
     */
    net_rx(oldbuf, len);
  }
}

void
e1000_intr(void)
{
  // Tell the e1000 we've seen this interrupt;
  // without this the e1000 won't raise any
  // further interrupts.
  regs[E1000_ICR] = 0xffffffff;

  e1000_recv();
}
