// Copyright 2020 Fuzhou Rockchip Electronics Co., Ltd. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <assert.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "common/sample_common.h"
#include "rkmedia_api.h"
#include "rkmedia_venc.h"

static bool quit = false;
static void sigterm_handler(int sig) {
  fprintf(stderr, "signal %d\n", sig);
  quit = true;
}

static FILE *save_file;

void video_packet_cb(MEDIA_BUFFER mb) {
  static int jpeg_id = 0;
  printf("Get JPEG packet[%d]:ptr:%p, fd:%d, size:%zu, mode:%d, channel:%d, "
         "timestamp:%lld\n",
         jpeg_id++, RK_MPI_MB_GetPtr(mb), RK_MPI_MB_GetFD(mb),
         RK_MPI_MB_GetSize(mb), RK_MPI_MB_GetModeID(mb),
         RK_MPI_MB_GetChannelID(mb), RK_MPI_MB_GetTimestamp(mb));

  if (save_file)
    fwrite(RK_MPI_MB_GetPtr(mb), 1, RK_MPI_MB_GetSize(mb), save_file);

  RK_MPI_MB_ReleaseBuffer(mb);
}

#define TEST_ARGB32_YELLOW 0xFFFFFF00
#define TEST_ARGB32_RED 0xFFFF0033
#define TEST_ARGB32_BLUE 0xFF003399
#define TEST_ARGB32_TRANS 0x00FFFFFF

static void set_argb8888_buffer(RK_U32 *buf, RK_U32 size, RK_U32 color) {
  for (RK_U32 i = 0; buf && (i < size); i++)
    *(buf + i) = color;
}

static char optstr[] = "?:m:a::h";

static const struct option long_options[] = {
    {"aiq", optional_argument, NULL, 'a'},
    {"mode", required_argument, NULL, 'm'},
    {"help", no_argument, NULL, 'h'},
    {NULL, 0, NULL, 0},
};

static void print_usage(char *name) {
  printf("#Usage Example: \n");
  printf("  %s [-m cbr] [-m /oem/etc/iqfiles]\n", name);
  printf("  @[-m] Set RcMode:cbr/vbr. defalut:vbr\n");
  printf("  @[-a] the dirpath of iqfiles. set dirpath empty to using "
         "path:\"/oem/etc/iqfiles/\", default: NULL\n");
}

