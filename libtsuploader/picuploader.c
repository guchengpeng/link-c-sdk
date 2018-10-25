#include "picuploader.h"
#include "resource.h"
#include "queue.h"
#include <qiniu/io.h>
#include <qiniu/rs.h>
#include <curl/curl.h>
#include "tsuploaderapi.h"
#include "fixjson.h"

#define LINK_PIC_UPLOAD_MAX_FILENAME 256
enum LinkPicUploadSignalType {
        LinkPicUploadSignalIgnore = 0,
        LinkPicUploadSignalStop,
        LinkPicUploadGetPicSignalCallback,
        LinkPicUploadSignalUpload
};

enum LinkGetPictureSyncMode {
        LinkGetPictureModeSync = 1,
        LinkGetPictureModeAsync = 2
};

typedef struct {
        LinkAsyncInterface asyncWait_;
        pthread_t workerId_;
        LinkCircleQueue *pSignalQueue_;
        int nQuit_;
        LinkPicUploadArg picUpSettings_;
}PicUploader;

typedef struct {
        LinkAsyncInterface asyncWait_;
        enum LinkPicUploadSignalType signalType_;
        enum LinkPicUploadType upType_;
        char *pData;
        int nDataLen;
        int64_t nTimestamp; //file name need
        pthread_t uploadPicThread;
        char deviceId[64];
        PicUploader *pPicUploader;
        enum LinkGetPictureSyncMode syncMode_;
}LinkPicUploadSignal;


static void * uploadPicture(void *_pOpaque);

static int pushUploadSignal(PicUploader *pPicUploader, LinkPicUploadSignal *pSig) {
        pSig->pPicUploader = pPicUploader;
        pSig->signalType_ = LinkPicUploadSignalUpload;
        return pPicUploader->pSignalQueue_->Push(pPicUploader->pSignalQueue_, (char *)pSig, sizeof(LinkPicUploadSignal));
}

int LinkSendSyncGetPictureSingalToPictureUploader(PictureUploader *pPicUploader, const char *pDeviceId, int nDeviceIdLen, int64_t nTimestamp) {
        LinkPicUploadSignal sig;
        memset(&sig, 0, sizeof(LinkPicUploadSignal));
        sig.signalType_ = LinkPicUploadGetPicSignalCallback;
        sig.nTimestamp = nTimestamp;
        sig.syncMode_ = LinkGetPictureModeSync;
        if( nDeviceIdLen > sizeof(sig.deviceId)) {
                LinkLogWarn("deviceid too long:%d(%s)", nDeviceIdLen, pDeviceId);
                nDeviceIdLen = sizeof(sig.deviceId)-1;
        }
        memcpy(sig.deviceId, pDeviceId, nDeviceIdLen);
        PicUploader *pPicUp = (PicUploader*)pPicUploader;
        return pPicUp->pSignalQueue_->Push(pPicUp->pSignalQueue_, (char *)&sig, sizeof(LinkPicUploadSignal));
}

int LinkSendAsyncGetPictureSingalToPictureUploader(PictureUploader *pPicUploader, const char *pDeviceId, int nDeviceIdLen, int64_t nTimestamp) {
        LinkPicUploadSignal sig;
        memset(&sig, 0, sizeof(LinkPicUploadSignal));
        sig.signalType_ = LinkPicUploadGetPicSignalCallback;
        sig.nTimestamp = nTimestamp;
        sig.syncMode_ = LinkGetPictureModeAsync;
        if( nDeviceIdLen > sizeof(sig.deviceId)) {
                LinkLogWarn("deviceid too long:%d(%s)", nDeviceIdLen, pDeviceId);
                nDeviceIdLen = sizeof(sig.deviceId)-1;
        }
        memcpy(sig.deviceId, pDeviceId, nDeviceIdLen);
        PicUploader *pPicUp = (PicUploader*)pPicUploader;
        return pPicUp->pSignalQueue_->Push(pPicUp->pSignalQueue_, (char *)&sig, sizeof(LinkPicUploadSignal));
}

int LinkSendUploadPictureToPictureUploader(PictureUploader *pPicUploader, void *pOpaque, const char *pBuf, int nBuflen, enum LinkPicUploadType type) {
        PicUploader *pPicUp = (PicUploader *)pPicUploader;
        LinkPicUploadSignal* pSig = (LinkPicUploadSignal*)pOpaque;
        pSig->pPicUploader = (char *)pPicUp;
        pSig->signalType_ = LinkPicUploadSignalUpload;
        pSig->pData = pBuf;
        pSig->nDataLen = nBuflen;
        return pPicUp->pSignalQueue_->Push(pPicUp->pSignalQueue_, (char *)pSig, sizeof(LinkPicUploadSignal));
}

