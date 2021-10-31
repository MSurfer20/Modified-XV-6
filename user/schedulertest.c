#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fcntl.h"


#define NFORK 10
#define IO 5

int main() {
  int n, pid;
  int wtime, rtime;
  int twtime=0, trtime=0;
  for(n=0; n < NFORK;n++) {
      pid = fork();
      if (pid < 0)
          break;
      if (pid == 0) {
#if SCHEDULER!=1
          if (n < IO) {
            sleep(50); // IO bound processes
          } else {
#endif
            for (volatile int i = 0; i < 1000000000; i++) {} // CPU bound process 
#if SCHEDULER!=1
          }
#endif
          printf("Process %d finished", n);
          exit(0);
      } else {
#if SCHEDULER==2
        set_priority(80, pid); // Will only matter for PBS, set lower priority for IO bound processes 
#endif
      }
  }
//   PBS TESTING
//   set_priority(30, 12);
//   set_priority(5, 13);
//   set_priority(23, 11);
//   printf("YAYYYYYY");

// MLFQ Testing
//   for(volatile int x=0; x<10000000;x++){}
// printf("YAYYYYYY");
//   printf("NAYA FORKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKK");
//   pid = fork();
//       if (pid == 0) {
//         for (volatile int i = 0; i < 1000000000; i++) {} // CPU bound process 
//           }
//           printf("Process %d finished\n", n);
//           exit(0);
//   for(volatile int x=0; x<100000;x++){}
//   pid = fork();
//       if (pid == 0) {
// #if SCHEDULER!=1
//           if (n < IO) {
//             sleep(200); // IO bound processes
//           } else {
// #endif
//             for (volatile int i = 0; i < 1000000000; i++) {} // CPU bound process 
// #if SCHEDULER!=1
//           }
// #endif
//           printf("Process %d finished\n", n);
//           exit(0);
//       } else {
// #if SCHEDULER==2
//         set_priority(80, pid); // Will only matter for PBS, set lower priority for IO bound processes 
// #endif
//       } 
  for(;n > 0; n--) {
      if(waitx(0,&wtime,&rtime) >= 0) {
          trtime += rtime;
          twtime += wtime;
      } 
  }
  printf("Average rtime %d,  wtime %d\n", trtime / NFORK, twtime / NFORK);
  exit(0);
}
