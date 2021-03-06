/*
 *
 *   Copyright (C) 2017 Sergey Shramchenko
 *   https://github.com/srg70/pvr.puzzle.tv
 *
 *  Copyright (C) 2013 Alex Deryskyba (alex@codesnake.com)
 *  https://bitbucket.org/codesnake/pvr.sovok.tv_xbmc_addon
 *
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *  http://www.gnu.org/copyleft/gpl.html
 *
 */

#if (defined(_WIN32) || defined(__WIN32__))
#include <windows.h>
#ifdef GetObject
#undef GetObject
#endif
#endif

#include <stdio.h>
#include <algorithm>
#include <list>
#include "kodi/libXBMC_addon.h"
#include "kodi/Filesystem.h"
// Patch for Kodi buggy VFSDirEntry declaration
struct VFSDirEntry_Patch
{
    char* label;             //!< item label
    char* title;             //!< item title
    char* path;              //!< item path
    unsigned int num_props;  //!< Number of properties attached to item
    VFSProperty* properties; //!< Properties
    //    time_t date_time;        //!< file creation date & time
    bool folder;             //!< Item is a folder
    uint64_t size;           //!< Size of file represented by item
};

#include "p8-platform/util/util.h"
#include "p8-platform/util/timeutils.h"
#include "p8-platform/threads/mutex.h"
#include "p8-platform/util/StringUtils.h"


#include "timeshift_buffer.h"
#include "file_cache_buffer.hpp"
#include "memory_cache_buffer.hpp"
#include "plist_buffer.h"
#include "direct_buffer.h"
#include "simple_cyclic_buffer.hpp"
#include "helpers.h"
#include "pvr_client_base.h"
#include "globals.hpp"
#include "HttpEngine.hpp"
#include "client_core_base.hpp"


using namespace std;
using namespace ADDON;
using namespace Buffers;
using namespace PvrClient;
using namespace Globals;
using namespace P8PLATFORM;

namespace CurlUtils
{
    extern void SetCurlTimeout(long timeout);
}
// NOTE: avoid '.' (dot) char in path. Causes to deadlock in Kodi code.
static const char* s_DefaultCacheDir = "special://temp/pvr-puzzle-tv";
static const char* s_DefaultRecordingsDir = "special://temp/pvr-puzzle-tv/recordings";
static std::string s_LocalRecPrefix = "Local";
static std::string s_RemoteRecPrefix = "On Server";

const int RELOAD_EPG_MENU_HOOK = 1;
const int RELOAD_RECORDINGS_MENU_HOOK = 2;

ADDON_STATUS PVRClientBase::Init(PVR_PROPERTIES* pvrprops)
{
    m_clientCore = NULL;
    m_inputBuffer = NULL;
    m_recordBuffer = m_localRecordBuffer = NULL;
    
    LogDebug( "User path: %s", pvrprops->strUserPath);
    LogDebug( "Client path: %s", pvrprops->strClientPath);
    //auto g_strUserPath   = pvrprops->strUserPath;
    m_clientPath = pvrprops->strClientPath;
    m_userPath = pvrprops->strUserPath;
    
    char buffer[1024];
    
    int curlTimout = 15;
    XBMC->GetSetting("curl_timeout", &curlTimout);
    int channelTimeout = 5;
     XBMC->GetSetting("channel_reload_timeout", &channelTimeout);
   
    
    bool isTimeshiftEnabled;
    XBMC->GetSetting("enable_timeshift", &isTimeshiftEnabled);
    string timeshiftPath;
    if (XBMC->GetSetting("timeshift_path", &buffer))
        timeshiftPath = buffer;
    string recordingsPath;
    if (XBMC->GetSetting("recordings_path", &buffer))
        recordingsPath = buffer;
    
    int timeshiftBufferSize = 0;
    XBMC->GetSetting("timeshift_size", &timeshiftBufferSize);
    timeshiftBufferSize *= 1024*1024;
    
    int cacheSizeLimit = 0;
    XBMC->GetSetting("timeshift_off_cache_limit", &cacheSizeLimit);
    cacheSizeLimit *= 1024*1024;
    
    
    TimeshiftBufferType timeshiftBufferType = k_TimeshiftBufferMemory;
    XBMC->GetSetting("timeshift_type", &timeshiftBufferType);
    
    m_rpcPort = 8080;
    XBMC->GetSetting("rpc_local_port", &m_rpcPort);
    
    m_channelIndexOffset = 0;
    XBMC->GetSetting("channel_index_offset", &m_channelIndexOffset);
    
    m_addCurrentEpgToArchive = true;
    XBMC->GetSetting("archive_for_current_epg_item", &m_addCurrentEpgToArchive);

    long waitForInetTimeout = 0;
    XBMC->GetSetting("wait_for_inet", &waitForInetTimeout);
    
    if(waitForInetTimeout > 0){
        char* message = XBMC->GetLocalizedString(32022);
        XBMC->QueueNotification(QUEUE_INFO, message);
        XBMC->FreeString(message);
        
        P8PLATFORM::CTimeout waitForInet(waitForInetTimeout * 1000);
        bool connected = false;
        long timeLeft = 0;
        do {
            timeLeft = waitForInet.TimeLeft();
            connected = HttpEngine::CheckInternetConnection(timeLeft/1000);
            if(!connected)
                P8PLATFORM::CEvent::Sleep(1000);
        }while(!connected && timeLeft > 0);
        if(!connected) {
            char* message = XBMC->GetLocalizedString(32023);
            XBMC->QueueNotification(QUEUE_ERROR, message);
            XBMC->FreeString(message);
        }
    }
    
    CurlUtils::SetCurlTimeout(curlTimout);
    SetChannelReloadTimeout(channelTimeout);
    SetTimeshiftEnabled(isTimeshiftEnabled);
    SetTimeshiftPath(timeshiftPath);
    SetRecordingsPath(recordingsPath);
    SetTimeshiftBufferSize(timeshiftBufferSize);
    SetTimeshiftBufferType(timeshiftBufferType);
    SetCacheLimit(cacheSizeLimit);
    
    PVR_MENUHOOK hook = {RELOAD_EPG_MENU_HOOK, 32050, PVR_MENUHOOK_ALL};
    PVR->AddMenuHook(&hook);

    hook = {RELOAD_RECORDINGS_MENU_HOOK, 32051, PVR_MENUHOOK_ALL};
    PVR->AddMenuHook(&hook);

    // Local recordings path prefix
    char* localizedString  = XBMC->GetLocalizedString(32014);
    s_LocalRecPrefix = localizedString;
    XBMC->FreeString(localizedString);
    // Remote recordings path prefix
    localizedString  = XBMC->GetLocalizedString(32015);
    s_RemoteRecPrefix = localizedString;
    XBMC->FreeString(localizedString);
    
    m_liveChannelId =  m_localRecordChannelId = UnknownChannelId;
    m_lastBytesRead = 1;
    m_lastRecordingsAmount = 0;
    
    return ADDON_STATUS_OK;
    
}

