// Copyright 2019 Fuzhou Rockchip Electronics Co., Ltd. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <algorithm>
#include <condition_variable>
#include <fcntl.h>
#include <mutex>
#include <string>
#include <unistd.h>

#include "encoder.h"
#include "image.h"
#include "key_string.h"
#include "media_config.h"
#include "media_type.h"
#include "message.h"
#include "stream.h"
#include "utils.h"

#include "osd/color_table.h"
#include "rkmedia_adec.h"
#include "rkmedia_api.h"
#include "rkmedia_buffer.h"
#include "rkmedia_buffer_impl.h"
#include "rkmedia_utils.h"

using namespace easymedia;

#define LOG_TAG "RKMEDIA:"

typedef enum rkCHN_STATUS {
  CHN_STATUS_CLOSED,
  CHN_STATUS_READY, // params is confirmed.
  CHN_STATUS_OPEN,
  CHN_STATUS_BIND,
  // ToDo...
} CHN_STATUS;

typedef struct _RkmediaVencAttr {
  VENC_CHN_ATTR_S attr;
  VENC_RC_PARAM_S stRcPara;
  VENC_ROI_ATTR_S astRoiAttr[8];
} RkmediaVencAttr;

typedef struct _RkmediaVIAttr { VI_CHN_ATTR_S attr; } RkmediaVIAttr;

typedef struct _RkmediaAIAttr { AI_CHN_ATTR_S attr; } RkmediaAIAttr;

typedef struct _RkmediaAOAttr { AO_CHN_ATTR_S attr; } RkmediaAOAttr;

typedef struct _RkmediaAENCAttr { AENC_CHN_ATTR_S attr; } RkmediaAENCAttr;

typedef struct _RkmediaADECAttr { ADEC_CHN_ATTR_S attr; } RkmediaADECAttr;

typedef ALGO_MD_ATTR_S RkmediaMDAttr;
typedef ALGO_OD_ATTR_S RkmediaODAttr;

#define RKMEDIA_CHNNAL_BUFFER_LIMIT 3
#define RKMEDIA_CHNNAL_BUFFER_GOD_MODE_LIMIT 1

typedef struct _RkmediaChannel {
  MOD_ID_E mode_id;
  RK_U16 chn_id;
  CHN_STATUS status;
  std::shared_ptr<easymedia::Flow> rkmedia_flow;
  // Some functions need a pipeline to complete,
  // the first Flow is placed in rkmedia_flow,
  // and other flows are placed in rkmedia_flow_list for management.
  // For example:
  // vi flow --> venc flow --> file save flow
  // rkmedia_flow : vi flow
  // rkmedia_flow_list : venc flow : file save flow.
  std::list<std::shared_ptr<easymedia::Flow>> rkmedia_flow_list;
  OutCbFunc cb;
  EventCbFunc event_cb;
  union {
    RkmediaVIAttr vi_attr;
    RkmediaVencAttr venc_attr;
    RkmediaAIAttr ai_attr;
    RkmediaAOAttr ao_attr;
    RkmediaAENCAttr aenc_attr;
    RkmediaMDAttr md_attr;
    RkmediaODAttr od_attr;
    RkmediaADECAttr adec_attr;
  };
  RK_U16 bind_ref;
  std::mutex buffer_mtx;
  std::condition_variable buffer_cond;
  bool buffer_cond_quit;
  int wake_fd[2];
  std::list<MEDIA_BUFFER> buffer_list;

  // used for venc osd.
  RK_BOOL bColorTblInit;
  // Enabling dichotomy will speed up the search for the color table,
  // but will sort the color table set by the user in ascending order.
  RK_BOOL bColorDichotomyEnable;
  // 256 color table
  RK_U32 u32ArgbColorTbl[256];

  // used for region luma.
  std::mutex luma_buf_mtx;
  std::condition_variable luma_buf_cond;
  bool luma_buf_quit;
  bool luma_buf_start;
  std::shared_ptr<easymedia::MediaBuffer> luma_rkmedia_buf;
} RkmediaChannel;

RkmediaChannel g_vi_chns[VI_MAX_CHN_NUM];
std::mutex g_vi_mtx;

RkmediaChannel g_venc_chns[VENC_MAX_CHN_NUM];
std::mutex g_venc_mtx;

RkmediaChannel g_ai_chns[AI_MAX_CHN_NUM];
std::mutex g_ai_mtx;

RkmediaChannel g_ao_chns[AO_MAX_CHN_NUM];
std::mutex g_ao_mtx;

RkmediaChannel g_aenc_chns[AENC_MAX_CHN_NUM];
std::mutex g_aenc_mtx;

RkmediaChannel g_algo_md_chns[ALGO_MD_MAX_CHN_NUM];
std::mutex g_algo_md_mtx;

RkmediaChannel g_algo_od_chns[ALGO_OD_MAX_CHN_NUM];
std::mutex g_algo_od_mtx;

RkmediaChannel g_rga_chns[RGA_MAX_CHN_NUM];
std::mutex g_rga_mtx;

RkmediaChannel g_adec_chns[ADEC_MAX_CHN_NUM];
std::mutex g_adec_mtx;

RkmediaChannel g_vo_chns[RGA_MAX_CHN_NUM];
std::mutex g_vo_mtx;

static inline void RkmediaPushPipFd(int fd) {
  int i = 0;
  ssize_t count = write(fd, &i, sizeof(i));
  if (count < 0)
    LOG("ERROR: %s: write(%d) failed: %s\n", __func__, fd, strerror(errno));
}

static inline void RkmediaPopPipFd(int fd) {
  int i = 0;
  ssize_t read_size = (ssize_t)sizeof(i);
  ssize_t ret = read(fd, &i, sizeof(i));
  if (ret != read_size)
    LOG("ERROR: %s: Read(%d) failed: %s\n", __func__, fd, strerror(errno));
}

static int RkmediaChnPushBuffer(RkmediaChannel *ptrChn, MEDIA_BUFFER buffer) {
  if (!ptrChn || !buffer)
    return -1;

  ptrChn->buffer_mtx.lock();
  if ((ptrChn->mode_id == RK_ID_VI) &&
      (ptrChn->vi_attr.attr.enWorkMode == VI_WORK_MODE_GOD_MODE) &&
      (ptrChn->buffer_list.size() >= RKMEDIA_CHNNAL_BUFFER_GOD_MODE_LIMIT)) {
    LOGD("WARN: Mode[%d]:Chn[%d] drop buffer, Please get buffer in time!\n",
         ptrChn->mode_id, ptrChn->chn_id);
    MEDIA_BUFFER mb = ptrChn->buffer_list.front();
    ptrChn->buffer_list.pop_front();
    RK_MPI_MB_ReleaseBuffer(mb);
    if (ptrChn->wake_fd[0] > 0)
      RkmediaPopPipFd(ptrChn->wake_fd[0]);
  } else if (ptrChn->buffer_list.size() >= RKMEDIA_CHNNAL_BUFFER_LIMIT) {
    LOG("WARN: Mode[%d]:Chn[%d] drop buffer, Please get buffer in time!\n",
        ptrChn->mode_id, ptrChn->chn_id);
    MEDIA_BUFFER mb = ptrChn->buffer_list.front();
    ptrChn->buffer_list.pop_front();
    RK_MPI_MB_ReleaseBuffer(mb);
    if (ptrChn->wake_fd[0] > 0)
      RkmediaPopPipFd(ptrChn->wake_fd[0]);
  }
  if (!ptrChn->buffer_cond_quit) {
    ptrChn->buffer_list.push_back(buffer);
    if (ptrChn->wake_fd[1] > 0)
      RkmediaPushPipFd(ptrChn->wake_fd[1]);
  } else {
    RK_MPI_MB_ReleaseBuffer(buffer);
  }

  ptrChn->buffer_cond.notify_all();
  ptrChn->buffer_mtx.unlock();
  pthread_yield();

  return 0;
}

static MEDIA_BUFFER RkmediaChnPopBuffer(RkmediaChannel *ptrChn,
                                        RK_S32 s32MilliSec) {
  if (!ptrChn)
    return NULL;

  std::unique_lock<std::mutex> lck(ptrChn->buffer_mtx);
  if (ptrChn->buffer_list.empty()) {
    if (s32MilliSec < 0 && !ptrChn->buffer_cond_quit) {
      ptrChn->buffer_cond.wait(lck);
    } else if (s32MilliSec > 0) {
      if (ptrChn->buffer_cond.wait_for(
              lck, std::chrono::milliseconds(s32MilliSec)) ==
          std::cv_status::timeout) {
        LOG("INFO: %s: Mode[%d]:Chn[%d] get mediabuffer timeout!\n", __func__,
            ptrChn->mode_id, ptrChn->chn_id);
        return NULL;
      }
    } else {
      return NULL;
    }
  }

  MEDIA_BUFFER mb = NULL;
  if (!ptrChn->buffer_list.empty()) {
    if (ptrChn->wake_fd[0] > 0)
      RkmediaPopPipFd(ptrChn->wake_fd[0]);
    mb = ptrChn->buffer_list.front();
    ptrChn->buffer_list.pop_front();
  }

  return mb;
}

static void RkmediaChnInitBuffer(RkmediaChannel *ptrChn) {
  if (!ptrChn)
    return;

  ptrChn->buffer_mtx.lock();
  ptrChn->buffer_cond_quit = false;
  ptrChn->buffer_mtx.unlock();
}

static void RkmediaChnClearBuffer(RkmediaChannel *ptrChn) {
  if (!ptrChn)
    return;

  LOGD("#%p Mode[%d]:Chn[%d] clear media buffer start...\n", ptrChn,
       ptrChn->mode_id, ptrChn->chn_id);
  MEDIA_BUFFER mb = NULL;
  ptrChn->buffer_mtx.lock();
  while (!ptrChn->buffer_list.empty()) {
    mb = ptrChn->buffer_list.front();
    ptrChn->buffer_list.pop_front();
    RK_MPI_MB_ReleaseBuffer(mb);
  }
  ptrChn->buffer_cond_quit = true;
  ptrChn->buffer_cond.notify_all();
  ptrChn->buffer_mtx.unlock();
  LOGD("#%p Mode[%d]:Chn[%d] clear media buffer end...\n", ptrChn,
       ptrChn->mode_id, ptrChn->chn_id);
}

/********************************************************************
 * SYS Ctrl api
 ********************************************************************/
static void Reset_Channel_Table(RkmediaChannel *tbl, int cnt, MOD_ID_E mid) {
  for (int i = 0; i < cnt; i++) {
    tbl[i].mode_id = mid;
    tbl[i].chn_id = i;
    tbl[i].status = CHN_STATUS_CLOSED;
    tbl[i].cb = nullptr;
    tbl[i].event_cb = nullptr;
    tbl[i].bind_ref = 0;
    tbl[i].bColorTblInit = RK_FALSE;
    tbl[i].bColorDichotomyEnable = RK_FALSE;
    memset(tbl[i].u32ArgbColorTbl, 0, 0);
  }
}

RK_S32 RK_MPI_SYS_Init() {
  LOG_INIT();

  Reset_Channel_Table(g_vi_chns, VI_MAX_CHN_NUM, RK_ID_VI);
  Reset_Channel_Table(g_venc_chns, VENC_MAX_CHN_NUM, RK_ID_VENC);
  Reset_Channel_Table(g_ai_chns, AI_MAX_CHN_NUM, RK_ID_AI);
  Reset_Channel_Table(g_aenc_chns, AENC_MAX_CHN_NUM, RK_ID_AENC);
  Reset_Channel_Table(g_ao_chns, AO_MAX_CHN_NUM, RK_ID_AO);
  Reset_Channel_Table(g_algo_md_chns, ALGO_MD_MAX_CHN_NUM, RK_ID_ALGO_MD);
  Reset_Channel_Table(g_algo_od_chns, ALGO_OD_MAX_CHN_NUM, RK_ID_ALGO_OD);
  Reset_Channel_Table(g_rga_chns, RGA_MAX_CHN_NUM, RK_ID_RGA);
  Reset_Channel_Table(g_adec_chns, ADEC_MAX_CHN_NUM, RK_ID_ADEC);
  Reset_Channel_Table(g_vo_chns, VO_MAX_CHN_NUM, RK_ID_VO);

  return RK_ERR_SYS_OK;
}

RK_VOID RK_MPI_SYS_DumpChn(MOD_ID_E enModId) {
  RK_U16 u16ChnMaxCnt = 0;
  RkmediaChannel *pChns = NULL;
  switch (enModId) {
  case RK_ID_VI:
    u16ChnMaxCnt = VI_MAX_CHN_NUM;
    pChns = g_vi_chns;
    break;
  case RK_ID_VENC:
    u16ChnMaxCnt = VENC_MAX_CHN_NUM;
    pChns = g_venc_chns;
    break;
  default:
    LOG("ERROR: To do...\n");
    return;
  }

  LOG("Dump Mode:%d:\n", enModId);
  for (RK_U16 i = 0; i < u16ChnMaxCnt; i++) {
    LOG("  Chn[%d]->status:%d\n", i, pChns[i].status);
    LOG("  Chn[%d]->bind_ref:%d\n", i, pChns[i].bind_ref);
    LOG("  Chn[%d]->output_cb:%p\n", i, pChns[i].cb);
    LOG("  Chn[%d]->event_cb:%p\n\n", i, pChns[i].event_cb);
  }
}

RK_S32 RK_MPI_SYS_Bind(const MPP_CHN_S *pstSrcChn,
                       const MPP_CHN_S *pstDestChn) {
  std::shared_ptr<easymedia::Flow> src;
  std::shared_ptr<easymedia::Flow> sink;
  RkmediaChannel *src_chn = NULL;
  RkmediaChannel *dst_chn = NULL;

  if (!pstSrcChn || !pstDestChn)
    return -RK_ERR_SYS_ILLEGAL_PARAM;

  LOG("\n%s %s: Bind Mode[%d]:Chn[%d] to Mode[%d]:Chn[%d]...\n", LOG_TAG,
      __func__, pstSrcChn->enModId, pstSrcChn->s32ChnId, pstDestChn->enModId,
      pstDestChn->s32ChnId);

  switch (pstSrcChn->enModId) {
  case RK_ID_VI:
    src = g_vi_chns[pstSrcChn->s32ChnId].rkmedia_flow;
    src_chn = &g_vi_chns[pstSrcChn->s32ChnId];
    break;
  case RK_ID_VENC:
    src = g_venc_chns[pstSrcChn->s32ChnId].rkmedia_flow;
    src_chn = &g_venc_chns[pstSrcChn->s32ChnId];
    break;
  case RK_ID_AI:
    src = g_ai_chns[pstSrcChn->s32ChnId].rkmedia_flow;
    src_chn = &g_ai_chns[pstSrcChn->s32ChnId];
    break;
  case RK_ID_AENC:
    src = g_aenc_chns[pstSrcChn->s32ChnId].rkmedia_flow;
    src_chn = &g_aenc_chns[pstSrcChn->s32ChnId];
    break;
  case RK_ID_RGA:
    src = g_rga_chns[pstSrcChn->s32ChnId].rkmedia_flow;
    src_chn = &g_rga_chns[pstSrcChn->s32ChnId];
    break;
  case RK_ID_ADEC:
    src = g_adec_chns[pstSrcChn->s32ChnId].rkmedia_flow;
    src_chn = &g_adec_chns[pstSrcChn->s32ChnId];
    break;
  default:
    return -RK_ERR_SYS_NOT_SUPPORT;
  }

  if ((src_chn->status < CHN_STATUS_OPEN) || (!src)) {
    LOG("ERROR: %s Src Mode[%d]:Chn[%d] is not ready!\n", __func__,
        pstSrcChn->enModId, pstSrcChn->s32ChnId);
    return -RK_ERR_SYS_NOTREADY;
  }

  switch (pstDestChn->enModId) {
  case RK_ID_VENC:
    sink = g_venc_chns[pstDestChn->s32ChnId].rkmedia_flow;
    dst_chn = &g_venc_chns[pstDestChn->s32ChnId];
    break;
  case RK_ID_AO:
    sink = g_ao_chns[pstDestChn->s32ChnId].rkmedia_flow;
    dst_chn = &g_ao_chns[pstDestChn->s32ChnId];
    break;
  case RK_ID_AENC:
    sink = g_aenc_chns[pstDestChn->s32ChnId].rkmedia_flow;
    dst_chn = &g_aenc_chns[pstDestChn->s32ChnId];
    break;
  case RK_ID_ALGO_MD:
    sink = g_algo_md_chns[pstDestChn->s32ChnId].rkmedia_flow;
    dst_chn = &g_algo_md_chns[pstDestChn->s32ChnId];
    break;
  case RK_ID_ALGO_OD:
    sink = g_algo_od_chns[pstDestChn->s32ChnId].rkmedia_flow;
    dst_chn = &g_algo_od_chns[pstDestChn->s32ChnId];
    break;
  case RK_ID_RGA:
    sink = g_rga_chns[pstDestChn->s32ChnId].rkmedia_flow;
    dst_chn = &g_rga_chns[pstDestChn->s32ChnId];
    break;
  case RK_ID_ADEC:
    sink = g_adec_chns[pstDestChn->s32ChnId].rkmedia_flow;
    dst_chn = &g_adec_chns[pstDestChn->s32ChnId];
    break;
  case RK_ID_VO:
    sink = g_vo_chns[pstDestChn->s32ChnId].rkmedia_flow;
    dst_chn = &g_vo_chns[pstDestChn->s32ChnId];
    break;
  default:
    return -RK_ERR_SYS_NOT_SUPPORT;
  }

  if ((dst_chn->status < CHN_STATUS_OPEN) || (!sink)) {
    LOG("ERROR: %s Dst Mode[%d]:Chn[%d] is not ready!\n", __func__,
        pstDestChn->enModId, pstDestChn->s32ChnId);
    return -RK_ERR_SYS_NOTREADY;
  }

  // Rkmedia flow bind
  src->AddDownFlow(sink, 0, 0);
  // Generally, after the previous Chn is bound to the next stage,
  // FlowOutputCallback will be disabled.Because the VI needs to calculate
  // the brightness, the VI still retains the FlowOutputCallback after
  // binding the lower-level Chn.
  if (src_chn->mode_id != RK_ID_VI)
    src->SetOutputCallBack(NULL, NULL);

  // change status frome OPEN to BIND.
  src_chn->status = CHN_STATUS_BIND;
  src_chn->bind_ref++;
  dst_chn->status = CHN_STATUS_BIND;
  dst_chn->bind_ref++;

  return RK_ERR_SYS_OK;
}

RK_S32 RK_MPI_SYS_UnBind(const MPP_CHN_S *pstSrcChn,
                         const MPP_CHN_S *pstDestChn) {
  std::shared_ptr<easymedia::Flow> src;
  std::shared_ptr<easymedia::Flow> sink;
  RkmediaChannel *src_chn = NULL;
  RkmediaChannel *dst_chn = NULL;

  LOG("\n%s %s: UnBind Mode[%d]:Chn[%d] to Mode[%d]:Chn[%d]...\n", LOG_TAG,
      __func__, pstSrcChn->enModId, pstSrcChn->s32ChnId, pstDestChn->enModId,
      pstDestChn->s32ChnId);

  switch (pstSrcChn->enModId) {
  case RK_ID_VI:
    src = g_vi_chns[pstSrcChn->s32ChnId].rkmedia_flow;
    src_chn = &g_vi_chns[pstSrcChn->s32ChnId];
    break;
  case RK_ID_VENC:
    src = g_venc_chns[pstSrcChn->s32ChnId].rkmedia_flow;
    src_chn = &g_venc_chns[pstSrcChn->s32ChnId];
    break;
  case RK_ID_AI:
    src = g_ai_chns[pstSrcChn->s32ChnId].rkmedia_flow;
    src_chn = &g_ai_chns[pstSrcChn->s32ChnId];
    break;
  case RK_ID_AO:
    src = g_ao_chns[pstSrcChn->s32ChnId].rkmedia_flow;
    src_chn = &g_ao_chns[pstSrcChn->s32ChnId];
    break;
  case RK_ID_AENC:
    src = g_aenc_chns[pstSrcChn->s32ChnId].rkmedia_flow;
    src_chn = &g_aenc_chns[pstSrcChn->s32ChnId];
    break;
  case RK_ID_RGA:
    src = g_rga_chns[pstSrcChn->s32ChnId].rkmedia_flow;
    src_chn = &g_rga_chns[pstSrcChn->s32ChnId];
    break;
  case RK_ID_ADEC:
    src = g_adec_chns[pstSrcChn->s32ChnId].rkmedia_flow;
    src_chn = &g_adec_chns[pstSrcChn->s32ChnId];
    break;
  default:
    return -RK_ERR_SYS_NOT_SUPPORT;
  }

  if ((src_chn->status != CHN_STATUS_BIND))
    return -RK_ERR_SYS_NOT_PERM;

  if ((src_chn->bind_ref <= 0) || (!src)) {
    LOG("ERROR: %s Src Mode[%d]:Chn[%d]'s parameter does not match the "
        "status!\n",
        __func__, pstSrcChn->enModId, pstSrcChn->s32ChnId);
    return -RK_ERR_SYS_NOT_PERM;
  }

  switch (pstDestChn->enModId) {
  case RK_ID_VI:
    sink = g_vi_chns[pstDestChn->s32ChnId].rkmedia_flow;
    dst_chn = &g_vi_chns[pstDestChn->s32ChnId];
    break;
  case RK_ID_VENC:
    sink = g_venc_chns[pstDestChn->s32ChnId].rkmedia_flow;
    dst_chn = &g_venc_chns[pstDestChn->s32ChnId];
    break;
  case RK_ID_AI:
    sink = g_ai_chns[pstDestChn->s32ChnId].rkmedia_flow;
    dst_chn = &g_ai_chns[pstDestChn->s32ChnId];
    break;
  case RK_ID_AO:
    sink = g_ao_chns[pstDestChn->s32ChnId].rkmedia_flow;
    dst_chn = &g_ao_chns[pstDestChn->s32ChnId];
    break;
  case RK_ID_AENC:
    sink = g_aenc_chns[pstDestChn->s32ChnId].rkmedia_flow;
    dst_chn = &g_aenc_chns[pstDestChn->s32ChnId];
    break;
  case RK_ID_RGA:
    sink = g_rga_chns[pstDestChn->s32ChnId].rkmedia_flow;
    dst_chn = &g_rga_chns[pstDestChn->s32ChnId];
    break;
  case RK_ID_ADEC:
    sink = g_adec_chns[pstDestChn->s32ChnId].rkmedia_flow;
    dst_chn = &g_adec_chns[pstDestChn->s32ChnId];
    break;
  case RK_ID_ALGO_MD:
    sink = g_algo_md_chns[pstDestChn->s32ChnId].rkmedia_flow;
    dst_chn = &g_algo_md_chns[pstDestChn->s32ChnId];
    break;
  case RK_ID_ALGO_OD:
    sink = g_algo_od_chns[pstDestChn->s32ChnId].rkmedia_flow;
    dst_chn = &g_algo_od_chns[pstDestChn->s32ChnId];
    break;
  case RK_ID_VO:
    sink = g_vo_chns[pstDestChn->s32ChnId].rkmedia_flow;
    dst_chn = &g_vo_chns[pstDestChn->s32ChnId];
    break;
  default:
    return -RK_ERR_SYS_NOT_SUPPORT;
  }

  if ((dst_chn->status != CHN_STATUS_BIND))
    return -RK_ERR_SYS_NOT_PERM;

  if ((dst_chn->bind_ref <= 0) || (!sink)) {
    LOG("ERROR: %s Dst Mode[%d]:Chn[%d]'s parameter does not match the "
        "status!\n",
        __func__, pstDestChn->enModId, pstDestChn->s32ChnId);
    return -RK_ERR_SYS_NOT_PERM;
  }

  // Rkmedia flow unbind
  src->RemoveDownFlow(sink);

  src_chn->bind_ref--;
  dst_chn->bind_ref--;
  // change status frome BIND to OPEN.
  if (src_chn->bind_ref <= 0) {
    src_chn->status = CHN_STATUS_OPEN;
    src_chn->bind_ref = 0;
  }
  if (dst_chn->bind_ref == 0) {
    dst_chn->status = CHN_STATUS_OPEN;
    dst_chn->bind_ref = 0;
  }

  return RK_ERR_SYS_OK;
}

static MB_TYPE_E GetBufferType(RkmediaChannel *target_chn) {
  MB_TYPE_E type = (MB_TYPE_E)0;

  if (!target_chn)
    return type;

  switch (target_chn->mode_id) {
  case RK_ID_VI:
  case RK_ID_RGA:
    type = MB_TYPE_IMAGE;
    break;
  case RK_ID_VENC:
    if (target_chn->venc_attr.attr.stVencAttr.enType == RK_CODEC_TYPE_H264)
      type = MB_TYPE_H264;
    else if (target_chn->venc_attr.attr.stVencAttr.enType == RK_CODEC_TYPE_H265)
      type = MB_TYPE_H265;
    else if (target_chn->venc_attr.attr.stVencAttr.enType ==
             RK_CODEC_TYPE_MJPEG)
      type = MB_TYPE_MJPEG;
    else if (target_chn->venc_attr.attr.stVencAttr.enType == RK_CODEC_TYPE_JPEG)
      type = MB_TYPE_JPEG;
    break;
  case RK_ID_AI:
  case RK_ID_AENC:
    type = MB_TYPE_AUDIO;
    break;
  default:
    break;
  }

  return type;
}

static void
FlowOutputCallback(void *handle,
                   std::shared_ptr<easymedia::MediaBuffer> rkmedia_mb) {
  if (!rkmedia_mb)
    return;

  if (!handle) {
    LOG("ERROR: %s without handle!\n", __func__);
    return;
  }

  RkmediaChannel *target_chn = (RkmediaChannel *)handle;
  if (target_chn->status < CHN_STATUS_OPEN) {
    LOG("ERROR: %s chn[mode:%d] in status[%d] should not call output "
        "callback!\n",
        __func__, target_chn->mode_id, target_chn->status);
    return;
  }

  MB_TYPE_E mb_type = GetBufferType(target_chn);

  if (target_chn->mode_id == RK_ID_VI) {
    std::unique_lock<std::mutex> lck(target_chn->luma_buf_mtx);
    if (!target_chn->luma_buf_quit && target_chn->luma_buf_start)
      target_chn->luma_rkmedia_buf = rkmedia_mb;
    target_chn->luma_buf_cond.notify_all();
    // Generally, after the previous Chn is bound to the next stage,
    // FlowOutputCallback will be disabled. Because the VI needs to
    // calculate the brightness, the VI still retains the FlowOutputCallback
    // after binding the lower-level Chn.
    if (((target_chn->status == CHN_STATUS_BIND) &&
         (target_chn->vi_attr.attr.enWorkMode != VI_WORK_MODE_GOD_MODE)) ||
        (target_chn->vi_attr.attr.enWorkMode == VI_WORK_MODE_LUMA_ONLY))
      return;
  }

  MEDIA_BUFFER_IMPLE *mb = new MEDIA_BUFFER_IMPLE;
  if (!mb) {
    LOG("ERROR: %s mode[%d]:chn[%d] no space left for new mb!\n", __func__,
        target_chn->mode_id, target_chn->chn_id);
    return;
  }
  mb->ptr = rkmedia_mb->GetPtr();
  mb->fd = rkmedia_mb->GetFD();
  mb->size = rkmedia_mb->GetValidSize();
  mb->rkmedia_mb = rkmedia_mb;
  mb->mode_id = target_chn->mode_id;
  mb->chn_id = target_chn->chn_id;
  mb->timestamp = (RK_U64)rkmedia_mb->GetUSTimeStamp();
  mb->type = mb_type;
  if ((mb_type == MB_TYPE_H264) || (mb_type == MB_TYPE_H265)) {
    mb->flag = (rkmedia_mb->GetUserFlag() & MediaBuffer::kIntra)
                   ? VENC_NALU_IDRSLICE
                   : VENC_NALU_PSLICE;
    mb->tsvc_level = rkmedia_mb->GetTsvcLevel();
  } else {
    mb->flag = 0;
    mb->tsvc_level = 0;
  }
  if (mb_type == MB_TYPE_IMAGE) {
    easymedia::ImageBuffer *rkmedia_ib =
        static_cast<easymedia::ImageBuffer *>(rkmedia_mb.get());
    mb->stImageInfo.u32Width = rkmedia_ib->GetWidth();
    mb->stImageInfo.u32Height = rkmedia_ib->GetHeight();
    mb->stImageInfo.u32HorStride = rkmedia_ib->GetVirWidth();
    mb->stImageInfo.u32VerStride = rkmedia_ib->GetVirHeight();
    std::string strPixFmt = PixFmtToString(rkmedia_ib->GetPixelFormat());
    mb->stImageInfo.enImgType = StringToImageType(strPixFmt);
  }
  // RK_MPI_SYS_GetMediaBuffer and output callback function,
  // can only choose one.
  if (target_chn->cb)
    target_chn->cb(mb);
  else
    RkmediaChnPushBuffer(target_chn, mb);
}

