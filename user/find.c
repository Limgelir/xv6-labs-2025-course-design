#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"
#include "kernel/param.h"
#include "user/user.h"

#define PATH_SIZE 512

// 返回路径的最后一个组成部分。
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

// 将固定长度的目录项名称转换为普通 C 字符串后比较。
int
dir_name_equal(char *dir_name, char *target)
{
  char name[DIRSIZ + 1];

  memmove(name, dir_name, DIRSIZ);
  name[DIRSIZ] = '\0';

  return strcmp(name, target) == 0;
}

// 判断是否为 "." 或 ".."。
int
is_dot_directory(char *dir_name)
{
  return dir_name_equal(dir_name, ".") ||
         dir_name_equal(dir_name, "..");
}

// 对一个匹配文件执行命令。
// command_argv 中保存命令及其原有参数，
// 本函数将文件路径追加到参数列表末尾。
void
execute_command(char *path, char **command_argv)
{
  int pid;
  int i;
  char *exec_argv[MAXARG];

  // 复制原命令及其参数。
  for(i = 0; command_argv[i] != 0; i++){
    if(i >= MAXARG - 2){
      fprintf(2, "find: too many exec arguments\n");
      return;
    }

    exec_argv[i] = command_argv[i];
  }

  // 将匹配文件路径放到最后一个参数位置。
  exec_argv[i] = path;
  exec_argv[i + 1] = 0;

  pid = fork();

  if(pid < 0){
    fprintf(2, "find: fork failed\n");
    return;
  }

  if(pid == 0){
    exec(exec_argv[0], exec_argv);

    // 只有 exec 失败时才会执行到这里。
    fprintf(2, "find: exec %s failed\n", exec_argv[0]);
    exit(1);
  }

  wait(0);
}

// 处理匹配到的文件。
void
handle_match(char *path, int exec_mode, char **command_argv)
{
  if(exec_mode){
    execute_command(path, command_argv);
  } else {
    printf("%s\n", path);
  }
}

// 递归查找。
void
find(char *path, char *target, int exec_mode, char **command_argv)
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
      handle_match(path, exec_mode, command_argv);
    }
    break;

  case T_DIR:
    if(strlen(path) + 1 + DIRSIZ + 1 > sizeof(buffer)){
      fprintf(2, "find: path too long\n");
      break;
    }

    strcpy(buffer, path);
    p = buffer + strlen(buffer);

    if(p > buffer && *(p - 1) != '/'){
      *p++ = '/';
    }

    while(read(fd, &de, sizeof(de)) == sizeof(de)){
      if(de.inum == 0){
        continue;
      }

      if(is_dot_directory(de.name)){
        continue;
      }

      memmove(p, de.name, DIRSIZ);
      p[DIRSIZ] = '\0';

      find(buffer, target, exec_mode, command_argv);
    }
    break;
  }

  close(fd);
}

int
main(int argc, char *argv[])
{
  int exec_mode = 0;
  char **command_argv = 0;

  if(argc == 3){
    // 普通 find 模式。
    exec_mode = 0;
  } else if(argc >= 5 && strcmp(argv[3], "-exec") == 0){
    // find -exec 模式。
    exec_mode = 1;
    command_argv = &argv[4];
  } else {
    fprintf(2, "Usage: find path name [-exec command args...]\n");
    exit(1);
  }

  find(argv[1], argv[2], exec_mode, command_argv);

  exit(0);
}
