#include "rkmedia.h"

int rkmedia_init()
{
    int ret = 0;
    ret = RK_MPI_SYS_Init(); // 初始化rkmedia
    if (ret)
    {
        std::cout << "RK_MPI_SYS_Init failed! ret=" << ret << std::endl;
        return -1;
    }
    std::cout << "RK_MPI_SYS_Init success!" << std::endl;

    // 创建VI通道
    VI_CHN_ATTR_S vi_chn_attr;
    memset(&vi_chn_attr, 0, sizeof(vi_chn_attr));
    vi_chn_attr.pcVideoNode = "rkispp_scale0"; // video节点路径
    vi_chn_attr.u32Width = 1920;               // 宽
    vi_chn_attr.u32Height = 1080;              // 高
    vi_chn_attr.u32BufCnt = 4;                 // 缓冲区个数 should be >=4
    vi_chn_attr.enPixFmt = IMAGE_TYPE_NV12;    // 图像格式
    vi_chn_attr.enWorkMode = VI_WORK_MODE_NORMAL;
    ret = RK_MPI_VI_SetChnAttr(0, 0, &vi_chn_attr); // 设置通道属性
    if (ret)
    {
        std::cout << "RK_VI_SETCHNATTR failed! ret=" << ret << std::endl;
        return -1;
    }
    std::cout << "RK_VI_SETCHNATTR success!" << std::endl;

    ret = RK_MPI_VI_EnableChn(0, 0); // 启用通道
    if (ret)
    {
        std::cout << "RK_VI_ENABLECHN failed! ret=" << ret << std::endl;
        return -1;
    }
    std::cout << "RK_VI_ENABLECHN success!" << std::endl;

    // 创建VENC通道
    VENC_CHN_ATTR_S venc_chn_attr;
    memset(&venc_chn_attr, 0, sizeof(venc_chn_attr));
    // stVencAttr为编码属性
    venc_chn_attr.stVencAttr.enType = RK_CODEC_TYPE_H264; // 编码类型
    venc_chn_attr.stVencAttr.imageType = IMAGE_TYPE_NV12; // 图像格式
    venc_chn_attr.stVencAttr.u32PicHeight = 1080;         // 编码图片高度
    venc_chn_attr.stVencAttr.u32VirHeight = 1080;         // 编码图片虚拟高度,手册说一般和picHeight相同，若是大于picHeight，则需要16对齐
    venc_chn_attr.stVencAttr.u32PicWidth = 1920;          // 编码图片宽度
    venc_chn_attr.stVencAttr.u32VirWidth = 1920;          // 编码图片虚拟宽度，手册说一般和PicWidth相同，若是大于PicWidth，则需要16对齐
    /**
     * 编码等级
     * H.264: 66: baseline; 77:MP; 100:HP;
     * H.265: default:Main;
     * Jpege/MJpege: default:Baseline
     */
    venc_chn_attr.stVencAttr.u32Profile = 77;

    // stRcAttr为码率控制属性
    venc_chn_attr.stRcAttr.enRcMode = VENC_RC_MODE_H264CBR; // 码率控制模式,CBR为固定码率，VBR为可变码率
    // 此处的stRcAttr.x，x要根据码率模式去选择
    venc_chn_attr.stRcAttr.stH264Cbr.u32Gop = 30;                  // GOP大小
    venc_chn_attr.stRcAttr.stH264Cbr.u32BitRate = 1920 * 1080 * 2; // 平均比特率，取值范围：[2000, 98000000]。
    venc_chn_attr.stRcAttr.stH264Cbr.fr32DstFrameRateDen = 0;      // 数据源帧率分母
    venc_chn_attr.stRcAttr.stH264Cbr.fr32DstFrameRateNum = 30;     // 数据源帧率分子
    venc_chn_attr.stRcAttr.stH264Cbr.u32SrcFrameRateDen = 0;       // 编码帧率分母
    venc_chn_attr.stRcAttr.stH264Cbr.u32SrcFrameRateNum = 30;      // 编码帧率分子
    ret = RK_MPI_VENC_CreateChn(0, &venc_chn_attr);                // 创建venc通道，此处通道号为0
    if (ret)
    {
        printf("ERROR: create VENC[0] error! ret=%d\n", ret);
        return -1;
    }


    // 将VI通道和VENC通道连接起来
    MPP_CHN_S stSrcChn;
    stSrcChn.enModId = RK_ID_VI; // 模块ID
    stSrcChn.s32DevId = 0;       // 设备号
    stSrcChn.s32ChnId = 0;       // 通道号
    MPP_CHN_S stDestChn;
    stDestChn.enModId = RK_ID_VENC;               // 模块ID
    stDestChn.s32DevId = 0;                       // 设备号
    stDestChn.s32ChnId = 0;                       // 通道号
    ret = RK_MPI_SYS_Bind(&stSrcChn, &stDestChn); // 绑定VI和VENC通道
    if (ret)
    {
        printf("ERROR: Bind VI[0] and VENC[0] error! ret=%d\n", ret);
        return -1;
    }

    printf("%s initial finish\n", __func__);
    return 0;
}