RK_S32 RK_MPI_SYS_RegisterOutCb(const MPP_CHN_S *pstChn, OutCbFunc cb) {
  std::shared_ptr<easymedia::Flow> flow;
  RkmediaChannel *target_chn = NULL;

  switch (pstChn->enModId) {
  case RK_ID_VI:
    target_chn = &g_vi_chns[pstChn->s32ChnId];
    break;
  case RK_ID_VENC:
    target_chn = &g_venc_chns[pstChn->s32ChnId];
    break;
  case RK_ID_AI:
    target_chn = &g_ai_chns[pstChn->s32ChnId];
    break;
  case RK_ID_AENC:
    target_chn = &g_aenc_chns[pstChn->s32ChnId];
    break;
  case RK_ID_RGA:
    target_chn = &g_rga_chns[pstChn->s32ChnId];
    break;
  case RK_ID_ADEC:
    target_chn = &g_adec_chns[pstChn->s32ChnId];
    break;
  case RK_ID_VO:
    target_chn = &g_vo_chns[pstChn->s32ChnId];
    break;
  default:
    return -RK_ERR_SYS_NOT_SUPPORT;
  }

  if (target_chn->status < CHN_STATUS_OPEN)
    return -RK_ERR_SYS_NOTREADY;

  if (!target_chn->rkmedia_flow_list.empty())
    flow = target_chn->rkmedia_flow_list.back();
  else if (target_chn->rkmedia_flow)
    flow = target_chn->rkmedia_flow;

  if (!flow) {
    LOG("ERROR: <ModeID:%d ChnID:%d> fatal error!"
        "Status does not match the resource\n",
        pstChn->enModId, pstChn->s32ChnId);
    return -RK_ERR_SYS_NOT_PERM;
  }

  target_chn->cb = cb;
  // flow->SetOutputCallBack(target_chn, FlowOutputCallback);

  return RK_ERR_SYS_OK;
}

static void FlowEventCallback(void *handle, void *data) {
  if (!data)
    return;

  if (!handle) {
    LOG("ERROR: %s without handle!\n", __func__);
    return;
  }

  RkmediaChannel *target_chn = (RkmediaChannel *)handle;
  if (target_chn->status < CHN_STATUS_OPEN) {
    LOG("ERROR: %s chn[mode:%d] in status[%d] should not call output "
        "callback!\n",
        __func__, target_chn->mode_id, target_chn->status);
    return;
  }

  if (!target_chn->event_cb) {
    LOG("ERROR: %s chn[mode:%d] in has no callback!\n", __func__,
        target_chn->mode_id);
    return;
  }

  switch (target_chn->mode_id) {
  case RK_ID_ALGO_MD: {
    MoveDetectEvent *rkmedia_md_event = (MoveDetectEvent *)data;
    MoveDetecInfo *rkmedia_md_info = rkmedia_md_event->data;
    EVENT_S stEvent;

    stEvent.mode_id = RK_ID_ALGO_MD;
    stEvent.type = RK_EVENT_MD;
    stEvent.md_event.u16Cnt = rkmedia_md_event->info_cnt;
    stEvent.md_event.u32Width = rkmedia_md_event->ori_width;
    stEvent.md_event.u32Height = rkmedia_md_event->ori_height;
    for (int i = 0; i < rkmedia_md_event->info_cnt; i++) {
      stEvent.md_event.stRects[i].s32X = (RK_S32)rkmedia_md_info[i].x;
      stEvent.md_event.stRects[i].s32Y = (RK_S32)rkmedia_md_info[i].y;
      stEvent.md_event.stRects[i].u32Width = (RK_U32)rkmedia_md_info[i].w;
      stEvent.md_event.stRects[i].u32Width = (RK_U32)rkmedia_md_info[i].h;
    }
    target_chn->event_cb(&stEvent);
  } break;
  case RK_ID_ALGO_OD: {
    OcclusionDetectEvent *rkmedia_od_event = (OcclusionDetectEvent *)data;
    OcclusionDetecInfo *rkmedia_od_info = rkmedia_od_event->data;
    EVENT_S stEvent;

    stEvent.mode_id = RK_ID_ALGO_OD;
    stEvent.type = RK_EVENT_OD;
    stEvent.stOdEvent.u16Cnt = rkmedia_od_event->info_cnt;
    stEvent.stOdEvent.u32Width = rkmedia_od_event->img_width;
    stEvent.stOdEvent.u32Height = rkmedia_od_event->img_height;
    for (int i = 0; i < rkmedia_od_event->info_cnt; i++) {
      stEvent.stOdEvent.stRects[i].s32X = (RK_S32)rkmedia_od_info[i].x;
      stEvent.stOdEvent.stRects[i].s32Y = (RK_S32)rkmedia_od_info[i].y;
      stEvent.stOdEvent.stRects[i].u32Width = (RK_U32)rkmedia_od_info[i].w;
      stEvent.stOdEvent.stRects[i].u32Height = (RK_U32)rkmedia_od_info[i].h;
      stEvent.stOdEvent.u16Occlusion[i] = (RK_U16)rkmedia_od_info[i].occlusion;
    }
    target_chn->event_cb(&stEvent);
  } break;
  default:
    LOG("ERROR: Channle Mode ID:%d not support event callback!\n",
        target_chn->mode_id);
    break;
  }
}

RK_S32 RK_MPI_SYS_RegisterEventCb(const MPP_CHN_S *pstChn, EventCbFunc cb) {
  std::shared_ptr<easymedia::Flow> flow;
  RkmediaChannel *target_chn = NULL;

  switch (pstChn->enModId) {
  case RK_ID_ALGO_MD:
    if (g_algo_md_chns[pstChn->s32ChnId].status < CHN_STATUS_OPEN)
      return -RK_ERR_SYS_NOTREADY;
    flow = g_algo_md_chns[pstChn->s32ChnId].rkmedia_flow;
    target_chn = &g_algo_md_chns[pstChn->s32ChnId];
    break;
  case RK_ID_ALGO_OD:
    if (g_algo_od_chns[pstChn->s32ChnId].status < CHN_STATUS_OPEN)
      return -RK_ERR_SYS_NOTREADY;
    flow = g_algo_od_chns[pstChn->s32ChnId].rkmedia_flow;
    target_chn = &g_algo_od_chns[pstChn->s32ChnId];
    break;
  default:
    return -RK_ERR_SYS_NOT_SUPPORT;
  }

  target_chn->event_cb = cb;
  flow->SetEventCallBack(target_chn, FlowEventCallback);

  return RK_ERR_SYS_OK;
}

MEDIA_BUFFER RK_MPI_SYS_GetMediaBuffer(MOD_ID_E enModID, RK_S32 s32ChnID,
                                       RK_S32 s32MilliSec) {
  RkmediaChannel *target_chn = NULL;

  switch (enModID) {
  case RK_ID_VI:
    if (s32ChnID < 0 || s32ChnID >= VI_MAX_CHN_NUM) {
      LOG("ERROR: %s invalid VI ChnID[%d]\n", __func__, s32ChnID);
      return NULL;
    }
    target_chn = &g_vi_chns[s32ChnID];
    break;
  case RK_ID_VENC:
    if (s32ChnID < 0 || s32ChnID >= VENC_MAX_CHN_NUM) {
      LOG("ERROR: %s invalid AENC ChnID[%d]\n", __func__, s32ChnID);
      return NULL;
    }
    target_chn = &g_venc_chns[s32ChnID];
    break;
  case RK_ID_AI:
    if (s32ChnID < 0 || s32ChnID >= AI_MAX_CHN_NUM) {
      LOG("ERROR: %s invalid AI ChnID[%d]\n", __func__, s32ChnID);
      return NULL;
    }
    target_chn = &g_ai_chns[s32ChnID];
    break;
  case RK_ID_AENC:
    if (s32ChnID < 0 || s32ChnID > AENC_MAX_CHN_NUM) {
      LOG("ERROR: %s invalid AENC ChnID[%d]\n", __func__, s32ChnID);
      return NULL;
    }
    target_chn = &g_aenc_chns[s32ChnID];
    break;
  case RK_ID_RGA:
    if (s32ChnID < 0 || s32ChnID > RGA_MAX_CHN_NUM) {
      LOG("ERROR: %s invalid RGA ChnID[%d]\n", __func__, s32ChnID);
      return NULL;
    }
    target_chn = &g_rga_chns[s32ChnID];
    break;
  case RK_ID_ADEC:
    if (s32ChnID < 0 || s32ChnID > ADEC_MAX_CHN_NUM) {
      LOG("ERROR: %s invalid RGA ChnID[%d]\n", __func__, s32ChnID);
      return NULL;
    }
    target_chn = &g_adec_chns[s32ChnID];
    break;
  default:
    LOG("ERROR: %s invalid modeID[%d]\n", __func__, enModID);
    return NULL;
  }

  if (target_chn->status < CHN_STATUS_OPEN) {
    LOG("ERROR: %s Mode[%d]:Chn[%d] in status[%d], "
        "this operation is not allowed!\n",
        __func__, enModID, s32ChnID, target_chn->status);
    return NULL;
  }

  return RkmediaChnPopBuffer(target_chn, s32MilliSec);
}

RK_S32 RK_MPI_SYS_SendMediaBuffer(MOD_ID_E enModID, RK_S32 s32ChnID,
                                  MEDIA_BUFFER buffer) {
  RkmediaChannel *target_chn = NULL;
  std::mutex *target_mutex = NULL;

  switch (enModID) {
  case RK_ID_VENC:
    if (s32ChnID < 0 || s32ChnID >= VENC_MAX_CHN_NUM)
      return -RK_ERR_SYS_ILLEGAL_PARAM;
    target_chn = &g_venc_chns[s32ChnID];
    target_mutex = &g_venc_mtx;
    break;
  case RK_ID_AENC:
    if (s32ChnID < 0 || s32ChnID > AENC_MAX_CHN_NUM)
      return -RK_ERR_SYS_ILLEGAL_PARAM;
    target_chn = &g_aenc_chns[s32ChnID];
    target_mutex = &g_aenc_mtx;
    break;
  case RK_ID_ALGO_MD:
    if (s32ChnID < 0 || s32ChnID > ALGO_MD_MAX_CHN_NUM)
      return -RK_ERR_SYS_ILLEGAL_PARAM;
    target_chn = &g_algo_md_chns[s32ChnID];
    target_mutex = &g_algo_md_mtx;
    break;
  case RK_ID_ALGO_OD:
    if (s32ChnID < 0 || s32ChnID > ALGO_OD_MAX_CHN_NUM)
      return -RK_ERR_SYS_ILLEGAL_PARAM;
    target_chn = &g_algo_od_chns[s32ChnID];
    target_mutex = &g_algo_od_mtx;
    break;
  case RK_ID_ADEC:
    if (s32ChnID < 0 || s32ChnID > ADEC_MAX_CHN_NUM)
      return -RK_ERR_SYS_ILLEGAL_PARAM;
    target_chn = &g_adec_chns[s32ChnID];
    target_mutex = &g_adec_mtx;
    break;
  case RK_ID_AO:
    if (s32ChnID < 0 || s32ChnID > AO_MAX_CHN_NUM)
      return -RK_ERR_SYS_ILLEGAL_PARAM;
    target_chn = &g_ao_chns[s32ChnID];
    target_mutex = &g_ao_mtx;
    break;
  case RK_ID_RGA:
    if (s32ChnID < 0 || s32ChnID > RGA_MAX_CHN_NUM)
      return -RK_ERR_SYS_ILLEGAL_PARAM;
    target_chn = &g_rga_chns[s32ChnID];
    target_mutex = &g_rga_mtx;
    break;
  case RK_ID_VO:
    if (s32ChnID < 0 || s32ChnID > VO_MAX_CHN_NUM)
      return -RK_ERR_SYS_ILLEGAL_PARAM;
    target_chn = &g_vo_chns[s32ChnID];
    target_mutex = &g_vo_mtx;
    break;
  default:
    return -RK_ERR_SYS_NOT_SUPPORT;
  }

  MEDIA_BUFFER_IMPLE *mb = (MEDIA_BUFFER_IMPLE *)buffer;
  target_mutex->lock();
  if (target_chn->rkmedia_flow) {
    target_chn->rkmedia_flow->SendInput(mb->rkmedia_mb, 0);
  } else {
    target_mutex->unlock();
    return -RK_ERR_SYS_NOT_PERM;
  }

  target_mutex->unlock();
  return RK_ERR_SYS_OK;
}

/********************************************************************
 * Vi api
 ********************************************************************/
RK_S32 RK_MPI_VI_SetChnAttr(VI_PIPE ViPipe, VI_CHN ViChn,
                            const VI_CHN_ATTR_S *pstChnAttr) {
  if ((ViPipe < 0) || (ViChn < 0) || (ViChn > VI_MAX_CHN_NUM))
    return -RK_ERR_VI_INVALID_CHNID;

  if (!pstChnAttr || !pstChnAttr->pcVideoNode)
    return -RK_ERR_VI_ILLEGAL_PARAM;

  g_vi_mtx.lock();
  if (g_vi_chns[ViChn].status != CHN_STATUS_CLOSED) {
    g_vi_mtx.unlock();
    return -RK_ERR_VI_BUSY;
  }

  memcpy(&g_vi_chns[ViChn].vi_attr.attr, pstChnAttr, sizeof(VI_CHN_ATTR_S));
  g_vi_chns[ViChn].status = CHN_STATUS_READY;
  g_vi_mtx.unlock();

  return RK_ERR_SYS_OK;
}

RK_S32 RK_MPI_VI_EnableChn(VI_PIPE ViPipe, VI_CHN ViChn) {
  if ((ViPipe < 0) || (ViChn < 0) || (ViChn > VI_MAX_CHN_NUM))
    return -RK_ERR_VI_INVALID_CHNID;

  g_vi_mtx.lock();
  if (g_vi_chns[ViChn].status != CHN_STATUS_READY) {
    g_vi_mtx.unlock();
    return (g_vi_chns[ViChn].status > CHN_STATUS_READY) ? -RK_ERR_VI_EXIST
                                                        : -RK_ERR_VI_NOT_CONFIG;
  }

  LOG("\n%s %s: Enable VI[%d:%d]:%s, %dx%d Start...\n", LOG_TAG, __func__,
      ViPipe, ViChn, g_vi_chns[ViChn].vi_attr.attr.pcVideoNode,
      g_vi_chns[ViChn].vi_attr.attr.u32Width,
      g_vi_chns[ViChn].vi_attr.attr.u32Height);

  // Reading yuv from camera
  std::string flow_name = "source_stream";
  std::string flow_param;
  PARAM_STRING_APPEND(flow_param, KEY_NAME, "v4l2_capture_stream");
  std::string stream_param;
  PARAM_STRING_APPEND_TO(stream_param, KEY_USE_LIBV4L2, 1);
  PARAM_STRING_APPEND_TO(stream_param, KEY_CAMERA_ID, ViPipe);
  PARAM_STRING_APPEND(stream_param, KEY_DEVICE,
                      g_vi_chns[ViChn].vi_attr.attr.pcVideoNode);
  PARAM_STRING_APPEND(stream_param, KEY_V4L2_CAP_TYPE,
                      KEY_V4L2_C_TYPE(VIDEO_CAPTURE));
  if (g_vi_chns[ViChn].vi_attr.attr.enBufType == VI_CHN_BUF_TYPE_MMAP) {
    PARAM_STRING_APPEND(stream_param, KEY_V4L2_MEM_TYPE,
                        KEY_V4L2_M_TYPE(MEMORY_MMAP));
  } else {
    PARAM_STRING_APPEND(stream_param, KEY_V4L2_MEM_TYPE,
                        KEY_V4L2_M_TYPE(MEMORY_DMABUF));
  }
  PARAM_STRING_APPEND_TO(stream_param, KEY_FRAMES,
                         g_vi_chns[ViChn].vi_attr.attr.u32BufCnt);
  PARAM_STRING_APPEND(
      stream_param, KEY_OUTPUTDATATYPE,
      ImageTypeToString(g_vi_chns[ViChn].vi_attr.attr.enPixFmt));
  PARAM_STRING_APPEND_TO(stream_param, KEY_BUFFER_WIDTH,
                         g_vi_chns[ViChn].vi_attr.attr.u32Width);
  PARAM_STRING_APPEND_TO(stream_param, KEY_BUFFER_HEIGHT,
                         g_vi_chns[ViChn].vi_attr.attr.u32Height);
  flow_param = easymedia::JoinFlowParam(flow_param, 1, stream_param);
  LOGD("\n#VI: v4l2 source flow param:\n%s\n", flow_param.c_str());
  RK_S8 s8RetryCnt = 3;
  while (s8RetryCnt > 0) {
    g_vi_chns[ViChn].rkmedia_flow = easymedia::REFLECTOR(
        Flow)::Create<easymedia::Flow>(flow_name.c_str(), flow_param.c_str());
    if (g_vi_chns[ViChn].rkmedia_flow)
      break; // Stop while
    LOG("WARN: VI[%d]:\"%s\" buffer may be occupied by other modules or apps, "
        "try again...\n",
        ViChn, g_vi_chns[ViChn].vi_attr.attr.pcVideoNode);
    s8RetryCnt--;
    msleep(50);
  }

  if (!g_vi_chns[ViChn].rkmedia_flow) {
    g_vi_mtx.unlock();
    return -RK_ERR_VI_BUSY;
  }

  g_vi_chns[ViChn].luma_buf_mtx.lock();
  g_vi_chns[ViChn].luma_buf_quit = false;
  g_vi_chns[ViChn].luma_buf_start = false;
  g_vi_chns[ViChn].luma_buf_mtx.unlock();

  RkmediaChnInitBuffer(&g_vi_chns[ViChn]);
  g_vi_chns[ViChn].rkmedia_flow->SetOutputCallBack(&g_vi_chns[ViChn],
                                                   FlowOutputCallback);
  g_vi_chns[ViChn].status = CHN_STATUS_OPEN;

  g_vi_mtx.unlock();
  LOG("\n%s %s: Enable VI[%d:%d]:%s, %dx%d End...\n", LOG_TAG, __func__, ViPipe,
      ViChn, g_vi_chns[ViChn].vi_attr.attr.pcVideoNode,
      g_vi_chns[ViChn].vi_attr.attr.u32Width,
      g_vi_chns[ViChn].vi_attr.attr.u32Height);

  return RK_ERR_SYS_OK;
}

RK_S32 RK_MPI_VI_DisableChn(VI_PIPE ViPipe, VI_CHN ViChn) {
  if ((ViPipe < 0) || (ViChn < 0) || (ViChn > VI_MAX_CHN_NUM))
    return -RK_ERR_SYS_ILLEGAL_PARAM;

  g_vi_mtx.lock();
  if (g_vi_chns[ViChn].status == CHN_STATUS_BIND) {
    g_vi_mtx.unlock();
    return -RK_ERR_SYS_NOT_PERM;
  }

  LOG("\n%s %s: Disable VI[%d:%d]:%s, %dx%d Start...\n", LOG_TAG, __func__,
      ViPipe, ViChn, g_vi_chns[ViChn].vi_attr.attr.pcVideoNode,
      g_vi_chns[ViChn].vi_attr.attr.u32Width,
      g_vi_chns[ViChn].vi_attr.attr.u32Height);
  RkmediaChnClearBuffer(&g_vi_chns[ViChn]);
  g_vi_chns[ViChn].status = CHN_STATUS_CLOSED;
  g_vi_chns[ViChn].luma_buf_mtx.lock();
  g_vi_chns[ViChn].luma_rkmedia_buf.reset();
  g_vi_chns[ViChn].luma_buf_cond.notify_all();
  g_vi_chns[ViChn].luma_buf_quit = true;
  g_vi_chns[ViChn].luma_buf_mtx.unlock();
  // VI flow Should be released last
  g_vi_chns[ViChn].rkmedia_flow.reset();
  if (!g_vi_chns[ViChn].buffer_list.empty()) {
    LOG("\n%s %s: clear buffer list again...\n", LOG_TAG, __func__);
    RkmediaChnClearBuffer(&g_vi_chns[ViChn]);
  }
  g_vi_mtx.unlock();

  LOG("\n%s %s: Disable VI[%d:%d]:%s, %dx%d End...\n", LOG_TAG, __func__,
      ViPipe, ViChn, g_vi_chns[ViChn].vi_attr.attr.pcVideoNode,
      g_vi_chns[ViChn].vi_attr.attr.u32Width,
      g_vi_chns[ViChn].vi_attr.attr.u32Height);

  return RK_ERR_SYS_OK;
}

static RK_U64
rkmediaCalculateRegionLuma(std::shared_ptr<easymedia::ImageBuffer> &rkmedia_mb,
                           const RECT_S *ptrRect) {
  RK_U64 sum = 0;
  ImageInfo &imgInfo = rkmedia_mb->GetImageInfo();

  if ((imgInfo.pix_fmt != PIX_FMT_YUV420P) &&
      (imgInfo.pix_fmt != PIX_FMT_NV12) && (imgInfo.pix_fmt != PIX_FMT_NV21) &&
      (imgInfo.pix_fmt != PIX_FMT_YUV422P) &&
      (imgInfo.pix_fmt != PIX_FMT_NV16) && (imgInfo.pix_fmt != PIX_FMT_NV61)) {
    LOG("ERROR: %s not support image type!\n", __func__);
    return 0;
  }

  if (((RK_S32)(ptrRect->s32X + ptrRect->u32Width) > imgInfo.width) ||
      ((RK_S32)(ptrRect->s32Y + ptrRect->u32Height) > imgInfo.height)) {
    LOG("ERROR: %s rect[%d,%d,%u,%u] out of image wxh[%d, %d]\n", __func__,
        ptrRect->s32X, ptrRect->s32Y, ptrRect->u32Width, ptrRect->u32Height,
        imgInfo.width, imgInfo.height);
    return 0;
  }

  RK_U32 line_size = imgInfo.vir_width;
  RK_U8 *rect_start =
      (RK_U8 *)rkmedia_mb->GetPtr() + ptrRect->s32Y * line_size + ptrRect->s32X;
  for (RK_U32 i = 0; i < ptrRect->u32Height; i++) {
    RK_U8 *line_start = rect_start + i * line_size;
    for (RK_U32 j = 0; j < ptrRect->u32Width; j++) {
      sum += *(line_start + j);
    }
  }

  return sum;
}

RK_S32 RK_MPI_VI_StartRegionLuma(VI_CHN ViChn) {
  if ((ViChn < 0) || (ViChn > VI_MAX_CHN_NUM))
    return -RK_ERR_VI_INVALID_CHNID;
  if (g_vi_chns[ViChn].status < CHN_STATUS_OPEN)
    return -RK_ERR_VI_NOTREADY;

  g_vi_chns[ViChn].luma_buf_mtx.lock();
  g_vi_chns[ViChn].luma_buf_start = true;
  g_vi_chns[ViChn].luma_buf_mtx.unlock();

  return RK_ERR_SYS_OK;
}

RK_S32 RK_MPI_VI_StopRegionLuma(VI_CHN ViChn) {
  if ((ViChn < 0) || (ViChn > VI_MAX_CHN_NUM))
    return -RK_ERR_VI_INVALID_CHNID;
  if (g_vi_chns[ViChn].status < CHN_STATUS_OPEN)
    return -RK_ERR_VI_NOTREADY;

  g_vi_chns[ViChn].luma_buf_mtx.lock();
  g_vi_chns[ViChn].luma_buf_start = false;
  g_vi_chns[ViChn].luma_rkmedia_buf.reset();
  g_vi_chns[ViChn].luma_buf_mtx.unlock();

  return RK_ERR_SYS_OK;
}

RK_S32 RK_MPI_VI_GetChnRegionLuma(VI_PIPE ViPipe, VI_CHN ViChn,
                                  const VIDEO_REGION_INFO_S *pstRegionInfo,
                                  RK_U64 *pu64LumaData, RK_S32 s32MilliSec) {
  RK_U32 u32ImgWidth = 0;
  RK_U32 u32ImgHeight = 0;
  RK_U32 u32XOffset = 0;
  RK_U32 u32YOffset = 0;

  if ((ViPipe < 0) || (ViChn < 0) || (ViChn > VI_MAX_CHN_NUM))
    return -RK_ERR_VI_INVALID_CHNID;

  if (!pstRegionInfo || !pstRegionInfo->u32RegionNum || !pu64LumaData)
    return -RK_ERR_VI_ILLEGAL_PARAM;

  std::shared_ptr<easymedia::ImageBuffer> rkmedia_mb;
  RkmediaChannel *target_chn = &g_vi_chns[ViChn];

  if (target_chn->status < CHN_STATUS_OPEN)
    return -RK_ERR_VI_NOTREADY;

  // input rgn check.
  u32ImgWidth = g_vi_chns[ViChn].vi_attr.attr.u32Width;
  u32ImgHeight = g_vi_chns[ViChn].vi_attr.attr.u32Height;
  for (RK_U32 i = 0; i < pstRegionInfo->u32RegionNum; i++) {
    u32XOffset =
        pstRegionInfo->pstRegion[i].s32X + pstRegionInfo->pstRegion[i].u32Width;
    u32YOffset = pstRegionInfo->pstRegion[i].s32Y +
                 pstRegionInfo->pstRegion[i].u32Height;
    if ((u32XOffset > u32ImgWidth) || (u32YOffset > u32ImgHeight)) {
      LOG("ERROR: [%s]: LumaRgn[%d]:<%d, %d, %d, %d> is invalid for "
          "VI[%d]:%dx%d\n",
          __func__, i, pstRegionInfo->pstRegion[i].s32X,
          pstRegionInfo->pstRegion[i].s32Y,
          pstRegionInfo->pstRegion[i].u32Width,
          pstRegionInfo->pstRegion[i].u32Height, ViChn, u32ImgWidth,
          u32ImgHeight);
      return -RK_ERR_VI_ILLEGAL_PARAM;
    }
  }

  {
    // The {} here is to limit the scope of locking. The lock is only
    // used to find the buffer, and the accumulation of the buffer is
    // outside the lock range. This is good for frame rate.
    std::unique_lock<std::mutex> lck(target_chn->luma_buf_mtx);
    target_chn->luma_buf_start = true;
    if (!target_chn->luma_rkmedia_buf) {
      if (s32MilliSec < 0 && !target_chn->luma_buf_quit) {
        target_chn->luma_buf_cond.wait(lck);
      } else if (s32MilliSec > 0) {
        if (target_chn->luma_buf_cond.wait_for(
                lck, std::chrono::milliseconds(s32MilliSec)) ==
            std::cv_status::timeout)
          return -RK_ERR_VI_TIMEOUT;
      } else {
        return -RK_ERR_VI_BUF_EMPTY;
      }
    }
    if (target_chn->luma_rkmedia_buf)
      rkmedia_mb = std::static_pointer_cast<easymedia::ImageBuffer>(
          target_chn->luma_rkmedia_buf);

    target_chn->luma_rkmedia_buf.reset();
  }

  if (!rkmedia_mb)
    return -RK_ERR_VI_BUF_EMPTY;

  for (RK_U32 i = 0; i < pstRegionInfo->u32RegionNum; i++)
    *(pu64LumaData + i) =
        rkmediaCalculateRegionLuma(rkmedia_mb, (pstRegionInfo->pstRegion + i));

  return RK_ERR_SYS_OK;
}

RK_S32 RK_MPI_VI_StartStream(VI_PIPE ViPipe, VI_CHN ViChn) {
  if ((ViPipe < 0) || (ViChn < 0) || (ViChn > VI_MAX_CHN_NUM))
    return -RK_ERR_VI_INVALID_CHNID;

  g_vi_mtx.lock();
  if (g_vi_chns[ViChn].status < CHN_STATUS_OPEN) {
    g_vi_mtx.unlock();
    return -RK_ERR_VI_BUSY;
  }

  if (!g_vi_chns[ViChn].rkmedia_flow) {
    g_vi_mtx.unlock();
    return -RK_ERR_VI_NOTREADY;
  }

  g_vi_chns[ViChn].rkmedia_flow->StartStream();
  g_vi_mtx.unlock();

  return RK_ERR_SYS_OK;
}

/********************************************************************
 * Venc api
 ********************************************************************/
