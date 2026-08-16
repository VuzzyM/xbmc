/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "DemuxMultiSource.h"

#include "DVDDemuxUtils.h"
#include "DVDFactoryDemuxer.h"
#include "DVDInputStreams/DVDInputStream.h"
#include "Util.h"
#include "cores/VideoPlayer/Interface/TimingConstants.h"
#include "utils/log.h"


CDemuxMultiSource::CDemuxMultiSource() = default;

CDemuxMultiSource::~CDemuxMultiSource()
{
  Dispose();
}

void CDemuxMultiSource::Abort()
{
  for (auto& iter : m_demuxerMap)
    iter.second->Abort();
}

void CDemuxMultiSource::Dispose()
{
  while (!m_demuxerQueue.empty())
  {
    m_demuxerQueue.pop();
  }

  m_demuxerMap.clear();
  m_DemuxerToInputStreamMap.clear();
  m_pInput = NULL;

  m_masterDemuxerId = -1;
  m_activeDemuxerId = -1;
}

void CDemuxMultiSource::EnableStream(int64_t demuxerId, int id, bool enable)
{
  auto iter = m_demuxerMap.find(demuxerId);
  if (iter == m_demuxerMap.end())
    return;

  iter->second->EnableStream(demuxerId, id, enable);

  if (enable && demuxerId != m_masterDemuxerId)
    m_activeDemuxerId = demuxerId;
  else if (!enable && demuxerId == m_activeDemuxerId)
    m_activeDemuxerId = -1;
}

void CDemuxMultiSource::Flush()
{
  for (auto& iter : m_demuxerMap)
    iter.second->Flush();
}

int CDemuxMultiSource::GetNrOfStreams() const
{
  int streamsCount = 0;
  for (auto& iter : m_demuxerMap)
    streamsCount += iter.second->GetNrOfStreams();

  return streamsCount;
}

CDemuxStream* CDemuxMultiSource::GetStream(int64_t demuxerId, int iStreamId) const
{
  auto iter = m_demuxerMap.find(demuxerId);
  if (iter != m_demuxerMap.end())
  {
    return iter->second->GetStream(demuxerId, iStreamId);
  }
  else
    return NULL;
}

std::vector<CDemuxStream*> CDemuxMultiSource::GetStreams() const
{
  std::vector<CDemuxStream*> streams;

  for (auto& iter : m_demuxerMap)
  {
    for (auto& stream : iter.second->GetStreams())
    {
      streams.push_back(stream);
    }
  }
  return streams;
}

std::string CDemuxMultiSource::GetStreamCodecName(int64_t demuxerId, int iStreamId)
{
  auto iter = m_demuxerMap.find(demuxerId);
  if (iter != m_demuxerMap.end())
  {
    return iter->second->GetStreamCodecName(demuxerId, iStreamId);
  }
  else
    return "";
};

int CDemuxMultiSource::GetStreamLength()
{
  int length = 0;
  for (auto& iter : m_demuxerMap)
  {
    length = std::max(length, iter.second->GetStreamLength());
  }

  return length;
}

bool CDemuxMultiSource::Open(const std::shared_ptr<CDVDInputStream>& pInput)
{
  if (!pInput)
    return false;

  m_pInput = std::dynamic_pointer_cast<InputStreamMultiStreams>(pInput);

  if (!m_pInput)
    return false;

  auto iter = m_pInput->m_InputStreams.begin();
  while (iter != m_pInput->m_InputStreams.end())
  {
    DemuxPtr demuxer = DemuxPtr(CDVDFactoryDemuxer::CreateDemuxer((*iter)));
    if (!demuxer)
    {
      iter = m_pInput->m_InputStreams.erase(iter);
    }
    else
    {
      SetMissingStreamDetails(demuxer);

      m_demuxerMap[demuxer->GetDemuxerId()] = demuxer;
      m_DemuxerToInputStreamMap[demuxer] = *iter;
      m_demuxerQueue.emplace(-1.0, demuxer);
      if (m_masterDemuxerId == -1)
        m_masterDemuxerId = demuxer->GetDemuxerId();
      ++iter;
    }
  }
  return !m_demuxerMap.empty();
}

bool CDemuxMultiSource::Reset()
{
  bool ret = true;
  for (auto& iter : m_demuxerMap)
  {
    if (!iter.second->Reset())
      ret = false;
  }
  return ret;
}

DemuxPacket* CDemuxMultiSource::Read()
{
  if (m_demuxerQueue.empty())
    return NULL;

  DemuxPtr currentDemuxer = m_demuxerQueue.top().second;
  m_demuxerQueue.pop();

  if (!currentDemuxer)
    return NULL;

  DemuxPacket* packet = currentDemuxer->Read();
  if (packet)
  {
    double readTime = 0;
    if (packet->dts != DVD_NOPTS_VALUE)
      readTime = packet->dts;
    else
      readTime = packet->pts;
    m_demuxerQueue.emplace(readTime, currentDemuxer);
  }
  else
  {
    auto input = m_DemuxerToInputStreamMap.find(currentDemuxer);
    if (input != m_DemuxerToInputStreamMap.end())
    {
      if (input->second->IsEOF())
      {
        CLog::Log(LOGDEBUG, "{} - Demuxer for file {} is at eof, removed it from the queue",
                  __FUNCTION__, CURL::GetRedacted(currentDemuxer->GetFileName()));
      }
      else    //maybe add an error counter?
        m_demuxerQueue.emplace(-1.0, currentDemuxer);
    }
  }

  return packet;
}

bool CDemuxMultiSource::SeekTime(double time, bool backwards, double* startpts)
{
  DemuxQueue demuxerQueue;
  bool ret = false;

  for (auto& iter : m_demuxerMap)
  {
    const int64_t demuxerId = iter.first;
    const DemuxPtr& demuxer = iter.second;

    // Seek only the master video demuxer and the currently
    // selected external audio demuxer.
    if (demuxerId != m_masterDemuxerId &&
        demuxerId != m_activeDemuxerId)
      continue;

    double demuxerStartPts = DVD_NOPTS_VALUE;

    if (demuxer->SeekTime(time, backwards, &demuxerStartPts))
    {
      if (demuxerStartPts != DVD_NOPTS_VALUE)
        demuxerQueue.emplace(demuxerStartPts, demuxer);
      else
        demuxerQueue.emplace(time * DVD_TIME_BASE, demuxer);
      if (startpts && *startpts == DVD_NOPTS_VALUE &&
          demuxerStartPts != DVD_NOPTS_VALUE)
      {
        *startpts = demuxerStartPts;
      }
      CLog::Log(LOGDEBUG, "{} - seek demuxer {} to {:f}",
                __FUNCTION__, demuxer->GetFileName(), time);
      ret = true;
    }
    else
    {
      CLog::Log(LOGDEBUG, "{} - failed to seek demuxer {} to {:f}",
                __FUNCTION__, demuxer->GetFileName(), time);
    }
  }
  m_demuxerQueue = std::move(demuxerQueue);
  return ret;
}

void CDemuxMultiSource::SetMissingStreamDetails(const DemuxPtr& demuxer)
{
  std::string baseFileName = m_pInput->GetFileName();
  std::string fileName = demuxer->GetFileName();
  for (auto& stream : demuxer->GetStreams())
  {
    ExternalStreamInfo info = CUtil::GetExternalStreamDetailsFromFilename(baseFileName, fileName);

    if (stream->flags == StreamFlags::FLAG_NONE)
    {
      stream->flags = static_cast<StreamFlags>(info.flag);
    }
    if (stream->language.empty())
    {
      stream->language = info.language;
    }
  }
}
