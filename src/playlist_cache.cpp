/*
 *
 *   Copyright (C) 2018 Sergey Shramchenko
 *   https://github.com/srg70/pvr.puzzle.tv
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


#include "p8-platform/os.h"
#include <memory>
#include <list>
#include <inttypes.h>
#include "playlist_cache.hpp"
#include "Playlist.hpp"
#include "globals.hpp"

using namespace Globals;

namespace Buffers {
    PlaylistCache::PlaylistCache(const std::string &playlistUrl, PlaylistBufferDelegate delegate)
    : m_totalLength(0)
    , m_bitrate(0.0)
    , m_playlist(playlistUrl)
    , m_playlistTimeOffset(0.0)
    , m_delegate(delegate)
    , m_cacheSizeInBytes(0)
    , m_currentSegmentIndex(0)
    , m_currentSegmentPositionFactor(0.0)
    , m_cacheSizeLimit((nullptr != delegate) ? delegate->SegmentsAmountToCache() * 3 * 1024 * 1024 : 0) // ~3MByte/chunck (usualy 6 sec)
    {
        ReloadPlaylist();
        m_currentSegmentIndex = m_dataToLoad.front().index;
    }
    
    PlaylistCache::~PlaylistCache()  {
    }
    
    void PlaylistCache::ReloadPlaylist(){
        
 
        m_playlist.Reload();
        {
            SegmentInfo info;
            bool hasMore = true;
            while(m_playlist.NextSegment(info, hasMore)) {
                m_dataToLoad.push_back(info);
                if(!hasMore)
                    break;
            }
        }
        
        
        // For VOD we can fill data offset for segments already.
        // Do it only first time, i.e. when m_bitrate == 0
        bool shouldCalculateOffset = m_playlist.IsVod() && m_bitrate == 0;
        if (shouldCalculateOffset) {
            MutableSegment::TimeOffset timeOffaset = 0.0;
            MutableSegment::DataOffset dataOfset = 0;
            
            // Stat first 3 segments to calculate bitrate
            auto it = m_dataToLoad.begin();
            auto last = it + 3;
            while(shouldCalculateOffset && it != last) {
                struct __stat64 stat;
                shouldCalculateOffset &= (0 == XBMC->StatFile(it->url.c_str(), &stat));
                if(!shouldCalculateOffset){
                    LogError("PlaylistCache: failed to obtain file stat for %s. Total length %" PRId64 "(%f Bps)", it->url.c_str(), m_totalLength, Bitrate());
                }
                MutableSegment* segment = new MutableSegment(*it, timeOffaset);
                segment->_length = stat.st_size;
                m_segments[it->index] = std::unique_ptr<MutableSegment>(segment);
                timeOffaset += segment->Duration();
                m_totalLength += segment->_length;

                ++it;
            }
            if(!shouldCalculateOffset){
                LogError("PlaylistCache: failed to initialize VOD stream. No seek will be available!");
                return;
            }
            
            // Define "virtual" bitrate.
            m_bitrate =  m_totalLength/timeOffaset;
            
            last  = m_dataToLoad.end();
            while(it!=last) {
                MutableSegment* segment = new MutableSegment(*it, timeOffaset);
                segment->_length = m_bitrate * it->duration;
                m_segments[it->index] = std::unique_ptr<MutableSegment>(segment);
                timeOffaset += segment->Duration();
                m_totalLength += segment->_length;
                ++it;
            }
            
            // We would like to load last segment just after first to be ready for seek to end of stream
            // Move last to second.
            m_dataToLoad.insert(m_dataToLoad.begin() + 1, m_dataToLoad.back());
            m_dataToLoad.pop_back();

         }
        if(shouldCalculateOffset){
            LogError("PlaylistCache: playlist reloaded. Total length %" PRId64 "(%f B/sec).", m_totalLength, Bitrate());
        }
    }
    
    MutableSegment* PlaylistCache::SegmentToFill()  {

        if(IsFull())
            return nullptr;
        if(m_dataToLoad.empty()) {
            return nullptr;
        }
        
        SegmentInfo info;
        bool found = false;
        // Skip all valid segment
        do{
            info = m_dataToLoad.front();
            m_dataToLoad.pop_front();
            if(m_segments.count(info.index) > 0) {
                found = !m_segments[info.index]->IsValid();
            } else {
                found = true;
            }
        }while(!found && !m_dataToLoad.empty());
        // Do we have segment info to fill?
        if(!found) {
            return nullptr;
        }
        // VOD contains static segments.
        // No new segment needed, just return old "empty" segment
        if(m_playlist.IsVod() && m_segments.count(info.index) > 0) {
            LogDebug("PlaylistCache: start LOADING segment %" PRIu64 ".", info.index);
            return m_segments[info.index].get();
        }
        // Calculate time and data offsets
        MutableSegment::TimeOffset timeOffaset = m_playlistTimeOffset;
        // Do we have previous segment?
        if(m_segments.count(info.index - 1) > 0){
            // Override playlist initial offsetts with actual values
            const auto& prevSegment = m_segments[info.index - 1];
            timeOffaset = prevSegment->timeOffset + prevSegment->Duration();
        }
        MutableSegment* retVal = new MutableSegment(info, timeOffaset);
        m_segments[info.index] = std::unique_ptr<MutableSegment>(retVal);
        LogDebug("PlaylistCache: start LOADING segment %" PRIu64 ".", info.index);
        return retVal;
    }
    
    void PlaylistCache::SegmentReady(MutableSegment* segment) {
        segment->DataReady();
        m_cacheSizeInBytes += segment->Size();
        LogDebug("PlaylistCache: segment %" PRIu64 " added. Cache size %d bytes", segment->info.index, m_cacheSizeInBytes);
    }
    
    Segment* PlaylistCache::NextSegment() {
        
        if(m_segments.size() == 0) {
            return nullptr;
        }
        
        MutableSegment* retVal = nullptr;
        
        if(m_segments.count(m_currentSegmentIndex) > 0) {
            auto& seg = m_segments[m_currentSegmentIndex];
            // Segment found. Check data availability
            if(seg->IsValid()) {
                size_t posInSegment = m_currentSegmentPositionFactor * seg->Size();
                seg->Seek(posInSegment);
                retVal = seg.get();
                LogDebug("PlaylistCache: READING from segment %" PRIu64 ". Position in segment %d.", seg->info.index, posInSegment);
            } else {
                LogDebug("PlaylistCache: segment %" PRIu64 " found, but has NO data.", seg->info.index);
                // Segment is not ready yet
                retVal = nullptr;
            }
        }
        else {
            LogError("PlaylistCache: wrong current segment index %" PRIu64 ". Total %d segments.", m_currentSegmentIndex, m_segments.size());
            
        }
        
        // Free older segments when cache is full
        // or we are on live stream (no caching requered)
        if(IsFull() || !CanSeek()) {
            int64_t idx = -1;
            auto runner = m_segments.begin();
            const auto end = m_segments.end();
            while(runner->first < m_currentSegmentIndex) {
                if(runner->second->IsValid()) {
                    idx = runner->first;
                    break;
                }
                ++runner;
            }
            if(-1 == idx) {
                auto rrunner = m_segments.rbegin();
                const auto rend = m_segments.rend();
                ++rrunner; // Skip last segment, preserve in cache
                while(rrunner != rend &&  rrunner->first > m_currentSegmentIndex) {
                    if(rrunner->second->IsValid()){
                        idx = rrunner->first;
                        break;
                    }
                    ++rrunner;
                }
            }
            // Remove segment before current
            if( idx != -1) {
                m_cacheSizeInBytes -= m_segments[idx]->Size();
                if(!CanSeek()) {
                    m_segments.erase(idx);
                } else {
                    // Preserve stream length info for VOD segment
                    m_segments[idx]->Free();
                }
                LogDebug("PlaylistCache: segment %" PRIu64 " removed. Cache size %d bytes", idx, m_cacheSizeInBytes);
            } else {
                LogDebug("PlaylistCache: cache is full but no segments to free. First idx %" PRIu64 ". Current idx %" PRIu64 " Size %d bytes", idx, m_currentSegmentIndex, m_cacheSizeInBytes);
            }
        }
        if(nullptr != retVal) {
            ++m_currentSegmentIndex;
            m_currentSegmentPositionFactor = 0.0;
        }
        return retVal;
    }

    // Find segment in playlist by time offset,caalculated from position
    bool PlaylistCache::PrepareSegmentForPosition(int64_t position) {
        // Can't seek without delegate
        if(!CanSeek())
            return false;

        m_dataToLoad = TSegmentInfos();
        MutableSegment::TimeOffset timeOffset = TimeOffsetFromProsition(position);

        // VOD plailist contains all segments already
        // So just move loading iterator to position
        if(m_playlist.IsVod()) {
            bool found = false;
            for (const auto& pSeg : m_segments) {
                const MutableSegment::TimeOffset segmentTime = pSeg.second->timeOffset;
                const float segmentDuration = pSeg.second->Duration();
                if( segmentTime <= timeOffset && timeOffset < segmentTime + segmentDuration) {
                    found = true;
                    m_currentSegmentIndex = pSeg.first;
                    m_playlist.SetNextSegmentIndex(m_currentSegmentIndex);
                    // Calculate position inside segment
                    // as rational part of time offset
                    m_currentSegmentPositionFactor = (timeOffset - segmentTime) / segmentDuration;
                    if(m_currentSegmentPositionFactor > 1.0) {
                        LogError("PlaylistCache: segment position factor can't be > 1.0. Requested offset %f, segment timestamp %f, duration %f", timeOffset, segmentTime, segmentDuration);
                    } else {
                        LogDebug("PlaylistCache:  seek positon ratio %f", m_currentSegmentPositionFactor);
                    }
                    break;
                }
            }
            if(!found){
                LogDebug("PlaylistCache: position %" PRId64 " can't be seek. Time offset %f. Total duration %f."  , position, timeOffset, m_delegate->Duration());
                // Can't be
                return false;
            }
        } else {
            auto url = m_delegate->UrlForTimeshift(timeOffset, nullptr);
            m_playlist = Playlist(url);
            ReloadPlaylist();
        }
//        if(succeeded)
        {
            m_playlistTimeOffset = timeOffset;
        }
        return true;
    }
    
    bool PlaylistCache::HasSegmentsToFill() const {
        return !m_dataToLoad.empty();
    }

    bool PlaylistCache::IsEof(int64_t position) const {
        return m_playlist.IsVod() /*&& check position*/;
    }

