// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2013 The Bitcoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef BITCOIN_POLICYESTIMATOR_H
#define BITCOIN_POLICYESTIMATOR_H

#include <map>
#include <string>
#include <vector>

class CAutoFile;
class CFeeRate;
class CTxMemPoolEntry;

/**
 * We will instantiate two instances of this class, one to track transactions
 * that were included in a block due to fee, and one for tx's included due to
 * priority.  We will lump transactions into a bucket according to their approximate
 * fee or priority and then track how long it took for those txs to be included in a block
 */
class TxConfirmStats
{
private:
    //Define the buckets we will group transactions into (both fee buckets and priority buckets)
    std::vector<double> buckets;              // The upper-bound of the range for the bucket (non-inclusive)
    std::map<double, unsigned int> bucketMap; // Map of bucket upper-bound to index into all vectors by bucket

    // For each bucket X:
    // Count the total # of txs in each bucket
    // Track the historical moving average of this total over blocks
    std::vector<double> txCtAvg;
    // and calcuate the total for the current block to update the moving average
    std::vector<int> curBlockTxCt;

    // Count the total # of txs confirmed within Y blocks in each bucket
    // Track the historical moving average of theses totals over blocks
    std::vector<std::vector<double> > confAvg; // confAvg[Y][X]
    // and calcuate the totals for the current block to update the moving averages
    std::vector<std::vector<int> > curBlockConf; // curBlockConf[Y][X]

    // Sum the total priority/fee of all tx's in each bucket
    // Track the historical moving average of this total over blocks
    std::vector<double> avg;
    // and calcuate the total for the current block to update the moving average
    std::vector<double> curBlockVal;

    // Combine the conf counts with tx counts to calculate the confirmation % for each Y,X
    // Combine the total value with the tx counts to calculate the avg fee/priority per bucket

    std::string dataTypeString;
    double decay;

public:
    /**
     * Initialize the data structures.  This is called by BlockPolicyEstimator's
     * constructor with default values.
     * @param defaultBuckets contains the upper limits for the bucket boundries
     * @param maxConfirms max number of confirms to track
     * @param decay how much to decay the historical moving average per block
     * @param dataTypeString for logging purposes
     */
    void Initialize(std::vector<double> defaultBuckets, unsigned int maxConfirms, double decay, std::string dataTypeString);

    /** Clear the state of the curBlock variables to start counting for the new block */
    void ClearCurrent();

    /**
     * Record a new transaction data point in the current block stats
     * @param blocksToConfirm the number of blocks it took this transaction to confirm
     * @param val either the fee or the priority when entered of the transaction
     * @warning blocksToConfirm is 1-based and has to be >= 1
     */
    void Record(int blocksToConfirm, double val);

    /** Update our estimates by decaying our historical moving average and updating
        with the data gathered from the current block */
    void UpdateMovingAverages();

    /**
     * Calculate a fee or priority estimate.  Find the lowest value bucket (or range of buckets
     * to make sure we have enough data points) whose transactions still have sufficient liklihood
     * of being confirmed withing the target number of confirmations
     * @param confTarget target number of confirmations
     * @param sufficientTxVal required average number of transactions per block in a bucket range
     * @param minSuccess the success probability we require
     */
    double EstimateMedianVal(int confTarget, double sufficientTxVal, double minSuccess);

    /** Return the max number of confirms we're tracking */
    unsigned int GetMaxConfirms() { return confAvg.size(); }

    /** Write state of estimation data to a file*/
    void Write(CAutoFile& fileout);

    /**
     * Read saved state of estimation data from a file and replace all internal data structures and
     * variables with this state.
     */
    void Read(CAutoFile& filein);
};



/** Track confirm delays up to 25 blocks, can't estimate beyond that */
static const unsigned int MAX_BLOCK_CONFIRMS = 25;

/** Decay of .998 is a half-life of 346 blocks or about 2.4 days */
static const double DEFAULT_DECAY = .998;

/** Require greater than 80% of X fee transactions to be confirmed within Y blocks for X to be big enough */
static const double MIN_SUCCESS_PCT = .85;

/** Require an avg of 1 tx in the combined fee bucket per block to have stat significance */
static const double SUFFICIENT_FEETXS = 1;

/** Require only an avg of 1 tx every 10 blocks in the combined pri block (way less pri txs) */
static const double SUFFICIENT_PRITXS = .1;

/** Approximately the AllowFree cutoff */
static const double MIN_PRIORITY_VAL = 1e8;

// We have to lump transactions into buckets based on fee or priority, but we want to be able
// to give accurate estimates over a large range of potential fees and priorities
// Therefore it makes sense to exponentially space the buckets
// It's nice to have buckets at the powers of 10, so lets space them by an integral root of 10
// and choose values that give a good distribution of buckets

/** Default values for fee buckets spaced by a factor of 10^1/12 */
static const double FEELIST[] = {0, 1000, 1212, 1468, 1778, 2154, 2610, 3162,
                                 3831, 4642, 5623, 6813, 8254, 10000, 12115,
                                 14678, 17783, 21544, 26102, 31622, 38312, 46416,
                                 56234, 68129, 82540, 100000, 121153, 146780,
                                 177828, 215443, 261016, 316228, 383119, 464159,
                                 562341, 681292, 825404, 1000000, 1e16};

/** Default values for priority buckets spaced by a factor of 10 */
static const double PRILIST[] = {1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e99};

/**
 *  We want to be able to estimate fees or priorities that are needed on tx's to be included in
 * a certain number of blocks.  Everytime a block is added to the best chain, this class records
 * stats on the transactions included in that block
 */
class CBlockPolicyEstimator
{
public:
    /** Create new BlockPolicyEstimator and initialize stats tracking classes with default values */
    CBlockPolicyEstimator();

    /** Process all the transactions that have been included in a block */
    void processBlock(unsigned int nBlockHeight, std::vector<CTxMemPoolEntry>& entries);

    /** Process a transaction */
    void processTransaction(unsigned int nBlockHeight, const CTxMemPoolEntry& entry);

    /** Return a fee estimate */
    CFeeRate estimateFee(int confTarget);

    /** Return a priority estimate */
    double estimatePriority(int confTarget);

    /** Write estimation data to a file */
    void Write(CAutoFile& fileout);

    /** Read estimation data from a file */
    void Read(CAutoFile& filein, const CFeeRate& minRelayFee);

private:
    unsigned int nBestSeenHeight;

    /** Classes to track historical data on transaction confirmations */
    TxConfirmStats feeStats, priStats;

    /** Have to categorize tx's in a block as being included because of fee or priority */
    enum FeePriVal { lowVal = 0,
                     highVal = 1,
                     zeroVal = 2 };
};
#endif /*BITCOIN_POLICYESTIMATOR_H */
