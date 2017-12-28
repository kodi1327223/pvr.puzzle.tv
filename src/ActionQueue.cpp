/*
 *
 *   Copyright (C) 2017 Sergey Shramchenko
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

#include "ActionQueue.hpp"

static const int32_t INFINITE_QUEUE_TIMEOUT = 0x7FFFFFFF;

void* CActionQueue::Process()
{
    while (!IsStopped())
    {
        IActionQueueItem* action = NULL;
        // Do not wait infinite when pipeline is stopped.
        // Could be no new task will arrive, e.g. on StopThead()
        if( _actions.Pop(action, (_willStop) ? 1000 : INFINITE_QUEUE_TIMEOUT))
        {
            if(NULL == action)
                continue;
            if(_willStop)
                action->Cancel();
            else
                action->Perform();
            delete action;
        }
        
    }
    return NULL;
}

void CActionQueue::TerminatePipeline()
{
    // In case of no active tasks unlocks .Pop() waiting
    _willStop = false;
    PerformAsync([this] {
        _willStop = true;
    }, [](const CActionQueue::ActionResult& s) {});
    _willStop = true;

}

bool CActionQueue::StopThread(int iWaitMs)
{
    TerminatePipeline();
    return this->CThread::StopThread(iWaitMs);
}

CActionQueue::~CActionQueue(void)
{
    StopThread(0);
}