static RK_S32 RkmediaCreateJpegSnapPipeline(RkmediaChannel *VenChn) {
  std::shared_ptr<easymedia::Flow> video_encoder_flow;
  std::shared_ptr<easymedia::Flow> video_decoder_flow;
  std::shared_ptr<easymedia::Flow> video_jpeg_flow;
  std::shared_ptr<easymedia::Flow> video_rga_flow;
  RK_BOOL bEnableRga = RK_FALSE;
  RK_U32 u32InFpsNum = 1;
  RK_U32 u32InFpsDen = 1;
  RK_U32 u32OutFpsNum = 1;
  RK_U32 u32OutFpsDen = 1;
  RK_S32 s32ZoomWidth = 0;
  RK_S32 s32ZoomHeight = 0;
  RK_S32 s32ZoomVirWidth = 0;
  RK_S32 s32ZoomVirHeight = 0;
  const RK_CHAR *pcRkmediaRcMode = nullptr;
  const RK_CHAR *pcRkmediaCodecType = nullptr;
  VENC_CHN_ATTR_S *stVencChnAttr = &VenChn->venc_attr.attr;
  VENC_ROTATION_E enRotation = stVencChnAttr->stVencAttr.enRotation;
  // pre encoder bps, in FIXQP mode, bps is invalid.
  RK_S32 pre_enc_bps = 2000000;
  RK_S32 mjpeg_bps = 0;
  RK_S32 video_width = stVencChnAttr->stVencAttr.u32PicWidth;
  RK_S32 video_height = stVencChnAttr->stVencAttr.u32PicHeight;
  RK_S32 vir_width = stVencChnAttr->stVencAttr.u32VirWidth;
  RK_S32 vir_height = stVencChnAttr->stVencAttr.u32VirHeight;
  std::string pixel_format =
      ImageTypeToString(stVencChnAttr->stVencAttr.imageType);

  if (stVencChnAttr->stVencAttr.enType == RK_CODEC_TYPE_MJPEG) {
    // MJPEG:
    if (stVencChnAttr->stRcAttr.enRcMode == VENC_RC_MODE_MJPEGCBR) {
      mjpeg_bps = stVencChnAttr->stRcAttr.stMjpegCbr.u32BitRate;
      u32InFpsNum = stVencChnAttr->stRcAttr.stMjpegCbr.u32SrcFrameRateNum;
      u32InFpsDen = stVencChnAttr->stRcAttr.stMjpegCbr.u32SrcFrameRateDen;
      u32OutFpsNum = stVencChnAttr->stRcAttr.stMjpegCbr.fr32DstFrameRateNum;
      u32OutFpsDen = stVencChnAttr->stRcAttr.stMjpegCbr.fr32DstFrameRateDen;
      pcRkmediaRcMode = KEY_CBR;
    } else if (stVencChnAttr->stRcAttr.enRcMode == VENC_RC_MODE_MJPEGVBR) {
      mjpeg_bps = stVencChnAttr->stRcAttr.stMjpegVbr.u32BitRate;
      u32InFpsNum = stVencChnAttr->stRcAttr.stMjpegVbr.u32SrcFrameRateNum;
      u32InFpsDen = stVencChnAttr->stRcAttr.stMjpegVbr.u32SrcFrameRateDen;
      u32OutFpsNum = stVencChnAttr->stRcAttr.stMjpegVbr.fr32DstFrameRateNum;
      u32OutFpsDen = stVencChnAttr->stRcAttr.stMjpegVbr.fr32DstFrameRateDen;
      pcRkmediaRcMode = KEY_VBR;
    } else {
      LOG("ERROR: [%s]: Invalid RcMode[%d]\n", __func__,
          stVencChnAttr->stRcAttr.enRcMode);
      return -RK_ERR_VENC_ILLEGAL_PARAM;
    }

    if ((mjpeg_bps < 2000) || (mjpeg_bps > 100000000)) {
      LOG("ERROR: [%s]: Invalid BitRate[%d], should be [2000, 100000000]\n",
          __func__, mjpeg_bps);
      return -RK_ERR_VENC_ILLEGAL_PARAM;
    }
    if (!u32InFpsNum) {
      LOG("ERROR: [%s]: Invalid src frame rate [%d/%d]\n", __func__,
          u32InFpsNum, u32InFpsDen);
      return -RK_ERR_VENC_ILLEGAL_PARAM;
    }
    if (!u32OutFpsNum) {
      LOG("ERROR: [%s]: Invalid dst frame rate [%d/%d]\n", __func__,
          u32OutFpsNum, u32OutFpsDen);
      return -RK_ERR_VENC_ILLEGAL_PARAM;
    }
    pcRkmediaCodecType = VIDEO_MJPEG;
    // Scaling parameter analysis
    s32ZoomWidth = stVencChnAttr->stVencAttr.stAttrMjpege.u32ZoomWidth;
    s32ZoomHeight = stVencChnAttr->stVencAttr.stAttrMjpege.u32ZoomHeight;
    s32ZoomVirWidth = stVencChnAttr->stVencAttr.stAttrMjpege.u32ZoomVirWidth;
    s32ZoomVirHeight = stVencChnAttr->stVencAttr.stAttrMjpege.u32ZoomVirHeight;
  } else {
    // JPEG
    pcRkmediaCodecType = IMAGE_JPEG;
    // Scaling parameter analysis
    s32ZoomWidth = stVencChnAttr->stVencAttr.stAttrJpege.u32ZoomWidth;
    s32ZoomHeight = stVencChnAttr->stVencAttr.stAttrJpege.u32ZoomHeight;
    s32ZoomVirWidth = stVencChnAttr->stVencAttr.stAttrJpege.u32ZoomVirWidth;
    s32ZoomVirHeight = stVencChnAttr->stVencAttr.stAttrJpege.u32ZoomVirHeight;
  }

  std::string flow_name = "video_enc";
  std::string flow_param = "";
  std::string enc_param = "";
  PARAM_STRING_APPEND(flow_param, KEY_NAME, "rkmpp");
  PARAM_STRING_APPEND(flow_param, KEY_INPUTDATATYPE, pixel_format);
  PARAM_STRING_APPEND(flow_param, KEY_OUTPUTDATATYPE, VIDEO_H265);
  PARAM_STRING_APPEND_TO(enc_param, KEY_BUFFER_WIDTH, video_width);
  PARAM_STRING_APPEND_TO(enc_param, KEY_BUFFER_HEIGHT, video_height);
  PARAM_STRING_APPEND_TO(enc_param, KEY_BUFFER_VIR_WIDTH, vir_width);
  PARAM_STRING_APPEND_TO(enc_param, KEY_BUFFER_VIR_HEIGHT, vir_height);
  PARAM_STRING_APPEND_TO(enc_param, KEY_COMPRESS_BITRATE, pre_enc_bps);
  PARAM_STRING_APPEND_TO(enc_param, KEY_COMPRESS_BITRATE_MAX, pre_enc_bps);
  PARAM_STRING_APPEND_TO(enc_param, KEY_COMPRESS_BITRATE_MIN, pre_enc_bps);
  PARAM_STRING_APPEND(enc_param, KEY_VIDEO_GOP, "1");
  // set input fps
  std::string str_fps_in;
  str_fps_in.append(std::to_string(u32InFpsNum))
      .append("/")
      .append(std::to_string(u32InFpsDen));
  PARAM_STRING_APPEND(enc_param, KEY_FPS_IN, str_fps_in);
  // set output fps
  std::string str_fps_out;
  str_fps_out.append(std::to_string(u32OutFpsNum))
      .append("/")
      .append(std::to_string(u32OutFpsDen));
  PARAM_STRING_APPEND(enc_param, KEY_FPS, str_fps_out);
  // jpeg pre encoder work in fixqp mode
  PARAM_STRING_APPEND(enc_param, KEY_COMPRESS_RC_MODE, KEY_FIXQP);
  PARAM_STRING_APPEND(enc_param, KEY_COMPRESS_QP_INIT, "15");
  PARAM_STRING_APPEND_TO(enc_param, KEY_ROTATION, enRotation);

  flow_param = easymedia::JoinFlowParam(flow_param, 1, enc_param);
  LOGD("\n#JPEG: Pre encoder flow param:\n%s\n", flow_param.c_str());
  video_encoder_flow = easymedia::REFLECTOR(Flow)::Create<easymedia::Flow>(
      flow_name.c_str(), flow_param.c_str());
  if (!video_encoder_flow) {
    LOG("ERROR: [%s]: Create flow %s failed\n", __func__, flow_name.c_str());
    return -RK_ERR_VENC_ILLEGAL_PARAM;
  }

  flow_name = "video_dec";
  flow_param = "";
  PARAM_STRING_APPEND(flow_param, KEY_NAME, "rkmpp");
  PARAM_STRING_APPEND(flow_param, KEY_INPUTDATATYPE, VIDEO_H265);
  PARAM_STRING_APPEND(flow_param, KEY_OUTPUTDATATYPE, IMAGE_NV12);
  std::string dec_param = "";
  PARAM_STRING_APPEND(dec_param, KEY_INPUTDATATYPE, VIDEO_H265);
  PARAM_STRING_APPEND_TO(dec_param, KEY_MPP_SPLIT_MODE, 0);
  PARAM_STRING_APPEND_TO(dec_param, KEY_OUTPUT_TIMEOUT, -1);

  flow_param = easymedia::JoinFlowParam(flow_param, 1, dec_param);
  LOGD("\n#JPEG: Pre decoder flow param:\n%s\n", flow_param.c_str());
  video_decoder_flow = easymedia::REFLECTOR(Flow)::Create<easymedia::Flow>(
      flow_name.c_str(), flow_param.c_str());
  if (!video_decoder_flow) {
    LOG("ERROR: [%s]: Create flow %s failed\n", __func__, flow_name.c_str());
    return -RK_ERR_VENC_ILLEGAL_PARAM;
  }

  RK_S32 jpeg_width = video_width;
  RK_S32 jpeg_height = video_height;
  RK_S32 s32RgaWidth = s32ZoomWidth;
  RK_S32 s32RgaHeight = s32ZoomHeight;
  RK_S32 s32RgaVirWidht = UPALIGNTO16(s32ZoomVirWidth);
  RK_S32 s32RgaVirHeight = UPALIGNTO16(s32ZoomVirHeight);
  if ((enRotation == VENC_ROTATION_90) || (enRotation == VENC_ROTATION_270)) {
    jpeg_width = video_height;
    jpeg_height = video_width;
    s32RgaWidth = s32ZoomHeight;
    s32RgaHeight = s32ZoomWidth;
    s32RgaVirWidht = UPALIGNTO16(s32ZoomVirHeight);
    s32RgaVirHeight = UPALIGNTO16(s32ZoomVirWidth);
  }

  RK_S32 jpeg_vir_height = UPALIGNTO(jpeg_height, 8);
  // The virtual width of the image output by the hevc decoder
  // is an odd multiple of 256.
  RK_S32 jpeg_vir_width = UPALIGNTO(jpeg_width, 256);
  if (((jpeg_vir_width / 256) % 2) == 0)
    jpeg_vir_width += 256;

  // When the zoom parameter is valid and is not equal to
  // the original resolution, the zoom function will be turned on.
  if ((s32RgaWidth > 0) && (s32RgaHeight > 0) && (s32RgaVirWidht > 0) &&
      (s32RgaVirHeight > 0) &&
      ((s32RgaWidth != video_width) || (s32RgaHeight != video_height) ||
       (s32RgaVirWidht != vir_width) || (s32RgaVirHeight != vir_height))) {
    flow_name = "filter";
    flow_param = "";
    PARAM_STRING_APPEND(flow_param, KEY_NAME, "rkrga");
    PARAM_STRING_APPEND(flow_param, KEY_INPUTDATATYPE, IMAGE_NV12);
    // Set output buffer type.
    PARAM_STRING_APPEND(flow_param, KEY_OUTPUTDATATYPE, IMAGE_NV12);
    // Set output buffer size.
    PARAM_STRING_APPEND_TO(flow_param, KEY_BUFFER_WIDTH, s32RgaWidth);
    PARAM_STRING_APPEND_TO(flow_param, KEY_BUFFER_HEIGHT, s32RgaHeight);
    PARAM_STRING_APPEND_TO(flow_param, KEY_BUFFER_VIR_WIDTH, s32RgaVirWidht);
    PARAM_STRING_APPEND_TO(flow_param, KEY_BUFFER_VIR_HEIGHT, s32RgaVirHeight);
    // enable buffer pool?
    // PARAM_STRING_APPEND(flow_param, KEY_MEM_TYPE, KEY_MEM_HARDWARE);
    // PARAM_STRING_APPEND_TO(flow_param, KEY_MEM_CNT, u16BufPoolCnt);

    std::string filter_param = "";
    ImageRect src_rect = {0, 0, jpeg_width, jpeg_height};
    ImageRect dst_rect = {0, 0, s32RgaWidth, s32RgaHeight};
    std::vector<ImageRect> rect_vect;
    rect_vect.push_back(src_rect);
    rect_vect.push_back(dst_rect);
    PARAM_STRING_APPEND(filter_param, KEY_BUFFER_RECT,
                        easymedia::TwoImageRectToString(rect_vect).c_str());
    PARAM_STRING_APPEND_TO(filter_param, KEY_BUFFER_ROTATE, 0);
    flow_param = easymedia::JoinFlowParam(flow_param, 1, filter_param);
    LOGD("\n#JPEG: Pre process flow param:\n%s\n", flow_param.c_str());
    video_rga_flow = easymedia::REFLECTOR(Flow)::Create<easymedia::Flow>(
        flow_name.c_str(), flow_param.c_str());
    if (!video_rga_flow) {
      LOG("ERROR: [%s]: Create flow filter:rga failed\n", __func__);
      return -RK_ERR_VENC_ILLEGAL_PARAM;
    }
    // enable rga process.
    bEnableRga = RK_TRUE;
    jpeg_width = s32RgaWidth;
    jpeg_height = s32RgaHeight;
    jpeg_vir_width = s32RgaVirWidht;
    jpeg_vir_height = s32RgaVirHeight;
  }

  flow_name = "video_enc";
  flow_param = "";
  PARAM_STRING_APPEND(flow_param, KEY_NAME, "rkmpp");
  PARAM_STRING_APPEND(flow_param, KEY_INPUTDATATYPE, IMAGE_NV12);
  PARAM_STRING_APPEND(flow_param, KEY_OUTPUTDATATYPE, pcRkmediaCodecType);
  enc_param = "";
  PARAM_STRING_APPEND_TO(enc_param, KEY_BUFFER_WIDTH, jpeg_width);
  PARAM_STRING_APPEND_TO(enc_param, KEY_BUFFER_HEIGHT, jpeg_height);
  PARAM_STRING_APPEND_TO(enc_param, KEY_BUFFER_VIR_WIDTH, jpeg_vir_width);
  PARAM_STRING_APPEND_TO(enc_param, KEY_BUFFER_VIR_HEIGHT, jpeg_vir_height);

  if (stVencChnAttr->stVencAttr.enType == RK_CODEC_TYPE_MJPEG) {
    // MJPEG
    PARAM_STRING_APPEND_TO(enc_param, KEY_COMPRESS_BITRATE_MAX, mjpeg_bps);
    PARAM_STRING_APPEND(enc_param, KEY_FPS, str_fps_out);
    PARAM_STRING_APPEND(enc_param, KEY_FPS_IN, str_fps_out);
    PARAM_STRING_APPEND(enc_param, KEY_COMPRESS_RC_MODE, pcRkmediaRcMode);
  } else {
    // JPEG
    PARAM_STRING_APPEND_TO(enc_param, KEY_JPEG_QFACTOR, 50);
  }

  flow_param = easymedia::JoinFlowParam(flow_param, 1, enc_param);
  LOGD("\n#JPEG: [%s] encoder flow param:\n%s\n", pcRkmediaCodecType,
       flow_param.c_str());
  video_jpeg_flow = easymedia::REFLECTOR(Flow)::Create<easymedia::Flow>(
      flow_name.c_str(), flow_param.c_str());
  if (!video_jpeg_flow) {
    LOG("ERROR: [%s]: Create flow %s failed\n", __func__, flow_name.c_str());
    return -RK_ERR_VENC_ILLEGAL_PARAM;
  }

#if DEBUG_JPEG_SAVE_H265
  std::shared_ptr<easymedia::Flow> video_save_flow;
  flow_name = "file_write_flow";
  flow_param = "";
  PARAM_STRING_APPEND(flow_param, KEY_PATH, "/tmp/jpeg.h265");
  PARAM_STRING_APPEND(flow_param, KEY_OPEN_MODE, "w+");
  printf("\n#FileWrite:\n%s\n", flow_param.c_str());
  video_save_flow = easymedia::REFLECTOR(Flow)::Create<easymedia::Flow>(
      flow_name.c_str(), flow_param.c_str());
  if (!video_save_flow) {
    fprintf(stderr, "Create flow %s failed\n", flow_name.c_str());
    exit(EXIT_FAILURE);
  }
  video_encoder_flow->AddDownFlow(video_save_flow, 0, 0);
#endif // DEBUG_JPEG_SAVE_H265

  video_encoder_flow->SetFlowTag("JpegPreEncoder");
  video_decoder_flow->SetFlowTag("JpegPreDecoder");
  video_jpeg_flow->SetFlowTag("JpegEncoder");
  if (bEnableRga)
    video_rga_flow->SetFlowTag("JpegRgaFilter");

  // rkmedia flow bind.
  if (bEnableRga) {
    video_rga_flow->AddDownFlow(video_jpeg_flow, 0, 0);
    video_decoder_flow->AddDownFlow(video_rga_flow, 0, 0);
  } else {
    video_decoder_flow->AddDownFlow(video_jpeg_flow, 0, 0);
  }
  video_encoder_flow->AddDownFlow(video_decoder_flow, 0, 0);
  // Init buffer list.
  RkmediaChnInitBuffer(VenChn);
  video_jpeg_flow->SetOutputCallBack(VenChn, FlowOutputCallback);

  VenChn->rkmedia_flow = video_encoder_flow;
  VenChn->rkmedia_flow_list.push_back(video_decoder_flow);
  if (bEnableRga)
    VenChn->rkmedia_flow_list.push_back(video_rga_flow);
  VenChn->rkmedia_flow_list.push_back(video_jpeg_flow);
  if (pipe2(VenChn->wake_fd, O_CLOEXEC) == -1) {
    VenChn->wake_fd[0] = 0;
    VenChn->wake_fd[1] = 0;
    LOG("WARN: %s Create pipe failed!\n");
  }
  VenChn->status = CHN_STATUS_OPEN;

  return RK_ERR_SYS_OK;
}

RK_S32 RK_MPI_VENC_CreateChn(VENC_CHN VeChn, VENC_CHN_ATTR_S *stVencChnAttr) {
  if ((VeChn < 0) || (VeChn >= VENC_MAX_CHN_NUM))
    return -RK_ERR_VENC_INVALID_CHNID;

  if (!stVencChnAttr)
    return -RK_ERR_VENC_NULL_PTR;

  g_venc_mtx.lock();
  if (g_venc_chns[VeChn].status != CHN_STATUS_CLOSED) {
    g_venc_mtx.unlock();
    return -RK_ERR_VENC_EXIST;
  }

  if ((stVencChnAttr->stVencAttr.enRotation != VENC_ROTATION_0) &&
      (stVencChnAttr->stVencAttr.enRotation != VENC_ROTATION_90) &&
      (stVencChnAttr->stVencAttr.enRotation != VENC_ROTATION_180) &&
      (stVencChnAttr->stVencAttr.enRotation != VENC_ROTATION_270)) {
    LOG("WARN: Venc[%d]: rotation invalid! use default 0\n", VeChn);
    stVencChnAttr->stVencAttr.enRotation = VENC_ROTATION_0;
  }

  LOG("\n%s %s: Enable VENC[%d], Type:%d Start...\n", LOG_TAG, __func__, VeChn,
      stVencChnAttr->stVencAttr.enType);

  // save venc_attr to venc chn.
  memset(&g_venc_chns[VeChn].venc_attr, 0, sizeof(RkmediaVencAttr));
  memcpy(&g_venc_chns[VeChn].venc_attr.attr, stVencChnAttr,
         sizeof(VENC_CHN_ATTR_S));

  if ((stVencChnAttr->stVencAttr.enType == RK_CODEC_TYPE_JPEG) ||
      (stVencChnAttr->stVencAttr.enType == RK_CODEC_TYPE_MJPEG)) {
    RK_S32 ret = RkmediaCreateJpegSnapPipeline(&g_venc_chns[VeChn]);
    g_venc_mtx.unlock();
    LOG("\n%s %s: Enable VENC[%d], Type:%d End...\n", LOG_TAG, __func__, VeChn,
        stVencChnAttr->stVencAttr.enType);
    return ret;
  }

  std::string flow_name = "video_enc";
  std::string flow_param;
  PARAM_STRING_APPEND(flow_param, KEY_NAME, "rkmpp");
  PARAM_STRING_APPEND(flow_param, KEY_INPUTDATATYPE,
                      ImageTypeToString(stVencChnAttr->stVencAttr.imageType));
  PARAM_STRING_APPEND(flow_param, KEY_OUTPUTDATATYPE,
                      CodecToString(stVencChnAttr->stVencAttr.enType));

  std::string enc_param;
  PARAM_STRING_APPEND_TO(enc_param, KEY_BUFFER_WIDTH,
                         stVencChnAttr->stVencAttr.u32PicWidth);
  PARAM_STRING_APPEND_TO(enc_param, KEY_BUFFER_HEIGHT,
                         stVencChnAttr->stVencAttr.u32PicHeight);
  PARAM_STRING_APPEND_TO(enc_param, KEY_BUFFER_VIR_WIDTH,
                         stVencChnAttr->stVencAttr.u32VirWidth);
  PARAM_STRING_APPEND_TO(enc_param, KEY_BUFFER_VIR_HEIGHT,
                         stVencChnAttr->stVencAttr.u32VirHeight);
  PARAM_STRING_APPEND_TO(enc_param, KEY_ROTATION,
                         stVencChnAttr->stVencAttr.enRotation);
  switch (stVencChnAttr->stVencAttr.enType) {
  case RK_CODEC_TYPE_H264:
    PARAM_STRING_APPEND_TO(enc_param, KEY_PROFILE,
                           stVencChnAttr->stVencAttr.u32Profile);
    break;
  default:
    break;
  }

  std::string str_fps_in, str_fps;
  switch (stVencChnAttr->stRcAttr.enRcMode) {
  case VENC_RC_MODE_H264CBR:
    PARAM_STRING_APPEND(enc_param, KEY_COMPRESS_RC_MODE, KEY_CBR);
    PARAM_STRING_APPEND_TO(enc_param, KEY_VIDEO_GOP,
                           stVencChnAttr->stRcAttr.stH264Cbr.u32Gop);
    PARAM_STRING_APPEND_TO(enc_param, KEY_COMPRESS_BITRATE,
                           stVencChnAttr->stRcAttr.stH264Cbr.u32BitRate);
    str_fps_in
        .append(std::to_string(
            stVencChnAttr->stRcAttr.stH264Cbr.u32SrcFrameRateNum))
        .append("/")
        .append(std::to_string(
            stVencChnAttr->stRcAttr.stH264Cbr.u32SrcFrameRateDen));
    PARAM_STRING_APPEND(enc_param, KEY_FPS_IN, str_fps_in);

    str_fps
        .append(std::to_string(
            stVencChnAttr->stRcAttr.stH264Cbr.fr32DstFrameRateNum))
        .append("/")
        .append(std::to_string(
            stVencChnAttr->stRcAttr.stH264Cbr.fr32DstFrameRateDen));
    PARAM_STRING_APPEND(enc_param, KEY_FPS, str_fps);
    break;
  case VENC_RC_MODE_H264VBR:
    PARAM_STRING_APPEND(enc_param, KEY_COMPRESS_RC_MODE, KEY_VBR);
    PARAM_STRING_APPEND_TO(enc_param, KEY_VIDEO_GOP,
                           stVencChnAttr->stRcAttr.stH264Vbr.u32Gop);
    PARAM_STRING_APPEND_TO(enc_param, KEY_COMPRESS_BITRATE_MAX,
                           stVencChnAttr->stRcAttr.stH264Vbr.u32MaxBitRate);
    str_fps_in
        .append(std::to_string(
            stVencChnAttr->stRcAttr.stH264Vbr.u32SrcFrameRateNum))
        .append("/")
        .append(std::to_string(
            stVencChnAttr->stRcAttr.stH264Vbr.u32SrcFrameRateDen));
    PARAM_STRING_APPEND(enc_param, KEY_FPS_IN, str_fps_in);

    str_fps
        .append(std::to_string(
            stVencChnAttr->stRcAttr.stH264Vbr.fr32DstFrameRateNum))
        .append("/")
        .append(std::to_string(
            stVencChnAttr->stRcAttr.stH264Vbr.fr32DstFrameRateDen));
    PARAM_STRING_APPEND(enc_param, KEY_FPS, str_fps);
    break;
  case VENC_RC_MODE_H264AVBR:
    PARAM_STRING_APPEND(enc_param, KEY_COMPRESS_RC_MODE, KEY_AVBR);
    PARAM_STRING_APPEND_TO(enc_param, KEY_VIDEO_GOP,
                           stVencChnAttr->stRcAttr.stH264Avbr.u32Gop);
    PARAM_STRING_APPEND_TO(enc_param, KEY_COMPRESS_BITRATE_MAX,
                           stVencChnAttr->stRcAttr.stH264Avbr.u32MaxBitRate);
    str_fps_in
        .append(std::to_string(
            stVencChnAttr->stRcAttr.stH264Avbr.u32SrcFrameRateNum))
        .append("/")
        .append(std::to_string(
            stVencChnAttr->stRcAttr.stH264Avbr.u32SrcFrameRateDen));
    PARAM_STRING_APPEND(enc_param, KEY_FPS_IN, str_fps_in);

    str_fps
        .append(std::to_string(
            stVencChnAttr->stRcAttr.stH264Vbr.fr32DstFrameRateNum))
        .append("/")
        .append(std::to_string(
            stVencChnAttr->stRcAttr.stH264Vbr.fr32DstFrameRateDen));
    PARAM_STRING_APPEND(enc_param, KEY_FPS, str_fps);
    break;
  case VENC_RC_MODE_H265CBR:
    PARAM_STRING_APPEND(enc_param, KEY_COMPRESS_RC_MODE, KEY_CBR);
    PARAM_STRING_APPEND_TO(enc_param, KEY_VIDEO_GOP,
                           stVencChnAttr->stRcAttr.stH265Cbr.u32Gop);
    PARAM_STRING_APPEND_TO(enc_param, KEY_COMPRESS_BITRATE,
                           stVencChnAttr->stRcAttr.stH265Cbr.u32BitRate);
    str_fps_in
        .append(std::to_string(
            stVencChnAttr->stRcAttr.stH265Cbr.u32SrcFrameRateNum))
        .append("/")
        .append(std::to_string(
            stVencChnAttr->stRcAttr.stH265Cbr.u32SrcFrameRateDen));
    PARAM_STRING_APPEND(enc_param, KEY_FPS_IN, str_fps_in);

    str_fps
        .append(std::to_string(
            stVencChnAttr->stRcAttr.stH265Cbr.fr32DstFrameRateNum))
        .append("/")
        .append(std::to_string(
            stVencChnAttr->stRcAttr.stH265Cbr.fr32DstFrameRateDen));
    PARAM_STRING_APPEND(enc_param, KEY_FPS, str_fps);
    break;
  case VENC_RC_MODE_H265VBR:
    PARAM_STRING_APPEND(enc_param, KEY_COMPRESS_RC_MODE, KEY_VBR);
    PARAM_STRING_APPEND_TO(enc_param, KEY_VIDEO_GOP,
                           stVencChnAttr->stRcAttr.stH265Vbr.u32Gop);
    PARAM_STRING_APPEND_TO(enc_param, KEY_COMPRESS_BITRATE_MAX,
                           stVencChnAttr->stRcAttr.stH265Vbr.u32MaxBitRate);

    str_fps_in
        .append(std::to_string(
            stVencChnAttr->stRcAttr.stH265Vbr.u32SrcFrameRateNum))
        .append("/")
        .append(std::to_string(
            stVencChnAttr->stRcAttr.stH265Vbr.u32SrcFrameRateDen));
    PARAM_STRING_APPEND(enc_param, KEY_FPS_IN, str_fps_in);

    str_fps
        .append(std::to_string(
            stVencChnAttr->stRcAttr.stH265Vbr.fr32DstFrameRateNum))
        .append("/")
        .append(std::to_string(
            stVencChnAttr->stRcAttr.stH265Vbr.fr32DstFrameRateDen));
    PARAM_STRING_APPEND(enc_param, KEY_FPS, str_fps);
    break;
  case VENC_RC_MODE_H265AVBR:
    PARAM_STRING_APPEND(enc_param, KEY_COMPRESS_RC_MODE, KEY_AVBR);
    PARAM_STRING_APPEND_TO(enc_param, KEY_VIDEO_GOP,
                           stVencChnAttr->stRcAttr.stH265Avbr.u32Gop);
    PARAM_STRING_APPEND_TO(enc_param, KEY_COMPRESS_BITRATE_MAX,
                           stVencChnAttr->stRcAttr.stH265Avbr.u32MaxBitRate);

    str_fps_in
        .append(std::to_string(
            stVencChnAttr->stRcAttr.stH265Avbr.u32SrcFrameRateNum))
        .append("/")
        .append(std::to_string(
            stVencChnAttr->stRcAttr.stH265Avbr.u32SrcFrameRateDen));
    PARAM_STRING_APPEND(enc_param, KEY_FPS_IN, str_fps_in);

    str_fps
        .append(std::to_string(
            stVencChnAttr->stRcAttr.stH265Avbr.fr32DstFrameRateNum))
        .append("/")
        .append(std::to_string(
            stVencChnAttr->stRcAttr.stH265Avbr.fr32DstFrameRateDen));
    PARAM_STRING_APPEND(enc_param, KEY_FPS, str_fps);
    break;
  case VENC_RC_MODE_MJPEGCBR:
    PARAM_STRING_APPEND(enc_param, KEY_COMPRESS_RC_MODE, KEY_CBR);
    PARAM_STRING_APPEND_TO(enc_param, KEY_COMPRESS_BITRATE,
                           stVencChnAttr->stRcAttr.stMjpegCbr.u32BitRate);
    str_fps_in
        .append(std::to_string(
            stVencChnAttr->stRcAttr.stMjpegCbr.u32SrcFrameRateNum))
        .append("/")
        .append(std::to_string(
            stVencChnAttr->stRcAttr.stMjpegCbr.u32SrcFrameRateDen));
    PARAM_STRING_APPEND(enc_param, KEY_FPS_IN, str_fps_in);

    str_fps
        .append(std::to_string(
            stVencChnAttr->stRcAttr.stMjpegCbr.fr32DstFrameRateNum))
        .append("/")
        .append(std::to_string(
            stVencChnAttr->stRcAttr.stMjpegCbr.fr32DstFrameRateDen));
    PARAM_STRING_APPEND(enc_param, KEY_FPS, str_fps);
    break;
  default:
    break;
  }

  PARAM_STRING_APPEND_TO(enc_param, KEY_FULL_RANGE, 0);
  // PARAM_STRING_APPEND_TO(enc_param, KEY_REF_FRM_CFG,
  //                       stVencChnAttr->stGopAttr.enGopMode);

  flow_param = easymedia::JoinFlowParam(flow_param, 1, enc_param);
  g_venc_chns[VeChn].rkmedia_flow = easymedia::REFLECTOR(
      Flow)::Create<easymedia::Flow>("video_enc", flow_param.c_str());
  if (!g_venc_chns[VeChn].rkmedia_flow) {
    g_venc_mtx.unlock();
    return -RK_ERR_VENC_BUSY;
  }
  // easymedia::video_encoder_enable_statistics(g_venc_chns[VeChn].rkmedia_flow,
  // 1);
  RkmediaChnInitBuffer(&g_venc_chns[VeChn]);
  g_venc_chns[VeChn].rkmedia_flow->SetOutputCallBack(&g_venc_chns[VeChn],
                                                     FlowOutputCallback);
  if (pipe2(g_venc_chns[VeChn].wake_fd, O_CLOEXEC) == -1) {
    g_venc_chns[VeChn].wake_fd[0] = 0;
    g_venc_chns[VeChn].wake_fd[1] = 0;
    LOG("WARN: %s Create pipe failed!\n");
  }
  g_venc_chns[VeChn].status = CHN_STATUS_OPEN;
  g_venc_mtx.unlock();
  if (stVencChnAttr->stGopAttr.enGopMode >= VENC_GOPMODE_NORMALP) {
    RK_MPI_VENC_SetGopMode(VeChn, &stVencChnAttr->stGopAttr);
  }
  LOG("\n%s %s: Enable VENC[%d], Type:%d End...\n", LOG_TAG, __func__, VeChn,
      stVencChnAttr->stVencAttr.enType);

  return RK_ERR_SYS_OK;
}

