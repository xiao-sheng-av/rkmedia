// 此demo功能：获取gc2053摄像头一帧为1080p大小的图像，并进行保存
#include <cstdio>
#include <cstring>
#include <rkmedia_api.h>

int main()
{
    int ret = 0;
    FILE *save_file = fopen("./vi_get_frame.nv12", "w");
    ret = RK_MPI_SYS_Init(); // 初始化rkmedia
    if (ret)
    {
        printf("RK_MPI_SYS_Init failed! ret=%d\n", ret);
        return -1;
    }
    else
        printf("RK_MPI_SYS_Init success!\n");

    VI_CHN_ATTR_S vi_chn_attr;
    memset(&vi_chn_attr, 0, sizeof(vi_chn_attr));
    vi_chn_attr.pcVideoNode = "rkispp_scale1"; // video节点路径
    vi_chn_attr.u32Width = 1920;               // 宽
    vi_chn_attr.u32Height = 1080;              // 高
    vi_chn_attr.enPixFmt = IMAGE_TYPE_NV12;    // 图像格式
    vi_chn_attr.u32BufCnt = 3;                 // 缓冲区个数
    // VI_WORK_MODE_LUMA_ONLY模式(亮度模式)，用于VI亮度统计，在此模式下VI没有输出，并且无法从VI获取数据。
    // VI_WORK_MODE_NORMAL模式(正常模式)，该模式下正常读取Camera数据并发给后级。
    vi_chn_attr.enWorkMode = VI_WORK_MODE_NORMAL;
    // RK_S32 RK_MPI_VI_EnableChn(VI_PIPE ViPipe, VI_CHN ViChn); VI_PIPE:VI管道号  VI_CHN:VI通道号
    ret = RK_MPI_VI_SetChnAttr(0, 0, &vi_chn_attr); // 设置通道属性
    ret |= RK_MPI_VI_EnableChn(0, 0);               // 启用通道
    if (ret)
    {
        printf("RK_VI enable failed! ret=%d\n", ret);
        return -1;
    }
    else
        printf("RK_VI enable success!\n");

    ret = RK_MPI_VI_StartStream(0, 0); // 启动视频流
    if (ret)
    {
        printf("Start VI[0] failed! ret=%d\n", ret);
        return -1;
    }

    MEDIA_BUFFER mb = NULL;
    mb = RK_MPI_SYS_GetMediaBuffer(RK_ID_VI, 0, -1); // 获取图像

    MB_IMAGE_INFO_S stImageInfo = {0};
    ret = RK_MPI_MB_GetImageInfo(mb, &stImageInfo); // 从缓冲区BUFFER获取图像信息
    if (ret)
        printf("Warn: Get image info failed! ret = %d\n", ret);

    printf("Get Frame:ptr:%p, fd:%d, size:%zu, mode:%d, channel:%d, "
           "timestamp:%lld, ImgInfo:<wxh %dx%d, fmt 0x%x>\n",
           RK_MPI_MB_GetPtr(mb), RK_MPI_MB_GetFD(mb), RK_MPI_MB_GetSize(mb),
           RK_MPI_MB_GetModeID(mb), RK_MPI_MB_GetChannelID(mb),
           RK_MPI_MB_GetTimestamp(mb), stImageInfo.u32Width,
           stImageInfo.u32Height, stImageInfo.enImgType);

    // RK_MPI_MB_GetPtr(mb)从指定的MEDIA_BUFFER中获取缓冲区数据指针。RK_MPI_MB_GetSize(mb)从指定的MEDIA_BUFFER中获取缓冲区数据大小。
    fwrite(RK_MPI_MB_GetPtr(mb), 1, RK_MPI_MB_GetSize(mb), save_file);
    printf(">>>>>>>>>>>>>>> Test END <<<<<<<<<<<<<<<<<<<<<<\n");
    RK_MPI_MB_ReleaseBuffer(mb); // 释放缓冲区。
    RK_MPI_VI_DisableChn(0, 0);  // 关闭通道
    fclose(save_file);
    return 0;
}