PVRClientBase::~PVRClientBase()
{
    Cleanup();
}
void PVRClientBase::Cleanup()
{
    CloseLiveStream();
    CloseRecordedStream();
    if(m_localRecordBuffer)
        SAFE_DELETE(m_localRecordBuffer);
}

void PVRClientBase::OnSystemSleep()
{
    Cleanup();
    DestroyCoreSafe();
}
void PVRClientBase::OnSystemWake()
{
    CreateCoreSafe(false);
}


ADDON_STATUS PVRClientBase::SetSetting(const char *settingName, const void *settingValue)
{
    if (strcmp(settingName, "enable_timeshift") == 0)
    {
          SetTimeshiftEnabled(*(bool *)(settingValue));
    }
    else if (strcmp(settingName, "channel_reload_timeout") == 0)
    {
        SetChannelReloadTimeout(*(int *)(settingValue));
    }
    else if (strcmp(settingName, "timeshift_path") == 0)
    {
          SetTimeshiftPath((const char *)(settingValue));
    }
    else if (strcmp(settingName, "recordings_path") == 0)
    {
        SetRecordingsPath((const char *)(settingValue));
    }
    else if (strcmp(settingName, "timeshift_type") == 0)
    {
        SetTimeshiftBufferType(*(TimeshiftBufferType*)(settingValue));
    }
    else if (strcmp(settingName, "timeshift_size") == 0)
    {
        auto size = *(int *)(settingValue);
        size *= 1024*1024;
        SetTimeshiftBufferSize(size);
    }
    else if (strcmp(settingName, "timeshift_off_cache_limit") == 0)
    {
        auto size = *(int *)(settingValue);
        size *= 1024*1024;
        SetCacheLimit(size);
    }
    else if (strcmp(settingName, "curl_timeout") == 0)
    {
        CurlUtils::SetCurlTimeout(*(int *)(settingValue));
    }
    else if (strcmp(settingName, "rpc_local_port") == 0)
    {
        m_rpcPort = *(int *)(settingValue);
    }
    else if (strcmp(settingName, "archive_for_current_epg_item") == 0)
    {
        m_addCurrentEpgToArchive = *(bool *)(settingValue);
        return ADDON_STATUS_NEED_RESTART;
    }
    else if (strcmp(settingName, "channel_index_offset") == 0)
    {
        m_channelIndexOffset = *(int *)(settingValue);
        return ADDON_STATUS_NEED_RESTART;
    }

    return ADDON_STATUS_OK;
}

PVR_ERROR PVRClientBase::GetAddonCapabilities(PVR_ADDON_CAPABILITIES *pCapabilities)
{
    //pCapabilities->bSupportsEPG = true;
    pCapabilities->bSupportsTV = true;
    //pCapabilities->bSupportsRadio = true;
    //pCapabilities->bSupportsChannelGroups = true;
    //pCapabilities->bHandlesInputStream = true;
    //pCapabilities->bSupportsRecordings = true;
    pCapabilities->bSupportsTimers = true;

//    pCapabilities->bSupportsChannelScan = false;
//    pCapabilities->bHandlesDemuxing = false;
//    pCapabilities->bSupportsRecordingPlayCount = false;
//    pCapabilities->bSupportsLastPlayedPosition = false;
//    pCapabilities->bSupportsRecordingEdl = false;

    // Kodi 18
    pCapabilities->bSupportsRecordingsRename = false;
    pCapabilities->bSupportsRecordingsLifetimeChange = false;
    pCapabilities->bSupportsDescrambleInfo = false;

    return PVR_ERROR_NO_ERROR;
}

bool PVRClientBase::CanPauseStream()
{
    return IsTimeshiftEnabled();
}