RK_S32 RK_MPI_VENC_GetVencChnAttr(VENC_CHN VeChn,
                                  VENC_CHN_ATTR_S *stVencChnAttr) {
  if (!stVencChnAttr)
    return RK_ERR_SYS_ILLEGAL_PARAM;

  if ((VeChn < 0) || (VeChn >= VENC_MAX_CHN_NUM))
    return -RK_ERR_VENC_INVALID_CHNID;

  g_venc_mtx.lock();
  if (g_venc_chns[VeChn].status < CHN_STATUS_OPEN) {
    g_venc_mtx.unlock();
    return -RK_ERR_VENC_NOTREADY;
  }

  memcpy(stVencChnAttr, &g_venc_chns[VeChn].venc_attr.attr,
         sizeof(VENC_CHN_ATTR_S));
  g_venc_mtx.unlock();

  return RK_ERR_SYS_OK;
}

RK_S32 RK_MPI_VENC_SetVencChnAttr(VENC_CHN VeChn,
                                  VENC_CHN_ATTR_S *stVencChnAttr) {
  if (!stVencChnAttr)
    return RK_ERR_SYS_ILLEGAL_PARAM;

  if ((VeChn < 0) || (VeChn >= VENC_MAX_CHN_NUM))
    return -RK_ERR_VENC_INVALID_CHNID;

  int ret = 0;
  // set venc other para
  if (stVencChnAttr->stVencAttr.u32Profile !=
          g_venc_chns[VeChn].venc_attr.attr.stVencAttr.u32Profile ||
      stVencChnAttr->stVencAttr.stAttrH264e.u32Level !=
          g_venc_chns[VeChn].venc_attr.attr.stVencAttr.stAttrH264e.u32Level) {
    ret |= RK_MPI_VENC_SetAvcProfile(
        VeChn, stVencChnAttr->stVencAttr.u32Profile,
        stVencChnAttr->stVencAttr.stAttrH264e.u32Level);
  }
  if (ret)
    return ret;

  if (g_venc_chns[VeChn].venc_attr.attr.stVencAttr.u32VirWidth !=
          stVencChnAttr->stVencAttr.u32VirWidth ||
      g_venc_chns[VeChn].venc_attr.attr.stVencAttr.u32VirHeight !=
          stVencChnAttr->stVencAttr.u32VirHeight ||
      g_venc_chns[VeChn].venc_attr.attr.stVencAttr.u32PicWidth !=
          stVencChnAttr->stVencAttr.u32PicWidth ||
      g_venc_chns[VeChn].venc_attr.attr.stVencAttr.u32PicHeight !=
          stVencChnAttr->stVencAttr.u32PicHeight) {
    VENC_RESOLUTION_PARAM_S solution_attr;
    solution_attr.u32VirWidth = stVencChnAttr->stVencAttr.u32VirWidth;
    solution_attr.u32VirHeight = stVencChnAttr->stVencAttr.u32VirHeight;
    solution_attr.u32Width = stVencChnAttr->stVencAttr.u32PicWidth;
    solution_attr.u32Height = stVencChnAttr->stVencAttr.u32PicHeight;
    ret |= RK_MPI_VENC_SetResolution(VeChn, solution_attr);
  }
  if (ret)
    return ret;

  // set rc
  if (stVencChnAttr->stRcAttr.enRcMode !=
      g_venc_chns[VeChn].venc_attr.attr.stRcAttr.enRcMode) {
    ret |= RK_MPI_VENC_SetRcMode(VeChn, stVencChnAttr->stRcAttr.enRcMode);
  }

  RK_U32 u32BitRate = 0;
  RK_U32 u32MinBitRate = 0;
  RK_U32 u32MaxBitRate = 0;
  switch (g_venc_chns[VeChn].venc_attr.attr.stRcAttr.enRcMode) {
  case VENC_RC_MODE_H264CBR:
    if (g_venc_chns[VeChn].venc_attr.attr.stRcAttr.stH264Cbr.u32BitRate !=
        stVencChnAttr->stRcAttr.stH264Cbr.u32BitRate)
      u32BitRate = stVencChnAttr->stRcAttr.stH264Cbr.u32BitRate;
    break;
  case VENC_RC_MODE_H264VBR:
    if (g_venc_chns[VeChn].venc_attr.attr.stRcAttr.stH264Vbr.u32MaxBitRate !=
        stVencChnAttr->stRcAttr.stH264Vbr.u32MaxBitRate)
      u32MaxBitRate = stVencChnAttr->stRcAttr.stH264Vbr.u32MaxBitRate;
    break;
  case VENC_RC_MODE_H264AVBR:
    if (g_venc_chns[VeChn].venc_attr.attr.stRcAttr.stH264Avbr.u32MaxBitRate !=
        stVencChnAttr->stRcAttr.stH264Avbr.u32MaxBitRate)
      u32MaxBitRate = stVencChnAttr->stRcAttr.stH264Avbr.u32MaxBitRate;
    break;
  case VENC_RC_MODE_MJPEGCBR:
    if (g_venc_chns[VeChn].venc_attr.attr.stRcAttr.stMjpegCbr.u32BitRate !=
        stVencChnAttr->stRcAttr.stMjpegCbr.u32BitRate)
      u32BitRate = stVencChnAttr->stRcAttr.stMjpegCbr.u32BitRate;
    break;
  case VENC_RC_MODE_MJPEGVBR:
    if (g_venc_chns[VeChn].venc_attr.attr.stRcAttr.stMjpegVbr.u32BitRate !=
        stVencChnAttr->stRcAttr.stMjpegVbr.u32BitRate)
      u32BitRate = stVencChnAttr->stRcAttr.stMjpegVbr.u32BitRate;
    break;
  case VENC_RC_MODE_H265CBR:
    if (g_venc_chns[VeChn].venc_attr.attr.stRcAttr.stH265Cbr.u32BitRate !=
        stVencChnAttr->stRcAttr.stH265Cbr.u32BitRate)
      u32BitRate = stVencChnAttr->stRcAttr.stH265Cbr.u32BitRate;
    break;
  case VENC_RC_MODE_H265VBR:
    if (g_venc_chns[VeChn].venc_attr.attr.stRcAttr.stH265Vbr.u32MaxBitRate !=
        stVencChnAttr->stRcAttr.stH265Vbr.u32MaxBitRate)
      u32MaxBitRate = stVencChnAttr->stRcAttr.stH265Vbr.u32MaxBitRate;
    break;
  case VENC_RC_MODE_H265AVBR:
    if (g_venc_chns[VeChn].venc_attr.attr.stRcAttr.stH265Avbr.u32MaxBitRate !=
        stVencChnAttr->stRcAttr.stH265Avbr.u32MaxBitRate)
      u32MaxBitRate = stVencChnAttr->stRcAttr.stH265Avbr.u32MaxBitRate;
    break;
  case VENC_RC_MODE_BUTT:
    break;
  }
  if (u32BitRate != 0 || u32MinBitRate != 0 || u32MaxBitRate != 0) {
    ret |=
        RK_MPI_VENC_SetBitrate(VeChn, u32BitRate, u32MinBitRate, u32MaxBitRate);
    if (ret)
      return ret;
  } else {
    return -RK_ERR_VENC_ILLEGAL_PARAM;
  }

  switch (g_venc_chns[VeChn].venc_attr.attr.stRcAttr.enRcMode) {
  case VENC_RC_MODE_H264CBR:
    if (g_venc_chns[VeChn]
                .venc_attr.attr.stRcAttr.stH264Cbr.u32SrcFrameRateNum !=
            stVencChnAttr->stRcAttr.stH264Cbr.u32SrcFrameRateNum ||
        g_venc_chns[VeChn]
                .venc_attr.attr.stRcAttr.stH264Cbr.u32SrcFrameRateDen !=
            stVencChnAttr->stRcAttr.stH264Cbr.u32SrcFrameRateDen ||
        g_venc_chns[VeChn]
                .venc_attr.attr.stRcAttr.stH264Cbr.fr32DstFrameRateNum !=
            stVencChnAttr->stRcAttr.stH264Cbr.fr32DstFrameRateNum ||
        g_venc_chns[VeChn]
                .venc_attr.attr.stRcAttr.stH264Cbr.fr32DstFrameRateDen !=
            stVencChnAttr->stRcAttr.stH264Cbr.fr32DstFrameRateDen) {
      ret |= RK_MPI_VENC_SetFps(
          VeChn, stVencChnAttr->stRcAttr.stH264Cbr.u32SrcFrameRateNum,
          stVencChnAttr->stRcAttr.stH264Cbr.u32SrcFrameRateDen,
          stVencChnAttr->stRcAttr.stH264Cbr.fr32DstFrameRateNum,
          stVencChnAttr->stRcAttr.stH264Cbr.fr32DstFrameRateDen);
    }
    break;
  case VENC_RC_MODE_H264VBR:
    if (g_venc_chns[VeChn]
                .venc_attr.attr.stRcAttr.stH264Vbr.u32SrcFrameRateNum !=
            stVencChnAttr->stRcAttr.stH264Vbr.u32SrcFrameRateNum ||
        g_venc_chns[VeChn]
                .venc_attr.attr.stRcAttr.stH264Vbr.u32SrcFrameRateDen !=
            stVencChnAttr->stRcAttr.stH264Vbr.u32SrcFrameRateDen ||
        g_venc_chns[VeChn]
                .venc_attr.attr.stRcAttr.stH264Vbr.fr32DstFrameRateNum !=
            stVencChnAttr->stRcAttr.stH264Vbr.fr32DstFrameRateNum ||
        g_venc_chns[VeChn]
                .venc_attr.attr.stRcAttr.stH264Vbr.fr32DstFrameRateDen !=
            stVencChnAttr->stRcAttr.stH264Vbr.fr32DstFrameRateDen) {
      ret |= RK_MPI_VENC_SetFps(
          VeChn, stVencChnAttr->stRcAttr.stH264Vbr.u32SrcFrameRateNum,
          stVencChnAttr->stRcAttr.stH264Vbr.u32SrcFrameRateDen,
          stVencChnAttr->stRcAttr.stH264Vbr.fr32DstFrameRateNum,
          stVencChnAttr->stRcAttr.stH264Vbr.fr32DstFrameRateDen);
    }
    break;
  case VENC_RC_MODE_H264AVBR:
    if (g_venc_chns[VeChn]
                .venc_attr.attr.stRcAttr.stH264Avbr.u32SrcFrameRateNum !=
            stVencChnAttr->stRcAttr.stH264Avbr.u32SrcFrameRateNum ||
        g_venc_chns[VeChn]
                .venc_attr.attr.stRcAttr.stH264Avbr.u32SrcFrameRateDen !=
            stVencChnAttr->stRcAttr.stH264Avbr.u32SrcFrameRateDen ||
        g_venc_chns[VeChn]
                .venc_attr.attr.stRcAttr.stH264Avbr.fr32DstFrameRateNum !=
            stVencChnAttr->stRcAttr.stH264Avbr.fr32DstFrameRateNum ||
        g_venc_chns[VeChn]
                .venc_attr.attr.stRcAttr.stH264Avbr.fr32DstFrameRateDen !=
            stVencChnAttr->stRcAttr.stH264Avbr.fr32DstFrameRateDen) {
      ret |= RK_MPI_VENC_SetFps(
          VeChn, stVencChnAttr->stRcAttr.stH264Avbr.u32SrcFrameRateNum,
          stVencChnAttr->stRcAttr.stH264Avbr.u32SrcFrameRateDen,
          stVencChnAttr->stRcAttr.stH264Avbr.fr32DstFrameRateNum,
          stVencChnAttr->stRcAttr.stH264Avbr.fr32DstFrameRateDen);
    }
    break;
  case VENC_RC_MODE_MJPEGCBR:
    if (g_venc_chns[VeChn]
                .venc_attr.attr.stRcAttr.stMjpegCbr.u32SrcFrameRateNum !=
            stVencChnAttr->stRcAttr.stMjpegCbr.u32SrcFrameRateNum ||
        g_venc_chns[VeChn]
                .venc_attr.attr.stRcAttr.stMjpegCbr.u32SrcFrameRateDen !=
            stVencChnAttr->stRcAttr.stMjpegCbr.u32SrcFrameRateDen ||
        g_venc_chns[VeChn]
                .venc_attr.attr.stRcAttr.stMjpegCbr.fr32DstFrameRateNum !=
            stVencChnAttr->stRcAttr.stMjpegCbr.fr32DstFrameRateNum ||
        g_venc_chns[VeChn]
                .venc_attr.attr.stRcAttr.stMjpegCbr.fr32DstFrameRateDen !=
            stVencChnAttr->stRcAttr.stMjpegCbr.fr32DstFrameRateDen) {
      ret |= RK_MPI_VENC_SetFps(
          VeChn, stVencChnAttr->stRcAttr.stMjpegCbr.u32SrcFrameRateNum,
          stVencChnAttr->stRcAttr.stMjpegCbr.u32SrcFrameRateDen,
          stVencChnAttr->stRcAttr.stMjpegCbr.fr32DstFrameRateNum,
          stVencChnAttr->stRcAttr.stMjpegCbr.fr32DstFrameRateDen);
    }
    break;
  case VENC_RC_MODE_MJPEGVBR:
    if (g_venc_chns[VeChn]
                .venc_attr.attr.stRcAttr.stMjpegVbr.u32SrcFrameRateNum !=
            stVencChnAttr->stRcAttr.stMjpegVbr.u32SrcFrameRateNum ||
        g_venc_chns[VeChn]
                .venc_attr.attr.stRcAttr.stMjpegVbr.u32SrcFrameRateDen !=
            stVencChnAttr->stRcAttr.stMjpegVbr.u32SrcFrameRateDen ||
        g_venc_chns[VeChn]
                .venc_attr.attr.stRcAttr.stMjpegVbr.fr32DstFrameRateNum !=
            stVencChnAttr->stRcAttr.stMjpegVbr.fr32DstFrameRateNum ||
        g_venc_chns[VeChn]
                .venc_attr.attr.stRcAttr.stMjpegVbr.fr32DstFrameRateDen !=
            stVencChnAttr->stRcAttr.stMjpegVbr.fr32DstFrameRateDen) {
      ret |= RK_MPI_VENC_SetFps(
          VeChn, stVencChnAttr->stRcAttr.stMjpegVbr.u32SrcFrameRateNum,
          stVencChnAttr->stRcAttr.stMjpegVbr.u32SrcFrameRateDen,
          stVencChnAttr->stRcAttr.stMjpegVbr.fr32DstFrameRateNum,
          stVencChnAttr->stRcAttr.stMjpegVbr.fr32DstFrameRateDen);
    }
    break;
  case VENC_RC_MODE_H265CBR:
    if (g_venc_chns[VeChn]
                .venc_attr.attr.stRcAttr.stH265Cbr.u32SrcFrameRateNum !=
            stVencChnAttr->stRcAttr.stH265Cbr.u32SrcFrameRateNum ||
        g_venc_chns[VeChn]
                .venc_attr.attr.stRcAttr.stH265Cbr.u32SrcFrameRateDen !=
            stVencChnAttr->stRcAttr.stH265Cbr.u32SrcFrameRateDen ||
        g_venc_chns[VeChn]
                .venc_attr.attr.stRcAttr.stH265Cbr.fr32DstFrameRateNum !=
            stVencChnAttr->stRcAttr.stH265Cbr.fr32DstFrameRateNum ||
        g_venc_chns[VeChn]
                .venc_attr.attr.stRcAttr.stH265Cbr.fr32DstFrameRateDen !=
            stVencChnAttr->stRcAttr.stH265Cbr.fr32DstFrameRateDen) {
      ret |= RK_MPI_VENC_SetFps(
          VeChn, stVencChnAttr->stRcAttr.stH265Cbr.u32SrcFrameRateNum,
          stVencChnAttr->stRcAttr.stH265Cbr.u32SrcFrameRateDen,
          stVencChnAttr->stRcAttr.stH265Cbr.fr32DstFrameRateNum,
          stVencChnAttr->stRcAttr.stH265Cbr.fr32DstFrameRateDen);
    }
    break;
  case VENC_RC_MODE_H265VBR:
    if (g_venc_chns[VeChn]
                .venc_attr.attr.stRcAttr.stH265Vbr.u32SrcFrameRateNum !=
            stVencChnAttr->stRcAttr.stH265Vbr.u32SrcFrameRateNum ||
        g_venc_chns[VeChn]
                .venc_attr.attr.stRcAttr.stH265Vbr.u32SrcFrameRateDen !=
            stVencChnAttr->stRcAttr.stH265Vbr.u32SrcFrameRateDen ||
        g_venc_chns[VeChn]
                .venc_attr.attr.stRcAttr.stH265Vbr.fr32DstFrameRateNum !=
            stVencChnAttr->stRcAttr.stH265Vbr.fr32DstFrameRateNum ||
        g_venc_chns[VeChn]
                .venc_attr.attr.stRcAttr.stH265Vbr.fr32DstFrameRateDen !=
            stVencChnAttr->stRcAttr.stH265Vbr.fr32DstFrameRateDen) {
      ret |= RK_MPI_VENC_SetFps(
          VeChn, stVencChnAttr->stRcAttr.stH265Vbr.u32SrcFrameRateNum,
          stVencChnAttr->stRcAttr.stH265Vbr.u32SrcFrameRateDen,
          stVencChnAttr->stRcAttr.stH265Vbr.fr32DstFrameRateNum,
          stVencChnAttr->stRcAttr.stH265Vbr.fr32DstFrameRateDen);
    }
    break;
  case VENC_RC_MODE_H265AVBR:
    if (g_venc_chns[VeChn]
                .venc_attr.attr.stRcAttr.stH265Avbr.u32SrcFrameRateNum !=
            stVencChnAttr->stRcAttr.stH265Avbr.u32SrcFrameRateNum ||
        g_venc_chns[VeChn]
                .venc_attr.attr.stRcAttr.stH265Avbr.u32SrcFrameRateDen !=
            stVencChnAttr->stRcAttr.stH265Avbr.u32SrcFrameRateDen ||
        g_venc_chns[VeChn]
                .venc_attr.attr.stRcAttr.stH265Avbr.fr32DstFrameRateNum !=
            stVencChnAttr->stRcAttr.stH265Avbr.fr32DstFrameRateNum ||
        g_venc_chns[VeChn]
                .venc_attr.attr.stRcAttr.stH265Avbr.fr32DstFrameRateDen !=
            stVencChnAttr->stRcAttr.stH265Avbr.fr32DstFrameRateDen) {
      ret |= RK_MPI_VENC_SetFps(
          VeChn, stVencChnAttr->stRcAttr.stH265Avbr.u32SrcFrameRateNum,
          stVencChnAttr->stRcAttr.stH265Avbr.u32SrcFrameRateDen,
          stVencChnAttr->stRcAttr.stH265Avbr.fr32DstFrameRateNum,
          stVencChnAttr->stRcAttr.stH265Avbr.fr32DstFrameRateDen);
    }
    break;
  case VENC_RC_MODE_BUTT:
    break;
  }
  if (ret)
    return ret;

  // set gop
  if (stVencChnAttr->stGopAttr.enGopMode !=
          g_venc_chns[VeChn].venc_attr.attr.stGopAttr.enGopMode ||
      stVencChnAttr->stGopAttr.u32GopSize !=
          g_venc_chns[VeChn].venc_attr.attr.stGopAttr.u32GopSize ||
      stVencChnAttr->stGopAttr.s32IPQpDelta !=
          g_venc_chns[VeChn].venc_attr.attr.stGopAttr.s32IPQpDelta ||
      stVencChnAttr->stGopAttr.u32BgInterval !=
          g_venc_chns[VeChn].venc_attr.attr.stGopAttr.u32BgInterval ||
      stVencChnAttr->stGopAttr.s32ViQpDelta !=
          g_venc_chns[VeChn].venc_attr.attr.stGopAttr.s32ViQpDelta) {
    ret |= RK_MPI_VENC_SetGopMode(VeChn, &stVencChnAttr->stGopAttr);
  }

  return ret;
}

RK_S32 RK_MPI_VENC_CreateJpegLightChn(VENC_CHN VeChn,
                                      VENC_CHN_ATTR_S *stVencChnAttr) {
  if ((VeChn < 0) || (VeChn >= VENC_MAX_CHN_NUM))
    return -RK_ERR_VENC_INVALID_CHNID;

  if (!stVencChnAttr)
    return -RK_ERR_VENC_NULL_PTR;

  if ((stVencChnAttr->stVencAttr.enType != RK_CODEC_TYPE_JPEG) &&
      (stVencChnAttr->stVencAttr.enType != RK_CODEC_TYPE_MJPEG))
    return -RK_ERR_VENC_ILLEGAL_PARAM;

  if ((stVencChnAttr->stVencAttr.enRotation != VENC_ROTATION_0)) {
    LOG("ERROR: Venc[%d]: JpegLT: rotation not support!\n", VeChn);
    return -RK_ERR_VENC_NOT_SUPPORT;
  }

  if (g_venc_chns[VeChn].status != CHN_STATUS_CLOSED) {
    return -RK_ERR_VENC_EXIST;
  }

  LOG("\n%s %s: Enable VENC[%d], Type:%d Start...\n", LOG_TAG, __func__, VeChn,
      stVencChnAttr->stVencAttr.enType);

  std::shared_ptr<easymedia::Flow> video_jpeg_flow;
  RK_U32 u32InFpsNum = 1;
  RK_U32 u32InFpsDen = 1;
  RK_U32 u32OutFpsNum = 1;
  RK_U32 u32OutFpsDen = 1;
  const RK_CHAR *pcRkmediaRcMode = nullptr;
  const RK_CHAR *pcRkmediaCodecType = nullptr;

  RK_S32 mjpeg_bps = 0;
  RK_S32 video_width = stVencChnAttr->stVencAttr.u32PicWidth;
  RK_S32 video_height = stVencChnAttr->stVencAttr.u32PicHeight;
  RK_S32 vir_width = stVencChnAttr->stVencAttr.u32VirWidth;
  RK_S32 vir_height = stVencChnAttr->stVencAttr.u32VirHeight;
  std::string pixel_format =
      ImageTypeToString(stVencChnAttr->stVencAttr.imageType);

  if (stVencChnAttr->stVencAttr.enType == RK_CODEC_TYPE_MJPEG) {
    // MJPEG:
    if (stVencChnAttr->stRcAttr.enRcMode == VENC_RC_MODE_MJPEGCBR) {
      mjpeg_bps = stVencChnAttr->stRcAttr.stMjpegCbr.u32BitRate;
      u32InFpsNum = stVencChnAttr->stRcAttr.stMjpegCbr.u32SrcFrameRateNum;
      u32InFpsDen = stVencChnAttr->stRcAttr.stMjpegCbr.u32SrcFrameRateDen;
      u32OutFpsNum = stVencChnAttr->stRcAttr.stMjpegCbr.fr32DstFrameRateNum;
      u32OutFpsDen = stVencChnAttr->stRcAttr.stMjpegCbr.fr32DstFrameRateDen;
      pcRkmediaRcMode = KEY_CBR;
    } else if (stVencChnAttr->stRcAttr.enRcMode == VENC_RC_MODE_MJPEGVBR) {
      mjpeg_bps = stVencChnAttr->stRcAttr.stMjpegVbr.u32BitRate;
      u32InFpsNum = stVencChnAttr->stRcAttr.stMjpegVbr.u32SrcFrameRateNum;
      u32InFpsDen = stVencChnAttr->stRcAttr.stMjpegVbr.u32SrcFrameRateDen;
      u32OutFpsNum = stVencChnAttr->stRcAttr.stMjpegVbr.fr32DstFrameRateNum;
      u32OutFpsDen = stVencChnAttr->stRcAttr.stMjpegVbr.fr32DstFrameRateDen;
      pcRkmediaRcMode = KEY_VBR;
    } else {
      LOG("ERROR: [%s]: Invalid RcMode[%d]\n", __func__,
          stVencChnAttr->stRcAttr.enRcMode);
      return -RK_ERR_VENC_ILLEGAL_PARAM;
    }

    if ((mjpeg_bps < 2000) || (mjpeg_bps > 100000000)) {
      LOG("ERROR: [%s]: Invalid BitRate[%d], should be [2000, 100000000]\n",
          __func__, mjpeg_bps);
      return -RK_ERR_VENC_ILLEGAL_PARAM;
    }
    if (!u32InFpsNum) {
      LOG("ERROR: [%s]: Invalid src frame rate [%d/%d]\n", __func__,
          u32InFpsNum, u32InFpsDen);
      return -RK_ERR_VENC_ILLEGAL_PARAM;
    }
    if (!u32OutFpsNum) {
      LOG("ERROR: [%s]: Invalid dst frame rate [%d/%d]\n", __func__,
          u32OutFpsNum, u32OutFpsDen);
      return -RK_ERR_VENC_ILLEGAL_PARAM;
    }
    pcRkmediaCodecType = VIDEO_MJPEG;
  } else {
    // JPEG
    pcRkmediaCodecType = IMAGE_JPEG;
  }

  g_venc_mtx.lock();
  if (g_venc_chns[VeChn].status != CHN_STATUS_CLOSED) {
    g_venc_mtx.unlock();
    return -RK_ERR_VENC_EXIST;
  }
  // save venc_attr to venc chn.
  memcpy(&g_venc_chns[VeChn].venc_attr.attr, stVencChnAttr,
         sizeof(VENC_CHN_ATTR_S));

  std::string flow_name = "video_enc";
  std::string flow_param = "";
  std::string enc_param = "";
  PARAM_STRING_APPEND(flow_param, KEY_NAME, "rkmpp");
  PARAM_STRING_APPEND(flow_param, KEY_INPUTDATATYPE, IMAGE_NV12);
  PARAM_STRING_APPEND(flow_param, KEY_OUTPUTDATATYPE, pcRkmediaCodecType);
  PARAM_STRING_APPEND_TO(enc_param, KEY_BUFFER_WIDTH, video_width);
  PARAM_STRING_APPEND_TO(enc_param, KEY_BUFFER_HEIGHT, video_height);
  PARAM_STRING_APPEND_TO(enc_param, KEY_BUFFER_VIR_WIDTH, vir_width);
  PARAM_STRING_APPEND_TO(enc_param, KEY_BUFFER_VIR_HEIGHT, vir_height);

  if (stVencChnAttr->stVencAttr.enType == RK_CODEC_TYPE_MJPEG) {
    // MJPEG
    // set input fps
    std::string str_fps_in;
    str_fps_in.append(std::to_string(u32InFpsNum))
        .append("/")
        .append(std::to_string(u32InFpsDen));
    PARAM_STRING_APPEND(enc_param, KEY_FPS_IN, str_fps_in);
    // set output fps
    std::string str_fps_out;
    str_fps_out.append(std::to_string(u32OutFpsNum))
        .append("/")
        .append(std::to_string(u32OutFpsDen));
    PARAM_STRING_APPEND(enc_param, KEY_FPS, str_fps_out);
    PARAM_STRING_APPEND_TO(enc_param, KEY_COMPRESS_BITRATE_MAX, mjpeg_bps);
    PARAM_STRING_APPEND(enc_param, KEY_COMPRESS_RC_MODE, pcRkmediaRcMode);
  } else {
    // JPEG
    PARAM_STRING_APPEND_TO(enc_param, KEY_JPEG_QFACTOR, 50);
  }

  flow_param = easymedia::JoinFlowParam(flow_param, 1, enc_param);
  LOGD("\n#JPEG-LT: [%s] encoder flow param:\n%s\n", pcRkmediaCodecType,
       flow_param.c_str());
  video_jpeg_flow = easymedia::REFLECTOR(Flow)::Create<easymedia::Flow>(
      flow_name.c_str(), flow_param.c_str());
  if (!video_jpeg_flow) {
    LOG("ERROR: [%s]: Create flow %s failed\n", __func__, flow_name.c_str());
    g_venc_mtx.unlock();
    return -RK_ERR_VENC_ILLEGAL_PARAM;
  }

  video_jpeg_flow->SetFlowTag("JpegLightEncoder");
  // Init buffer list.
  RkmediaChnInitBuffer(&g_venc_chns[VeChn]);
  video_jpeg_flow->SetOutputCallBack(&g_venc_chns[VeChn], FlowOutputCallback);

  g_venc_chns[VeChn].rkmedia_flow = video_jpeg_flow;
  g_venc_chns[VeChn].rkmedia_flow_list.push_back(video_jpeg_flow);
  if (pipe2(g_venc_chns[VeChn].wake_fd, O_CLOEXEC) == -1) {
    g_venc_chns[VeChn].wake_fd[0] = 0;
    g_venc_chns[VeChn].wake_fd[1] = 0;
    LOG("WARN: %s Create pipe failed!\n");
  }
  g_venc_chns[VeChn].status = CHN_STATUS_OPEN;
  g_venc_mtx.unlock();

  return RK_ERR_SYS_OK;
}

