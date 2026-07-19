#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "net.h"

// xv6's ethernet and IP addresses.
static uint8 local_mac[ETHADDR_LEN] = {
  0x52, 0x54, 0x00, 0x12, 0x34, 0x56
};

static uint32 local_ip = MAKE_IP_ADDR(10, 0, 2, 15);

// QEMU host's ethernet address.
static uint8 host_mac[ETHADDR_LEN] = {
  0x52, 0x55, 0x0a, 0x00, 0x02, 0x02
};

static struct spinlock netlock;

/*
 * 最多同时保存的绑定端口数量。
 *
 * 官方只限制每个端口最多缓存 16 个数据包，
 * 没有限制端口表大小。当前实验测试使用的端口数量较少，
 * 32 个表项足够，并避免申请动态端口管理结构。
 */
#define MAX_BOUND_PORTS 32

/*
 * 官方要求每个端口最多缓存 16 个 UDP 数据包。
 */
#define MAX_UDP_QUEUE 16

/*
 * 一条已经接收、等待 recv() 读取的 UDP 数据报。
 *
 * packet_buf 保存完整的 Ethernet 帧缓冲区。
 * 该缓冲区由 E1000 接收驱动通过 kalloc() 分配，
 * 并在 recv() 处理完成或数据包被丢弃时释放。
 */
struct udp_datagram {
  char *packet_buf;
  int payload_len;
  uint32 src_ip;
  uint16 src_port;
};

/*
 * 一个已绑定端口及其 FIFO 接收队列。
 *
 * head：下一条要由 recv() 取出的数据包；
 * tail：下一条新数据包写入的位置；
 * count：当前排队数据包数量。
 */
struct udp_binding {
  int used;
  uint16 port;

  int head;
  int tail;
  int count;

  struct udp_datagram queue[MAX_UDP_QUEUE];
};

static struct udp_binding udp_bindings[MAX_BOUND_PORTS];

/*
 * 调用者必须持有 netlock。
 */
static struct udp_binding *
find_binding(uint16 port)
{
  for(int i = 0; i < MAX_BOUND_PORTS; i++){
    if(udp_bindings[i].used &&
       udp_bindings[i].port == port){
      return &udp_bindings[i];
    }
  }

  return 0;
}

void
netinit(void)
{
  initlock(&netlock, "netlock");
  memset(udp_bindings, 0, sizeof(udp_bindings));
}

//
// bind(int port)
// Prepare to receive UDP packets addressed to the port.
//
uint64
sys_bind(void)
{
  int port_arg;

  argint(0, &port_arg);

  if(port_arg < 0 || port_arg > 65535)
    return -1;

  uint16 port = (uint16)port_arg;

  acquire(&netlock);

  /*
   * 不允许重复绑定同一个端口。
   */
  if(find_binding(port) != 0){
    release(&netlock);
    return -1;
  }

  /*
   * 找一个空闲的绑定表项。
   */
  for(int i = 0; i < MAX_BOUND_PORTS; i++){
    if(!udp_bindings[i].used){
      struct udp_binding *binding = &udp_bindings[i];

      binding->used = 1;
      binding->port = port;
      binding->head = 0;
      binding->tail = 0;
      binding->count = 0;

      memset(binding->queue, 0, sizeof(binding->queue));

      release(&netlock);
      return 0;
    }
  }

  /*
   * 端口表已满。
   */
  release(&netlock);
  return -1;
}

//
// unbind(int port)
// Release any resources previously created by bind(port).
//
// 本实验测试不要求 unbind()，但这里实现完整清理逻辑。
//
uint64
sys_unbind(void)
{
  int port_arg;

  argint(0, &port_arg);

  if(port_arg < 0 || port_arg > 65535)
    return -1;

  uint16 port = (uint16)port_arg;

  acquire(&netlock);

  struct udp_binding *binding = find_binding(port);

  if(binding == 0){
    release(&netlock);
    return -1;
  }

  /*
   * 释放仍然排队、尚未被 recv() 读取的数据包。
   */
  while(binding->count > 0){
    struct udp_datagram *datagram =
      &binding->queue[binding->head];

    if(datagram->packet_buf != 0)
      kfree(datagram->packet_buf);

    memset(datagram, 0, sizeof(*datagram));

    binding->head =
      (binding->head + 1) % MAX_UDP_QUEUE;

    binding->count--;
  }

  binding->used = 0;
  binding->port = 0;
  binding->head = 0;
  binding->tail = 0;
  binding->count = 0;

  /*
   * 若有进程错误地在该端口上等待，让它重新检查条件。
   */
  wakeup(binding);

  release(&netlock);
  return 0;
}

