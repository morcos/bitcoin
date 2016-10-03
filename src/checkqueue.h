// Copyright (c) 2012-2015 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_CHECKQUEUE_H
#define BITCOIN_CHECKQUEUE_H

#include <algorithm>
#include <vector>

#include <boost/foreach.hpp>
#include <boost/thread/condition_variable.hpp>
#include <boost/thread/locks.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/lockfree/queue.hpp>
#include <forward_list>

template <typename T>
class CCheckQueueControl;

/** 
 * Queue for verifications that have to be performed.
  * The verifications are represented by a type T, which must provide an
  * operator(), returning a bool.
  *
  * One thread (the master) is assumed to push batches of verifications
  * onto the queue, where they are processed by N-1 worker threads. When
  * the master is done adding work, it temporarily joins the worker pool
  * as an N'th worker, until all jobs are done.
  */
template <typename T>
class CCheckQueue
{
private:
    std::forward_list<T> allChecks;
    
    //! Mutex to protect the inner state
    boost::mutex mutex;

    //! Worker threads block on this when out of work
    boost::condition_variable condWorker;

    //! The queue of elements to be processed.
    //! As the order of booleans doesn't matter, it is used as a LIFO (stack)
    boost::lockfree::queue<T*, boost::lockfree::capacity<10000>> queue;

    //! The temporary evaluation result.
    std::atomic<bool> fAllOk;

    std::atomic<bool> done[16];
    std::atomic<bool> allAdded;

    /** Internal function that does bulk of the verification work. */
    bool Loop(unsigned int id, bool fMaster = false)
    {
        bool cachedDone[16] = {false};
        bool fOk = true;
        bool localAllAdded = false;
        do {

            
            T* pcheck;
            // logically, the do loop starts here
            if (fMaster) {
                if (cachedDone[0] || !queue.pop(pcheck)) {
                    if (!cachedDone[0])
                        done[0].store(true);
                    while (true) {
                        bool allDone = true;
                        for (unsigned int i = 0; i <16 ; i++) { //only works if scriptcheck threads = 16
                            cachedDone[i] = cachedDone[i] || done[i].load();
                            allDone = allDone && cachedDone[i];
                        }
                        if (allDone) {
                            bool fRet = fAllOk.load(); //atomic operation
                            // reset the status for new work later
                            if (fMaster)
                                fAllOk.store(true); //atomic operation
                            for (unsigned int i = 0; i <16 ; i++) {
                                done[i].store(false);
                            }
                            // return the current status
                            return fRet;
                        }
                    }
                }
            }
            else {
                while (!queue.pop(pcheck)) {
                    if (localAllAdded) {
                        done[id].store(true);
                        boost::unique_lock<boost::mutex> lock(mutex);
                        condWorker.wait(lock); // wait for more work
                    }
                    else {
                        localAllAdded = allAdded.load();
                    }
                }
            }
            //should make sure there is not a race condition here where we check done[0] then wait and miss our notification
            // Check whether we need to do work at all
            fOk = fAllOk.load();  //atomic operation

            if (fOk)
                fOk = (*pcheck)();

            if (!fOk)
                fAllOk.store(false); //atomic operation
        } while (true);
    }

public:
    //! Create a new check queue
    CCheckQueue(unsigned int nBatchSizeIn) : fAllOk(true), allAdded(false) {}

    //! Worker thread
    void Thread(unsigned int id)
    {
        Loop(id);
    }

    //! Wait until execution finishes, and return whether all evaluations were successful.
    bool Wait()
    {
        allAdded.store(true);
        bool result = Loop(0, true);
        allChecks.clear();
        return result;
    }

    //! Add a batch of checks to the queue
    void Add(std::vector<T>& vChecks)
    {
        if (allAdded.load()) {// first time in new loop
            allAdded.store(false);
            condWorker.notify_all();
        }
        BOOST_FOREACH (T& check, vChecks) {
            allChecks.emplace_front(T());
            allChecks.front().swap(check);
            queue.push(&allChecks.front());
        }
    }

    ~CCheckQueue()
    {
    }

    //bool IsIdle()
    //{
    //    boost::unique_lock<boost::mutex> lock(mutex);
    //    return (nTotal == nIdle && nTodo == 0 && fAllOk == true);
    //}

};

/** 
 * RAII-style controller object for a CCheckQueue that guarantees the passed
 * queue is finished before continuing.
 */
template <typename T>
class CCheckQueueControl
{
private:
    CCheckQueue<T>* pqueue;
    bool fDone;

public:
    CCheckQueueControl(CCheckQueue<T>* pqueueIn) : pqueue(pqueueIn), fDone(false)
    {
        // passed queue is supposed to be unused, or NULL
        //if (pqueue != NULL) {
        //    bool isIdle = pqueue->IsIdle();
        //    assert(isIdle);
        //}
    }

    bool Wait()
    {
        if (pqueue == NULL)
            return true;
        bool fRet = pqueue->Wait();
        fDone = true;
        return fRet;
    }

    void Add(std::vector<T>& vChecks)
    {
        if (pqueue != NULL)
            pqueue->Add(vChecks);
    }

    ~CCheckQueueControl()
    {
        if (!fDone)
            Wait();
    }
};

#endif // BITCOIN_CHECKQUEUE_H