bool PVRClientBase::CanSeekStream()
{
    return IsTimeshiftEnabled();
}

ADDON_STATUS PVRClientBase::GetStatus()
{
    return  /*(m_sovokTV == NULL)? ADDON_STATUS_LOST_CONNECTION : */ADDON_STATUS_OK;
}

void PVRClientBase::SetRecordingsPath(const std::string& path){
    const char* nonEmptyPath = (path.empty()) ? s_DefaultRecordingsDir : path.c_str();
    if(!XBMC->DirectoryExists(nonEmptyPath))
        if(!XBMC->CreateDirectory(nonEmptyPath))
            LogError( "Failed to create recordings folder.");
    m_recordingsDir = nonEmptyPath;
}

void PVRClientBase::SetTimeshiftPath(const std::string& path){
    const char* nonEmptyPath = (path.empty()) ? s_DefaultCacheDir : path.c_str();
    if(!XBMC->DirectoryExists(nonEmptyPath))
        if(!XBMC->CreateDirectory(nonEmptyPath))
            LogError( "Failed to create cache folder");
    // Cleanup chache
    if(XBMC->DirectoryExists(nonEmptyPath))
    {
        VFSDirEntry* files;
        unsigned int num_files;
        if(XBMC->GetDirectory(nonEmptyPath, "*.bin", &files, &num_files)) {
            VFSDirEntry_Patch* patched_files = (VFSDirEntry_Patch*) files;
            for (int i = 0; i < num_files; ++i) {
                const VFSDirEntry_Patch& f = patched_files[i];
                if(!f.folder)
                    if(!XBMC->DeleteFile(f.path))
                        LogError( "Failed to delete timeshift folder entry %s", f.path);
            }
            XBMC->FreeDirectory(files, num_files);
        } else {
            LogError( "Failed obtain content of timeshift folder %s", nonEmptyPath);
        }
    }

    m_cacheDir = nonEmptyPath;
}

PVR_ERROR  PVRClientBase::MenuHook(const PVR_MENUHOOK &menuhook, const PVR_MENUHOOK_DATA &item)
{
    
    if(menuhook.iHookId == RELOAD_EPG_MENU_HOOK ) {
        char* message = XBMC->GetLocalizedString(32012);
        XBMC->QueueNotification(QUEUE_INFO, message);
        XBMC->FreeString(message);
        OnReloadEpg();
        m_clientCore->CallRpcAsync("{\"jsonrpc\": \"2.0\", \"method\": \"GUI.ActivateWindow\", \"params\": {\"window\": \"pvrsettings\"},\"id\": 1}",
                     [&] (rapidjson::Document& jsonRoot) {
                         char* message = XBMC->GetLocalizedString(32016);
                         XBMC->QueueNotification(QUEUE_INFO, message);
                         XBMC->FreeString(message);
                     },
                     [&](const ActionQueue::ActionResult& s) {});

    } else if(RELOAD_RECORDINGS_MENU_HOOK == menuhook.iHookId) {
//        char* message = XBMC->GetLocalizedString(32012);
//        XBMC->QueueNotification(QUEUE_INFO, message);
//        XBMC->FreeString(message);
        OnReloadRecordings();
    }
    return PVR_ERROR_NO_ERROR;
    
}

ADDON_STATUS PVRClientBase::OnReloadEpg()
{
    return ADDON_STATUS_OK;
}

ADDON_STATUS PVRClientBase::OnReloadRecordings()
{
    if(NULL == m_clientCore)
        return ADDON_STATUS_LOST_CONNECTION;
    
    ADDON_STATUS retVal = ADDON_STATUS_OK;
    m_clientCore->ReloadRecordings();
    return retVal;
}

#pragma mark - Channels

PVR_ERROR PVRClientBase::GetChannels(ADDON_HANDLE handle, bool bRadio)
{
    if(NULL == m_clientCore)
        return PVR_ERROR_SERVER_ERROR;
    
    for(auto& itChannel : m_clientCore->GetChannelList())
    {
        auto & channel = itChannel.second;
        if (bRadio == channel.IsRadio)
        {
            PVR_CHANNEL pvrChannel = { 0 };
            pvrChannel.iUniqueId = channel.Id;
            pvrChannel.iChannelNumber = channel.Number + m_channelIndexOffset;
            pvrChannel.bIsRadio = channel.IsRadio;
            strncpy(pvrChannel.strChannelName, channel.Name.c_str(), sizeof(pvrChannel.strChannelName));
            strncpy(pvrChannel.strIconPath, channel.IconPath.c_str(), sizeof(pvrChannel.strIconPath));
            
            LogDebug("==> CH %-5d - %-40s", pvrChannel.iChannelNumber, channel.Name.c_str());
            
            PVR->TransferChannelEntry(handle, &pvrChannel);
        }
    }
    
    return PVR_ERROR_NO_ERROR;
}

int PVRClientBase::GetChannelsAmount()
{
    if(NULL == m_clientCore)
        return -1;
    
    return m_clientCore->GetChannelList().size();
}
#pragma mark - Groups

int PVRClientBase::GetChannelGroupsAmount()
{
    if(NULL == m_clientCore)
        return -1;
    
    size_t numberOfGroups = m_clientCore->GetGroupList().size();
    return numberOfGroups;
}

