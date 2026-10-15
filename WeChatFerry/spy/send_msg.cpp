
#include "framework.h"
#include <sstream>
#include <vector>
#include <new>

#include "exec_sql.h"
#include "log.h"
#include "send_msg.h"
#include "spy_types.h"
#include "util.h"

extern HANDLE g_hEvent;
extern QWORD g_WeChatWinDllAddr;
extern string GetSelfWxid(); // Defined in spy.cpp

#define SRTM_SIZE 0x3F0

#define OS_NEW             0x1B5E140
#define OS_FREE            0x1B55850
#define OS_SEND_MSG_MGR    0x1B53FD0
#define OS_SEND_TEXT       0x22C6B60
#define OS_SEND_IMAGE      0x22BC2F0
#define OS_GET_APP_MSG_MGR 0x1B557D0
#define OS_SEND_FILE       0x20D0230
#define OS_RTM_NEW         0x1B5D690
#define OS_RTM_FREE        0x1B5CA60
#define OS_SEND_RICH_TEXT  0x20DA210
#define OS_SEND_PAT_MSG    0x2CAEC00
#define OS_FORWARD_MSG     0x22C60E0
#define OS_GET_EMOTION_MGR 0x1BCEF10
#define OS_SEND_EMOTION    0x21B52D5
#define OS_XML_BUFSIGN     0x24F0D70
#define OS_SEND_XML        0x20CF360

typedef QWORD (*New_t)(QWORD);
typedef QWORD (*Free_t)(QWORD);
typedef QWORD (*SendMsgMgr_t)();
typedef QWORD (*GetAppMsgMgr_t)();
typedef QWORD (*SendTextMsg_t)(QWORD, QWORD, QWORD, QWORD, QWORD, QWORD, QWORD, QWORD);
typedef QWORD (*SendImageMsg_t)(QWORD, QWORD, QWORD, QWORD, QWORD);
typedef QWORD (*SendFileMsg_t)(QWORD, QWORD, QWORD, QWORD, QWORD, QWORD *, QWORD, QWORD *, QWORD, QWORD *, QWORD,
                               QWORD);
typedef QWORD (*SendRichTextMsg_t)(QWORD, QWORD, QWORD);
typedef QWORD (*SendPatMsg_t)(QWORD, QWORD);
typedef QWORD (*ForwardMsg_t)(QWORD, QWORD, QWORD, QWORD);
typedef QWORD (*GetEmotionMgr_t)();
typedef QWORD (*SendEmotion_t)(QWORD, QWORD, QWORD, QWORD, QWORD, QWORD, QWORD, QWORD);

typedef QWORD (*XmlBufSign_t)(QWORD, QWORD, QWORD);
typedef QWORD (*SendXmlMsg_t)(QWORD, QWORD, QWORD, QWORD, QWORD, QWORD, QWORD, QWORD, QWORD, QWORD);

void SendTextMessage(string wxid, string msg, string atWxids)
{
    QWORD success  = 0;
    wstring wsWxid = String2Wstring(wxid);
    wstring wsMsg  = String2Wstring(msg);
    WxString wxMsg(wsMsg);
    WxString wxWxid(wsWxid);

    vector<wstring> vAtWxids;
    vector<WxString> vWxAtWxids;
    if (!atWxids.empty()) {
        wstringstream wss(String2Wstring(atWxids));
        while (wss.good()) {
            wstring wstr;
            getline(wss, wstr, L',');
            vAtWxids.push_back(wstr);
            WxString wxAtWxid(vAtWxids.back());
            vWxAtWxids.push_back(wxAtWxid);
        }
    } else {
        WxString wxEmpty = WxString();
        vWxAtWxids.push_back(wxEmpty);
    }

    QWORD wxAters = (QWORD) & ((RawVector_t *)&vWxAtWxids)->start;

    char buffer[0x460]            = { 0 };
    SendMsgMgr_t funcSendMsgMgr   = (SendMsgMgr_t)(g_WeChatWinDllAddr + OS_SEND_MSG_MGR);
    SendTextMsg_t funcSendTextMsg = (SendTextMsg_t)(g_WeChatWinDllAddr + OS_SEND_TEXT);
    Free_t funcFree               = (Free_t)(g_WeChatWinDllAddr + OS_FREE);
    funcSendMsgMgr();
    success = funcSendTextMsg((QWORD)(&buffer), (QWORD)(&wxWxid), (QWORD)(&wxMsg), wxAters, 1, 1, 0, 0);
    funcFree((QWORD)(&buffer));
}