RK_S32 RK_MPI_VENC_GetRcParam(VENC_CHN VeChn, VENC_RC_PARAM_S *pstRcParam) {
  if (!pstRcParam)
    return RK_ERR_SYS_ILLEGAL_PARAM;
  if ((VeChn < 0) || (VeChn >= VENC_MAX_CHN_NUM))
    return -RK_ERR_VENC_INVALID_CHNID;
  if (g_venc_chns[VeChn].status < CHN_STATUS_OPEN)
    return -RK_ERR_VENC_NOTREADY;

  g_venc_mtx.lock();
  memcpy(&pstRcParam, &g_venc_chns[VeChn].venc_attr.stRcPara,
         sizeof(VENC_RC_PARAM_S));
  g_venc_mtx.unlock();

  return RK_ERR_SYS_OK;
}

RK_S32 RK_MPI_VENC_SetRcParam(VENC_CHN VeChn,
                              const VENC_RC_PARAM_S *pstRcParam) {
  if ((VeChn < 0) || (VeChn >= VENC_MAX_CHN_NUM))
    return -RK_ERR_VENC_INVALID_CHNID;
  if (g_venc_chns[VeChn].status < CHN_STATUS_OPEN)
    return -RK_ERR_VENC_NOTREADY;

  g_venc_mtx.lock();

  VideoEncoderQp qp;

  qp.qp_init = pstRcParam->s32FirstFrameStartQp;
  switch (g_venc_chns[VeChn].venc_attr.attr.stVencAttr.enType) {
  case RK_CODEC_TYPE_H264:
    qp.qp_step = pstRcParam->stParamH264.u32StepQp;
    qp.qp_max = pstRcParam->stParamH264.u32MaxQp;
    qp.qp_min = pstRcParam->stParamH264.u32MinQp;
    qp.qp_max_i = pstRcParam->stParamH264.u32MaxIQp;
    qp.qp_min_i = pstRcParam->stParamH264.u32MinIQp;
    break;
  case RK_CODEC_TYPE_H265:
    qp.qp_step = pstRcParam->stParamH265.u32StepQp;
    qp.qp_max = pstRcParam->stParamH265.u32MaxQp;
    qp.qp_min = pstRcParam->stParamH265.u32MinQp;
    qp.qp_max_i = pstRcParam->stParamH265.u32MaxIQp;
    qp.qp_min_i = pstRcParam->stParamH265.u32MinIQp;
    break;
  case RK_CODEC_TYPE_JPEG:
    break;
  default:
    break;
  }
  int ret = video_encoder_set_qp(g_venc_chns[VeChn].rkmedia_flow, qp);
  if (!ret) {
    memcpy(&g_venc_chns[VeChn].venc_attr.stRcPara, pstRcParam,
           sizeof(VENC_RC_PARAM_S));
  }
  g_venc_mtx.unlock();
  return ret;
}

RK_S32 RK_MPI_VENC_SetJpegParam(VENC_CHN VeChn,
                                const VENC_JPEG_PARAM_S *pstJpegParam) {
  if ((VeChn < 0) || (VeChn >= VENC_MAX_CHN_NUM))
    return -RK_ERR_VENC_INVALID_CHNID;

  if (!pstJpegParam)
    return -RK_ERR_VENC_NULL_PTR;

  if (pstJpegParam->u32Qfactor > 99 || !pstJpegParam->u32Qfactor) {
    LOG("ERROR:[%s] u32Qfactor(%d) is invalid, should be [1, 99]\n", __func__,
        pstJpegParam->u32Qfactor);
    return -RK_ERR_VENC_ILLEGAL_PARAM;
  }

  if ((g_venc_chns[VeChn].status < CHN_STATUS_OPEN) ||
      g_venc_chns[VeChn].rkmedia_flow_list.empty())
    return -RK_ERR_VENC_NOTREADY;

  if (g_venc_chns[VeChn].venc_attr.attr.stVencAttr.enType != RK_CODEC_TYPE_JPEG)
    return -RK_ERR_VENC_NOT_PERM;

  std::shared_ptr<easymedia::Flow> rkmedia_flow =
      g_venc_chns[VeChn].rkmedia_flow_list.back();
  int ret = easymedia::jpeg_encoder_set_qfactor(rkmedia_flow,
                                                pstJpegParam->u32Qfactor);
  return ret;
}

RK_S32 RK_MPI_VENC_SetRcMode(VENC_CHN VeChn, VENC_RC_MODE_E RcMode) {
  RK_S32 ret = 0;
  const RK_CHAR *key_value = NULL;

  if ((VeChn < 0) || (VeChn >= VENC_MAX_CHN_NUM))
    return -RK_ERR_VENC_INVALID_CHNID;
  if (g_venc_chns[VeChn].status < CHN_STATUS_OPEN)
    return -RK_ERR_VENC_NOTREADY;

  switch (RcMode) {
  case VENC_RC_MODE_H264CBR:
  case VENC_RC_MODE_MJPEGCBR:
  case VENC_RC_MODE_H265CBR:
    key_value = KEY_CBR;
    break;
  case VENC_RC_MODE_H264VBR:
  case VENC_RC_MODE_H265VBR:
  case VENC_RC_MODE_MJPEGVBR:
    key_value = KEY_VBR;
    break;
  case VENC_RC_MODE_H264AVBR:
  case VENC_RC_MODE_H265AVBR:
    key_value = KEY_AVBR;
    break;
  default:
    break;
  }

  if (!key_value)
    return -RK_ERR_VENC_NOT_SUPPORT;

  g_venc_mtx.lock();
  if (g_venc_chns[VeChn].rkmedia_flow) {
    ret = video_encoder_set_rc_mode(g_venc_chns[VeChn].rkmedia_flow, key_value);
    ret = ret ? -RK_ERR_VENC_ILLEGAL_PARAM : RK_ERR_SYS_OK;
  } else {
    ret = -RK_ERR_VENC_NOTREADY;
  }
  g_venc_mtx.unlock();

  return ret;
}

RK_S32 RK_MPI_VENC_SetRcQuality(VENC_CHN VeChn, VENC_RC_QUALITY_E RcQuality) {
  if ((VeChn < 0) || (VeChn >= VENC_MAX_CHN_NUM))
    return -RK_ERR_VENC_INVALID_CHNID;
  if (g_venc_chns[VeChn].status < CHN_STATUS_OPEN)
    return -RK_ERR_VENC_NOTREADY;

  g_venc_mtx.lock();
  switch (RcQuality) {
  case VENC_RC_QUALITY_HIGHEST:
    video_encoder_set_rc_quality(g_venc_chns[VeChn].rkmedia_flow, KEY_HIGHEST);
    break;
  case VENC_RC_QUALITY_HIGHER:
    video_encoder_set_rc_quality(g_venc_chns[VeChn].rkmedia_flow, KEY_HIGHER);
    break;
  case VENC_RC_QUALITY_HIGH:
    video_encoder_set_rc_quality(g_venc_chns[VeChn].rkmedia_flow, KEY_HIGH);
    break;
  case VENC_RC_QUALITY_MEDIUM:
    video_encoder_set_rc_quality(g_venc_chns[VeChn].rkmedia_flow, KEY_MEDIUM);
    break;
  case VENC_RC_QUALITY_LOW:
    video_encoder_set_rc_quality(g_venc_chns[VeChn].rkmedia_flow, KEY_LOW);
    break;
  case VENC_RC_QUALITY_LOWER:
    video_encoder_set_rc_quality(g_venc_chns[VeChn].rkmedia_flow, KEY_LOWER);
    break;
  case VENC_RC_QUALITY_LOWEST:
    video_encoder_set_rc_quality(g_venc_chns[VeChn].rkmedia_flow, KEY_LOWEST);
    break;
  default:
    break;
  }
  g_venc_mtx.unlock();
  return RK_ERR_SYS_OK;
}

RK_S32 RK_MPI_VENC_SetBitrate(VENC_CHN VeChn, RK_U32 u32BitRate,
                              RK_U32 u32MinBitRate, RK_U32 u32MaxBitRate) {
  if ((VeChn < 0) || (VeChn >= VENC_MAX_CHN_NUM))
    return -RK_ERR_VENC_INVALID_CHNID;
  if (g_venc_chns[VeChn].status < CHN_STATUS_OPEN)
    return -RK_ERR_VENC_NOTREADY;

  g_venc_mtx.lock();
  std::shared_ptr<easymedia::Flow> target_flow;
  if (!g_venc_chns[VeChn].rkmedia_flow_list.empty())
    target_flow = g_venc_chns[VeChn].rkmedia_flow_list.back();
  else if (g_venc_chns[VeChn].rkmedia_flow)
    target_flow = g_venc_chns[VeChn].rkmedia_flow;
  int ret = video_encoder_set_bps(target_flow, u32BitRate, u32MinBitRate,
                                  u32MaxBitRate);
  if (!ret) {
    switch (g_venc_chns[VeChn].venc_attr.attr.stRcAttr.enRcMode) {
    case VENC_RC_MODE_H264CBR:
      g_venc_chns[VeChn].venc_attr.attr.stRcAttr.stH264Cbr.u32BitRate =
          u32BitRate;
      break;
    case VENC_RC_MODE_H264VBR:
      g_venc_chns[VeChn].venc_attr.attr.stRcAttr.stH264Vbr.u32MaxBitRate =
          u32MaxBitRate;
      break;
    case VENC_RC_MODE_H264AVBR:
      g_venc_chns[VeChn].venc_attr.attr.stRcAttr.stH264Avbr.u32MaxBitRate =
          u32MaxBitRate;
      break;
    case VENC_RC_MODE_MJPEGCBR:
      g_venc_chns[VeChn].venc_attr.attr.stRcAttr.stMjpegCbr.u32BitRate =
          u32BitRate;
      break;
    case VENC_RC_MODE_MJPEGVBR:
      g_venc_chns[VeChn].venc_attr.attr.stRcAttr.stMjpegVbr.u32BitRate =
          u32BitRate;
      break;
    case VENC_RC_MODE_H265CBR:
      g_venc_chns[VeChn].venc_attr.attr.stRcAttr.stH265Cbr.u32BitRate =
          u32BitRate;
      break;
    case VENC_RC_MODE_H265VBR:
      g_venc_chns[VeChn].venc_attr.attr.stRcAttr.stH265Vbr.u32MaxBitRate =
          u32MaxBitRate;
      break;
    case VENC_RC_MODE_H265AVBR:
      g_venc_chns[VeChn].venc_attr.attr.stRcAttr.stH265Avbr.u32MaxBitRate =
          u32MaxBitRate;
      break;
    case VENC_RC_MODE_BUTT:
      break;
    }
  }
  g_venc_mtx.unlock();
  return RK_ERR_SYS_OK;
}

RK_S32 RK_MPI_VENC_RequestIDR(VENC_CHN VeChn, RK_BOOL bInstant _UNUSED) {
  if ((VeChn < 0) || (VeChn >= VENC_MAX_CHN_NUM))
    return -RK_ERR_VENC_INVALID_CHNID;
  if (g_venc_chns[VeChn].status < CHN_STATUS_OPEN)
    return -RK_ERR_VENC_NOTREADY;

  g_venc_mtx.lock();
  video_encoder_force_idr(g_venc_chns[VeChn].rkmedia_flow);

  g_venc_mtx.unlock();
  return RK_ERR_SYS_OK;
}

RK_S32 RK_MPI_VENC_SetFps(VENC_CHN VeChn, RK_U8 u8OutNum, RK_U8 u8OutDen,
                          RK_U8 u8InNum, RK_U8 u8InDen) {
  if ((VeChn < 0) || (VeChn >= VENC_MAX_CHN_NUM))
    return -RK_ERR_VENC_INVALID_CHNID;
  if (g_venc_chns[VeChn].status < CHN_STATUS_OPEN)
    return -RK_ERR_VENC_NOTREADY;

  g_venc_mtx.lock();
  int ret = video_encoder_set_fps(g_venc_chns[VeChn].rkmedia_flow, u8OutNum,
                                  u8OutDen, u8InNum, u8InDen);
  if (!ret) {
    switch (g_venc_chns[VeChn].venc_attr.attr.stRcAttr.enRcMode) {
    case VENC_RC_MODE_H264CBR:
      g_venc_chns[VeChn].venc_attr.attr.stRcAttr.stH264Cbr.u32SrcFrameRateNum =
          u8InNum;
      g_venc_chns[VeChn].venc_attr.attr.stRcAttr.stH264Cbr.u32SrcFrameRateDen =
          u8InDen;
      g_venc_chns[VeChn].venc_attr.attr.stRcAttr.stH264Cbr.fr32DstFrameRateNum =
          u8OutNum;
      g_venc_chns[VeChn].venc_attr.attr.stRcAttr.stH264Cbr.fr32DstFrameRateDen =
          u8OutDen;
      break;
    case VENC_RC_MODE_H264VBR:
      g_venc_chns[VeChn].venc_attr.attr.stRcAttr.stH264Vbr.u32SrcFrameRateNum =
          u8InNum;
      g_venc_chns[VeChn].venc_attr.attr.stRcAttr.stH264Vbr.u32SrcFrameRateDen =
          u8InDen;
      g_venc_chns[VeChn].venc_attr.attr.stRcAttr.stH264Vbr.fr32DstFrameRateNum =
          u8OutNum;
      g_venc_chns[VeChn].venc_attr.attr.stRcAttr.stH264Vbr.fr32DstFrameRateDen =
          u8OutDen;
      break;
    case VENC_RC_MODE_H264AVBR:
      g_venc_chns[VeChn].venc_attr.attr.stRcAttr.stH264Avbr.u32SrcFrameRateNum =
          u8InNum;
      g_venc_chns[VeChn].venc_attr.attr.stRcAttr.stH264Avbr.u32SrcFrameRateDen =
          u8InDen;
      g_venc_chns[VeChn]
          .venc_attr.attr.stRcAttr.stH264Avbr.fr32DstFrameRateNum = u8OutNum;
      g_venc_chns[VeChn]
          .venc_attr.attr.stRcAttr.stH264Avbr.fr32DstFrameRateDen = u8OutDen;
      break;
    case VENC_RC_MODE_MJPEGCBR:
      g_venc_chns[VeChn].venc_attr.attr.stRcAttr.stMjpegCbr.u32SrcFrameRateNum =
          u8InNum;
      g_venc_chns[VeChn].venc_attr.attr.stRcAttr.stMjpegCbr.u32SrcFrameRateDen =
          u8InDen;
      g_venc_chns[VeChn]
          .venc_attr.attr.stRcAttr.stMjpegCbr.fr32DstFrameRateNum = u8OutNum;
      g_venc_chns[VeChn]
          .venc_attr.attr.stRcAttr.stMjpegCbr.fr32DstFrameRateDen = u8OutDen;
      break;
    case VENC_RC_MODE_MJPEGVBR:
      g_venc_chns[VeChn].venc_attr.attr.stRcAttr.stMjpegVbr.u32SrcFrameRateNum =
          u8InNum;
      g_venc_chns[VeChn].venc_attr.attr.stRcAttr.stMjpegVbr.u32SrcFrameRateDen =
          u8InDen;
      g_venc_chns[VeChn]
          .venc_attr.attr.stRcAttr.stMjpegVbr.fr32DstFrameRateNum = u8OutNum;
      g_venc_chns[VeChn]
          .venc_attr.attr.stRcAttr.stMjpegVbr.fr32DstFrameRateDen = u8OutDen;
      break;
    case VENC_RC_MODE_H265CBR:
      g_venc_chns[VeChn].venc_attr.attr.stRcAttr.stH265Cbr.u32SrcFrameRateNum =
          u8InNum;
      g_venc_chns[VeChn].venc_attr.attr.stRcAttr.stH265Cbr.u32SrcFrameRateDen =
          u8InDen;
      g_venc_chns[VeChn].venc_attr.attr.stRcAttr.stH265Cbr.fr32DstFrameRateNum =
          u8OutNum;
      g_venc_chns[VeChn].venc_attr.attr.stRcAttr.stH265Cbr.fr32DstFrameRateDen =
          u8OutDen;
      break;
    case VENC_RC_MODE_H265VBR:
      g_venc_chns[VeChn].venc_attr.attr.stRcAttr.stH265Vbr.u32SrcFrameRateNum =
          u8InNum;
      g_venc_chns[VeChn].venc_attr.attr.stRcAttr.stH265Vbr.u32SrcFrameRateDen =
          u8InDen;
      g_venc_chns[VeChn].venc_attr.attr.stRcAttr.stH265Vbr.fr32DstFrameRateNum =
          u8OutNum;
      g_venc_chns[VeChn].venc_attr.attr.stRcAttr.stH265Vbr.fr32DstFrameRateDen =
          u8OutDen;
      break;
    case VENC_RC_MODE_H265AVBR:
      g_venc_chns[VeChn].venc_attr.attr.stRcAttr.stH265Avbr.u32SrcFrameRateNum =
          u8InNum;
      g_venc_chns[VeChn].venc_attr.attr.stRcAttr.stH265Avbr.u32SrcFrameRateDen =
          u8InDen;
      g_venc_chns[VeChn]
          .venc_attr.attr.stRcAttr.stH265Avbr.fr32DstFrameRateNum = u8OutNum;
      g_venc_chns[VeChn]
          .venc_attr.attr.stRcAttr.stH265Avbr.fr32DstFrameRateDen = u8OutDen;
      break;
    case VENC_RC_MODE_BUTT:
      break;
    }
  }
  g_venc_mtx.unlock();
  return RK_ERR_SYS_OK;
}
RK_S32 RK_MPI_VENC_SetGop(VENC_CHN VeChn, RK_U32 u32Gop) {
  if ((VeChn < 0) || (VeChn >= VENC_MAX_CHN_NUM))
    return -RK_ERR_VENC_INVALID_CHNID;
  if (g_venc_chns[VeChn].status < CHN_STATUS_OPEN)
    return -RK_ERR_VENC_NOTREADY;

  g_venc_mtx.lock();
  int ret = video_encoder_set_gop_size(g_venc_chns[VeChn].rkmedia_flow, u32Gop);
  if (!ret) {
    g_venc_chns[VeChn].venc_attr.attr.stGopAttr.u32GopSize = u32Gop;
    switch (g_venc_chns[VeChn].venc_attr.attr.stRcAttr.enRcMode) {
    case VENC_RC_MODE_H264CBR:
      g_venc_chns[VeChn].venc_attr.attr.stRcAttr.stH264Cbr.u32Gop = u32Gop;
      break;
    case VENC_RC_MODE_H264VBR:
      g_venc_chns[VeChn].venc_attr.attr.stRcAttr.stH264Vbr.u32Gop = u32Gop;
      break;
    case VENC_RC_MODE_H264AVBR:
      g_venc_chns[VeChn].venc_attr.attr.stRcAttr.stH264Vbr.u32Gop = u32Gop;
      break;
    case VENC_RC_MODE_MJPEGCBR:
      break;
    case VENC_RC_MODE_MJPEGVBR:
      break;
    case VENC_RC_MODE_H265CBR:
      g_venc_chns[VeChn].venc_attr.attr.stRcAttr.stH265Cbr.u32Gop = u32Gop;
      break;
    case VENC_RC_MODE_H265VBR:
      g_venc_chns[VeChn].venc_attr.attr.stRcAttr.stH265Vbr.u32Gop = u32Gop;
      break;
    case VENC_RC_MODE_H265AVBR:
      g_venc_chns[VeChn].venc_attr.attr.stRcAttr.stH265Avbr.u32Gop = u32Gop;
      break;
    case VENC_RC_MODE_BUTT:
      break;
    }
  }
  g_venc_mtx.unlock();
  return RK_ERR_SYS_OK;
}

RK_S32 RK_MPI_VENC_SetAvcProfile(VENC_CHN VeChn, RK_U32 u32Profile,
                                 RK_U32 u32Level) {
  if ((VeChn < 0) || (VeChn >= VENC_MAX_CHN_NUM))
    return -RK_ERR_VENC_INVALID_CHNID;
  if (g_venc_chns[VeChn].status < CHN_STATUS_OPEN)
    return -RK_ERR_VENC_NOTREADY;
  if (g_venc_chns[VeChn].venc_attr.attr.stVencAttr.enType != RK_CODEC_TYPE_H264)
    return -RK_ERR_VENC_NOT_SUPPORT;
  g_venc_mtx.lock();
  int ret = video_encoder_set_avc_profile(g_venc_chns[VeChn].rkmedia_flow,
                                          u32Profile, u32Level);
  if (!ret) {
    g_venc_chns[VeChn].venc_attr.attr.stVencAttr.u32Profile = u32Profile;
    g_venc_chns[VeChn].venc_attr.attr.stVencAttr.stAttrH264e.u32Level =
        u32Level;
  }
  g_venc_mtx.unlock();
  return ret;
}

RK_S32 RK_MPI_VENC_InsertUserData(VENC_CHN VeChn, RK_U8 *pu8Data,
                                  RK_U32 u32Len) {

  if ((VeChn < 0) || (VeChn >= VENC_MAX_CHN_NUM))
    return -RK_ERR_VENC_INVALID_CHNID;
  if (g_venc_chns[VeChn].status < CHN_STATUS_OPEN)
    return -RK_ERR_VENC_NOTREADY;

  g_venc_mtx.lock();
  video_encoder_set_userdata(g_venc_chns[VeChn].rkmedia_flow, pu8Data, u32Len);

  g_venc_mtx.unlock();
  return RK_ERR_SYS_OK;
}

RK_S32 RK_MPI_VENC_GetRoiAttr(VENC_CHN VeChn, VENC_ROI_ATTR_S *pstRoiAttr,
                              RK_S32 roi_index) {
  if (!pstRoiAttr)
    return RK_ERR_SYS_ILLEGAL_PARAM;
  if ((VeChn < 0) || (VeChn >= VENC_MAX_CHN_NUM))
    return -RK_ERR_VENC_INVALID_CHNID;
  if (g_venc_chns[VeChn].status < CHN_STATUS_OPEN)
    return -RK_ERR_VENC_NOTREADY;
  if (roi_index < 0 || roi_index > 8)
    return -RK_ERR_VENC_ILLEGAL_PARAM;

  g_venc_mtx.lock();
  memcpy(pstRoiAttr, &g_venc_chns[VeChn].venc_attr.astRoiAttr[roi_index],
         sizeof(VENC_ROI_ATTR_S));
  g_venc_mtx.unlock();

  return RK_ERR_SYS_OK;
}

RK_S32 RK_MPI_VENC_SetRoiAttr(VENC_CHN VeChn, const VENC_ROI_ATTR_S *pstRoiAttr,
                              RK_S32 region_cnt) {
  int ret = 0;

  if ((VeChn < 0) || (VeChn >= VENC_MAX_CHN_NUM))
    return -RK_ERR_VENC_INVALID_CHNID;
  if (g_venc_chns[VeChn].status < CHN_STATUS_OPEN)
    return -RK_ERR_VENC_NOTREADY;
  if ((pstRoiAttr == nullptr && region_cnt > 0) || region_cnt > 8)
    return -RK_ERR_VENC_ILLEGAL_PARAM;
  if (g_venc_chns[VeChn].venc_attr.attr.stVencAttr.enType !=
          RK_CODEC_TYPE_H264 &&
      g_venc_chns[VeChn].venc_attr.attr.stVencAttr.enType != RK_CODEC_TYPE_H265)
    return -RK_ERR_VENC_NOT_PERM;

  int img_width;
  int img_height;
  int x_offset = 0;
  int y_offset = 0;
  int valid_rgn_cnt = 0;
  EncROIRegion regions[region_cnt];

  if ((g_venc_chns[VeChn].venc_attr.attr.stVencAttr.enRotation ==
       VENC_ROTATION_90) ||
      (g_venc_chns[VeChn].venc_attr.attr.stVencAttr.enRotation ==
       VENC_ROTATION_270)) {
    img_width = g_venc_chns[VeChn].venc_attr.attr.stVencAttr.u32PicHeight;
    img_height = g_venc_chns[VeChn].venc_attr.attr.stVencAttr.u32PicWidth;
  } else {
    img_width = g_venc_chns[VeChn].venc_attr.attr.stVencAttr.u32PicWidth;
    img_height = g_venc_chns[VeChn].venc_attr.attr.stVencAttr.u32PicHeight;
  }

  // cfg regions with args
  memset(regions, 0, sizeof(EncROIRegion) * region_cnt);
  for (int i = 0; i < region_cnt; i++) {
    if (!pstRoiAttr[i].bEnable)
      continue;

    regions[valid_rgn_cnt].x = pstRoiAttr[i].stRect.s32X;
    regions[valid_rgn_cnt].y = pstRoiAttr[i].stRect.s32Y;
    regions[valid_rgn_cnt].w = pstRoiAttr[i].stRect.u32Width;
    regions[valid_rgn_cnt].h = pstRoiAttr[i].stRect.u32Height;
    regions[valid_rgn_cnt].intra = pstRoiAttr[i].bIntra;
    regions[valid_rgn_cnt].abs_qp_en = pstRoiAttr[i].bAbsQp;
    regions[valid_rgn_cnt].qp_area_idx = pstRoiAttr[i].u32Index;
    regions[valid_rgn_cnt].quality = pstRoiAttr[i].s32Qp;
    regions[valid_rgn_cnt].area_map_en = 1;

    x_offset = pstRoiAttr[i].stRect.s32X + pstRoiAttr[i].stRect.u32Width;
    y_offset = pstRoiAttr[i].stRect.s32Y + pstRoiAttr[i].stRect.u32Height;
    if ((x_offset > img_width) || (y_offset > img_height))
      return -RK_ERR_VENC_ILLEGAL_PARAM;

    valid_rgn_cnt++;
  }

  g_venc_mtx.lock();
  ret = video_encoder_set_roi_regions(g_venc_chns[VeChn].rkmedia_flow, regions,
                                      valid_rgn_cnt);
  if (!ret) {
    for (int i = 0; i < region_cnt; i++) {
      memcpy(&g_venc_chns[VeChn].venc_attr.astRoiAttr[i], &pstRoiAttr[i],
             sizeof(VENC_ROI_ATTR_S));
    }
  }
  g_venc_mtx.unlock();

  return ret ? -RK_ERR_VENC_NOTREADY : RK_ERR_SYS_OK;
}

