#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  if(argc < 3){
    fprintf(2, "Usage: setpriority priority pid\n");
    exit(1);
  }
  int priority, pid;
  priority=atoi(argv[1]);
  pid=atoi(argv[2]);
  setpriority(priority, pid);
  exit(0);
}
