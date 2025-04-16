#include <cstdio>
#include <rkmedia_api.h>


int main()
{
    int ret = 0;
    ret = RK_MPI_SYS_Init();
    if(ret != RK_ERR_SYS_OK)
    {
        printf("RK_MPI_SYS_Init failed! ret=%d\n", ret);
        return -1;
    }
    else printf("RK_MPI_SYS_Init success!\n");
    return 0;
}