//
// recv(int dport, int *src, short *sport, char *buf, int maxlen)
//
// If a UDP packet is already queued for dport, return it.
// Otherwise wait until such a packet arrives.
//
uint64
sys_recv(void)
{
  int dport_arg;
  uint64 src_addr;
  uint64 sport_addr;
  uint64 user_buf_addr;
  int maxlen;

  argint(0, &dport_arg);
  argaddr(1, &src_addr);
  argaddr(2, &sport_addr);
  argaddr(3, &user_buf_addr);
  argint(4, &maxlen);

  if(dport_arg < 0 || dport_arg > 65535)
    return -1;

  if(maxlen < 0)
    return -1;

  uint16 dport = (uint16)dport_arg;
  struct proc *p = myproc();

  /*
   * 先在全局锁下查找端口。
   */
  acquire(&netlock);

  struct udp_binding *binding = find_binding(dport);

  if(binding == 0){
    release(&netlock);
    return -1;
  }

  /*
   * 没有数据包时阻塞。
   *
   * sleep() 会原子地释放 netlock；
   * 被 wakeup() 唤醒后会重新取得 netlock。
   *
   * 必须使用 while，而不是 if，因为进程被唤醒后，
   * 数据包可能已经被另一个等待者取走。
   */
  while(binding->count == 0){
    if(killed(p)){
      release(&netlock);
      return -1;
    }

    /*
     * unbind() 可能在等待期间取消端口。
     */
    if(!binding->used || binding->port != dport){
      release(&netlock);
      return -1;
    }

    sleep(binding, &netlock);
  }

  /*
   * 从 FIFO 队列头取出最早到达的数据包。
   *
   * 先复制队列元数据到局部变量，再清空队列槽位，
   * 这样释放锁后其他数据包可以继续入队。
   */
  struct udp_datagram datagram =
    binding->queue[binding->head];

  memset(&binding->queue[binding->head],
         0,
         sizeof(binding->queue[binding->head]));

  binding->head =
    (binding->head + 1) % MAX_UDP_QUEUE;

  binding->count--;

  release(&netlock);

  /*
   * packet_buf 中保存：
   *
   * Ethernet header
   * IP header
   * UDP header
   * UDP payload
   */
  struct eth *eth =
    (struct eth *)datagram.packet_buf;

  struct ip *ip =
    (struct ip *)(eth + 1);

  struct udp *udp =
    (struct udp *)(ip + 1);

  char *payload =
    (char *)(udp + 1);

  int copy_len = datagram.payload_len;

  if(copy_len > maxlen)
    copy_len = maxlen;

  /*
   * 将源 IP 复制到用户地址 *src。
   * 队列中保存的已经是 Host Byte Order。
   */
  if(copyout(p->pagetable,
             src_addr,
             (char *)&datagram.src_ip,
             sizeof(datagram.src_ip)) < 0){
    kfree(datagram.packet_buf);
    return -1;
  }

  /*
   * 将源 UDP 端口复制到用户地址 *sport。
   * 队列中保存的已经是 Host Byte Order。
   */
  if(copyout(p->pagetable,
             sport_addr,
             (char *)&datagram.src_port,
             sizeof(datagram.src_port)) < 0){
    kfree(datagram.packet_buf);
    return -1;
  }

  /*
   * 最多复制 maxlen 字节的 UDP payload。
   */
  if(copy_len > 0){
    if(copyout(p->pagetable,
               user_buf_addr,
               payload,
               copy_len) < 0){
      kfree(datagram.packet_buf);
      return -1;
    }
  }

  /*
   * 数据包已从队列取出并完成用户态复制。
   * 释放 E1000 原接收缓冲区。
   */
  kfree(datagram.packet_buf);

  return copy_len;
}

// This code is lifted from FreeBSD's ping.c, and is copyright
// by the Regents of the University of California.
static unsigned short
in_cksum(const unsigned char *addr, int len)
{
  int nleft = len;
  const unsigned short *w = (const unsigned short *)addr;
  unsigned int sum = 0;
  unsigned short answer = 0;

  /*
   * Our algorithm is simple, using a 32 bit accumulator (sum),
   * we add sequential 16 bit words to it, and at the end,
   * fold back all the carry bits from the top 16 bits into
   * the lower 16 bits.
   */
  while(nleft > 1){
    sum += *w++;
    nleft -= 2;
  }

  /* Mop up an odd byte, if necessary. */
  if(nleft == 1){
    *(unsigned char *)(&answer) =
      *(const unsigned char *)w;
    sum += answer;
  }

  /* Add back carry outs from top 16 bits to low 16 bits. */
  sum = (sum & 0xffff) + (sum >> 16);
  sum += (sum >> 16);

  answer = ~sum;
  return answer;
}

