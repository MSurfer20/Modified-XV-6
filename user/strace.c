#include "../kernel/types.h"
#include "user/user.h"

int
main(int argc, char** argv)
{
    if(argc<=2)
    {
        printf("Invalid args entered for strace command.\n");
        exit(1);
    }

    int pid = fork();

    if(pid<0)
    {
        // perror("Error forking");
        exit(1);
    }
    if(pid==0)
    {
        int mask=atoi(argv[1]);
        int trace_ret=trace(mask);
        if(trace_ret<0)
        {
            printf("Error in tracing.\n");
            exit(1);
        }
        int exec_return = exec(argv[2], argv+2);
        if(exec_return<0)
        {
            printf("Error running the command.\n");
            exit(1);
        }
    }
    else wait(0);
    exit(0);
}