PVR_ERROR PVRClientBase::GetChannelGroups(ADDON_HANDLE handle, bool bRadio)
{
    if(NULL == m_clientCore)
        return PVR_ERROR_SERVER_ERROR;
    
    if (!bRadio)
    {
        PVR_CHANNEL_GROUP pvrGroup = { 0 };
        pvrGroup.bIsRadio = false;
        for (auto& itGroup : m_clientCore->GetGroupList())
        {
            strncpy(pvrGroup.strGroupName, itGroup.second.Name.c_str(), sizeof(pvrGroup.strGroupName));
            PVR->TransferChannelGroup(handle, &pvrGroup);
        }
    }
    
    return PVR_ERROR_NO_ERROR;
}

PVR_ERROR PVRClientBase::GetChannelGroupMembers(ADDON_HANDLE handle, const PVR_CHANNEL_GROUP& group)
{
    if(NULL == m_clientCore)
        return PVR_ERROR_SERVER_ERROR;
    
    auto& groups = m_clientCore->GetGroupList();
    auto itGroup =  std::find_if(groups.begin(), groups.end(), [&](const GroupList::value_type& v ){
        return strcmp(group.strGroupName, v.second.Name.c_str()) == 0;
    });
    if (itGroup != groups.end())
    {
        for (auto it : itGroup->second.Channels)
        {
            PVR_CHANNEL_GROUP_MEMBER pvrGroupMember = { 0 };
            strncpy(pvrGroupMember.strGroupName, itGroup->second.Name.c_str(), sizeof(pvrGroupMember.strGroupName));
            pvrGroupMember.iChannelUniqueId = it;
            PVR->TransferChannelGroupMember(handle, &pvrGroupMember);
        }
    }
   
    return PVR_ERROR_NO_ERROR;
}

#pragma mark - EPG

PVR_ERROR PVRClientBase::GetEPGForChannel(ADDON_HANDLE handle, const PVR_CHANNEL& channel, time_t iStart, time_t iEnd)
{
    if(NULL == m_clientCore)
        return PVR_ERROR_SERVER_ERROR;
    
    m_clientCore->GetPhase(IClientCore::k_InitPhase)->Wait();
   
    EpgEntryList epgEntries;
    m_clientCore->GetEpg(channel.iUniqueId, iStart, iEnd, epgEntries);
    EpgEntryList::const_iterator itEpgEntry = epgEntries.begin();
    for (int i = 0; itEpgEntry != epgEntries.end(); ++itEpgEntry, ++i)
    {
        EPG_TAG tag = { 0 };
        tag.iUniqueBroadcastId = itEpgEntry->first;
        itEpgEntry->second.FillEpgTag(tag);
        PVR->TransferEpgEntry(handle, &tag);
    }
    return PVR_ERROR_NO_ERROR;
}

#pragma mark - Streams

InputBuffer*  PVRClientBase::BufferForUrl(const std::string& url )
{
    InputBuffer* buffer = NULL;
    const std::string m3uExt = ".m3u";
    const std::string m3u8Ext = ".m3u8";
    if( url.find(m3u8Ext) != std::string::npos || url.find(m3uExt) != std::string::npos)
        buffer = new Buffers::PlaylistBuffer(url, NULL); // No segments cache for live playlist
    else
        buffer = new DirectBuffer(url);
    return buffer;
}

std::string PVRClientBase::GetStreamUrl(ChannelId channel)
{
    if(NULL == m_clientCore)
        return string();
    return  m_clientCore->GetUrl(channel);
}

bool PVRClientBase::OpenLiveStream(const PVR_CHANNEL& channel)
{
    m_lastBytesRead = 1;
    bool succeeded = OpenLiveStream(channel.iUniqueId, GetStreamUrl(channel.iUniqueId));
    bool tryToRecover = !succeeded;
    while(tryToRecover) {
        string url = GetNextStreamUrl(channel.iUniqueId);
        if(url.empty()) {// nomore streams
            LogDebug("No alternative stream found.");
            break;
        }
        succeeded = OpenLiveStream(channel.iUniqueId, url);
        tryToRecover = !succeeded;
    }
    
    return succeeded;

}

Buffers::ICacheBuffer* PVRClientBase::CreateLiveCache() const {
    if (m_isTimeshiftEnabled){
        if(k_TimeshiftBufferFile == m_timeshiftBufferType) {
            return new Buffers::FileCacheBuffer(m_cacheDir, m_timshiftBufferSize /  Buffers::FileCacheBuffer::CHUNK_FILE_SIZE_LIMIT);
        } else {
            return new Buffers::MemoryCacheBuffer(m_timshiftBufferSize /  Buffers::MemoryCacheBuffer::CHUNK_SIZE_LIMIT);
        }
    }
    else
        return new Buffers::SimpleCyclicBuffer(m_cacheSizeLimit / Buffers::SimpleCyclicBuffer::CHUNK_SIZE_LIMIT);

}

bool PVRClientBase::OpenLiveStream(ChannelId channelId, const std::string& url )
{
    
    if(channelId == m_liveChannelId && IsLiveInRecording())
        return true; // Do not change url of local recording stream

    if(channelId == m_localRecordChannelId) {
        CLockObject lock(m_mutex);
        m_liveChannelId = m_localRecordChannelId;
        m_inputBuffer = m_localRecordBuffer;
        return true;
    }

    m_liveChannelId = UnknownChannelId;
    if (url.empty())
        return false;
    try
    {
        InputBuffer* buffer = BufferForUrl(url);
        CLockObject lock(m_mutex);
        m_inputBuffer = new Buffers::TimeshiftBuffer(buffer, CreateLiveCache());

    }
    catch (InputBufferException &ex)
    {
        LogError(  "PVRClientBase: input buffer error in OpenLiveStream: %s", ex.what());
        return false;
    }
    m_liveChannelId = channelId;
    return true;
}