//
// send(int sport, int dst, int dport, char *buf, int len)
//
uint64
sys_send(void)
{
  struct proc *p = myproc();
  int sport;
  int dst;
  int dport;
  uint64 bufaddr;
  int len;

  argint(0, &sport);
  argint(1, &dst);
  argint(2, &dport);
  argaddr(3, &bufaddr);
  argint(4, &len);

  if(len < 0)
    return -1;

  int total =
    len +
    sizeof(struct eth) +
    sizeof(struct ip) +
    sizeof(struct udp);

  if(total > PGSIZE)
    return -1;

  char *buf = kalloc();

  if(buf == 0){
    printf("sys_send: kalloc failed\n");
    return -1;
  }

  memset(buf, 0, PGSIZE);

  struct eth *eth = (struct eth *)buf;

  memmove(eth->dhost, host_mac, ETHADDR_LEN);
  memmove(eth->shost, local_mac, ETHADDR_LEN);

  eth->type = htons(ETHTYPE_IP);

  struct ip *ip = (struct ip *)(eth + 1);

  ip->ip_vhl = 0x45; // IPv4, header length 5 * 4 bytes.
  ip->ip_tos = 0;
  ip->ip_len =
    htons(sizeof(struct ip) + sizeof(struct udp) + len);
  ip->ip_id = 0;
  ip->ip_off = 0;
  ip->ip_ttl = 100;
  ip->ip_p = IPPROTO_UDP;
  ip->ip_src = htonl(local_ip);
  ip->ip_dst = htonl(dst);
  ip->ip_sum =
    in_cksum((unsigned char *)ip, sizeof(*ip));

  struct udp *udp =
    (struct udp *)(ip + 1);

  udp->sport = htons(sport);
  udp->dport = htons(dport);
  udp->ulen =
    htons(len + sizeof(struct udp));

  char *payload =
    (char *)(udp + 1);

  if(copyin(p->pagetable,
            payload,
            bufaddr,
            len) < 0){
    kfree(buf);
    printf("send: copyin failed\n");
    return -1;
  }

  /*
   * e1000_transmit() 成功后接管 buf 的所有权。
   * 如果发送环已满并返回失败，调用者仍需释放 buf。
   */
  if(e1000_transmit(buf, total) < 0){
    kfree(buf);
    return -1;
  }

  return 0;
}