static void * listenPicUpload(void *_pOpaque)
{
        PicUploader *pPicUploader = (PicUploader *)_pOpaque;

        while(!pPicUploader->nQuit_) {
                fprintf(stderr, "==========\n");
                LinkPicUploadSignal sig;
                int ret = pPicUploader->pSignalQueue_->PopWithTimeout(pPicUploader->pSignalQueue_, (char *)(&sig),
                                                                       sizeof(LinkPicUploadSignal), 24 * 60 * 60);
                
                LinkUploaderStatInfo info;
                pPicUploader->pSignalQueue_->GetStatInfo(pPicUploader->pSignalQueue_, &info);
                LinkLogDebug("signal queue:%d", info.nLen_);
                if (ret == LINK_TIMEOUT) {
                        continue;
                }
                LinkPicUploadSignal *pUpInfo;
                if (ret == sizeof(LinkPicUploadSignal)) {

                        switch (sig.signalType_) {
                                case LinkPicUploadSignalIgnore:
                                        LinkLogWarn("ignore signal");
                                        continue;
                                case LinkPicUploadSignalStop:
                                        return NULL;
                                case LinkPicUploadSignalUpload:
                                        pUpInfo = (LinkPicUploadSignal*)malloc(sizeof(LinkPicUploadSignal));
                                        if (pUpInfo == NULL) {
                                                LinkLogWarn("upload picture:%lld no memory", sig.nTimestamp);
                                        } else {
                                                memcpy(pUpInfo, &sig, sizeof(sig));
                                                ret = pthread_create(&pUpInfo->uploadPicThread, NULL, uploadPicture, pUpInfo);
                                        }
                                
                                        break;
                                case LinkPicUploadGetPicSignalCallback:
                                        if (pPicUploader->picUpSettings_.getPicCallback) {
                                                void *pSigBackup = NULL;
                                                if (sig.syncMode_ == LinkGetPictureModeAsync) {
                                                        pSigBackup = malloc(sizeof(sig));
                                                        memcpy(pSigBackup, &sig, sizeof(sig));
                                                }
                                                int ret = pPicUploader->picUpSettings_.getPicCallback(
                                                                                            pPicUploader->picUpSettings_.pGetPicCallbackOpaque,
                                                                                            pSigBackup, &sig.pData, &sig.nDataLen, &sig.upType_);
                                                if (sig.syncMode_ == LinkGetPictureModeSync && ret == 0)
                                                        pushUploadSignal(pPicUploader, &sig);
                                        }
                                        break;
                        }

                }
        }
        return NULL;
}

static int waitUploadMgrThread(void * _pOpaque) {
        PicUploader *p = (PicUploader*)_pOpaque;
        p->nQuit_ = 1;
        pthread_join(p->workerId_, NULL);
        LinkDestroyQueue(&p->pSignalQueue_);
        free(p);
        return 0;
}

int LinkNewPictureUploader(PictureUploader **_pPicUploader, LinkPicUploadArg *pArg) {
        PicUploader * pPicUploader = (PicUploader *) malloc(sizeof(PicUploader));
        if (pPicUploader == NULL) {
                return LINK_NO_MEMORY;
        }
        memset(pPicUploader, 0, sizeof(PicUploader));
        
        int ret = LinkNewCircleQueue(&pPicUploader->pSignalQueue_, 0, TSQ_FIX_LENGTH, sizeof(LinkPicUploadSignal) + sizeof(int), 50);
        if (ret != 0) {
                free(pPicUploader);
                return ret;
        }
        pPicUploader->asyncWait_.function = waitUploadMgrThread;
        pPicUploader->picUpSettings_ = *pArg;
        
        ret = pthread_create(&pPicUploader->workerId_, NULL, listenPicUpload, pPicUploader);
        if (ret != 0) {
                LinkDestroyQueue(&pPicUploader->pSignalQueue_);
                free(pPicUploader);
                return LINK_THREAD_ERROR;
        }
        *_pPicUploader = (PictureUploader *)pPicUploader;
        
        return LINK_SUCCESS;
}

static int waitUploadThread(void * _pOpaque) {
        LinkPicUploadSignal *pSig = (LinkPicUploadSignal*)_pOpaque;
        pthread_join(pSig->uploadPicThread, NULL);
        free(pSig);
        return 0;
}