void PVRClientBase::CloseLiveStream()
{
    CLockObject lock(m_mutex);
    m_liveChannelId = UnknownChannelId;
    if(m_inputBuffer && !IsLiveInRecording()) {
        LogNotice("PVRClientBase: closing input sream...");
        SAFE_DELETE(m_inputBuffer);
        LogNotice("PVRClientBase: input sream closed.");
    } else{
        m_inputBuffer = nullptr;
    }
}

int PVRClientBase::ReadLiveStream(unsigned char* pBuffer, unsigned int iBufferSize)
{
    CLockObject lock(m_mutex);

    int bytesRead = m_inputBuffer->Read(pBuffer, iBufferSize, m_channelReloadTimeout * 1000);
    // Assuming stream hanging.
    // Try to restart current channel only when previous read operation succeeded.
    if (bytesRead != iBufferSize && m_lastBytesRead > 0 && !IsLiveInRecording()) {
        LogError("PVRClientBase:: trying to restart current channel.");

        string url = GetStreamUrl(GetLiveChannelId());
        if(!url.empty()){
            char* message = XBMC->GetLocalizedString(32000);
            XBMC->QueueNotification(QUEUE_INFO, message);
            XBMC->FreeString(message);
            SwitchChannel(GetLiveChannelId(), url);
            bytesRead = m_inputBuffer->Read(pBuffer, iBufferSize, m_channelReloadTimeout * 1000);
        }
   }
    m_lastBytesRead = bytesRead;
    return bytesRead;
}

long long PVRClientBase::SeekLiveStream(long long iPosition, int iWhence)
{
    CLockObject lock(m_mutex);
    return m_inputBuffer->Seek(iPosition, iWhence);
}

long long PVRClientBase::PositionLiveStream()
{
    CLockObject lock(m_mutex);
    return m_inputBuffer->GetPosition();
}

long long PVRClientBase::LengthLiveStream()
{
    CLockObject lock(m_mutex);
    return m_inputBuffer->GetLength();
}

bool PVRClientBase::SwitchChannel(const PVR_CHANNEL& channel)
{
    return SwitchChannel(channel.iUniqueId, GetStreamUrl(channel.iUniqueId));
}

bool PVRClientBase::SwitchChannel(ChannelId channelId, const std::string& url)
{
    if(url.empty())
        return false;
    CLockObject lock(m_mutex);
    if(IsLiveInRecording() || channelId == m_localRecordChannelId)
        return OpenLiveStream(channelId, url); // Split/join live and recording streams (when nesessry)
    
    m_liveChannelId = channelId;
    return m_inputBuffer->SwitchStream(url); // Just change live stream
}

void PVRClientBase::SetTimeshiftEnabled(bool enable)
{
    m_isTimeshiftEnabled = enable;
}

void PVRClientBase::SetChannelReloadTimeout(int timeout)
{
    m_channelReloadTimeout = timeout;
}

void PVRClientBase::SetCacheLimit(uint64_t size)
{
    m_cacheSizeLimit = size;
}

void PVRClientBase::SetTimeshiftBufferSize(uint64_t size)
{
    m_timshiftBufferSize = size;
}

void PVRClientBase::SetTimeshiftBufferType(PVRClientBase::TimeshiftBufferType type)
{
    m_timeshiftBufferType = type;
}

#pragma mark - Recordings

int PVRClientBase::GetRecordingsAmount(bool deleted)
{
    if(NULL == m_clientCore)
        return -1;
    
    if(deleted)
        return -1;
    
    if(!m_clientCore->GetPhase(IClientCore::k_EpgLoadingPhase)->IsDone())
        return 0;
    
    int size = 0;
    IClientCore::EpgEntryAction action = [&size](const EpgEntryList::value_type& p)
    {
        if(p.second.HasArchive)
            ++size;
        return true;
    };
    m_clientCore->ForEachEpg(action);
//    if(size == 0){
//        m_clientCore->ReloadRecordings();
//        m_clientCore->ForEachEpg(action);
//    }
    
    // Add local recordings
    if(XBMC->DirectoryExists(m_recordingsDir.c_str()))
    {
        VFSDirEntry* files;
        unsigned int num_files;
        if(XBMC->GetDirectory(m_recordingsDir.c_str(), "", &files, &num_files)) {
            VFSDirEntry_Patch* patched_files = (VFSDirEntry_Patch*) files;
            for (int i = 0; i < num_files; ++i) {
                const VFSDirEntry_Patch& f = patched_files[i];
                if(f.folder)
                    ++size;

            }
            XBMC->FreeDirectory(files, num_files);
        } else {
            LogError( "Failed obtain content of local recordings folder (amount) %s", m_recordingsDir.c_str());
        }

    }

    if(m_lastRecordingsAmount  != size)
        PVR->TriggerRecordingUpdate();
    LogDebug("PVRClientBase: found %d recordings. Was %d", size, m_lastRecordingsAmount);
    m_lastRecordingsAmount = size;
    return size;
    
}