RK_S32 RK_MPI_VENC_SetResolution(VENC_CHN VeChn,
                                 VENC_RESOLUTION_PARAM_S stResolutionParam) {
  if ((VeChn < 0) || (VeChn >= VENC_MAX_CHN_NUM))
    return -RK_ERR_VENC_INVALID_CHNID;
  if (g_venc_chns[VeChn].status < CHN_STATUS_OPEN)
    return -RK_ERR_VENC_NOTREADY;

  g_venc_mtx.lock();
  VideoResolutionCfg vid_cfg;
  vid_cfg.width = stResolutionParam.u32Width;
  vid_cfg.height = stResolutionParam.u32Height;
  vid_cfg.vir_width = stResolutionParam.u32VirWidth;
  vid_cfg.vir_height = stResolutionParam.u32VirHeight;

  int ret =
      video_encoder_set_resolution(g_venc_chns[VeChn].rkmedia_flow, &vid_cfg);
  if (!ret) {
    g_venc_chns[VeChn].venc_attr.attr.stVencAttr.u32VirWidth =
        stResolutionParam.u32VirWidth;
    g_venc_chns[VeChn].venc_attr.attr.stVencAttr.u32VirHeight =
        stResolutionParam.u32VirHeight;
    g_venc_chns[VeChn].venc_attr.attr.stVencAttr.u32PicWidth =
        stResolutionParam.u32Width;
    g_venc_chns[VeChn].venc_attr.attr.stVencAttr.u32PicHeight =
        stResolutionParam.u32Height;
  }
  g_venc_mtx.unlock();
  return RK_ERR_SYS_OK;
}

RK_S32 RK_MPI_VENC_DestroyChn(VENC_CHN VeChn) {
  if ((VeChn < 0) || (VeChn >= VENC_MAX_CHN_NUM))
    return -RK_ERR_VENC_INVALID_CHNID;

  g_venc_mtx.lock();
  if (g_venc_chns[VeChn].status == CHN_STATUS_BIND) {
    g_venc_mtx.unlock();
    return -RK_ERR_VENC_BUSY;
  }
  LOG("\n%s %s: Disable VENC[%d] Start...\n", LOG_TAG, __func__, VeChn);
  if (g_venc_chns[VeChn].rkmedia_flow) {
    if (!g_venc_chns[VeChn].rkmedia_flow_list.empty()) {
      auto ptrRkmediaFlow = g_venc_chns[VeChn].rkmedia_flow_list.front();
      g_venc_chns[VeChn].rkmedia_flow->RemoveDownFlow(ptrRkmediaFlow);
    }
    g_venc_chns[VeChn].rkmedia_flow.reset();
  }

  while (!g_venc_chns[VeChn].rkmedia_flow_list.empty()) {
    auto ptrRkmediaFlow0 = g_venc_chns[VeChn].rkmedia_flow_list.front();
    g_venc_chns[VeChn].rkmedia_flow_list.pop_front();
    if (!g_venc_chns[VeChn].rkmedia_flow_list.empty()) {
      auto ptrRkmediaFlow1 = g_venc_chns[VeChn].rkmedia_flow_list.front();
      ptrRkmediaFlow0->RemoveDownFlow(ptrRkmediaFlow1);
    }
    ptrRkmediaFlow0.reset();
  }
  RkmediaChnClearBuffer(&g_venc_chns[VeChn]);
  g_venc_chns[VeChn].status = CHN_STATUS_CLOSED;
  if (g_venc_chns[VeChn].wake_fd[0] > 0) {
    close(g_venc_chns[VeChn].wake_fd[0]);
    g_venc_chns[VeChn].wake_fd[0] = 0;
  }
  if (g_venc_chns[VeChn].wake_fd[1] > 0) {
    close(g_venc_chns[VeChn].wake_fd[1]);
    g_venc_chns[VeChn].wake_fd[1] = 0;
  }
  g_venc_mtx.unlock();
  LOG("\n%s %s: Disable VENC[%d] End...\n", LOG_TAG, __func__, VeChn);

  return RK_ERR_SYS_OK;
}

RK_S32 RK_MPI_VENC_GetFd(VENC_CHN VeChn) {
  if ((VeChn < 0) || (VeChn >= VENC_MAX_CHN_NUM))
    return -RK_ERR_VENC_INVALID_CHNID;

  int rcv_fd = 0;
  g_venc_mtx.lock();
  if (g_venc_chns[VeChn].status < CHN_STATUS_OPEN) {
    g_venc_mtx.unlock();
    return -RK_ERR_VENC_NOTREADY;
  }
  rcv_fd = g_venc_chns[VeChn].wake_fd[0];
  g_venc_mtx.unlock();

  return rcv_fd;
}

RK_S32 RK_MPI_VENC_QueryStatus(VENC_CHN VeChn, VENC_CHN_STATUS_S *pstStatus) {
  if ((VeChn < 0) || (VeChn > VENC_MAX_CHN_NUM))
    return -RK_ERR_VENC_INVALID_CHNID;

  if (!pstStatus)
    return -RK_ERR_VENC_ILLEGAL_PARAM;

  g_venc_mtx.lock();
  if ((g_venc_chns[VeChn].status < CHN_STATUS_OPEN) ||
      (!g_venc_chns[VeChn].rkmedia_flow)) {
    g_venc_mtx.unlock();
    return -RK_ERR_VENC_NOTREADY;
  }

  RK_U32 u32BufferTotalCnt = 0;
  RK_U32 u32BufferUsedCnt = 0;
  g_venc_chns[VeChn].rkmedia_flow->GetCachedBufferNum(u32BufferTotalCnt,
                                                      u32BufferUsedCnt);
  g_venc_mtx.unlock();
  pstStatus->u32LeftFrames = u32BufferUsedCnt;
  pstStatus->u32TotalFrames = u32BufferTotalCnt;

  g_venc_chns[VeChn].buffer_mtx.lock();
  pstStatus->u32TotalPackets = RKMEDIA_CHNNAL_BUFFER_LIMIT;
  pstStatus->u32LeftPackets = g_venc_chns[VeChn].buffer_list.size();
  g_venc_chns[VeChn].buffer_mtx.unlock();

  return RK_ERR_SYS_OK;
}

RK_S32 RK_MPI_VENC_SetGopMode(VENC_CHN VeChn, VENC_GOP_ATTR_S *pstGopModeAttr) {
  if (!pstGopModeAttr)
    return -RK_ERR_VENC_ILLEGAL_PARAM;

  if ((VeChn < 0) || (VeChn >= VENC_MAX_CHN_NUM))
    return -RK_ERR_VENC_INVALID_CHNID;

  if (g_venc_chns[VeChn].status < CHN_STATUS_OPEN)
    return -RK_ERR_VENC_NOTREADY;

  EncGopModeParam rkmedia_param;
  memset(&rkmedia_param, 0, sizeof(rkmedia_param));
  switch (pstGopModeAttr->enGopMode) {
  case VENC_GOPMODE_NORMALP:
    rkmedia_param.mode = GOP_MODE_NORMALP;
    rkmedia_param.ip_qp_delta = pstGopModeAttr->s32IPQpDelta;
    break;
  case VENC_GOPMODE_TSVC:
    rkmedia_param.mode = GOP_MODE_TSVC3;
    break;
  case VENC_GOPMODE_SMARTP:
    rkmedia_param.mode = GOP_MODE_SMARTP;
    rkmedia_param.ip_qp_delta = pstGopModeAttr->s32IPQpDelta;
    rkmedia_param.interval = pstGopModeAttr->u32BgInterval;
    rkmedia_param.gop_size = pstGopModeAttr->u32GopSize;
    break;
  default:
    LOG("ERROR: %s invalid gop mode(%d)!\n", pstGopModeAttr->enGopMode);
    return -RK_ERR_VENC_ILLEGAL_PARAM;
  }

  g_venc_mtx.lock();
  int ret = easymedia::video_encoder_set_gop_mode(
      g_venc_chns[VeChn].rkmedia_flow, &rkmedia_param);
  if (!ret) {
    memcpy(&g_venc_chns[VeChn].venc_attr.attr.stGopAttr, pstGopModeAttr,
           sizeof(VENC_GOP_ATTR_S));
  }
  g_venc_mtx.unlock();
  return ret;
}

RK_S32 RK_MPI_VENC_RGN_Init(VENC_CHN VeChn, VENC_COLOR_TBL_S *stColorTbl) {
  if ((VeChn < 0) || (VeChn >= VENC_MAX_CHN_NUM))
    return -RK_ERR_VENC_INVALID_CHNID;

  if (g_venc_chns[VeChn].status < CHN_STATUS_OPEN) {
    LOG("ERROR: Venc[%d] should be opened before init osd!\n");
    return -RK_ERR_VENC_NOTREADY;
  }

  int ret = 0;
  const RK_U32 *pu32ArgbColorTbl = NULL;
  RK_U32 u32AVUYColorTbl[VENC_RGN_COLOR_NUM] = {0};
  if (stColorTbl) {
    if (stColorTbl->bColorDichotomyEnable) {
      LOG("\n%s %s: User define color tbl(Dichotomy:True)...\n", LOG_TAG,
          __func__, VeChn);
      std::sort(stColorTbl->u32ArgbTbl,
                stColorTbl->u32ArgbTbl + VENC_RGN_COLOR_NUM);
      g_venc_chns[VeChn].bColorDichotomyEnable = RK_TRUE;
    } else {
      LOG("\n%s %s: User define color tbl(Dichotomy:False)...\n", LOG_TAG,
          __func__, VeChn);
      g_venc_chns[VeChn].bColorDichotomyEnable = RK_FALSE;
    }
    pu32ArgbColorTbl = stColorTbl->u32ArgbTbl;
  } else {
    LOG("\n%s %s: Default color tbl(Dichotomy:True)...\n", LOG_TAG, __func__,
        VeChn);
    g_venc_chns[VeChn].bColorDichotomyEnable = RK_TRUE;
    pu32ArgbColorTbl = u32DftARGB8888ColorTbl;
  }

  color_tbl_argb_to_avuy(pu32ArgbColorTbl, u32AVUYColorTbl);
  g_venc_mtx.lock();
  ret = easymedia::video_encoder_set_osd_plt(g_venc_chns[VeChn].rkmedia_flow,
                                             u32AVUYColorTbl);
  if (ret) {
    g_venc_mtx.unlock();
    return -RK_ERR_VENC_ILLEGAL_PARAM;
  }

  memcpy(g_venc_chns[VeChn].u32ArgbColorTbl, pu32ArgbColorTbl,
         VENC_RGN_COLOR_NUM * 4);
  g_venc_chns[VeChn].bColorTblInit = RK_TRUE;
  g_venc_mtx.unlock();
  return RK_ERR_SYS_OK;
}

static RK_VOID Argb8888_To_Region_Data(VENC_CHN VeChn,
                                       const BITMAP_S *pstBitmap, RK_U8 *data,
                                       RK_U32 canvasWidth,
                                       RK_U32 canvasHeight) {
  RK_U32 TargetWidth, TargetHeight;
  RK_U32 ColorValue;
  RK_U32 *BitmapLineStart;
  RK_U8 *CanvasLineStart;

  TargetWidth =
      (pstBitmap->u32Width > canvasWidth) ? canvasWidth : pstBitmap->u32Width;
  TargetHeight = (pstBitmap->u32Height > canvasHeight) ? canvasHeight
                                                       : pstBitmap->u32Height;

  LOGD("%s Bitmap[%d, %d] -> Canvas[%d, %d], target=<%d, %d>\n", __func__,
       pstBitmap->u32Width, pstBitmap->u32Height, canvasWidth, canvasHeight,
       TargetWidth, TargetHeight);

  // Initialize all pixels to transparent color
  if ((canvasWidth > pstBitmap->u32Width) ||
      (canvasHeight > pstBitmap->u32Height)) {
    RK_U8 TransColorId = find_argb_color_tbl_by_order(
        g_venc_chns[VeChn].u32ArgbColorTbl, PALETTE_TABLE_LEN, 0x00000000);
    memset(data, TransColorId, canvasWidth * canvasHeight);
  }

  for (RK_U32 i = 0; i < TargetHeight; i++) {
    BitmapLineStart = (RK_U32 *)pstBitmap->pData + i * pstBitmap->u32Width;
    CanvasLineStart = data + i * canvasWidth;
    for (RK_U32 j = 0; j < TargetWidth; j++) {
      ColorValue = *(BitmapLineStart + j);

      if (g_venc_chns[VeChn].bColorDichotomyEnable) {
        *(CanvasLineStart + j) = find_argb_color_tbl_by_dichotomy(
            g_venc_chns[VeChn].u32ArgbColorTbl, PALETTE_TABLE_LEN, ColorValue);
      } else {
        *(CanvasLineStart + j) = find_argb_color_tbl_by_order(
            g_venc_chns[VeChn].u32ArgbColorTbl, PALETTE_TABLE_LEN, ColorValue);
      }
    }
  }
}

RK_S32 RK_MPI_VENC_RGN_SetBitMap(VENC_CHN VeChn,
                                 const OSD_REGION_INFO_S *pstRgnInfo,
                                 const BITMAP_S *pstBitmap) {
  RK_U8 *rkmedia_osd_data;
  RK_U32 total_pix_num = 0;
  RK_S32 ret = RK_ERR_SYS_OK;

  if ((VeChn < 0) || (VeChn >= VENC_MAX_CHN_NUM))
    return -RK_ERR_VENC_INVALID_CHNID;

  if ((g_venc_chns[VeChn].status < CHN_STATUS_OPEN) ||
      (g_venc_chns[VeChn].bColorTblInit == RK_FALSE))
    return -RK_ERR_VENC_NOTREADY;

  if (pstRgnInfo && !pstRgnInfo->u8Enable) {
    OsdRegionData rkmedia_osd_rgn;
    memset(&rkmedia_osd_rgn, 0, sizeof(rkmedia_osd_rgn));
    rkmedia_osd_rgn.region_id = pstRgnInfo->enRegionId;
    rkmedia_osd_rgn.enable = pstRgnInfo->u8Enable;
    ret = easymedia::video_encoder_set_osd_region(
        g_venc_chns[VeChn].rkmedia_flow, &rkmedia_osd_rgn);
    if (ret)
      ret = -RK_ERR_VENC_NOT_PERM;
    return ret;
  }

  if (!pstBitmap || !pstBitmap->pData || !pstBitmap->u32Width ||
      !pstBitmap->u32Height)
    return -RK_ERR_VENC_ILLEGAL_PARAM;

  if (!pstRgnInfo || !pstRgnInfo->u32Width || !pstRgnInfo->u32Height)
    return -RK_ERR_VENC_ILLEGAL_PARAM;

  if ((pstRgnInfo->u32PosX % 16) || (pstRgnInfo->u32PosY % 16) ||
      (pstRgnInfo->u32Width % 16) || (pstRgnInfo->u32Height % 16)) {
    LOG("ERROR: <x, y, w, h> = <%d, %d, %d, %d> must be 16 aligned!\n",
        pstRgnInfo->u32PosX, pstRgnInfo->u32PosY, pstRgnInfo->u32Width,
        pstRgnInfo->u32Height);
    return -RK_ERR_VENC_ILLEGAL_PARAM;
  }

  total_pix_num = pstRgnInfo->u32Width * pstRgnInfo->u32Height;
  rkmedia_osd_data = (RK_U8 *)malloc(total_pix_num);
  if (!rkmedia_osd_data) {
    LOG("ERROR: No space left! RgnInfo pixels(%d)\n", total_pix_num);
    return -RK_ERR_VENC_NOMEM;
  }

  switch (pstBitmap->enPixelFormat) {
  case PIXEL_FORMAT_ARGB_8888:
    Argb8888_To_Region_Data(VeChn, pstBitmap, rkmedia_osd_data,
                            pstRgnInfo->u32Width, pstRgnInfo->u32Height);
    break;
  default:
    LOG("ERROR: Not support bitmap pixel format:%d\n",
        pstBitmap->enPixelFormat);
    ret = -RK_ERR_VENC_NOT_SUPPORT;
    break;
  }

  OsdRegionData rkmedia_osd_rgn;
  rkmedia_osd_rgn.buffer = rkmedia_osd_data;
  rkmedia_osd_rgn.region_id = pstRgnInfo->enRegionId;
  rkmedia_osd_rgn.pos_x = pstRgnInfo->u32PosX;
  rkmedia_osd_rgn.pos_y = pstRgnInfo->u32PosY;
  rkmedia_osd_rgn.width = pstRgnInfo->u32Width;
  rkmedia_osd_rgn.height = pstRgnInfo->u32Height;
  rkmedia_osd_rgn.inverse = pstRgnInfo->u8Inverse;
  rkmedia_osd_rgn.enable = pstRgnInfo->u8Enable;
  ret = easymedia::video_encoder_set_osd_region(g_venc_chns[VeChn].rkmedia_flow,
                                                &rkmedia_osd_rgn);
  if (ret)
    ret = -RK_ERR_VENC_NOT_PERM;

  if (rkmedia_osd_data)
    free(rkmedia_osd_data);

  return ret;
}

RK_S32 RK_MPI_VENC_RGN_SetCover(VENC_CHN VeChn,
                                const OSD_REGION_INFO_S *pstRgnInfo,
                                const COVER_INFO_S *pstCoverInfo) {
  RK_U8 *rkmedia_cover_data;
  RK_U32 total_pix_num = 0;
  RK_S32 ret = RK_ERR_SYS_OK;
  RK_U8 color_id = 0xFF;

  if ((VeChn < 0) || (VeChn >= VENC_MAX_CHN_NUM))
    return -RK_ERR_VENC_INVALID_CHNID;

  if ((g_venc_chns[VeChn].status < CHN_STATUS_OPEN) ||
      (g_venc_chns[VeChn].bColorTblInit == RK_FALSE))
    return -RK_ERR_VENC_NOTREADY;

  if (pstRgnInfo && !pstRgnInfo->u8Enable) {
    OsdRegionData rkmedia_osd_rgn;
    memset(&rkmedia_osd_rgn, 0, sizeof(rkmedia_osd_rgn));
    rkmedia_osd_rgn.region_id = pstRgnInfo->enRegionId;
    rkmedia_osd_rgn.enable = pstRgnInfo->u8Enable;
    ret = easymedia::video_encoder_set_osd_region(
        g_venc_chns[VeChn].rkmedia_flow, &rkmedia_osd_rgn);
    if (ret)
      ret = -RK_ERR_VENC_NOT_PERM;
    return ret;
  }

  if (!pstCoverInfo)
    return -RK_ERR_VENC_ILLEGAL_PARAM;

  if (!pstRgnInfo || !pstRgnInfo->u32Width || !pstRgnInfo->u32Height)
    return -RK_ERR_VENC_ILLEGAL_PARAM;

  if ((pstRgnInfo->u32PosX % 16) || (pstRgnInfo->u32PosY % 16) ||
      (pstRgnInfo->u32Width % 16) || (pstRgnInfo->u32Height % 16)) {
    LOG("ERROR: <x, y, w, h> = <%d, %d, %d, %d> must be 16 aligned!\n",
        pstRgnInfo->u32PosX, pstRgnInfo->u32PosY, pstRgnInfo->u32Width,
        pstRgnInfo->u32Height);
    return -RK_ERR_VENC_ILLEGAL_PARAM;
  }

  total_pix_num = pstRgnInfo->u32Width * pstRgnInfo->u32Height;
  rkmedia_cover_data = (RK_U8 *)malloc(total_pix_num);
  if (!rkmedia_cover_data) {
    LOG("ERROR: No space left! RgnInfo pixels(%d)\n", total_pix_num);
    return -RK_ERR_VENC_NOMEM;
  }

  if (pstCoverInfo->enPixelFormat != PIXEL_FORMAT_ARGB_8888) {
    LOG("ERROR: Not support cover pixel format:%d\n",
        pstCoverInfo->enPixelFormat);
    return -RK_ERR_VENC_NOT_SUPPORT;
  }

  // find and fill color
  color_id =
      find_argb_color_tbl_by_order(g_venc_chns[VeChn].u32ArgbColorTbl,
                                   PALETTE_TABLE_LEN, pstCoverInfo->u32Color);
  memset(rkmedia_cover_data, color_id, total_pix_num);

  OsdRegionData rkmedia_osd_rgn;
  rkmedia_osd_rgn.buffer = rkmedia_cover_data;
  rkmedia_osd_rgn.region_id = pstRgnInfo->enRegionId;
  rkmedia_osd_rgn.pos_x = pstRgnInfo->u32PosX;
  rkmedia_osd_rgn.pos_y = pstRgnInfo->u32PosY;
  rkmedia_osd_rgn.width = pstRgnInfo->u32Width;
  rkmedia_osd_rgn.height = pstRgnInfo->u32Height;
  rkmedia_osd_rgn.inverse = pstRgnInfo->u8Inverse;
  rkmedia_osd_rgn.enable = pstRgnInfo->u8Enable;
  ret = easymedia::video_encoder_set_osd_region(g_venc_chns[VeChn].rkmedia_flow,
                                                &rkmedia_osd_rgn);
  if (ret)
    ret = -RK_ERR_VENC_NOT_PERM;

  if (rkmedia_cover_data)
    free(rkmedia_cover_data);

  return ret;
}

RK_S32
RK_MPI_VENC_RGN_SetPaletteId(VENC_CHN VeChn,
                             const OSD_REGION_INFO_S *pstRgnInfo,
                             const OSD_COLOR_PALETTE_BUF_S *pstColPalBuf) {
  RK_S32 ret = RK_ERR_SYS_OK;

  if ((VeChn < 0) || (VeChn >= VENC_MAX_CHN_NUM))
    return -RK_ERR_VENC_INVALID_CHNID;

  if ((g_venc_chns[VeChn].status < CHN_STATUS_OPEN) ||
      (g_venc_chns[VeChn].bColorTblInit == RK_FALSE))
    return -RK_ERR_VENC_NOTREADY;

  if (pstRgnInfo && !pstRgnInfo->u8Enable) {
    OsdRegionData rkmedia_osd_rgn;
    memset(&rkmedia_osd_rgn, 0, sizeof(rkmedia_osd_rgn));
    rkmedia_osd_rgn.region_id = pstRgnInfo->enRegionId;
    rkmedia_osd_rgn.enable = pstRgnInfo->u8Enable;
    ret = easymedia::video_encoder_set_osd_region(
        g_venc_chns[VeChn].rkmedia_flow, &rkmedia_osd_rgn);
    if (ret)
      ret = -RK_ERR_VENC_NOT_PERM;
    return ret;
  }

  if (!pstColPalBuf || !pstColPalBuf->pIdBuf)
    return -RK_ERR_VENC_ILLEGAL_PARAM;

  if (!pstRgnInfo || !pstRgnInfo->u32Width || !pstRgnInfo->u32Height)
    return -RK_ERR_VENC_ILLEGAL_PARAM;

  if ((pstRgnInfo->u32PosX % 16) || (pstRgnInfo->u32PosY % 16) ||
      (pstRgnInfo->u32Width % 16) || (pstRgnInfo->u32Height % 16)) {
    LOG("ERROR: <x, y, w, h> = <%d, %d, %d, %d> must be 16 aligned!\n",
        pstRgnInfo->u32PosX, pstRgnInfo->u32PosY, pstRgnInfo->u32Width,
        pstRgnInfo->u32Height);
    return -RK_ERR_VENC_ILLEGAL_PARAM;
  }

  if ((pstRgnInfo->u32Width != pstColPalBuf->u32Width) ||
      (pstRgnInfo->u32Height != pstColPalBuf->u32Height)) {
    LOG("ERROR: RgnInfo:%dx%d and ColorPaletteBuf:%dx%d not equal!\n",
        pstRgnInfo->u32Width, pstRgnInfo->u32Height, pstColPalBuf->u32Width,
        pstColPalBuf->u32Height);
    return -RK_ERR_VENC_ILLEGAL_PARAM;
  }

  OsdRegionData rkmedia_osd_rgn;
  rkmedia_osd_rgn.buffer = (RK_U8 *)pstColPalBuf->pIdBuf;
  rkmedia_osd_rgn.region_id = pstRgnInfo->enRegionId;
  rkmedia_osd_rgn.pos_x = pstRgnInfo->u32PosX;
  rkmedia_osd_rgn.pos_y = pstRgnInfo->u32PosY;
  rkmedia_osd_rgn.width = pstRgnInfo->u32Width;
  rkmedia_osd_rgn.height = pstRgnInfo->u32Height;
  rkmedia_osd_rgn.inverse = pstRgnInfo->u8Inverse;
  rkmedia_osd_rgn.enable = pstRgnInfo->u8Enable;
  ret = easymedia::video_encoder_set_osd_region(g_venc_chns[VeChn].rkmedia_flow,
                                                &rkmedia_osd_rgn);
  if (ret)
    ret = -RK_ERR_VENC_NOT_PERM;

  return ret;
}

RK_S32 RK_MPI_VENC_StartRecvFrame(VENC_CHN VeChn,
                                  const VENC_RECV_PIC_PARAM_S *pstRecvParam) {
  if ((VeChn < 0) || (VeChn >= VENC_MAX_CHN_NUM))
    return -RK_ERR_VENC_INVALID_CHNID;

  if (!pstRecvParam)
    return -RK_ERR_VENC_ILLEGAL_PARAM;

  if (!g_venc_chns[VeChn].rkmedia_flow)
    return -RK_ERR_VENC_NOTREADY;

  g_venc_chns[VeChn].rkmedia_flow->SetRunTimes(pstRecvParam->s32RecvPicNum);

  return RK_ERR_SYS_OK;
}

/********************************************************************
 * Ai api
 ********************************************************************/
static std::shared_ptr<easymedia::Flow>
create_flow(const std::string &flow_name, const std::string &flow_param,
            const std::string &elem_param) {
  auto &&param = easymedia::JoinFlowParam(flow_param, 1, elem_param);
  auto ret = easymedia::REFLECTOR(Flow)::Create<easymedia::Flow>(
      flow_name.c_str(), param.c_str());
  if (!ret)
    LOG("ERROR: Create flow %s failed\n", flow_name.c_str());
  return ret;
}

static std::shared_ptr<easymedia::Flow>
create_alsa_flow(std::string aud_in_path, SampleInfo &info, bool capture,
                 AI_LAYOUT_E enAiLayout) {
  std::string flow_name;
  std::string flow_param;
  std::string sub_param;
  std::string stream_name;

  if (capture) {
    // default sync mode
    flow_name = "source_stream";
    stream_name = "alsa_capture_stream";
  } else {
    flow_name = "output_stream";
    stream_name = "alsa_playback_stream";
    PARAM_STRING_APPEND(flow_param, KEK_THREAD_SYNC_MODEL, KEY_ASYNCCOMMON);
    PARAM_STRING_APPEND(flow_param, KEK_INPUT_MODEL, KEY_BLOCKING);
    PARAM_STRING_APPEND_TO(flow_param, KEY_INPUT_CACHE_NUM, 10);
  }

  PARAM_STRING_APPEND(flow_param, KEY_NAME, stream_name);
  PARAM_STRING_APPEND(sub_param, KEY_DEVICE, aud_in_path);
  PARAM_STRING_APPEND(sub_param, KEY_SAMPLE_FMT, SampleFmtToString(info.fmt));
  PARAM_STRING_APPEND_TO(sub_param, KEY_CHANNELS, info.channels);
  PARAM_STRING_APPEND_TO(sub_param, KEY_FRAMES, info.nb_samples);
  PARAM_STRING_APPEND_TO(sub_param, KEY_SAMPLE_RATE, info.sample_rate);
  PARAM_STRING_APPEND_TO(sub_param, KEY_LAYOUT, enAiLayout);

  auto audio_source_flow = create_flow(flow_name, flow_param, sub_param);
  return audio_source_flow;
}

