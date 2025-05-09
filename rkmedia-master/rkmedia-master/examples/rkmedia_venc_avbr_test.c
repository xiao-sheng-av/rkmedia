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
static FILE *g_save_file;

#define TEST_ARGB32_YELLOW 0xFFFFFF00
#define TEST_ARGB32_RED 0xFFFF0033

void video_packet_cb(MEDIA_BUFFER mb) {
#if 0
  const char *nalu_type = "Unknow";
  switch (RK_MPI_MB_GetFlag(mb)) {
  case VENC_NALU_IDRSLICE:
    nalu_type = "IDR Slice";
    break;
  case VENC_NALU_PSLICE:
    nalu_type = "P Slice";
    break;
  default:
  break;
  }

  if (RK_MPI_MB_IsViFrame(mb))
    nalu_type = "VI Slice";

  printf("Get Video Encoded packet[%s]:ptr:%p, fd:%d, size:%zu, mode:%d, level:%d\n",
         nalu_type, RK_MPI_MB_GetPtr(mb), RK_MPI_MB_GetFD(mb), RK_MPI_MB_GetSize(mb),
         RK_MPI_MB_GetModeID(mb), RK_MPI_MB_GetTsvcLevel(mb));
#endif
  if (g_save_file)
    fwrite(RK_MPI_MB_GetPtr(mb), 1, RK_MPI_MB_GetSize(mb), g_save_file);
  RK_MPI_MB_ReleaseBuffer(mb);
}

static int avbrStreamOn(int width, int height, int vi_chn, int venc_chn) {
  int ret = 0;

  printf("=====> Avbr Stream on: VI[%d] -> Venc[%d], wxh:%dx%d <=====\n",
         vi_chn, venc_chn, width, height);

  VI_CHN_ATTR_S vi_chn_attr;
  vi_chn_attr.pcVideoNode = "rkispp_scale0";
  vi_chn_attr.u32BufCnt = 3;
  vi_chn_attr.u32Width = width;
  vi_chn_attr.u32Height = height;
  vi_chn_attr.enPixFmt = IMAGE_TYPE_NV12;
  vi_chn_attr.enWorkMode = VI_WORK_MODE_NORMAL;
  ret = RK_MPI_VI_SetChnAttr(0, vi_chn, &vi_chn_attr);
  ret |= RK_MPI_VI_EnableChn(0, vi_chn);
  if (ret) {
    printf("TEST: ERROR: Create vi[%d] error! code:%d\n", vi_chn, ret);
    return -1;
  }

  VENC_CHN_ATTR_S venc_chn_attr;
  venc_chn_attr.stVencAttr.enType = RK_CODEC_TYPE_H265;
  venc_chn_attr.stVencAttr.imageType = IMAGE_TYPE_NV12;
  venc_chn_attr.stVencAttr.u32PicWidth = width;
  venc_chn_attr.stVencAttr.u32PicHeight = height;
  venc_chn_attr.stVencAttr.u32VirWidth = width;
  venc_chn_attr.stVencAttr.u32VirHeight = height;
  venc_chn_attr.stVencAttr.u32Profile = 77;
  venc_chn_attr.stRcAttr.enRcMode = VENC_RC_MODE_H265AVBR;
  venc_chn_attr.stRcAttr.stH265Avbr.u32Gop = 30;
  venc_chn_attr.stRcAttr.stH265Avbr.u32MaxBitRate = width * height * 30 / 14;
  venc_chn_attr.stRcAttr.stH265Avbr.fr32DstFrameRateDen = 1;
  venc_chn_attr.stRcAttr.stH265Avbr.fr32DstFrameRateNum = 30;
  venc_chn_attr.stRcAttr.stH265Avbr.u32SrcFrameRateDen = 1;
  venc_chn_attr.stRcAttr.stH265Avbr.u32SrcFrameRateNum = 30;
  ret = RK_MPI_VENC_CreateChn(0, &venc_chn_attr);
  if (ret) {
    printf("TEST: ERROR: Create venc[0] error! code:%d\n", ret);
    return -1;
  }

#if 0
  printf("### Start smartp mode ....\n");
  VENC_GOP_ATTR_S stGopModeAttr;
  stGopModeAttr.enGopMode = VENC_GOPMODE_SMARTP;
  stGopModeAttr.s32IPQpDelta = 3;
  stGopModeAttr.u32BgInterval = 300;
  stGopModeAttr.u32GopSize = 30;
  ret = RK_MPI_VENC_SetGopMode(0, &stGopModeAttr);
  if (ret) {
    printf("TEST: ERROR: Set gop mode error! code:%d\n", ret);
    return -1;
  }
#endif

  MPP_CHN_S stEncChn;
  stEncChn.enModId = RK_ID_VENC;
  stEncChn.s32DevId = 0;
  stEncChn.s32ChnId = venc_chn;
  ret = RK_MPI_SYS_RegisterOutCb(&stEncChn, video_packet_cb);
  if (ret) {
    printf("TEST: ERROR: Register cb for venc[0] error! code:%d\n", ret);
    return -1;
  }

  // Bind VI and VENC
  MPP_CHN_S stSrcChn;
  stSrcChn.enModId = RK_ID_VI;
  stSrcChn.s32DevId = 0;
  stSrcChn.s32ChnId = vi_chn;
  MPP_CHN_S stDestChn;
  stDestChn.enModId = RK_ID_VENC;
  stDestChn.s32DevId = 0;
  stDestChn.s32ChnId = venc_chn;
  ret = RK_MPI_SYS_Bind(&stSrcChn, &stDestChn);
  if (ret) {
    printf("TEST: ERROR: Bind vi[0] to venc[0] error! code:%d\n", ret);
    return -1;
  }

  return 0;
}

