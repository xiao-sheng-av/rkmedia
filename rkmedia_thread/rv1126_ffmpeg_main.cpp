#include <cstdio>
#include <iostream>
#include <cstring>
#include <unistd.h>
#include "ffmpeg_rtsp.h"
#include "rkmedia.h"
int main()
{
    int ret = 0;
    ret = rkmedia_init();
    if(ret != 0)
    {
        std::cout << "rkmedia_init error" << std::endl;
        return -1;
    }
    else std::cout << "rkmedia_init success" << std::endl;
    ffmpeg_rtsp();//开始推流
    std::cout << "success" << std::endl;
    while(1)
    {
        sleep(20);
    }

}