RK_S32 RK_MPI_AI_SetChnAttr(AI_CHN AiChn, const AI_CHN_ATTR_S *pstAttr) {
  if ((AiChn < 0) || (AiChn >= AI_MAX_CHN_NUM))
    return -RK_ERR_AI_INVALID_DEVID;

  g_ai_mtx.lock();
  if (!pstAttr || !pstAttr->pcAudioNode)
    return -RK_ERR_SYS_NOT_PERM;

  if (g_ai_chns[AiChn].status != CHN_STATUS_CLOSED) {
    g_ai_mtx.unlock();
    return -RK_ERR_AI_BUSY;
  }

  memcpy(&g_ai_chns[AiChn].ai_attr.attr, pstAttr, sizeof(AI_CHN_ATTR_S));
  g_ai_chns[AiChn].status = CHN_STATUS_READY;

  g_ai_mtx.unlock();
  return RK_ERR_SYS_OK;
}

RK_S32 RK_MPI_AI_EnableChn(AI_CHN AiChn) {
  if ((AiChn < 0) || (AiChn >= AI_MAX_CHN_NUM))
    return RK_ERR_AI_INVALID_DEVID;
  g_ai_mtx.lock();
  if (g_ai_chns[AiChn].status != CHN_STATUS_READY) {
    g_ai_mtx.unlock();
    return (g_ai_chns[AiChn].status > CHN_STATUS_READY) ? -RK_ERR_AI_EXIST
                                                        : -RK_ERR_AI_NOT_CONFIG;
  }
  SampleInfo info;
  info.channels = g_ai_chns[AiChn].ai_attr.attr.u32Channels;
  info.fmt = (SampleFormat)g_ai_chns[AiChn].ai_attr.attr.enSampleFormat;
  info.nb_samples = g_ai_chns[AiChn].ai_attr.attr.u32NbSamples;
  info.sample_rate = g_ai_chns[AiChn].ai_attr.attr.u32SampleRate;
  g_ai_chns[AiChn].rkmedia_flow =
      create_alsa_flow(g_ai_chns[AiChn].ai_attr.attr.pcAudioNode, info, RK_TRUE,
                       g_ai_chns[AiChn].ai_attr.attr.enAiLayout);
  if (!g_ai_chns[AiChn].rkmedia_flow) {
    g_ai_mtx.unlock();
    return -RK_ERR_AI_BUSY;
  }
  RkmediaChnInitBuffer(&g_ai_chns[AiChn]);
  g_ai_chns[AiChn].rkmedia_flow->SetOutputCallBack(&g_ai_chns[AiChn],
                                                   FlowOutputCallback);
  g_ai_chns[AiChn].status = CHN_STATUS_OPEN;

  g_ai_mtx.unlock();
  return RK_ERR_SYS_OK;
}

RK_S32 RK_MPI_AI_DisableChn(AI_CHN AiChn) {
  if ((AiChn < 0) || (AiChn > AI_MAX_CHN_NUM))
    return RK_ERR_AI_INVALID_DEVID;

  g_ai_mtx.lock();
  if (g_ai_chns[AiChn].status == CHN_STATUS_BIND) {
    g_ai_mtx.unlock();
    return -RK_ERR_AI_BUSY;
  }

  g_ai_chns[AiChn].rkmedia_flow.reset();
  RkmediaChnClearBuffer(&g_ai_chns[AiChn]);
  g_ai_chns[AiChn].status = CHN_STATUS_CLOSED;
  g_ai_mtx.unlock();

  return RK_ERR_SYS_OK;
}

RK_S32 RK_MPI_AI_SetVolume(AI_CHN AiChn, RK_S32 s32Volume) {
  if ((AiChn < 0) || (AiChn > AI_MAX_CHN_NUM))
    return RK_ERR_AI_INVALID_DEVID;
  g_ai_mtx.lock();
  if (g_ai_chns[AiChn].status <= CHN_STATUS_READY) {
    g_ai_mtx.unlock();
    return -RK_ERR_AI_NOTOPEN;
  }
  g_ai_chns[AiChn].rkmedia_flow->Control(easymedia::S_ALSA_VOLUME, &s32Volume);
  g_ai_mtx.unlock();
  return RK_ERR_SYS_OK;
}

RK_S32 RK_MPI_AI_GetVolume(AI_CHN AiChn, RK_S32 *ps32Volume) {
  if ((AiChn < 0) || (AiChn > AI_MAX_CHN_NUM))
    return RK_ERR_AI_INVALID_DEVID;
  g_ai_mtx.lock();
  if (g_ai_chns[AiChn].status <= CHN_STATUS_READY) {
    g_ai_mtx.unlock();
    return -RK_ERR_AI_NOTOPEN;
  }
  g_ai_chns[AiChn].rkmedia_flow->Control(easymedia::G_ALSA_VOLUME, ps32Volume);
  g_ai_mtx.unlock();
  return RK_ERR_SYS_OK;
}

RK_S32 RK_MPI_AI_StartStream(AI_CHN AiChn) {
  if ((AiChn < 0) || (AiChn > AI_MAX_CHN_NUM))
    return -RK_ERR_AI_INVALID_DEVID;

  g_ai_mtx.lock();
  if (g_ai_chns[AiChn].status < CHN_STATUS_OPEN) {
    g_ai_mtx.unlock();
    return -RK_ERR_AI_BUSY;
  }

  if (!g_ai_chns[AiChn].rkmedia_flow) {
    g_ai_mtx.unlock();
    return -RK_ERR_AI_NOTOPEN;
  }

  g_ai_chns[AiChn].rkmedia_flow->StartStream();
  g_ai_mtx.unlock();

  return RK_ERR_SYS_OK;
}

RK_S32 RK_MPI_AI_EnableVqe(AI_CHN AiChn) {
  if ((AiChn < 0) || (AiChn > AI_MAX_CHN_NUM))
    return RK_ERR_AI_INVALID_DEVID;
  g_ai_mtx.lock();
  if (g_ai_chns[AiChn].status <= CHN_STATUS_READY) {
    g_ai_mtx.unlock();
    return -RK_ERR_AI_NOTOPEN;
  }
  RK_BOOL bEnable = RK_TRUE;
  g_ai_chns[AiChn].rkmedia_flow->Control(easymedia::S_VQE_ENABLE, &bEnable);
  g_ai_mtx.unlock();
  return RK_ERR_SYS_OK;
}

RK_S32 RK_MPI_AI_DisableVqe(AI_CHN AiChn) {
  if ((AiChn < 0) || (AiChn > AI_MAX_CHN_NUM))
    return RK_ERR_AI_INVALID_DEVID;
  g_ai_mtx.lock();
  if (g_ai_chns[AiChn].status <= CHN_STATUS_READY) {
    g_ai_mtx.unlock();
    return -RK_ERR_AI_NOTOPEN;
  }
  RK_BOOL bEnable = RK_FALSE;
  g_ai_chns[AiChn].rkmedia_flow->Control(easymedia::S_VQE_ENABLE, &bEnable);
  g_ai_mtx.unlock();
  return RK_ERR_SYS_OK;
}

RK_S32 RK_MPI_AI_SetTalkVqeAttr(AI_CHN AiChn,
                                AI_TALKVQE_CONFIG_S *pstVqeConfig) {
  if ((AiChn < 0) || (AiChn > AI_MAX_CHN_NUM))
    return RK_ERR_AI_INVALID_DEVID;
  g_ai_mtx.lock();
  if (g_ai_chns[AiChn].status <= CHN_STATUS_READY) {
    g_ai_mtx.unlock();
    return -RK_ERR_AI_NOTOPEN;
  }
  VQE_CONFIG_S config;
  config.u32VQEMode = VQE_MODE_AI_TALK;
  config.stAiTalkConfig.u32OpenMask = pstVqeConfig->u32OpenMask;
  config.stAiTalkConfig.s32FrameSample = pstVqeConfig->s32FrameSample;
  config.stAiTalkConfig.s32WorkSampleRate = pstVqeConfig->s32WorkSampleRate;
  strncpy(config.stAiTalkConfig.aParamFilePath, pstVqeConfig->aParamFilePath,
          MAX_FILE_PATH_LEN - 1);
  g_ai_chns[AiChn].rkmedia_flow->Control(easymedia::S_VQE_ATTR, &config);
  g_ai_mtx.unlock();
  return RK_ERR_SYS_OK;
}

RK_S32 RK_MPI_AI_GetTalkVqeAttr(AI_CHN AiChn,
                                AI_TALKVQE_CONFIG_S *pstVqeConfig) {
  if ((AiChn < 0) || (AiChn > AI_MAX_CHN_NUM))
    return RK_ERR_AI_INVALID_DEVID;
  g_ai_mtx.lock();
  if (g_ai_chns[AiChn].status <= CHN_STATUS_READY) {
    g_ai_mtx.unlock();
    return -RK_ERR_AI_NOTOPEN;
  }
  VQE_CONFIG_S config;
  g_ai_chns[AiChn].rkmedia_flow->Control(easymedia::G_VQE_ATTR, &config);
  pstVqeConfig->u32OpenMask = config.stAiTalkConfig.u32OpenMask;
  pstVqeConfig->s32FrameSample = config.stAiTalkConfig.s32FrameSample;
  pstVqeConfig->s32WorkSampleRate = config.stAiTalkConfig.s32WorkSampleRate;
  strncpy(pstVqeConfig->aParamFilePath, config.stAiTalkConfig.aParamFilePath,
          MAX_FILE_PATH_LEN - 1);
  g_ai_mtx.unlock();
  return RK_ERR_SYS_OK;
}

RK_S32 RK_MPI_AI_SetRecordVqeAttr(AI_CHN AiChn,
                                  AI_RECORDVQE_CONFIG_S *pstVqeConfig) {
  if ((AiChn < 0) || (AiChn > AI_MAX_CHN_NUM))
    return RK_ERR_AI_INVALID_DEVID;
  g_ai_mtx.lock();
  if (g_ai_chns[AiChn].status <= CHN_STATUS_READY) {
    g_ai_mtx.unlock();
    return -RK_ERR_AI_NOTOPEN;
  }
  VQE_CONFIG_S config;
  config.u32VQEMode = VQE_MODE_AI_RECORD;
  config.stAiRecordConfig.u32OpenMask = pstVqeConfig->u32OpenMask;
  config.stAiRecordConfig.s32FrameSample = pstVqeConfig->s32FrameSample;
  config.stAiRecordConfig.s32WorkSampleRate = pstVqeConfig->s32WorkSampleRate;
  config.stAiRecordConfig.stAnrConfig.fPostAddGain =
      pstVqeConfig->stAnrConfig.fPostAddGain;
  config.stAiRecordConfig.stAnrConfig.fGmin = pstVqeConfig->stAnrConfig.fGmin;
  config.stAiRecordConfig.stAnrConfig.fNoiseFactor =
      pstVqeConfig->stAnrConfig.fNoiseFactor;
  g_ai_chns[AiChn].rkmedia_flow->Control(easymedia::S_VQE_ATTR, &config);
  g_ai_mtx.unlock();
  return RK_ERR_SYS_OK;
}

RK_S32 RK_MPI_AI_GetRecordVqeAttr(AI_CHN AiChn,
                                  AI_RECORDVQE_CONFIG_S *pstVqeConfig) {
  if ((AiChn < 0) || (AiChn > AI_MAX_CHN_NUM))
    return RK_ERR_AI_INVALID_DEVID;
  g_ai_mtx.lock();
  if (g_ai_chns[AiChn].status <= CHN_STATUS_READY) {
    g_ai_mtx.unlock();
    return -RK_ERR_AI_NOTOPEN;
  }
  VQE_CONFIG_S config;
  g_ai_chns[AiChn].rkmedia_flow->Control(easymedia::G_VQE_ATTR, &config);
  pstVqeConfig->u32OpenMask = config.stAiRecordConfig.u32OpenMask;
  pstVqeConfig->s32FrameSample = config.stAiRecordConfig.s32FrameSample;
  pstVqeConfig->s32WorkSampleRate = config.stAiRecordConfig.s32WorkSampleRate;
  pstVqeConfig->stAnrConfig.fPostAddGain =
      config.stAiRecordConfig.stAnrConfig.fPostAddGain;
  pstVqeConfig->stAnrConfig.fGmin = config.stAiRecordConfig.stAnrConfig.fGmin;
  pstVqeConfig->stAnrConfig.fNoiseFactor =
      config.stAiRecordConfig.stAnrConfig.fNoiseFactor;
  g_ai_mtx.unlock();
  return RK_ERR_SYS_OK;
}
/********************************************************************
 * Ao api
 ********************************************************************/
RK_S32 RK_MPI_AO_SetChnAttr(AO_CHN AoChn, const AO_CHN_ATTR_S *pstAttr) {
  if ((AoChn < 0) || (AoChn >= AO_MAX_CHN_NUM))
    return -RK_ERR_AO_INVALID_DEVID;

  g_ao_mtx.lock();
  if (!pstAttr || !pstAttr->pcAudioNode)
    return -RK_ERR_SYS_NOT_PERM;

  if (g_ao_chns[AoChn].status != CHN_STATUS_CLOSED) {
    g_ao_mtx.unlock();
    return -RK_ERR_AI_BUSY;
  }

  memcpy(&g_ao_chns[AoChn].ao_attr.attr, pstAttr, sizeof(AO_CHN_ATTR_S));
  g_ao_chns[AoChn].status = CHN_STATUS_READY;

  g_ao_mtx.unlock();
  return RK_ERR_SYS_OK;
}

RK_S32 RK_MPI_AO_EnableChn(AO_CHN AoChn) {
  if ((AoChn < 0) || (AoChn >= AO_MAX_CHN_NUM))
    return -RK_ERR_AO_INVALID_DEVID;
  g_ao_mtx.lock();
  if (g_ao_chns[AoChn].status != CHN_STATUS_READY) {
    g_ao_mtx.unlock();
    return (g_ao_chns[AoChn].status > CHN_STATUS_READY) ? -RK_ERR_VO_EXIST
                                                        : -RK_ERR_VO_NOT_CONFIG;
  }
  SampleInfo info;
  info.channels = g_ao_chns[AoChn].ao_attr.attr.u32Channels;
  info.fmt = (SampleFormat)g_ao_chns[AoChn].ao_attr.attr.enSampleFormat;
  info.nb_samples = g_ao_chns[AoChn].ao_attr.attr.u32NbSamples;
  info.sample_rate = g_ao_chns[AoChn].ao_attr.attr.u32SampleRate;
  g_ao_chns[AoChn].rkmedia_flow =
      create_alsa_flow(g_ao_chns[AoChn].ao_attr.attr.pcAudioNode, info,
                       RK_FALSE, AI_LAYOUT_NORMAL);
  if (!g_ao_chns[AoChn].rkmedia_flow) {
    g_ao_mtx.unlock();
    return -RK_ERR_AO_BUSY;
  }
  g_ao_chns[AoChn].status = CHN_STATUS_OPEN;
  RkmediaChnInitBuffer(&g_ao_chns[AoChn]);
  g_ao_mtx.unlock();
  return RK_ERR_SYS_OK;
}

RK_S32 RK_MPI_AO_DisableChn(AO_CHN AoChn) {
  if ((AoChn < 0) || (AoChn > AO_MAX_CHN_NUM))
    return -RK_ERR_AO_INVALID_DEVID;

  g_ao_mtx.lock();
  if (g_ao_chns[AoChn].status == CHN_STATUS_BIND) {
    g_ao_mtx.unlock();
    return -RK_ERR_AO_BUSY;
  }
  g_ao_chns[AoChn].rkmedia_flow.reset();
  RkmediaChnClearBuffer(&g_ao_chns[AoChn]);
  g_ao_chns[AoChn].status = CHN_STATUS_CLOSED;
  g_ao_mtx.unlock();

  return RK_ERR_SYS_OK;
}

RK_S32 RK_MPI_AO_QueryChnStat(AO_CHN AoChn, AO_CHN_STATE_S *pstStatus) {
  if ((AoChn < 0) || (AoChn > AO_MAX_CHN_NUM))
    return -RK_ERR_AO_INVALID_DEVID;

  if (!pstStatus)
    return -RK_ERR_AO_ILLEGAL_PARAM;

  g_ao_mtx.lock();
  if ((g_ao_chns[AoChn].status < CHN_STATUS_OPEN) ||
      (!g_ao_chns[AoChn].rkmedia_flow)) {
    g_ao_mtx.unlock();
    return -RK_ERR_AO_BUSY;
  }

  RK_U32 u32BufferTotalCnt = 0;
  RK_U32 u32BufferUsedCnt = 0;
  RK_U32 u32BufferFreeCnt = 0;
  g_ao_chns[AoChn].rkmedia_flow->GetCachedBufferNum(u32BufferTotalCnt,
                                                    u32BufferUsedCnt);
  g_ao_mtx.unlock();

  u32BufferFreeCnt = u32BufferTotalCnt - u32BufferUsedCnt;
  pstStatus->u32ChnTotalNum = u32BufferTotalCnt;
  pstStatus->u32ChnFreeNum = u32BufferFreeCnt;
  pstStatus->u32ChnBusyNum = u32BufferUsedCnt;

  return RK_ERR_SYS_OK;
}

RK_S32 RK_MPI_AO_ClearChnBuf(AO_CHN AoChn) {
  if ((AoChn < 0) || (AoChn > AO_MAX_CHN_NUM))
    return -RK_ERR_AO_INVALID_DEVID;

  g_ao_mtx.lock();
  if ((g_ao_chns[AoChn].status < CHN_STATUS_OPEN) ||
      (!g_ao_chns[AoChn].rkmedia_flow)) {
    g_ao_mtx.unlock();
    return -RK_ERR_AO_BUSY;
  }

  g_ao_chns[AoChn].rkmedia_flow->ClearCachedBuffers();
  g_ao_mtx.unlock();

  return RK_ERR_SYS_OK;
}

RK_S32 RK_MPI_AO_SetVolume(AO_CHN AoChn, RK_S32 s32Volume) {
  if ((AoChn < 0) || (AoChn > AO_MAX_CHN_NUM))
    return RK_ERR_AO_INVALID_DEVID;
  g_ao_mtx.lock();
  if (g_ao_chns[AoChn].status <= CHN_STATUS_READY) {
    g_ao_mtx.unlock();
    return -RK_ERR_AO_NOTOPEN;
  }
  g_ao_chns[AoChn].rkmedia_flow->Control(easymedia::S_ALSA_VOLUME, &s32Volume);
  g_ao_mtx.unlock();
  return RK_ERR_SYS_OK;
}

RK_S32 RK_MPI_AO_GetVolume(AO_CHN AoChn, RK_S32 *ps32Volume) {
  if ((AoChn < 0) || (AoChn > AO_MAX_CHN_NUM))
    return RK_ERR_AO_INVALID_DEVID;
  g_ao_mtx.lock();
  if (g_ao_chns[AoChn].status <= CHN_STATUS_READY) {
    g_ao_mtx.unlock();
    return -RK_ERR_AO_NOTOPEN;
  }
  g_ao_chns[AoChn].rkmedia_flow->Control(easymedia::G_ALSA_VOLUME, ps32Volume);
  g_ao_mtx.unlock();
  return RK_ERR_SYS_OK;
}

RK_S32 RK_MPI_AO_EnableVqe(AO_CHN AoChn) {
  if ((AoChn < 0) || (AoChn > AO_MAX_CHN_NUM))
    return RK_ERR_AO_INVALID_DEVID;
  g_ao_mtx.lock();
  if (g_ao_chns[AoChn].status <= CHN_STATUS_READY) {
    g_ao_mtx.unlock();
    return -RK_ERR_AO_NOTOPEN;
  }
  RK_BOOL bEnable = RK_TRUE;
  g_ao_chns[AoChn].rkmedia_flow->Control(easymedia::S_VQE_ENABLE, &bEnable);
  g_ao_mtx.unlock();
  return RK_ERR_SYS_OK;
}

RK_S32 RK_MPI_AO_DisableVqe(AO_CHN AoChn) {
  if ((AoChn < 0) || (AoChn > AO_MAX_CHN_NUM))
    return RK_ERR_AO_INVALID_DEVID;
  g_ao_mtx.lock();
  if (g_ao_chns[AoChn].status <= CHN_STATUS_READY) {
    g_ao_mtx.unlock();
    return -RK_ERR_AO_NOTOPEN;
  }
  RK_BOOL bEnable = RK_FALSE;
  g_ao_chns[AoChn].rkmedia_flow->Control(easymedia::S_VQE_ENABLE, &bEnable);
  g_ao_mtx.unlock();
  return RK_ERR_SYS_OK;
}

RK_S32 RK_MPI_AO_SetVqeAttr(AO_CHN AoChn, AO_VQE_CONFIG_S *pstVqeConfig) {
  if ((AoChn < 0) || (AoChn > AO_MAX_CHN_NUM))
    return RK_ERR_AO_INVALID_DEVID;
  g_ao_mtx.lock();
  if (g_ao_chns[AoChn].status <= CHN_STATUS_READY) {
    g_ao_mtx.unlock();
    return -RK_ERR_AO_NOTOPEN;
  }
  VQE_CONFIG_S config;
  config.u32VQEMode = VQE_MODE_AO;
  config.stAoConfig.u32OpenMask = pstVqeConfig->u32OpenMask;
  config.stAoConfig.s32FrameSample = pstVqeConfig->s32FrameSample;
  config.stAoConfig.s32WorkSampleRate = pstVqeConfig->s32WorkSampleRate;
  strncpy(config.stAoConfig.aParamFilePath, pstVqeConfig->aParamFilePath,
          MAX_FILE_PATH_LEN - 1);
  g_ao_chns[AoChn].rkmedia_flow->Control(easymedia::S_VQE_ATTR, &config);
  g_ao_mtx.unlock();
  return RK_ERR_SYS_OK;
}

RK_S32 RK_MPI_AO_GetVqeAttr(AO_CHN AoChn, AO_VQE_CONFIG_S *pstVqeConfig) {
  if ((AoChn < 0) || (AoChn > AO_MAX_CHN_NUM))
    return RK_ERR_AO_INVALID_DEVID;
  g_ao_mtx.lock();
  if (g_ao_chns[AoChn].status <= CHN_STATUS_READY) {
    g_ao_mtx.unlock();
    return -RK_ERR_AO_NOTOPEN;
  }

  VQE_CONFIG_S config;
  g_ao_chns[AoChn].rkmedia_flow->Control(easymedia::G_VQE_ATTR, &config);
  pstVqeConfig->u32OpenMask = config.stAoConfig.u32OpenMask;
  pstVqeConfig->s32FrameSample = config.stAoConfig.s32FrameSample;
  pstVqeConfig->s32WorkSampleRate = config.stAoConfig.s32WorkSampleRate;
  strncpy(pstVqeConfig->aParamFilePath, config.stAoConfig.aParamFilePath,
          MAX_FILE_PATH_LEN - 1);
  g_ao_mtx.unlock();
  return RK_ERR_SYS_OK;
}

/********************************************************************
 * Aenc api
 ********************************************************************/
RK_S32 RK_MPI_AENC_CreateChn(AENC_CHN AencChn, const AENC_CHN_ATTR_S *pstAttr) {
  if ((AencChn < 0) || (AencChn >= AENC_MAX_CHN_NUM))
    return -RK_ERR_AENC_INVALID_DEVID;

  if (!pstAttr)
    return -RK_ERR_SYS_NOT_PERM;
  g_aenc_mtx.lock();

  if (g_aenc_chns[AencChn].status != CHN_STATUS_CLOSED) {
    g_aenc_mtx.unlock();
    return -RK_ERR_AI_BUSY;
  }

  memcpy(&g_aenc_chns[AencChn].aenc_attr.attr, pstAttr,
         sizeof(AENC_CHN_ATTR_S));
  g_aenc_chns[AencChn].status = CHN_STATUS_READY;

  std::string flow_name;
  std::string param;
  flow_name = "audio_enc";
  param = "";
  CODEC_TYPE_E codec_type = g_aenc_chns[AencChn].aenc_attr.attr.enCodecType;
  PARAM_STRING_APPEND(param, KEY_NAME, "ffmpeg_aud");
  PARAM_STRING_APPEND(param, KEY_OUTPUTDATATYPE, CodecToString(codec_type));
  RK_S32 nb_sample = 0;
  RK_S32 channels = 0;
  RK_S32 sample_rate = 0;
  Sample_Format_E sample_format;
  switch (codec_type) {
  case RK_CODEC_TYPE_G711A:
    sample_format = RK_SAMPLE_FMT_S16;
    nb_sample = g_aenc_chns[AencChn].aenc_attr.attr.stAencG711A.u32NbSample;
    channels = g_aenc_chns[AencChn].aenc_attr.attr.stAencG711A.u32Channels;
    sample_rate = g_aenc_chns[AencChn].aenc_attr.attr.stAencG711A.u32SampleRate;
    break;
  case RK_CODEC_TYPE_G711U:
    sample_format = RK_SAMPLE_FMT_S16;
    nb_sample = g_aenc_chns[AencChn].aenc_attr.attr.stAencG711U.u32NbSample;
    channels = g_aenc_chns[AencChn].aenc_attr.attr.stAencG711U.u32Channels;
    sample_rate = g_aenc_chns[AencChn].aenc_attr.attr.stAencG711U.u32SampleRate;
    break;
  case RK_CODEC_TYPE_MP2:
    sample_format = RK_SAMPLE_FMT_S16;
    channels = g_aenc_chns[AencChn].aenc_attr.attr.stAencMP2.u32Channels;
    sample_rate = g_aenc_chns[AencChn].aenc_attr.attr.stAencMP2.u32SampleRate;
    break;
  case RK_CODEC_TYPE_AAC:
    sample_format = RK_SAMPLE_FMT_FLTP;
    channels = g_aenc_chns[AencChn].aenc_attr.attr.stAencAAC.u32Channels;
    sample_rate = g_aenc_chns[AencChn].aenc_attr.attr.stAencAAC.u32SampleRate;
    break;
  case RK_CODEC_TYPE_G726:
    sample_format = RK_SAMPLE_FMT_S16;
    channels = g_aenc_chns[AencChn].aenc_attr.attr.stAencG726.u32Channels;
    sample_rate = g_aenc_chns[AencChn].aenc_attr.attr.stAencG726.u32SampleRate;
    break;
  default:
    g_aenc_mtx.unlock();
    return -RK_ERR_AENC_CODEC_NOT_SUPPORT;
  }
  PARAM_STRING_APPEND(param, KEY_INPUTDATATYPE,
                      SampleFormatToString(sample_format));

  MediaConfig enc_config;
  SampleInfo aud_info = {(SampleFormat)sample_format, channels, sample_rate,
                         nb_sample};
  auto &ac = enc_config.aud_cfg;
  ac.sample_info = aud_info;
  ac.bit_rate = g_aenc_chns[AencChn].aenc_attr.attr.u32Bitrate;
  enc_config.type = Type::Audio;

  std::string enc_param;
  enc_param.append(
      easymedia::to_param_string(enc_config, CodecToString(codec_type)));
  param = easymedia::JoinFlowParam(param, 1, enc_param);
  g_aenc_chns[AencChn].rkmedia_flow = easymedia::REFLECTOR(
      Flow)::Create<easymedia::Flow>(flow_name.c_str(), param.c_str());
  if (!g_aenc_chns[AencChn].rkmedia_flow) {
    g_aenc_mtx.unlock();
    return -RK_ERR_AENC_BUSY;
  }
  RkmediaChnInitBuffer(&g_aenc_chns[AencChn]);
  g_aenc_chns[AencChn].rkmedia_flow->SetOutputCallBack(&g_aenc_chns[AencChn],
                                                       FlowOutputCallback);

  g_aenc_chns[AencChn].status = CHN_STATUS_OPEN;
  g_aenc_mtx.unlock();
  return RK_ERR_SYS_OK;
}

RK_S32 RK_MPI_AENC_DestroyChn(AENC_CHN AencChn) {
  if ((AencChn < 0) || (AencChn > AENC_MAX_CHN_NUM))
    return RK_ERR_AENC_INVALID_DEVID;

  g_aenc_mtx.lock();
  if (g_aenc_chns[AencChn].status == CHN_STATUS_BIND) {
    g_aenc_mtx.unlock();
    return -RK_ERR_AENC_BUSY;
  }

  g_aenc_chns[AencChn].rkmedia_flow.reset();
  RkmediaChnClearBuffer(&g_aenc_chns[AencChn]);
  g_aenc_chns[AencChn].status = CHN_STATUS_CLOSED;
  g_aenc_mtx.unlock();

  return RK_ERR_SYS_OK;
}