static int avbrStreamOff(int vi_chn, int venc_chn) {
  printf("=====> Avbr Stream off: VI[%d] -> Venc[%d] <=====\n", vi_chn,
         venc_chn);
  int ret = 0;
  MPP_CHN_S stSrcChn;
  stSrcChn.enModId = RK_ID_VI;
  stSrcChn.s32DevId = 0;
  stSrcChn.s32ChnId = vi_chn;
  MPP_CHN_S stDestChn;
  stDestChn.enModId = RK_ID_VENC;
  stDestChn.s32DevId = 0;
  stDestChn.s32ChnId = venc_chn;
  ret = RK_MPI_SYS_UnBind(&stSrcChn, &stDestChn);
  if (ret) {
    printf("ERROR: unbind VI[%d] and Venc[%d] failed! ret=%d\n", vi_chn,
           venc_chn, ret);
    return -1;
  }
  ret = RK_MPI_VI_DisableChn(0, vi_chn);
  if (ret) {
    printf("ERROR: Destroy VI[%d] failed! ret=%d\n", vi_chn, ret);
    return -1;
  }
  ret = RK_MPI_VENC_DestroyChn(venc_chn);
  if (ret) {
    printf("ERROR: Destroy Venc[%d] failed! ret=%d\n", venc_chn, ret);
    return -1;
  }

  return 0;
}

static RK_CHAR optstr[] = "?:a::h";
static const struct option long_options[] = {
    {"aiq", optional_argument, NULL, 'a'},
    {"help", no_argument, NULL, 'h'},
    {NULL, 0, NULL, 0},
};

static void print_usage(const RK_CHAR *name) {
  printf("usage example:\n");
#ifdef RKAIQ
  printf("\t%s [-a | --aiq /oem/etc/iqfiles/]\n", name);
  printf("\t-a | --aiq: enable aiq with dirpath provided, eg:-a "
         "/oem/etc/iqfiles/, "
         "set dirpath empty to using path by default, without this option aiq "
         "should run in other application\n");
#else
  printf("\t%s\n", name);
#endif
}

int main(int argc, char *argv[]) {
  g_save_file = fopen("/userdata/output.h265", "w");
  signal(SIGINT, sigterm_handler);
  int c;
  char *iq_file_dir = NULL;
  while ((c = getopt_long(argc, argv, optstr, long_options, NULL)) != -1) {
    const char *tmp_optarg = optarg;
    switch (c) {
    case 'a':
      if (!optarg && NULL != argv[optind] && '-' != argv[optind][0]) {
        tmp_optarg = argv[optind++];
      }
      if (tmp_optarg) {
        iq_file_dir = (char *)tmp_optarg;
      } else {
        iq_file_dir = "/oem/etc/iqfiles";
      }
      break;
    case 'h':
      print_usage(argv[0]);
      return 0;
    case '?':
    default:
      print_usage(argv[0]);
      return 0;
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

  RK_MPI_SYS_Init();
  if (avbrStreamOn(1920, 1080, 0, 0))
    return -1;

  while (!quit) {
    usleep(100);
  }

  if (iq_file_dir) {
#ifdef RKAIQ
    SAMPLE_COMM_ISP_Stop(); // isp aiq stop before vi streamoff
#endif
  }

  avbrStreamOff(0, 0);
  if (g_save_file) {
    printf("#VENC OSD TEST:: Close save file!\n");
    fclose(g_save_file);
  }

  printf("%s exit!\n", __func__);

  return 0;
}
