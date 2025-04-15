#include "rkmedia_ffmpeg_config.h" //ffmpeg的配置头文件
#include "rkmedia_container.h"     //rkmedia连接头文件
#include "ffmpeg_audio_queue.h"    //音频队列头文件
#include "ffmpeg_video_queue.h"    //视频队列头文件
#include "rkmedia_module_function.h"  //
#include "rkmedia_assignment_manage.h"

VIDEO_QUEUE * high_video_queue = NULL;
VIDEO_QUEUE * low_video_queue = NULL;

int main(int argc, char *argv[])
{
    if(argc < 5)
    {
        printf("Please Input ./rv1126_ffmpeg_main high_stream_type high_url_address low_stream_type low_url_address. Notice URL_TYPE: 0-->FLV  1-->TS\n");
        return -1;
    }
    //高分辨
    int high_protocol_type = atoi(argv[1]); //atoi将一个以字符形式表示的数字（字符串）转换为对应的整数
    char * high_network_address = argv[2];  //rtmp流

    //低分辨
    int low_protocol_type = atoi(argv[3]);
    char * low_network_address = argv[4];

    high_video_queue = new VIDEO_QUEUE(); //初始化所有VIDEO队列
    low_video_queue = new VIDEO_QUEUE();
    
    //init_rkmedia_ffmpeg_function();  //没用到
    init_rkmedia_module_function();  //初始化所有rkmedia的模块
    init_rv1126_first_assignment(high_protocol_type, high_network_address, low_protocol_type, low_network_address);  //开启推流任务
    
    while (1)
    {
       sleep(20);
    }
    
    return 0;
}
