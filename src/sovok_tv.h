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

#ifndef sovok_tv_h
#define sovok_tv_h

#include "client_core_base.hpp"
#include <string>
#include <map>
#include <set>
#include <vector>
#include <list>
#include <memory>
#include "ActionQueue.hpp"

typedef std::map<std::string, std::string> ParamList;
typedef std::vector<std::string> StreamerNamesList;

typedef std::map<PvrClient::ChannelId , int> SovokArchivesInfo;

class AuthFailedException : public PvrClient::ExceptionBase
{
};

class MissingHttpsSupportException : public PvrClient::ExceptionBase
{
};

class BadSessionIdException : public PvrClient::ExceptionBase
{
public:
    BadSessionIdException() : ExceptionBase("Session ID es empty.") {}
};

class UnknownStreamerIdException : public PvrClient::ExceptionBase
{
public:
    UnknownStreamerIdException() : ExceptionBase("Unknown streamer ID.") {}
};

class MissingApiException : public PvrClient::ExceptionBase
{
public:
    MissingApiException(const char* r) : ExceptionBase(r) {}
};

class ServerErrorException : public PvrClient::ExceptionBase
{
public:
    ServerErrorException(const char* r, int c) : ExceptionBase(r), code(c) {}
    const int code;
};



class SovokTV : public PvrClient::ClientCoreBase
{
public:
    typedef struct {
        bool Hidden;
        std::string GroupName;
        std::string FilterPattern;
    }CountryTemplate;
    
    typedef struct {
        bool IsOn;
        std::vector<CountryTemplate> Filters;
    } CountryFilter;
    
    SovokTV(const std::string &login, const std::string &password);
    ~SovokTV();

    const StreamerNamesList& GetStreamersList() const;
    
    void UpdateEpgForAllChannels(time_t startTime, time_t endTime);

    std::string GetUrl(PvrClient::ChannelId  channelId);
    std::string GetArchiveUrl(PvrClient::ChannelId  channelId, time_t startTime);

//    PvrClient::FavoriteList GetFavorites();

    int GetSreamerId() const { return m_streammerId; }
    void SetStreamerId(int streamerId);
    
    void SetPinCode(const std::string& code) {m_pinCode = code;}
    void SetCountryFilter(const CountryFilter& filter);

protected:
    void Init(bool clearEpgCache);
    void UpdateHasArchive(PvrClient::EpgEntry& entry);
    void BuildChannelAndGroupList();

private:
    typedef std::vector<std::string> StreamerIdsList;
    typedef std::function<void(const ActionQueue::ActionResult&)> TApiCallCompletion;
    
    struct ApiFunctionData;
        
    void GetEpgForAllChannelsForNHours(time_t startTime, short numberOfHours);

    bool Login(bool wait);
    void Logout();
    void Cleanup();
    
    template <typename TParser>
    void CallApiFunction(const ApiFunctionData& data, TParser parser);
    template <typename TParser>
    void CallApiAsync(const ApiFunctionData& data, TParser parser, TApiCallCompletion completion);
    
    void LoadSettings();
    void InitArchivesInfo();
    bool LoadStreamers();

    std::string m_login;
    std::string m_password;
    
    SovokArchivesInfo  m_archivesInfo;
    struct CountryFilterPrivate : public CountryFilter {
        CountryFilterPrivate() {IsOn = false;}
        CountryFilterPrivate(const CountryFilter& filter)
        : CountryFilter (filter)
        {
            Groups.resize(Filters.size());
        }
        CountryFilterPrivate& operator=(const CountryFilter& filter)
        {
           return *this = CountryFilterPrivate(filter);
        }
        std::vector<PvrClient::GroupList::key_type>  Groups;
    }m_countryFilter;
   
    int m_streammerId;
    long m_serverTimeShift;
    StreamerNamesList m_streamerNames;
    StreamerIdsList m_streamerIds;
    unsigned int m_epgActivityCounter;
    std::string m_pinCode;
};

#endif //sovok_tv_h