void SendImageMessage(string wxid, string path)
{
    wstring wsWxid = String2Wstring(wxid);
    wstring wsPath = String2Wstring(path);

    WxString wxWxid(wsWxid);
    WxString wxPath(wsPath);

    New_t funcNew                = (New_t)(g_WeChatWinDllAddr + OS_NEW);
    Free_t funcFree              = (Free_t)(g_WeChatWinDllAddr + OS_FREE);
    SendMsgMgr_t funcSendMsgMgr  = (SendMsgMgr_t)(g_WeChatWinDllAddr + OS_SEND_MSG_MGR);
    SendImageMsg_t funcSendImage = (SendImageMsg_t)(g_WeChatWinDllAddr + OS_SEND_IMAGE);

    char msg[0x460]    = { 0 };
    char msgTmp[0x460] = { 0 };
    QWORD *flag[10]    = { 0 };

    QWORD tmp1 = 0, tmp2 = 0;
    QWORD pMsgTmp = funcNew((QWORD)(&msgTmp));
    flag[8]       = &tmp1;
    flag[9]       = &tmp2;
    flag[1]       = (QWORD *)(pMsgTmp);

    QWORD pMsg    = funcNew((QWORD)(&msg));
    QWORD sendMgr = funcSendMsgMgr();
    funcSendImage(sendMgr, pMsg, (QWORD)(&wxWxid), (QWORD)(&wxPath), (QWORD)(&flag));
    funcFree(pMsg);
    funcFree(pMsgTmp);
}

namespace {
// 文件发送会消费并清空 WxString；缓冲区必须独立分配，不能借用 std::wstring::c_str()。
class OwnedFileString {
public:
    WxString value;

    OwnedFileString() = default;
    OwnedFileString(const OwnedFileString &) = delete;
    OwnedFileString &operator=(const OwnedFileString &) = delete;

    bool assign(const wstring &text)
    {
        if (text.size() > MAXDWORD || text.size() > (SIZE_MAX / sizeof(wchar_t)) - 1) return false;
        auto buffer = static_cast<wchar_t *>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (text.size() + 1) * sizeof(wchar_t)));
        if (!buffer) return false;
        wmemcpy(buffer, text.c_str(), text.size() + 1);
        value.wptr     = buffer;
        value.size     = static_cast<DWORD>(text.size());
        value.capacity = static_cast<DWORD>(text.size());
        return true;
    }

    ~OwnedFileString()
    {
        // 微信已消费时 value.wptr 为 nullptr；提前失败时只回收尚未移交的缓冲区。
        if (value.wptr) HeapFree(GetProcessHeap(), 0, const_cast<wchar_t *>(value.wptr));
    }
};
} // namespace

int SendFileMessage(string wxid, string path)
{
    try {
        OwnedFileString wxWxid;
        OwnedFileString wxPath;
        if (!wxWxid.assign(String2Wstring(wxid)) || !wxPath.assign(String2Wstring(path))) return -3;

        New_t funcNew                 = (New_t)(g_WeChatWinDllAddr + OS_NEW);
        Free_t funcFree               = (Free_t)(g_WeChatWinDllAddr + OS_FREE);
        GetAppMsgMgr_t funcGetAppMgr  = (GetAppMsgMgr_t)(g_WeChatWinDllAddr + OS_GET_APP_MSG_MGR);
        SendFileMsg_t funcSendFile    = (SendFileMsg_t)(g_WeChatWinDllAddr + OS_SEND_FILE);

        char msg[0x460] = { 0 };
        QWORD tmp1[4]   = { 0 };
        QWORD tmp2[4]   = { 0 };
        QWORD tmp3[4]   = { 0 };

        QWORD pMsg   = funcNew((QWORD)(&msg));
        QWORD appMgr = funcGetAppMgr();
        // 保留 3.9.11.25 的偏移和调用参数；只修正传入字符串的归属。
        funcSendFile(appMgr, pMsg, (QWORD)(&wxWxid.value), (QWORD)(&wxPath.value), 1, tmp1, 0, tmp2, 0, tmp3, 0, 0);
        funcFree(pMsg);
        return 0;
    } catch (const std::bad_alloc &) {
        return -3;
    }
}

int SendRichTextMessage(RichText_t &rt)
{ // TODO: Fix memory leak
    QWORD status = -1;

    New_t funcNew                          = (New_t)(g_WeChatWinDllAddr + OS_RTM_NEW);
    Free_t funcFree                        = (Free_t)(g_WeChatWinDllAddr + OS_RTM_FREE);
    GetAppMsgMgr_t funcGetAppMsgMgr        = (GetAppMsgMgr_t)(g_WeChatWinDllAddr + OS_GET_APP_MSG_MGR);
    SendRichTextMsg_t funcForwordPublicMsg = (SendRichTextMsg_t)(g_WeChatWinDllAddr + OS_SEND_RICH_TEXT);

    char *buff = (char *)HeapAlloc(GetProcessHeap(), 0, SRTM_SIZE);
    if (buff == NULL) {
        LOG_ERROR("Out of Memory...");
        return -1;
    }

    memset(buff, 0, SRTM_SIZE);
    funcNew((QWORD)buff);
    WxString *pReceiver = NewWxStringFromStr(rt.receiver);
    WxString *pTitle    = NewWxStringFromStr(rt.title);
    WxString *pUrl      = NewWxStringFromStr(rt.url);
    WxString *pThumburl = NewWxStringFromStr(rt.thumburl);
    WxString *pDigest   = NewWxStringFromStr(rt.digest);
    WxString *pAccount  = NewWxStringFromStr(rt.account);
    WxString *pName     = NewWxStringFromStr(rt.name);

    memcpy(buff + 0x8, pTitle, sizeof(WxString));
    memcpy(buff + 0x48, pUrl, sizeof(WxString));
    memcpy(buff + 0xB0, pThumburl, sizeof(WxString));
    memcpy(buff + 0xF0, pDigest, sizeof(WxString));
    memcpy(buff + 0x2C0, pAccount, sizeof(WxString));
    memcpy(buff + 0x2E0, pName, sizeof(WxString));

    QWORD mgr = funcGetAppMsgMgr();
    status    = funcForwordPublicMsg(mgr, (QWORD)(pReceiver), (QWORD)(buff));
    funcFree((QWORD)buff);

    return (int)status;
}