/********************************************************************
 * Algorithm::Move Detection api
 ********************************************************************/
RK_S32 RK_MPI_ALGO_MD_CreateChn(ALGO_MD_CHN MdChn,
                                const ALGO_MD_ATTR_S *pstMDAttr) {
  if ((MdChn < 0) || (MdChn > ALGO_MD_MAX_CHN_NUM))
    return -RK_ERR_ALGO_MD_INVALID_CHNID;

  if (!pstMDAttr)
    return -RK_ERR_ALGO_MD_ILLEGAL_PARAM;

  if (pstMDAttr->u16Sensitivity > 100) {
    LOG("ERROR: MD: invalid sensitivity(%d), should be <= 100\n",
        pstMDAttr->u16Sensitivity);
    return -RK_ERR_ALGO_MD_ILLEGAL_PARAM;
  }

  g_algo_md_mtx.lock();
  if (g_algo_md_chns[MdChn].status != CHN_STATUS_CLOSED) {
    g_algo_md_mtx.unlock();
    return -RK_ERR_ALGO_MD_EXIST;
  }

  LOG("\n%s %s: Enable MD[%d] Start...\n", LOG_TAG, __func__, MdChn);

  std::string flow_name = "move_detec";
  std::string flow_param = "";
  PARAM_STRING_APPEND(flow_param, KEY_NAME, "move_detec");
  PARAM_STRING_APPEND(flow_param, KEY_INPUTDATATYPE,
                      ImageTypeToString(pstMDAttr->imageType));
  PARAM_STRING_APPEND(flow_param, KEY_OUTPUTDATATYPE, "NULL");
  std::string md_param = "";
  PARAM_STRING_APPEND_TO(md_param, KEY_MD_SINGLE_REF, 1);
  PARAM_STRING_APPEND_TO(md_param, KEY_MD_SENSITIVITY,
                         pstMDAttr->u16Sensitivity);
  PARAM_STRING_APPEND_TO(md_param, KEY_MD_ORI_WIDTH, pstMDAttr->u32Width);
  PARAM_STRING_APPEND_TO(md_param, KEY_MD_ORI_HEIGHT, pstMDAttr->u32Height);
  PARAM_STRING_APPEND_TO(md_param, KEY_MD_DS_WIDTH, pstMDAttr->u32Width);
  PARAM_STRING_APPEND_TO(md_param, KEY_MD_DS_HEIGHT, pstMDAttr->u32Height);
  PARAM_STRING_APPEND_TO(md_param, KEY_MD_ROI_CNT, pstMDAttr->u16RoiCnt);
  std::string strRects;
  for (int i = 0; i < pstMDAttr->u16RoiCnt; i++) {
    strRects.append("(");
    strRects.append(std::to_string(pstMDAttr->stRoiRects[i].s32X));
    strRects.append(",");
    strRects.append(std::to_string(pstMDAttr->stRoiRects[i].s32Y));
    strRects.append(",");
    strRects.append(std::to_string(pstMDAttr->stRoiRects[i].u32Width));
    strRects.append(",");
    strRects.append(std::to_string(pstMDAttr->stRoiRects[i].u32Height));
    strRects.append(")");
  }
  PARAM_STRING_APPEND(md_param, KEY_MD_ROI_RECT, strRects.c_str());
  flow_param = easymedia::JoinFlowParam(flow_param, 1, md_param);
  LOGD("#MoveDetection flow param:\n%s\n", flow_param.c_str());
  g_algo_md_chns[MdChn].rkmedia_flow = easymedia::REFLECTOR(
      Flow)::Create<easymedia::Flow>(flow_name.c_str(), flow_param.c_str());
  if (!g_algo_md_chns[MdChn].rkmedia_flow) {
    g_algo_md_mtx.unlock();
    return -RK_ERR_ALGO_MD_BUSY;
  }
  g_algo_md_chns[MdChn].status = CHN_STATUS_OPEN;

  g_algo_md_mtx.unlock();
  LOG("\n%s %s: Enable MD[%d] END...\n", LOG_TAG, __func__, MdChn);

  return RK_ERR_SYS_OK;
}

RK_S32 RK_MPI_ALGO_MD_DestroyChn(ALGO_MD_CHN MdChn) {
  if ((MdChn < 0) || (MdChn > ALGO_MD_MAX_CHN_NUM))
    return -RK_ERR_ALGO_MD_INVALID_CHNID;

  g_algo_md_mtx.lock();
  if (g_algo_md_chns[MdChn].status == CHN_STATUS_BIND) {
    g_algo_md_mtx.unlock();
    return -RK_ERR_ALGO_MD_BUSY;
  }

  LOG("\n%s %s: Disable MD[%d] Start...\n", LOG_TAG, __func__, MdChn);
  if (g_algo_md_chns[MdChn].rkmedia_flow)
    g_algo_md_chns[MdChn].rkmedia_flow.reset();
  g_algo_md_chns[MdChn].status = CHN_STATUS_CLOSED;
  g_algo_md_mtx.unlock();
  LOG("\n%s %s: Disable MD[%d] End...\n", LOG_TAG, __func__, MdChn);

  return RK_ERR_SYS_OK;
}

RK_S32 RK_MPI_ALGO_MD_EnableSwitch(ALGO_MD_CHN MdChn, RK_BOOL bEnable) {
  if ((MdChn < 0) || (MdChn > ALGO_MD_MAX_CHN_NUM))
    return -RK_ERR_ALGO_MD_INVALID_CHNID;

  g_algo_md_mtx.lock();
  if (g_algo_md_chns[MdChn].status < CHN_STATUS_OPEN) {
    g_algo_md_mtx.unlock();
    return -RK_ERR_ALGO_MD_INVALID_CHNID;
  }
  RK_S32 s32Enable = bEnable ? 1 : 0;
  LOG("\n%s %s: MoveDetection[%d]:set status to %s.\n", LOG_TAG, __func__,
      MdChn, s32Enable ? "Enable" : "Disable");
  if (g_algo_md_chns[MdChn].rkmedia_flow)
    g_algo_md_chns[MdChn].rkmedia_flow->Control(easymedia::S_MD_ROI_ENABLE,
                                                s32Enable);
  g_algo_md_mtx.unlock();
  return RK_ERR_SYS_OK;
}

/********************************************************************
 * Algorithm::Occlusion Detection api
 ********************************************************************/
RK_S32 RK_MPI_ALGO_OD_CreateChn(ALGO_OD_CHN OdChn,
                                const ALGO_OD_ATTR_S *pstChnAttr) {
  if ((OdChn < 0) || (OdChn > ALGO_MD_MAX_CHN_NUM))
    return -RK_ERR_ALGO_OD_INVALID_CHNID;

  if (!pstChnAttr || pstChnAttr->u16RoiCnt > ALGO_OD_ROI_RET_MAX)
    return -RK_ERR_ALGO_OD_ILLEGAL_PARAM;

  switch (pstChnAttr->enImageType) {
  case IMAGE_TYPE_NV12:
  case IMAGE_TYPE_NV21:
  case IMAGE_TYPE_NV16:
  case IMAGE_TYPE_NV61:
  case IMAGE_TYPE_YUV420P:
  case IMAGE_TYPE_YUV422P:
  case IMAGE_TYPE_YV12:
  case IMAGE_TYPE_YV16:
    break;
  default:
    LOG("ERROR: OD: ImageType:%d not support!\n");
    return -RK_ERR_ALGO_OD_ILLEGAL_PARAM;
  }

  if (pstChnAttr->u16Sensitivity > 100) {
    LOG("ERROR: OD: sensitivity(%d) invalid, shlould be <= 100.\n",
        pstChnAttr->u16Sensitivity);
    return -RK_ERR_ALGO_OD_ILLEGAL_PARAM;
  }

  g_algo_od_mtx.lock();
  if (g_algo_od_chns[OdChn].status != CHN_STATUS_CLOSED) {
    g_algo_od_mtx.unlock();
    return -RK_ERR_ALGO_OD_EXIST;
  }

  memcpy(&g_algo_od_chns[OdChn].od_attr, pstChnAttr, sizeof(ALGO_OD_ATTR_S));

  std::string flow_name = "occlusion_detec";
  std::string flow_param = "";
  PARAM_STRING_APPEND(flow_param, KEY_NAME, "occlusion_detec");
  PARAM_STRING_APPEND(flow_param, KEY_INPUTDATATYPE,
                      ImageTypeToString(pstChnAttr->enImageType));
  PARAM_STRING_APPEND(flow_param, KEY_OUTPUTDATATYPE, "NULL");
  std::string od_param = "";
  PARAM_STRING_APPEND_TO(od_param, KEY_OD_SENSITIVITY,
                         pstChnAttr->u16Sensitivity);
  PARAM_STRING_APPEND_TO(od_param, KEY_OD_WIDTH, pstChnAttr->u32Width);
  PARAM_STRING_APPEND_TO(od_param, KEY_OD_HEIGHT, pstChnAttr->u32Height);
  PARAM_STRING_APPEND_TO(od_param, KEY_OD_ROI_CNT, pstChnAttr->u16RoiCnt);
  std::string strRects = "";
  for (int i = 0; i < pstChnAttr->u16RoiCnt; i++) {
    strRects.append("(");
    strRects.append(std::to_string(pstChnAttr->stRoiRects[i].s32X));
    strRects.append(",");
    strRects.append(std::to_string(pstChnAttr->stRoiRects[i].s32Y));
    strRects.append(",");
    strRects.append(std::to_string(pstChnAttr->stRoiRects[i].u32Width));
    strRects.append(",");
    strRects.append(std::to_string(pstChnAttr->stRoiRects[i].u32Height));
    strRects.append(")");
  }
  PARAM_STRING_APPEND(od_param, KEY_OD_ROI_RECT, strRects);
  flow_param = easymedia::JoinFlowParam(flow_param, 1, od_param);

  LOGD("\n#OcclusionDetection flow param:\n%s\n", flow_param.c_str());
  g_algo_od_chns[OdChn].rkmedia_flow = easymedia::REFLECTOR(
      Flow)::Create<easymedia::Flow>(flow_name.c_str(), flow_param.c_str());
  if (!g_algo_od_chns[OdChn].rkmedia_flow) {
    g_algo_od_mtx.unlock();
    return -RK_ERR_ALGO_OD_BUSY;
  }

  g_algo_od_chns[OdChn].status = CHN_STATUS_OPEN;
  g_algo_od_mtx.unlock();

  return RK_ERR_SYS_OK;
}

RK_S32 RK_MPI_ALGO_OD_DestroyChn(ALGO_OD_CHN OdChn) {
  if ((OdChn < 0) || (OdChn > ALGO_OD_MAX_CHN_NUM))
    return -RK_ERR_ALGO_OD_INVALID_CHNID;

  g_algo_od_mtx.lock();
  if (g_algo_od_chns[OdChn].status == CHN_STATUS_BIND) {
    g_algo_od_mtx.unlock();
    return -RK_ERR_ALGO_OD_BUSY;
  }

  g_algo_od_chns[OdChn].rkmedia_flow.reset();
  g_algo_od_chns[OdChn].status = CHN_STATUS_CLOSED;
  g_algo_od_mtx.unlock();

  return RK_ERR_SYS_OK;
}

RK_S32 RK_MPI_ALGO_OD_EnableSwitch(ALGO_OD_CHN OdChn, RK_BOOL bEnable) {
  if ((OdChn < 0) || (OdChn > ALGO_OD_MAX_CHN_NUM))
    return -RK_ERR_ALGO_OD_INVALID_CHNID;

  g_algo_od_mtx.lock();
  if (g_algo_od_chns[OdChn].status < CHN_STATUS_OPEN) {
    g_algo_od_mtx.unlock();
    return -RK_ERR_ALGO_OD_INVALID_CHNID;
  }
  RK_S32 s32Enable = bEnable ? 1 : 0;
  LOG("\n%s %s: OcclusionDetection[%d]:set status to %s.\n", LOG_TAG, __func__,
      OdChn, s32Enable ? "Enable" : "Disable");
  if (g_algo_od_chns[OdChn].rkmedia_flow)
    g_algo_od_chns[OdChn].rkmedia_flow->Control(easymedia::S_OD_ROI_ENABLE,
                                                s32Enable);
  g_algo_od_mtx.unlock();
  return RK_ERR_SYS_OK;
}

/********************************************************************
 * Rga api
 ********************************************************************/
RK_S32 RK_MPI_RGA_CreateChn(RGA_CHN RgaChn, RGA_ATTR_S *pstRgaAttr) {
  if ((RgaChn < 0) || (RgaChn > RGA_MAX_CHN_NUM))
    return -RK_ERR_RGA_INVALID_CHNID;

  if (!pstRgaAttr)
    return -RK_ERR_RGA_ILLEGAL_PARAM;

  LOG("\n%s %s: Enable RGA[%d], Rect<%d,%d,%d,%d> Start...\n", LOG_TAG,
      __func__, RgaChn, pstRgaAttr->stImgIn.u32X, pstRgaAttr->stImgIn.u32Y,
      pstRgaAttr->stImgIn.u32Width, pstRgaAttr->stImgIn.u32Height);

  RK_U32 u32InX = pstRgaAttr->stImgIn.u32X;
  RK_U32 u32InY = pstRgaAttr->stImgIn.u32Y;
  RK_U32 u32InWidth = pstRgaAttr->stImgIn.u32Width;
  RK_U32 u32InHeight = pstRgaAttr->stImgIn.u32Height;
  //  RK_U32 u32InVirWidth = pstRgaAttr->stImgIn.u32HorStride;
  //  RK_U32 u32InVirHeight = pstRgaAttr->stImgIn.u32VirStride;
  std::string InPixelFmt = ImageTypeToString(pstRgaAttr->stImgIn.imgType);

  RK_U32 u32OutX = pstRgaAttr->stImgOut.u32X;
  RK_U32 u32OutY = pstRgaAttr->stImgOut.u32Y;
  RK_U32 u32OutWidth = pstRgaAttr->stImgOut.u32Width;
  RK_U32 u32OutHeight = pstRgaAttr->stImgOut.u32Height;
  RK_U32 u32OutVirWidth = pstRgaAttr->stImgOut.u32HorStride;
  RK_U32 u32OutVirHeight = pstRgaAttr->stImgOut.u32VirStride;
  std::string OutPixelFmt = ImageTypeToString(pstRgaAttr->stImgOut.imgType);
  RK_BOOL bEnableBp = pstRgaAttr->bEnBufPool;
  RK_U16 u16BufPoolCnt = pstRgaAttr->u16BufPoolCnt;
  RK_U16 u16Rotaion = pstRgaAttr->u16Rotaion;
  if ((u16Rotaion != 0) && (u16Rotaion != 90) && (u16Rotaion != 180) &&
      (u16Rotaion != 270)) {
    LOG("ERROR: %s rotation only support: 0/90/180/270!\n", __func__);
    return -RK_ERR_RGA_ILLEGAL_PARAM;
  }

  g_rga_mtx.lock();
  if (g_rga_chns[RgaChn].status != CHN_STATUS_CLOSED) {
    g_rga_mtx.unlock();
    return -RK_ERR_RGA_EXIST;
  }

  std::string flow_name = "filter";
  std::string flow_param = "";
  PARAM_STRING_APPEND(flow_param, KEY_NAME, "rkrga");
  PARAM_STRING_APPEND(flow_param, KEY_INPUTDATATYPE, InPixelFmt);
  // Set output buffer type.
  PARAM_STRING_APPEND(flow_param, KEY_OUTPUTDATATYPE, OutPixelFmt);
  // Set output buffer size.
  PARAM_STRING_APPEND_TO(flow_param, KEY_BUFFER_WIDTH, u32OutWidth);
  PARAM_STRING_APPEND_TO(flow_param, KEY_BUFFER_HEIGHT, u32OutHeight);
  PARAM_STRING_APPEND_TO(flow_param, KEY_BUFFER_VIR_WIDTH, u32OutVirWidth);
  PARAM_STRING_APPEND_TO(flow_param, KEY_BUFFER_VIR_HEIGHT, u32OutVirHeight);
  // enable buffer pool?
  if (bEnableBp) {
    PARAM_STRING_APPEND(flow_param, KEY_MEM_TYPE, KEY_MEM_HARDWARE);
    PARAM_STRING_APPEND_TO(flow_param, KEY_MEM_CNT, u16BufPoolCnt);
  }

  std::string filter_param = "";
  ImageRect src_rect = {(RK_S32)u32InX, (RK_S32)u32InY, (RK_S32)u32InWidth,
                        (RK_S32)u32InHeight};
  ImageRect dst_rect = {(RK_S32)u32OutX, (RK_S32)u32OutY, (RK_S32)u32OutWidth,
                        (RK_S32)u32OutHeight};
  std::vector<ImageRect> rect_vect;
  rect_vect.push_back(src_rect);
  rect_vect.push_back(dst_rect);
  PARAM_STRING_APPEND(filter_param, KEY_BUFFER_RECT,
                      easymedia::TwoImageRectToString(rect_vect).c_str());
  PARAM_STRING_APPEND_TO(filter_param, KEY_BUFFER_ROTATE, u16Rotaion);
  flow_param = easymedia::JoinFlowParam(flow_param, 1, filter_param);
  LOGD("\n#Rkrga Filter flow param:\n%s\n", flow_param.c_str());
  g_rga_chns[RgaChn].rkmedia_flow = easymedia::REFLECTOR(
      Flow)::Create<easymedia::Flow>(flow_name.c_str(), flow_param.c_str());
  if (!g_rga_chns[RgaChn].rkmedia_flow) {
    g_rga_mtx.unlock();
    return -RK_ERR_RGA_BUSY;
  }
  g_rga_chns[RgaChn].rkmedia_flow->SetOutputCallBack(&g_rga_chns[RgaChn],
                                                     FlowOutputCallback);
  g_rga_chns[RgaChn].status = CHN_STATUS_OPEN;
  g_rga_mtx.unlock();
  LOG("\n%s %s: Enable RGA[%d], Rect<%d,%d,%d,%d> End...\n", LOG_TAG, __func__,
      RgaChn, pstRgaAttr->stImgIn.u32X, pstRgaAttr->stImgIn.u32Y,
      pstRgaAttr->stImgIn.u32Width, pstRgaAttr->stImgIn.u32Height);

  return RK_ERR_SYS_OK;
}

RK_S32 RK_MPI_RGA_DestroyChn(RGA_CHN RgaChn) {
  if ((RgaChn < 0) || (RgaChn > RGA_MAX_CHN_NUM))
    return -RK_ERR_RGA_INVALID_CHNID;

  g_rga_mtx.lock();
  if (g_rga_chns[RgaChn].status == CHN_STATUS_BIND) {
    g_rga_mtx.unlock();
    return -RK_ERR_RGA_BUSY;
  }
  LOG("\n%s %s: Disable RGA[%d] Start...\n", LOG_TAG, __func__, RgaChn);
  g_rga_chns[RgaChn].rkmedia_flow.reset();
  g_rga_chns[RgaChn].status = CHN_STATUS_CLOSED;
  g_rga_mtx.unlock();
  LOG("\n%s %s: Disable RGA[%d] End...\n", LOG_TAG, __func__, RgaChn);

  return RK_ERR_SYS_OK;
}

/********************************************************************
 * ADEC api
 ********************************************************************/
RK_S32 RK_MPI_ADEC_CreateChn(ADEC_CHN AdecChn, const ADEC_CHN_ATTR_S *pstAttr) {
  if ((AdecChn < 0) || (AdecChn >= ADEC_MAX_CHN_NUM))
    return -RK_ERR_ADEC_INVALID_DEVID;

  if (!pstAttr)
    return -RK_ERR_SYS_NOT_PERM;
  g_adec_mtx.lock();

  if (g_adec_chns[AdecChn].status != CHN_STATUS_CLOSED) {
    g_adec_mtx.unlock();
    return -RK_ERR_AI_BUSY;
  }

  memcpy(&g_adec_chns[AdecChn].adec_attr.attr, pstAttr,
         sizeof(ADEC_CHN_ATTR_S));
  g_adec_chns[AdecChn].status = CHN_STATUS_READY;

  RK_U32 channels = 0;
  RK_U32 sample_rate = 0;
  CODEC_TYPE_E codec_type = g_adec_chns[AdecChn].adec_attr.attr.enCodecType;
  switch (codec_type) {
  case CODEC_TYPE_AAC:
    break;
  case CODEC_TYPE_MP2:
    break;
  case CODEC_TYPE_G711A:
    channels = g_adec_chns[AdecChn].adec_attr.attr.stAdecG711A.u32Channels;
    sample_rate = g_adec_chns[AdecChn].adec_attr.attr.stAdecG711A.u32SampleRate;
    break;
  case CODEC_TYPE_G711U:
    channels = g_adec_chns[AdecChn].adec_attr.attr.stAdecG711U.u32Channels;
    sample_rate = g_adec_chns[AdecChn].adec_attr.attr.stAdecG711U.u32SampleRate;
    break;
  case CODEC_TYPE_G726:
    break;
  default:
    g_adec_mtx.unlock();
    return -RK_ERR_ADEC_CODEC_NOT_SUPPORT;
  }

  std::string flow_name;
  std::string flow_param;
  std::string dec_param;

  flow_name = "audio_dec";
  flow_param = "";
  PARAM_STRING_APPEND(flow_param, KEY_NAME, "ffmpeg_aud");

  dec_param = "";
  PARAM_STRING_APPEND(dec_param, KEY_INPUTDATATYPE, CodecToString(codec_type));
  PARAM_STRING_APPEND_TO(dec_param, KEY_CHANNELS, channels);
  PARAM_STRING_APPEND_TO(dec_param, KEY_SAMPLE_RATE, sample_rate);

  flow_param = easymedia::JoinFlowParam(flow_param, 1, dec_param);
  g_adec_chns[AdecChn].rkmedia_flow = easymedia::REFLECTOR(
      Flow)::Create<easymedia::Flow>(flow_name.c_str(), flow_param.c_str());
  if (!g_adec_chns[AdecChn].rkmedia_flow) {
    g_adec_mtx.unlock();
    return -RK_ERR_ADEC_BUSY;
  }
  RkmediaChnInitBuffer(&g_adec_chns[AdecChn]);
  g_adec_chns[AdecChn].rkmedia_flow->SetOutputCallBack(&g_adec_chns[AdecChn],
                                                       FlowOutputCallback);
  g_adec_chns[AdecChn].status = CHN_STATUS_OPEN;

  g_adec_mtx.unlock();
  return RK_ERR_SYS_OK;
}

RK_S32 RK_MPI_ADEC_DestroyChn(ADEC_CHN AdecChn) {
  if ((AdecChn < 0) || (AdecChn > ADEC_MAX_CHN_NUM))
    return RK_ERR_ADEC_INVALID_DEVID;

  g_adec_mtx.lock();
  if (g_adec_chns[AdecChn].status == CHN_STATUS_BIND) {
    g_adec_mtx.unlock();
    return -RK_ERR_ADEC_BUSY;
  }

  g_adec_chns[AdecChn].rkmedia_flow.reset();
  RkmediaChnClearBuffer(&g_adec_chns[AdecChn]);
  g_adec_chns[AdecChn].status = CHN_STATUS_CLOSED;
  g_adec_mtx.unlock();

  return RK_ERR_SYS_OK;
}

/********************************************************************
 * VO api
 ********************************************************************/
RK_S32 RK_MPI_VO_CreateChn(VO_CHN VoChn, const VO_CHN_ATTR_S *pstAttr) {
  int ret = RK_ERR_SYS_OK;
  const RK_CHAR *pcPlaneType = NULL;

  if ((VoChn < 0) || (VoChn >= VO_MAX_CHN_NUM))
    return -RK_ERR_VO_INVALID_DEVID;

  if (!pstAttr)
    return -RK_ERR_VO_ILLEGAL_PARAM;

  switch (pstAttr->emPlaneType) {
  case VO_PLANE_PRIMARY:
    pcPlaneType = "Primary";
    break;
  case VO_PLANE_OVERLAY:
    pcPlaneType = "Overlay";
    break;
  case VO_PLANE_CURSOR:
    pcPlaneType = "Cursor";
    break;
  default:
    break;
  }

  if (!pcPlaneType)
    return -RK_ERR_VO_ILLEGAL_PARAM;

  g_vo_mtx.lock();
  if (g_vo_chns[VoChn].status != CHN_STATUS_CLOSED) {
    g_vo_mtx.unlock();
    return -RK_ERR_VO_EXIST;
  }

  LOG("\n%s %s: Enable VO[%d] Start...\n", LOG_TAG, __func__, VoChn);

  std::string flow_name = "output_stream";
  std::string flow_param = "";
  PARAM_STRING_APPEND(flow_param, KEY_NAME, "drm_output_stream");

  std::string stream_param = "";
  if (!pstAttr->pcDevNode) {
    LOG("WARN: VO: use default DevNode:/dev/dri/card0\n");
    PARAM_STRING_APPEND(stream_param, KEY_DEVICE, "/dev/dri/card0");
  } else {
    PARAM_STRING_APPEND(stream_param, KEY_DEVICE, pstAttr->pcDevNode);
  }
  if (pstAttr->u16ConIdx)
    PARAM_STRING_APPEND_TO(stream_param, "connector_id", pstAttr->u16ConIdx);
  if (pstAttr->u16EncIdx)
    PARAM_STRING_APPEND_TO(stream_param, "encoder_id", pstAttr->u16EncIdx);
  if (pstAttr->u16CrtcIdx)
    PARAM_STRING_APPEND_TO(stream_param, "crtc_id", pstAttr->u16CrtcIdx);
  PARAM_STRING_APPEND_TO(stream_param, "framerate", pstAttr->u16Fps);
  PARAM_STRING_APPEND(stream_param, "plane_type", pcPlaneType);
  PARAM_STRING_APPEND_TO(stream_param, "ZPOS", pstAttr->u16Zpos);
  PARAM_STRING_APPEND(stream_param, KEY_OUTPUTDATATYPE,
                      ImageTypeToString(pstAttr->enImgType));
  flow_param = easymedia::JoinFlowParam(flow_param, 1, stream_param);
  LOGD("\n#DrmDisplay flow params:\n%s\n", flow_param.c_str());
  g_vo_chns[VoChn].rkmedia_flow = easymedia::REFLECTOR(
      Flow)::Create<easymedia::Flow>(flow_name.c_str(), flow_param.c_str());
  if (!g_vo_chns[VoChn].rkmedia_flow) {
    g_vo_mtx.unlock();
    return -RK_ERR_VO_BUSY;
  }

  if (pstAttr->stImgRect.s32X || pstAttr->stImgRect.s32Y ||
      pstAttr->stImgRect.u32Width || pstAttr->stImgRect.u32Height) {
    ImageRect ImgRect = {pstAttr->stImgRect.s32X, pstAttr->stImgRect.s32Y,
                         (int)pstAttr->stImgRect.u32Width,
                         (int)pstAttr->stImgRect.u32Height};
    if (g_vo_chns[VoChn].rkmedia_flow->Control(S_SOURCE_RECT, &ImgRect)) {
      g_vo_chns[VoChn].rkmedia_flow.reset();
      g_vo_mtx.unlock();
      return -RK_ERR_VO_ILLEGAL_PARAM;
    }
  }

  if (pstAttr->stDispRect.s32X || pstAttr->stDispRect.s32Y ||
      pstAttr->stDispRect.u32Width || pstAttr->stDispRect.u32Height) {
    ImageRect PlaneRect = {pstAttr->stDispRect.s32X, pstAttr->stDispRect.s32Y,
                           (int)pstAttr->stDispRect.u32Width,
                           (int)pstAttr->stDispRect.u32Height};
    if (g_vo_chns[VoChn].rkmedia_flow->Control(S_DESTINATION_RECT,
                                               &PlaneRect)) {
      g_vo_chns[VoChn].rkmedia_flow.reset();
      g_vo_mtx.unlock();
      return -RK_ERR_VO_ILLEGAL_PARAM;
    }
  }

  g_vo_chns[VoChn].status = CHN_STATUS_OPEN;
  g_vo_mtx.unlock();
  LOG("\n%s %s: Enable VO[%d] End!\n", LOG_TAG, __func__, VoChn);

  return ret;
}

RK_S32 RK_MPI_VO_DestroyChn(VO_CHN VoChn) {
  if ((VoChn < 0) || (VoChn >= VO_MAX_CHN_NUM))
    return -RK_ERR_VO_INVALID_DEVID;

  g_vo_mtx.lock();
  if (g_vo_chns[VoChn].status == CHN_STATUS_BIND) {
    g_vo_mtx.unlock();
    return -RK_ERR_ADEC_BUSY;
  }

  g_vo_chns[VoChn].rkmedia_flow.reset();
  g_vo_chns[VoChn].status = CHN_STATUS_CLOSED;
  g_vo_mtx.unlock();

  return RK_ERR_SYS_OK;
}