#pragma mark - Segment
    
    Segment::Segment(float duration)
    : _duration(duration)
    , _data(nullptr)
    {
        Init();
    }
        
//    const uint8_t* Segment::Pop(size_t requesred, size_t*  actual)
//    {
//        if(_begin == NULL)
//            _begin = &_data[0];
//
//        size_t available = _size - (_begin - &_data[0]);
//        *actual = std::min(requesred, available);
//        const uint8_t* retVal = _begin;
//        _begin += *actual;
//        return retVal;
//    }
    
    void Segment::Init() {
        if(_data != nullptr)
            free( _data);
        _data = nullptr;
        _size = 0;
        _begin = nullptr;
    }
    size_t Segment::Seek(size_t position)
    {
        if(nullptr != _data) {
            _begin = &_data[0] + std::min(position, _size);
        }
        return Position();
    }
    
    
    size_t Segment::Read(uint8_t* buffer, size_t size)
    {
        if(nullptr == _data)
            return 0;

        if(_begin == nullptr)
            _begin = &_data[0];
        size_t actual = std::min(size, BytesReady());
        if(actual < 0)
            LogDebug("Segment::Read: error size %d.  Bytes ready: %d", actual, BytesReady());
        memcpy(buffer, _begin, actual);
        _begin += actual;
        return actual;
    }
    
    
    Segment::~Segment()
    {
        if(_data != nullptr)
            free( _data);
    }

    void MutableSegment::Free(){
        Init();
        _isValid = false;
    }
    
    void MutableSegment::Push(const uint8_t* buffer, size_t size)
    {
        if(nullptr == buffer || 0 == size)
            return;
        
        void * ptr = realloc(_data, _size + size);
        if(NULL == ptr)
            throw PlaylistCacheException("Failed to re-allocate segment.");
        _data = (uint8_t*) ptr;
        memcpy(&_data[_size], buffer, size);
        _size += size;
        //    LogDebug(">>> Size: %d", _size);
//        if(_size > _length) {
//            _length = _size;
//        }
    }

}