int main(int argc, char *argv[]) {
  RK_S32 ret;
  const RK_CHAR *pcRcMode = "vbr";
  int c = 0;
  const char *iq_file_dir = NULL;

  opterr = 1;
  while ((c = getopt_long(argc, argv, optstr, long_options, NULL)) != -1) {
    const char *tmp_optarg = optarg;
    switch (c) {
    case 'm':
      pcRcMode = optarg;
      printf("#IN ARGS: pcRcMode: %s\n", pcRcMode);
      break;
    case 'a':
      if (!optarg && NULL != argv[optind] && '-' != argv[optind][0]) {
        tmp_optarg = argv[optind++];
      }
      if (tmp_optarg) {
        iq_file_dir = (char *)tmp_optarg;
      } else {
        iq_file_dir = "/oem/etc/iqfiles";
      }
      printf("#IN ARGS: the path of iqfiles: %s\n", iq_file_dir);
      break;
    case '?':
    default:
      print_usage(argv[0]);
      exit(0);
    }
  }

  if (iq_file_dir) {
#ifdef RKAIQ
    printf("#Aiq xml dirpath: %s\n\n", iq_file_dir);
    rk_aiq_working_mode_t hdr_mode = RK_AIQ_WORKING_MODE_NORMAL;
    RK_BOOL fec_enable = RK_FALSE;
    int fps = 30;
    SAMPLE_COMM_ISP_Init(hdr_mode, fec_enable, iq_file_dir);
    SAMPLE_COMM_ISP_Run();
    SAMPLE_COMM_ISP_SetFrameRate(fps);
#endif
  }

  ret = RK_MPI_SYS_Init();
  if (ret) {
    printf("Sys Init failed! ret=%d\n", ret);
    return -1;
  }

  VI_CHN_ATTR_S vi_chn_attr;
  vi_chn_attr.pcVideoNode = "rkispp_scale0";
  vi_chn_attr.u32BufCnt = 3;
  vi_chn_attr.u32Width = 1920;
  vi_chn_attr.u32Height = 1080;
  vi_chn_attr.enPixFmt = IMAGE_TYPE_NV12;
  vi_chn_attr.enWorkMode = VI_WORK_MODE_NORMAL;
  ret = RK_MPI_VI_SetChnAttr(0, 1, &vi_chn_attr);
  ret |= RK_MPI_VI_EnableChn(0, 1);
  if (ret) {
    printf("Create Vi failed! ret=%d\n", ret);
    return -1;
  }

  VENC_CHN_ATTR_S venc_chn_attr;
  memset(&venc_chn_attr, 0, sizeof(venc_chn_attr));
  venc_chn_attr.stVencAttr.enType = RK_CODEC_TYPE_MJPEG;
  venc_chn_attr.stVencAttr.imageType = IMAGE_TYPE_NV12;
  venc_chn_attr.stVencAttr.u32PicWidth = 1920;
  venc_chn_attr.stVencAttr.u32PicHeight = 1080;
  venc_chn_attr.stVencAttr.u32VirWidth = 1920;
  venc_chn_attr.stVencAttr.u32VirHeight = 1080;
  // venc_chn_attr.stVencAttr.enRotation = VENC_ROTATION_90;
  if (!strcmp(pcRcMode, "cbr")) {
    venc_chn_attr.stRcAttr.enRcMode = VENC_RC_MODE_MJPEGCBR;
    venc_chn_attr.stRcAttr.stMjpegCbr.fr32DstFrameRateDen = 1;
    venc_chn_attr.stRcAttr.stMjpegCbr.fr32DstFrameRateNum = 30;
    venc_chn_attr.stRcAttr.stMjpegCbr.u32SrcFrameRateDen = 1;
    venc_chn_attr.stRcAttr.stMjpegCbr.u32SrcFrameRateNum = 30;
    venc_chn_attr.stRcAttr.stMjpegCbr.u32BitRate = 50000000; // 50Mbps
  } else {
    venc_chn_attr.stRcAttr.enRcMode = VENC_RC_MODE_MJPEGVBR;
    venc_chn_attr.stRcAttr.stMjpegVbr.fr32DstFrameRateDen = 1;
    venc_chn_attr.stRcAttr.stMjpegVbr.fr32DstFrameRateNum = 30;
    venc_chn_attr.stRcAttr.stMjpegVbr.u32SrcFrameRateDen = 1;
    venc_chn_attr.stRcAttr.stMjpegVbr.u32SrcFrameRateNum = 30;
    venc_chn_attr.stRcAttr.stMjpegCbr.u32BitRate = 50000000; // 50Mbps
  }

  ret = RK_MPI_VENC_CreateChn(0, &venc_chn_attr);
  if (ret) {
    printf("Create Venc failed! ret=%d\n", ret);
    return -1;
  }

  save_file = fopen("/tmp/test.mjpg", "w");
  if (!save_file)
    printf("WARN: open /tmp/test.mjpg failed!\n");

  MPP_CHN_S stEncChn;
  stEncChn.enModId = RK_ID_VENC;
  stEncChn.s32ChnId = 0;
  ret = RK_MPI_SYS_RegisterOutCb(&stEncChn, video_packet_cb);
  if (ret) {
    printf("Register Output callback failed! ret=%d\n", ret);
    return -1;
  }

  MPP_CHN_S stSrcChn;
  stSrcChn.enModId = RK_ID_VI;
  stSrcChn.s32ChnId = 1;
  MPP_CHN_S stDestChn;
  stDestChn.enModId = RK_ID_VENC;
  stDestChn.s32ChnId = 0;
  ret = RK_MPI_SYS_Bind(&stSrcChn, &stDestChn);
  if (ret) {
    printf("Register Output callback failed! ret=%d\n", ret);
    return -1;
  }

  RK_MPI_VENC_RGN_Init(0, NULL);

  BITMAP_S BitMap;
  BitMap.enPixelFormat = PIXEL_FORMAT_ARGB_8888;
  BitMap.u32Width = 64;
  BitMap.u32Height = 256;
  BitMap.pData = malloc(BitMap.u32Width * 4 * BitMap.u32Height);
  RK_U8 *ColorData = (RK_U8 *)BitMap.pData;
  RK_U16 ColorBlockSize = BitMap.u32Height * BitMap.u32Width;
  set_argb8888_buffer((RK_U32 *)ColorData, ColorBlockSize / 4,
                      TEST_ARGB32_YELLOW);
  set_argb8888_buffer((RK_U32 *)(ColorData + ColorBlockSize),
                      ColorBlockSize / 4, TEST_ARGB32_TRANS);
  set_argb8888_buffer((RK_U32 *)(ColorData + 2 * ColorBlockSize),
                      ColorBlockSize / 4, TEST_ARGB32_RED);
  set_argb8888_buffer((RK_U32 *)(ColorData + 3 * ColorBlockSize),
                      ColorBlockSize / 4, TEST_ARGB32_BLUE);

  OSD_REGION_INFO_S RngInfo;
  RngInfo.enRegionId = REGION_ID_0;
  RngInfo.u32PosX = 0;
  RngInfo.u32PosY = 0;
  RngInfo.u32Width = 64;
  RngInfo.u32Height = 256;
  RngInfo.u8Enable = 1;
  RngInfo.u8Inverse = 0;
  RK_MPI_VENC_RGN_SetBitMap(0, &RngInfo, &BitMap);

  printf("%s initial finish\n", __func__);
  signal(SIGINT, sigterm_handler);

  while (!quit) {
    usleep(100);
  }

  if (iq_file_dir) {
#ifdef RKAIQ
    SAMPLE_COMM_ISP_Stop(); // isp aiq stop before vi streamoff
#endif
  }

  printf("%s exit!\n", __func__);
  RK_MPI_SYS_UnBind(&stSrcChn, &stDestChn);
  RK_MPI_VI_DisableChn(0, 1);
  RK_MPI_VENC_DestroyChn(0);

  if (save_file)
    fclose(save_file);

  return 0;
}