void PVRClientBase::FillRecording(const EpgEntryList::value_type& epgEntry, PVR_RECORDING& tag, const char* dirPrefix)
{
    const auto& epgTag = epgEntry.second;
    
    sprintf(tag.strRecordingId, "%d",  epgEntry.first);
    strncpy(tag.strTitle, epgTag.Title.c_str(), PVR_ADDON_NAME_STRING_LENGTH - 1);
    strncpy(tag.strPlot, epgTag.Description.c_str(), PVR_ADDON_DESC_STRING_LENGTH - 1);
    strncpy(tag.strChannelName, m_clientCore->GetChannelList().at(epgTag.ChannelId).Name.c_str(), PVR_ADDON_NAME_STRING_LENGTH - 1);
    tag.recordingTime = epgTag.StartTime;
    tag.iLifetime = 0; /* not implemented */
    
    tag.iDuration = epgTag.EndTime - epgTag.StartTime;
    tag.iEpgEventId = epgEntry.first;
    tag.iChannelUid = epgTag.ChannelId;
    tag.channelType = PVR_RECORDING_CHANNEL_TYPE_TV;
    if(!epgTag.IconPath.empty())
        strncpy(tag.strIconPath, epgTag.IconPath.c_str(), sizeof(tag.strIconPath) -1);
    
    string dirName(dirPrefix);
    dirName += '/';
    dirName += tag.strChannelName;
    char buff[20];
    strftime(buff, sizeof(buff), "/%d-%m-%y", localtime(&epgTag.StartTime));
    dirName += buff;
    strncpy(tag.strDirectory, dirName.c_str(), PVR_ADDON_NAME_STRING_LENGTH - 1);

}
PVR_ERROR PVRClientBase::GetRecordings(ADDON_HANDLE handle, bool deleted)
{

    if(NULL == m_clientCore)
        return PVR_ERROR_SERVER_ERROR;

    if(deleted)
        return PVR_ERROR_NOT_IMPLEMENTED;
    
    if(!m_clientCore->GetPhase(IClientCore::k_EpgLoadingPhase)->IsDone())
        return PVR_ERROR_NO_ERROR;

    
    PVR_ERROR result = PVR_ERROR_NO_ERROR;
    auto pThis = this;
    
    std::list<PVR_RECORDING> recs;
    IClientCore::EpgEntryAction action = [pThis ,&result, &recs](const EpgEntryList::value_type& epgEntry)
    {
        try {
            if(!epgEntry.second.HasArchive)
                return true;

            PVR_RECORDING tag = { 0 };
            pThis->FillRecording(epgEntry, tag, s_RemoteRecPrefix.c_str());
            recs.push_back(tag);
            return true;
        }
        catch (...)  {
            LogError( "%s: failed.", __FUNCTION__);
            result = PVR_ERROR_FAILED;
        }
        // Should not be here..
        return false;
    };
    m_clientCore->ForEachEpg(action);
    
    // Populate server recordings
    for (auto& r : recs) {
        PVR->TransferRecordingEntry(handle, &r);
    }
    // Add local recordings
    if(XBMC->DirectoryExists(m_recordingsDir.c_str()))
    {
        VFSDirEntry* files;
        unsigned int num_files;
        if(XBMC->GetDirectory(m_recordingsDir.c_str(), "", &files, &num_files)) {
            VFSDirEntry_Patch* patched_files = (VFSDirEntry_Patch*) files;
            for (int i = 0; i < num_files; ++i) {
                const VFSDirEntry_Patch& f = patched_files[i];
                if(f.folder)
                    continue;
                std::string infoPath = f.path;
                infoPath += PATH_SEPARATOR_CHAR;
                infoPath += "recording.inf";
                void* infoFile = XBMC->OpenFile(infoPath.c_str(), 0);
                if(nullptr == infoFile)
                    continue;
                PVR_RECORDING tag = { 0 };
                bool isValid = XBMC->ReadFile(infoFile, &tag, sizeof(tag)) == sizeof(tag);
                XBMC->CloseFile(infoFile);
                if(!isValid)
                    continue;
                
                PVR->TransferRecordingEntry(handle, &tag);
           }
            XBMC->FreeDirectory(files, num_files);
        } else {
            LogError( "Failed obtain content of local recordings folder %s", m_recordingsDir.c_str());
        }
        
    }

    return result;
}

PVR_ERROR PVRClientBase::DeleteRecording(const PVR_RECORDING &recording)
{
    PVR_ERROR result = PVR_ERROR_NO_ERROR;
    // Is recording local?
    if(!IsLocalRecording(recording))
        return PVR_ERROR_REJECTED;
    std::string dir = DirectoryForRecording(stoul(recording.strRecordingId));
    if(!XBMC->DirectoryExists(dir.c_str()))
        return PVR_ERROR_INVALID_PARAMETERS;

    
    if(XBMC->DirectoryExists(m_recordingsDir.c_str()))
    {
        VFSDirEntry* files;
        unsigned int num_files;
        if(XBMC->GetDirectory(m_recordingsDir.c_str(), "", &files, &num_files)) {
            VFSDirEntry_Patch* patched_files = (VFSDirEntry_Patch*) files;
            for (int i = 0; i < num_files; ++i) {
                const VFSDirEntry_Patch& f = patched_files[i];
                if(f.folder)
                    continue;
                if(!XBMC->DeleteFile(f.path))
                    return PVR_ERROR_FAILED;
            }
            XBMC->FreeDirectory(files, num_files);
        } else {
            LogError( "Failed obtain content of local recordings folder (delete) %s", m_recordingsDir.c_str());
        }
        
    }

    XBMC->RemoveDirectory(dir.c_str());
    PVR->TriggerRecordingUpdate();
    
    return PVR_ERROR_NO_ERROR;
}