int SendPatMessage(string roomid, string wxid)
{
    QWORD status = -1;

    wstring wsRoomid = String2Wstring(roomid);
    wstring wsWxid   = String2Wstring(wxid);
    WxString wxRoomid(wsRoomid);
    WxString wxWxid(wsWxid);

    SendPatMsg_t funcSendPatMsg = (SendPatMsg_t)(g_WeChatWinDllAddr + OS_SEND_PAT_MSG);

    status = funcSendPatMsg((QWORD)(&wxRoomid), (QWORD)(&wxWxid));
    return (int)status;
}

int ForwardMessage(QWORD msgid, string receiver)
{
    int status     = -1;
    uint32_t dbIdx = 0;
    QWORD localId  = 0;

    ForwardMsg_t funcForwardMsg = (ForwardMsg_t)(g_WeChatWinDllAddr + OS_FORWARD_MSG);
    if (GetLocalIdandDbidx(msgid, &localId, &dbIdx) != 0) {
        LOG_ERROR("Failed to get localId, Please check id: {}", to_string(msgid));
        return status;
    }

    WxString *pReceiver = NewWxStringFromStr(receiver);

    LARGE_INTEGER l;
    l.HighPart = dbIdx;
    l.LowPart  = (DWORD)localId;

    status = (int)funcForwardMsg((QWORD)pReceiver, l.QuadPart, 0x4, 0x0);

    return status;
}

void SendEmotionMessage(string wxid, string path)
{
    GetEmotionMgr_t GetEmotionMgr = (GetEmotionMgr_t)(g_WeChatWinDllAddr + OS_GET_EMOTION_MGR);
    SendEmotion_t SendEmotion     = (SendEmotion_t)(g_WeChatWinDllAddr + OS_SEND_EMOTION);

    WxString *pWxPath = NewWxStringFromStr(path);
    WxString *pWxWxid = NewWxStringFromStr(wxid);

    QWORD *buff = (QWORD *)HeapAlloc(GetProcessHeap(), 0, 0x20);
    if (buff == NULL) {
        LOG_ERROR("Out of Memory...");
        return;
    }

    memset(buff, 0, 0x20);
    QWORD mgr = GetEmotionMgr();
    SendEmotion(mgr, (QWORD)pWxPath, (QWORD)buff, (QWORD)pWxWxid, 2, (QWORD)buff, 0, (QWORD)buff);
}

void SendXmlMessage(string receiver, string xml, string path, QWORD type)
{
    if (g_WeChatWinDllAddr == 0) {
        return;
    }

    New_t funcNew   = (New_t)(g_WeChatWinDllAddr + OS_NEW);
    Free_t funcFree = (Free_t)(g_WeChatWinDllAddr + OS_FREE);

    XmlBufSign_t xmlBufSign = (XmlBufSign_t)(g_WeChatWinDllAddr + OS_XML_BUFSIGN);
    SendXmlMsg_t sendXmlMsg = (SendXmlMsg_t)(g_WeChatWinDllAddr + OS_SEND_XML);

    char buff[0x500]   = { 0 };
    char buff2[0x500]  = { 0 };
    char nullBuf[0x1C] = { 0 };

    QWORD pBuf  = (QWORD)(&buff);
    QWORD pBuf2 = (QWORD)(&buff2);

    funcNew(pBuf);
    funcNew(pBuf2);

    QWORD sbuf[4] = { 0, 0, 0, 0 };

    QWORD sign = xmlBufSign(pBuf2, (QWORD)(&sbuf), 0x1);

    WxString *pReceiver = NewWxStringFromStr(receiver);
    WxString *pXml      = NewWxStringFromStr(xml);
    WxString *pPath     = NewWxStringFromStr(path);
    WxString *pSender   = NewWxStringFromStr(GetSelfWxid());

    sendXmlMsg(pBuf, (QWORD)pSender, (QWORD)pReceiver, (QWORD)pXml, (QWORD)pPath, (QWORD)(&nullBuf), type, 0x4, sign,
               pBuf2);

    funcFree((QWORD)&buff);
    funcFree((QWORD)&buff2);
}
