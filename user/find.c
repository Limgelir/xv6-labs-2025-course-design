#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"
#include "user/user.h"

#define PATH_SIZE 512

// 返回路径中最后一个组成部分，也就是文件名。
// 例如：
// "./a/b"  -> "b"
// "README" -> "README"
char *
base_name(char *path)
{
  char *p;

  p = path + strlen(path);

  while(p > path && *(p - 1) != '/'){
    p--;
  }

  return p;
}

// 判断固定长度目录项名称是否等于目标名称。
//
// xv6 的 struct dirent.name 长度固定为 DIRSIZ，
// 它不一定以 '\0' 结尾，因此不能直接对 de.name 使用 strcmp。
int
dir_name_equal(char *dir_name, char *target)
{
  char name[DIRSIZ + 1];

  memmove(name, dir_name, DIRSIZ);
  name[DIRSIZ] = '\0';

  return strcmp(name, target) == 0;
}

// 判断目录项是否是 "." 或 ".."。
int
is_dot_directory(char *dir_name)
{
  return dir_name_equal(dir_name, ".") ||
         dir_name_equal(dir_name, "..");
}

// 递归查找。
void
find(char *path, char *target)
{
  int fd;
  char buffer[PATH_SIZE];
  char *p;
  struct dirent de;
  struct stat st;

  fd = open(path, O_RDONLY);
  if(fd < 0){
    fprintf(2, "find: cannot open %s\n", path);
    return;
  }

  if(fstat(fd, &st) < 0){
    fprintf(2, "find: cannot stat %s\n", path);
    close(fd);
    return;
  }

  switch(st.type){
  case T_FILE:
  case T_DEVICE:
    if(strcmp(base_name(path), target) == 0){
      printf("%s\n", path);
    }
    break;

  case T_DIR:
    // 预留：
    // 当前路径 + "/" + 最长目录项名称 + '\0'
    if(strlen(path) + 1 + DIRSIZ + 1 > sizeof(buffer)){
      fprintf(2, "find: path too long\n");
      break;
    }

    strcpy(buffer, path);
    p = buffer + strlen(buffer);

    // 避免根目录 "/" 拼接后变成 "//name"。
    if(p > buffer && *(p - 1) != '/'){
      *p++ = '/';
    }

    while(read(fd, &de, sizeof(de)) == sizeof(de)){
      // inode 编号为 0 表示该目录项未被使用。
      if(de.inum == 0){
        continue;
      }

      // 必须跳过 "." 和 ".."，否则会无限递归。
      if(is_dot_directory(de.name)){
        continue;
      }

      memmove(p, de.name, DIRSIZ);
      p[DIRSIZ] = '\0';

      find(buffer, target);
    }
    break;
  }

  close(fd);
}

int
main(int argc, char *argv[])
{
  if(argc != 3){
    fprintf(2, "Usage: find path name\n");
    exit(1);
  }

  find(argv[1], argv[2]);

  exit(0);
}