bool PVRClientBase::IsLiveInRecording() const
{
    return m_inputBuffer == m_localRecordBuffer;
}


bool PVRClientBase::IsLocalRecording(const PVR_RECORDING &recording) const
{
    return StringUtils::StartsWith(recording.strDirectory, s_LocalRecPrefix.c_str());
}

bool PVRClientBase::OpenRecordedStream(const PVR_RECORDING &recording)
{
    if(!IsLocalRecording(recording))
        return false;
    try {
        InputBuffer* buffer = new DirectBuffer(new FileCacheBuffer(DirectoryForRecording(stoul(recording.strRecordingId))));
        
        if(m_recordBuffer)
            SAFE_DELETE(m_recordBuffer);
        m_recordBuffer = buffer;
    } catch (std::exception ex) {
        LogError("OpenRecordedStream (local) exception: %s", ex.what());
    }
    
    return true;
}

bool PVRClientBase::OpenRecordedStream(const std::string& url,  Buffers::IPlaylistBufferDelegate* delegate)
{
     if (url.empty())
        return false;
    
    try
    {
        InputBuffer* buffer = NULL;
        
        const std::string m3uExt = ".m3u";
        const std::string m3u8Ext = ".m3u8";
        const bool isM3u = url.find(m3u8Ext) != std::string::npos || url.find(m3uExt) != std::string::npos;
        Buffers::PlaylistBufferDelegate plistDelegate(delegate);
        if(isM3u)
            buffer = new Buffers::PlaylistBuffer(url, plistDelegate);
        else
            buffer = new ArchiveBuffer(url);

        m_recordBuffer = buffer;
    }
    catch (InputBufferException & ex)
    {
        LogError(  "%s: failed. Can't open recording stream.", __FUNCTION__);
        return false;
    }
    
    return true;
    
}
void PVRClientBase::CloseRecordedStream(void)
{
    if(m_recordBuffer) {
        LogNotice("PVRClientBase: closing recorded sream...");
        SAFE_DELETE(m_recordBuffer);
        LogNotice("PVRClientBase: input recorded closed.");
    }
    
}

PVR_ERROR PVRClientBase::GetStreamReadChunkSize(int* chunksize) {
    // TODO: obtain from buffer...
    *chunksize = 32 * 1024; // 32K chunk.
    return PVR_ERROR_NO_ERROR;
}

int PVRClientBase::ReadRecordedStream(unsigned char *pBuffer, unsigned int iBufferSize)
{
    uint32_t timeoutMs = 5000;
    return (m_recordBuffer == NULL) ? -1 : m_recordBuffer->Read(pBuffer, iBufferSize, timeoutMs);
}

long long PVRClientBase::SeekRecordedStream(long long iPosition, int iWhence)
{
    return (m_recordBuffer == NULL) ? -1 : m_recordBuffer->Seek(iPosition, iWhence);
}

long long PVRClientBase::PositionRecordedStream(void)
{
    return (m_recordBuffer == NULL) ? -1 : m_recordBuffer->GetPosition();
}
long long PVRClientBase::LengthRecordedStream(void)
{
    return (m_recordBuffer == NULL) ? -1 : m_recordBuffer->GetLength();
}

PVR_ERROR PVRClientBase::IsEPGTagRecordable(const EPG_TAG*, bool* bIsRecordable)
{
    // Seems we can record all tags
    *bIsRecordable = true;
    return PVR_ERROR_NO_ERROR;
}

#pragma mark - Timer  delegate

std::string PVRClientBase::DirectoryForRecording(unsigned int epgId) const
{
    std::string recordingDir = m_recordingsDir;
    if(recordingDir[recordingDir.length() -1] != PATH_SEPARATOR_CHAR)
        recordingDir += PATH_SEPARATOR_CHAR;
    recordingDir += n_to_string(epgId);
    return recordingDir;
}

std::string PVRClientBase::PathForRecordingInfo(unsigned int epgId) const
{
    std::string infoPath = DirectoryForRecording(epgId);
    infoPath += PATH_SEPARATOR_CHAR;
    infoPath += "recording.inf";
    return infoPath;
}

