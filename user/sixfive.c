#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "user/user.h"

#define TOKEN_SIZE 128

// 判断字符是否为十进制数字。
int
is_digit(char ch)
{
  return ch >= '0' && ch <= '9';
}

// 处理一个由分隔符切分出来的 token。
// 只有 token 中的全部字符都是数字时，才把它看作整数。
void
process_token(char *token, int length)
{
  int i;
  int value = 0;

  if(length == 0){
    return;
  }

  for(i = 0; i < length; i++){
    if(!is_digit(token[i])){
      return;
    }

    value = value * 10 + token[i] - '0';
  }

  if(value % 5 == 0 || value % 6 == 0){
    printf("%d\n", value);
  }
}

// 读取并处理一个文件。
void
sixfive_file(char *filename)
{
  int fd;
  int n;
  int token_length = 0;
  char ch;
  char token[TOKEN_SIZE];
  char *separators = " -\r\t\n./,";

  fd = open(filename, O_RDONLY);
  if(fd < 0){
    fprintf(2, "sixfive: cannot open %s\n", filename);
    return;
  }

  while((n = read(fd, &ch, 1)) > 0){
    if(strchr(separators, ch) != 0){
      process_token(token, token_length);
      token_length = 0;
    } else {
      if(token_length < TOKEN_SIZE){
        token[token_length] = ch;
        token_length++;
      }
    }
  }

  // 文件末尾相当于一个隐含分隔符，
  // 因此最后一个 token 也必须被处理。
  process_token(token, token_length);

  if(n < 0){
    fprintf(2, "sixfive: read error in %s\n", filename);
  }

  close(fd);
}

int
main(int argc, char *argv[])
{
  int i;

  if(argc < 2){
    fprintf(2, "Usage: sixfive file...\n");
    exit(1);
  }

  for(i = 1; i < argc; i++){
    sixfive_file(argv[i]);
  }

  exit(0);
}
