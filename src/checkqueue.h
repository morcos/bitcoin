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
    //! Mutex to protect the inner state
    boost::mutex mutex[16];

    //! Worker threads block on this when out of work
    boost::condition_variable condWorker[16];

    //! The queue of elements to be processed.
    //! As the order of booleans doesn't matter, it is used as a LIFO (stack)
    std::vector<T> queue[16];

    //! The temporary evaluation result.
    std::atomic<bool> fAllOk;

    std::atomic<bool> done[16];

    unsigned int checknumber;

    /** Internal function that does bulk of the verification work. */
    bool Loop(unsigned int id, bool fMaster = false)
    {
        std::vector<T> vChecks;
        unsigned int nNow = 0;
        bool cachedDone[16] = {false};
        bool fOk = true;
        do {
            {
                boost::unique_lock<boost::mutex> lock(mutex[id]);

                // logically, the do loop starts here
                while (queue[id].empty()) {
                    if (fMaster) {
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
                    else {
                        if (done[0].load()) { //master isn't going to add any more to queue
                            done[id].store(true);
                        }
                        condWorker[id].wait(lock); // wait for more work
                        //should make sure there is not a race condition here where we check done[0] then wait and miss our notification
                    }
                }
                // Decide how many work units to process now.
                // * Do not try to do everything at once, but aim for increasingly smaller batches so
                //   all workers finish approximately simultaneously.
                // * Try to account for idle jobs which will instantly start helping.
                // * Don't do batches smaller than 1 (duh), or larger than nBatchSize.
                nNow = queue[id].size();
                vChecks.resize(nNow);
                for (unsigned int i = 0; i < nNow; i++) {
                    // We want the lock on the mutex to be as short as possible, so swap jobs from the global
                    // queue[id] to the local batch vector instead of copying.
                    vChecks[i].swap(queue[id].back());
                    queue[id].pop_back();
                }
            }

            // Check whether we need to do work at all
            fOk = fAllOk.load();  //atomic operation

            // execute work
            BOOST_FOREACH (T& check, vChecks)
                if (fOk)
                    fOk = check();
            vChecks.clear();
            if (!fOk)
                fAllOk.store(false); //atomic operation
        } while (true);
    }

public:
    //! Create a new check queue
    CCheckQueue(unsigned int nBatchSizeIn) : fAllOk(true), checknumber(0) {}

    //! Worker thread
    void Thread(unsigned int id)
    {
        Loop(id);
    }

    //! Wait until execution finishes, and return whether all evaluations were successful.
    bool Wait()
    {
        done[0].store(true);  //set master done to signal to other threads they can be done now
        for (unsigned int i=0;i<15;i++)
            condWorker[i+1].notify_one();
        checknumber = 0;
        return Loop(0, true);
    }

    //! Add a batch of checks to the queue
    void Add(std::vector<T>& vChecks)
    {
        BOOST_FOREACH (T& check, vChecks) {
            unsigned int threadid = checknumber % 15 + 1;
            checknumber++;
            boost::unique_lock<boost::mutex> lock(mutex[threadid]);
            queue[threadid].push_back(T());
            check.swap(queue[threadid].back());
            condWorker[threadid].notify_one();
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