bool PVRClientBase::StartRecordingFor(const PVR_TIMER &timer)
{
    if(NULL == m_clientCore)
        return false;

    bool hasEpg = false;
    auto pThis = this;
    PVR_RECORDING tag = { 0 };
    IClientCore::EpgEntryAction action = [pThis ,&tag, timer, &hasEpg](const EpgEntryList::value_type& epgEntry)
    {
        try {
            if(epgEntry.first != timer.iEpgUid)
                return true;
           
            pThis->FillRecording(epgEntry, tag, s_LocalRecPrefix.c_str());
            tag.recordingTime = time(nullptr);
            tag.iDuration = timer.endTime - tag.recordingTime;
            hasEpg = true;
            return false;
        }
        catch (...)  {
            LogError( "%s: failed.", __FUNCTION__);
            hasEpg = false;
        }
        // Should not be here...
        return false;
    };
    m_clientCore->ForEachEpg(action);
    
    if(!hasEpg) {
        LogError("StartRecordingFor(): timers without EPG are not supported.");
        return false;
    }
    
    std::string recordingDir = DirectoryForRecording(timer.iEpgUid);
    
    if(!XBMC->CreateDirectory(recordingDir.c_str())) {
        LogError("StartRecordingFor(): failed to create recording directory %s ", recordingDir.c_str());
        return false;
    }

    std::string infoPath = PathForRecordingInfo(timer.iEpgUid);
    void* infoFile = XBMC->OpenFileForWrite(infoPath.c_str() , true);
    if(nullptr == infoFile){
        LogError("StartRecordingFor(): failed to create recording info file %s ", infoPath.c_str());
        return false;
    }
    if(XBMC->WriteFile(infoFile, &tag, sizeof(tag))  != sizeof(tag)){
        LogError("StartRecordingFor(): failed to write recording info file %s ", infoPath.c_str());
        XBMC->CloseFile(infoFile);
        return false;
    }
    XBMC->CloseFile(infoFile);
    
    std::string url = m_clientCore ->GetUrl(timer.iClientChannelUid);
    m_localRecordChannelId = timer.iClientChannelUid;
    // When recording channel is same to live channel
    // merge live buffer with local recording
    if(m_liveChannelId == timer.iClientChannelUid){
//        CLockObject lock(m_mutex);
//        CloseLiveStream();
        m_inputBuffer->SwapCache( new Buffers::FileCacheBuffer(recordingDir, 255, false));
        m_localRecordBuffer = m_inputBuffer;
        m_liveChannelId = m_localRecordChannelId; // ???
        return true;
    }
    // otherwise just open new recording stream
    m_localRecordBuffer = new Buffers::TimeshiftBuffer(BufferForUrl(url), new Buffers::FileCacheBuffer(recordingDir, 255, false));

    return true;
}

bool PVRClientBase::StopRecordingFor(const PVR_TIMER &timer)
{
    void* infoFile = nullptr;
    // Update recording duration
    do {
        std::string infoPath = PathForRecordingInfo(timer.iEpgUid);
        void* infoFile = XBMC->OpenFileForWrite(infoPath.c_str() , false);
        if(nullptr == infoFile){
            LogError("StopRecordingFor(): failed to open recording info file %s ", infoPath.c_str());
            break;
        }
        PVR_RECORDING tag = {0};
        XBMC->SeekFile(infoFile, 0, SEEK_SET);
        if(XBMC->ReadFile(infoFile, &tag, sizeof(tag))  != sizeof(tag)){
            LogError("StopRecordingFor(): failed to read from recording info file %s ", infoPath.c_str());
            break;
        }
        tag.iDuration = time(nullptr) - tag.recordingTime;
        XBMC->SeekFile(infoFile, 0, SEEK_SET);
        if(XBMC->WriteFile(infoFile, &tag, sizeof(tag))  != sizeof(tag)){
            LogError("StopRecordingFor(): failed to write recording info file %s ", infoPath.c_str());
            XBMC->CloseFile(infoFile);
            break;
        }
    } while(false);
    if(nullptr != infoFile)
        XBMC->CloseFile(infoFile);
    
    // When recording channel is same to live channel
    // merge live buffer with local recording
    if(m_liveChannelId == timer.iClientChannelUid){
        //CLockObject lock(m_mutex);
        m_inputBuffer->SwapCache(CreateLiveCache());
        m_localRecordBuffer = nullptr;
    } else {
        if(m_localRecordBuffer) {
            m_localRecordChannelId = UnknownChannelId;
            SAFE_DELETE(m_localRecordBuffer);
        }
    }
    
    // trigger Kodi recordings update
    PVR->TriggerRecordingUpdate();
    return true;
    
}
bool PVRClientBase::FindEpgFor(const PVR_TIMER &timer)
{
    return true;
}



#pragma mark - Menus

PVR_ERROR PVRClientBase::CallMenuHook(const PVR_MENUHOOK &menuhook, const PVR_MENUHOOK_DATA &item)
{
    LogDebug( " >>>> !!!! Menu hook !!!! <<<<<");
    return MenuHook(menuhook, item);
}

#pragma mark - Playlist Utils
bool PVRClientBase::CheckPlaylistUrl(const std::string& url)
{
    auto f  = XBMC->OpenFile(url.c_str(), 0);
    
    if (nullptr == f) {
        char* message = XBMC->GetLocalizedString(32010);
        XBMC->QueueNotification(QUEUE_ERROR, message);
        XBMC->FreeString(message);
        return false;
    }
    
    XBMC->CloseFile(f);
    return true;
}

void PvrClient::EpgEntry::FillEpgTag(EPG_TAG& tag) const{
    tag.iUniqueChannelId = ChannelId;
    tag.strTitle = Title.c_str();
    tag.strPlot = Description.c_str();
    tag.startTime = StartTime;
    tag.endTime = EndTime;
    tag.strIconPath = IconPath.c_str();
    if(!Category.empty()) {
        tag.iGenreType = EPG_GENRE_USE_STRING;
        tag.strGenreDescription = Category.c_str();
    }
}

