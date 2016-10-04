// Copyright (c) 2012-2015 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_CHECKQUEUE_H
#define BITCOIN_CHECKQUEUE_H

#include <algorithm>
#include <vector>

#include <boost/foreach.hpp>
//#include <boost/thread/condition_variable.hpp>
//#include <boost/thread/locks.hpp>
//#include <boost/thread/mutex.hpp>
//#include <boost/lockfree/queue.hpp>
#include <forward_list>
#include <queue>
#include <mutex>
#include <condition_variable>

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
    std::mutex mutex;
    
    //! Worker threads block on this when out of work
    std::condition_variable condWorker;

    //! The queue of elements to be processed.
    //! As the order of booleans doesn't matter, it is used as a LIFO (stack)
    std::queue<T*> queue;

    //! The temporary evaluation result.
    std::atomic<bool> fAllOk;

    std::atomic<bool> done[16];
    bool allAdded;
    bool newBlock;

    std::vector<T*> checkPtrs;
    /** Internal function that does bulk of the verification work. */
    bool Loop(unsigned int id, bool fMaster = false)
    {
        bool cachedDone[16] = {false};
        bool fOk = true;
        bool localAllAdded = false;
        do {

            
            T* pcheck[16];
            unsigned int batchSize=1;
            // logically, the do loop starts here
            unsigned int qsize = 0;
            {
                std::lock_guard<std::mutex> lg(mutex);
                if (qsize = queue.size()) {
                    //To do, add more than 1 if qsize is big?
                    batchSize = std::max(1u,std::min(16u,qsize/2));
                    for (auto i = 0;i < batchSize; i++) {
                        pcheck[i] = queue.front();
                        queue.pop();
                    }
                    fOk = fAllOk.load();  // should this be here or outside of the lock or what?
                }
                else {
                    localAllAdded = allAdded; // this could also be outside of lock?
                }
            }

            if (qsize) {
                if (fOk)
                {
                    for (auto i =0; i < batchSize; i++)
                        fOk = fOk && (*pcheck[i])();
                }
                if (!fOk)
                    fAllOk.store(false); //atomic operation   (maybe this should not be atomic and take the lock here)

            }
            else {
                if (fMaster) {
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
                else {
                    if (localAllAdded) {
                        done[id].store(true);
                        localAllAdded = false;
                        std::unique_lock<std::mutex> lock(mutex);
                        condWorker.wait(lock); // wait for more work
                    }
                }
            }
        } while (true);
    }

public:
    //! Create a new check queue
    CCheckQueue(unsigned int nBatchSizeIn) : fAllOk(true), allAdded(false), newBlock(true) {}

    //! Worker thread
    void Thread(unsigned int id)
    {
        Loop(id);
    }

    //! Wait until execution finishes, and return whether all evaluations were successful.
    bool Wait()
    {
        if (newBlock) { // Cover the case where no checks were added
            std::unique_lock<std::mutex> lock(mutex);
            allAdded = true;
            condWorker.notify_all();
        }
        else {
            std::lock_guard<std::mutex> lg(mutex);
            if (checkPtrs.size()) {
                BOOST_FOREACH (T* pcheck, checkPtrs) {
                    queue.push(pcheck);
                }
            }
            allAdded = true;
        }
        checkPtrs.clear();
        bool result = Loop(0, true);
        allChecks.clear();
        newBlock = true;
        { 
            std::lock_guard<std::mutex> lg(mutex);
            allAdded = false;
        }
        return result;
    }

    //! Add a batch of checks to the queue
    void Add(std::vector<T>& vChecks)
    {
        if (newBlock) {// first time in new loop
            std::unique_lock<std::mutex> lock(mutex);
            condWorker.notify_all();
            newBlock = false;
        }

        BOOST_FOREACH (T& check, vChecks) {
            allChecks.emplace_front(T());
            allChecks.front().swap(check);
            checkPtrs.push_back(&allChecks.front());
        }
        if (checkPtrs.size() > 100)
        {
            {
            std::lock_guard<std::mutex> lg(mutex);
            BOOST_FOREACH (T* pcheck, checkPtrs) {
                queue.push(pcheck);
            }
            }
            checkPtrs.clear();
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