void
ip_rx(char *buf, int len)
{
  // Don't delete this printf; make grade depends on it.
  static int seen_ip = 0;

  if(seen_ip == 0)
    printf("ip_rx: received an IP packet\n");

  seen_ip = 1;

  /*
   * 至少需要包含固定长度的 Ethernet、IPv4 和 UDP 头。
   */
  int minimum_len =
    sizeof(struct eth) +
    sizeof(struct ip) +
    sizeof(struct udp);

  if(len < minimum_len){
    kfree(buf);
    return;
  }

  struct eth *eth =
    (struct eth *)buf;

  struct ip *ip =
    (struct ip *)(eth + 1);

  /*
   * 当前实验只处理 IPv4 且 IP Header 长度为 20 字节。
   *
   * ip_vhl 的高 4 位是版本，低 4 位是 32 位字数量。
   * 0x45 表示 IPv4 和 5 * 4 = 20 字节 Header。
   */
  if(ip->ip_vhl != 0x45){
    kfree(buf);
    return;
  }

  /*
   * 只接收 UDP 数据包。
   */
  if(ip->ip_p != IPPROTO_UDP){
    kfree(buf);
    return;
  }

  /*
   * 只接收发送给 xv6 本机 IP 的数据包。
   */
  if(ntohl(ip->ip_dst) != local_ip){
    kfree(buf);
    return;
  }

  uint16 ip_len =
    ntohs(ip->ip_len);

  /*
   * IP 总长度包含 IP Header，但不包含 Ethernet Header。
   */
  if(ip_len <
       sizeof(struct ip) + sizeof(struct udp)){
    kfree(buf);
    return;
  }

  if((int)(sizeof(struct eth) + ip_len) > len){
    kfree(buf);
    return;
  }

  struct udp *udp =
    (struct udp *)(ip + 1);

  uint16 udp_len =
    ntohs(udp->ulen);

  /*
   * UDP 长度包括 UDP Header。
   */
  if(udp_len < sizeof(struct udp)){
    kfree(buf);
    return;
  }

  /*
   * UDP 数据长度不能超过 IP Payload 长度。
   */
  if(udp_len >
       ip_len - sizeof(struct ip)){
    kfree(buf);
    return;
  }

  int payload_len =
    udp_len - sizeof(struct udp);

  uint16 dport =
    ntohs(udp->dport);

  uint16 sport =
    ntohs(udp->sport);

  uint32 src_ip =
    ntohl(ip->ip_src);

  /*
   * 查找目标端口，并在全局网络锁保护下入队。
   */
  acquire(&netlock);

  struct udp_binding *binding =
    find_binding(dport);

  /*
   * 目标端口未 bind：直接丢弃。
   */
  if(binding == 0){
    release(&netlock);
    kfree(buf);
    return;
  }

  /*
   * 官方要求每个端口最多缓存 16 个包。
   * 当前端口满时，只丢弃该端口的新数据包，
   * 不影响其他端口。
   */
  if(binding->count >= MAX_UDP_QUEUE){
    release(&netlock);
    kfree(buf);
    return;
  }

  struct udp_datagram *slot =
    &binding->queue[binding->tail];

  slot->packet_buf = buf;
  slot->payload_len = payload_len;
  slot->src_ip = src_ip;
  slot->src_port = sport;

  binding->tail =
    (binding->tail + 1) % MAX_UDP_QUEUE;

  binding->count++;

  /*
   * 唤醒在 recv(dport, ...) 中等待这个端口的进程。
   *
   * sys_recv() 使用 binding 作为 sleep channel，
   * 因此这里必须使用同一个 binding 指针。
   */
  wakeup(binding);

  release(&netlock);

  /*
   * buf 已进入端口队列，不能在这里 kfree()。
   * 后续由 sys_recv() 或 sys_unbind() 释放。
   */
}

//
// Send an ARP reply packet to tell QEMU to map
// xv6's IP address to its ethernet address.
//
void
arp_rx(char *inbuf)
{
  static int seen_arp = 0;

  if(seen_arp){
    kfree(inbuf);
    return;
  }

  printf("arp_rx: received an ARP packet\n");
  seen_arp = 1;

  struct eth *ineth =
    (struct eth *)inbuf;

  struct arp *inarp =
    (struct arp *)(ineth + 1);

  char *buf = kalloc();

  if(buf == 0)
    panic("send_arp_reply");

  struct eth *eth =
    (struct eth *)buf;

  // Ethernet destination = query source.
  memmove(eth->dhost,
          ineth->shost,
          ETHADDR_LEN);

  // Ethernet source = xv6's ethernet address.
  memmove(eth->shost,
          local_mac,
          ETHADDR_LEN);

  eth->type = htons(ETHTYPE_ARP);

  struct arp *arp =
    (struct arp *)(eth + 1);

  arp->hrd = htons(ARP_HRD_ETHER);
  arp->pro = htons(ETHTYPE_IP);
  arp->hln = ETHADDR_LEN;
  arp->pln = sizeof(uint32);
  arp->op = htons(ARP_OP_REPLY);

  memmove(arp->sha,
          local_mac,
          ETHADDR_LEN);

  arp->sip = htonl(local_ip);

  memmove(arp->tha,
          ineth->shost,
          ETHADDR_LEN);

  arp->tip = inarp->sip;

  /*
   * e1000_transmit() 成功后接管回复缓冲区。
   * 失败时需要由当前函数释放。
   */
  if(e1000_transmit(buf,
                    sizeof(*eth) +
                    sizeof(*arp)) < 0){
    kfree(buf);
  }

  kfree(inbuf);
}

void
net_rx(char *buf, int len)
{
  struct eth *eth =
    (struct eth *)buf;

  if(len >=
       sizeof(struct eth) +
       sizeof(struct arp) &&
     ntohs(eth->type) == ETHTYPE_ARP){
    arp_rx(buf);
  } else if(len >=
              sizeof(struct eth) +
              sizeof(struct ip) &&
            ntohs(eth->type) == ETHTYPE_IP){
    ip_rx(buf, len);
  } else {
    kfree(buf);
  }
}