static void * uploadPicture(void *_pOpaque) {
        LinkPicUploadSignal *pSig = (LinkPicUploadSignal*)_pOpaque;
        pSig->asyncWait_.function = waitUploadThread;
        
        // frame/ua/ts_start_timestamp/fragment_start_timestamp.jpeg
        char key[160] = {0};
        memset(key, 0, sizeof(key));
        snprintf(key, sizeof(key), "frame/%s/%lld/0.jpg", pSig->deviceId, pSig->nTimestamp);
        
        char uptoken[1024] = {0};
        int ret = pSig->pPicUploader->picUpSettings_.getTokenCallback(pSig->pPicUploader->picUpSettings_.pGetPicCallbackOpaque,
                                                                      uptoken, sizeof(uptoken));
        if (ret == LINK_BUFFER_IS_SMALL) {
                LinkLogError("token buffer %d is too small. drop file:%s", sizeof(uptoken), key);
                LinkPushFunction(pSig);
                return NULL;
        }
        
        Qiniu_Client client;
        Qiniu_Client_InitNoAuth(&client, 1024);
        
        Qiniu_Io_PutRet putRet;
        Qiniu_Io_PutExtra putExtra;
        Qiniu_Zero(putExtra);

        //设置机房域名
        LinkUploadZone upZone = LinkGetuploadZone();
#ifdef DISABLE_OPENSSL
        switch(upZone) {
                case LINK_ZONE_HUABEI:
                        Qiniu_Use_Zone_Huabei(Qiniu_False);
                        break;
                case LINK_ZONE_HUANAN:
                        Qiniu_Use_Zone_Huanan(Qiniu_False);
                        break;
                case LINK_ZONE_BEIMEI:
                        Qiniu_Use_Zone_Beimei(Qiniu_False);
                        break;
                case LINK_ZONE_DONGNANYA:
                        Qiniu_Use_Zone_Dongnanya(Qiniu_False);
                        break;
                default:
                        Qiniu_Use_Zone_Huadong(Qiniu_False);
                        break;
        }
#else
        switch(upZone) {
                case LINK_ZONE_HUABEI:
                        Qiniu_Use_Zone_Huabei(Qiniu_True);
                        break;
                case LINK_ZONE_HUANAN:
                        Qiniu_Use_Zone_Huanan(Qiniu_True);
                        break;
                case LINK_ZONE_BEIMEI:
                        Qiniu_Use_Zone_Beimei(Qiniu_True);
                        break;
                case LINK_ZONE_DONGNANYA:
                        Qiniu_Use_Zone_Dongnanya(Qiniu_True);
                        break;
                default:
                        Qiniu_Use_Zone_Huadong(Qiniu_True);
                        break;
        }
#endif

        Qiniu_Error error;
        if (pSig->upType_ == LinkPicUploadTypeFile) {
                error = Qiniu_Io_PutFile(&client, &putRet, uptoken, key, (const char*)pSig->pData, &putExtra);
        } else {
                error = Qiniu_Io_PutBuffer(&client, &putRet, uptoken, key, (const char*)pSig->pData,
                                               pSig->nDataLen, &putExtra);
        }
        
#ifdef __ARM
        report_status( error.code, key );// add by liyq to record ts upload status
#endif
        if (error.code != 200) {
                if (error.code == 401) {
                        LinkLogError("upload picture :%s httpcode=%d errmsg=%s", key, error.code, Qiniu_Buffer_CStr(&client.b));
                } else if (error.code >= 500) {
                        const char * pFullErrMsg = Qiniu_Buffer_CStr(&client.b);
                        char errMsg[256];
                        char *pMsg = GetErrorMsg(pFullErrMsg, errMsg, sizeof(errMsg));
                        if (pMsg) {
                                LinkLogError("upload picture :%s httpcode=%d errmsg={\"error\":\"%s\"}", key, error.code, pMsg);
                        }else {
                                LinkLogError("upload picture :%s httpcode=%d errmsg=%s", key, error.code,
                                             pFullErrMsg);
                        }
                } else {
                        const char *pCurlErrMsg = curl_easy_strerror(error.code);
                        if (pCurlErrMsg != NULL) {
                                LinkLogError("upload picture :%s errorcode=%d errmsg={\"error\":\"%s\"}", key, error.code, pCurlErrMsg);
                        } else {
                                LinkLogError("upload picture :%s errorcode=%d errmsg={\"error\":\"unknown error\"}", key, error.code);
                        }
                }
        } else {
                LinkLogDebug("upload picture key:%s success", key);
        }
        
        if (pSig->pPicUploader->picUpSettings_.getPictureFreeCallback) {
                pSig->pPicUploader->picUpSettings_.getPictureFreeCallback(pSig->pData, pSig->nDataLen);
        }
        
        Qiniu_Client_Cleanup(&client);
        LinkPushFunction(pSig);
        
        return NULL;
}

void LinkDestroyPictureUploader(PictureUploader **pPicUploader) {
        LinkPushFunction(*pPicUploader);
        *pPicUploader = NULL;
        return;
}