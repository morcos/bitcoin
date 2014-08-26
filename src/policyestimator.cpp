// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2013 The Bitcoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "policyestimator.h"

#include "main.h"
#include "txmempool.h"
#include "util.h"


void TxConfirmStats::Initialize(std::vector<double> defaultBuckets, unsigned int maxConfirms, double _decay, std::string _dataTypeString)
{
    decay = _decay;
    dataTypeString = _dataTypeString;
    for (unsigned int i = 0; i < defaultBuckets.size(); i++) {
        buckets.push_back(defaultBuckets[i]);
        bucketMap[defaultBuckets[i]] = i;
    }
    confAvg.resize(maxConfirms);
    curBlockConf.resize(maxConfirms);
    for (unsigned int i = 0; i < maxConfirms; i++) {
        confAvg[i].resize(buckets.size());
        curBlockConf[i].resize(buckets.size());
    }

    curBlockTxCt.resize(buckets.size());
    txCtAvg.resize(buckets.size());
    curBlockVal.resize(buckets.size());
    avg.resize(buckets.size());
}

// Zero out the data for the current block
void TxConfirmStats::ClearCurrent()
{
    for (unsigned int j = 0; j < buckets.size(); j++) {
        for (unsigned int i = 0; i < curBlockConf.size(); i++)
            curBlockConf[i][j] = 0;
        curBlockTxCt[j] = 0;
        curBlockVal[j] = 0;
    }
}


void TxConfirmStats::Record(int blocksToConfirm, double val)
{
    // blocksToConfirm is 1-based
    if (blocksToConfirm < 1)
        return;
    unsigned int bucketIndex = bucketMap.upper_bound(val)->second;
    for (size_t i = blocksToConfirm; i <= curBlockConf.size(); i++) {
        curBlockConf[i - 1][bucketIndex]++;
    }
    curBlockTxCt[bucketIndex]++;
    curBlockVal[bucketIndex] += val;
}

void TxConfirmStats::UpdateMovingAverages()
{
    for (unsigned int j = 0; j < buckets.size(); j++) {
        for (unsigned int i = 0; i < confAvg.size(); i++)
            confAvg[i][j] = confAvg[i][j] * decay + curBlockConf[i][j];
        avg[j] = avg[j] * decay + curBlockVal[j];
        txCtAvg[j] = txCtAvg[j] * decay + curBlockTxCt[j];
    }
}

// returns -1 on error conditions
double TxConfirmStats::EstimateMedianVal(int confTarget, double sufficientTxVal, double minSuccess)
{
    // Counters for a bucket (or range of buckets)
    double nConf = 0;
    double totalNum = 0;

    int maxbucketindex = buckets.size() - 1;

    // We'll combine buckets until we have enough samples.
    // The low and high variables will define the range we've combined
    // The best variables are the last range we saw which still had a high
    // enough confirmation rate to count as success.
    // The cur variables are the current range we're counting.
    unsigned int curHighBucket = maxbucketindex;
    unsigned int bestHighBucket = maxbucketindex;
    unsigned int curLowBucket = maxbucketindex;
    unsigned int bestLowBucket = maxbucketindex;
    bool foundAnswer = false;

    // Start counting from highest fee/pri transactions
    for (int bucket = maxbucketindex; bucket >= 0; bucket--) {
        curLowBucket = bucket;
        nConf += confAvg[confTarget - 1][bucket];
        totalNum += txCtAvg[bucket];

        // If we have a enough transaction data points in the range
        if (totalNum >= sufficientTxVal / (1 - decay)) {
            double curPct = nConf / totalNum;

            // Check to see if we are no longer getting confirmed at the success rate
            if (curPct < minSuccess)
                break;

            // Otherwise update the cumulative stats, and the bucket variables
            // and reset the counters
            else {
                foundAnswer = true;
                nConf = 0;
                totalNum = 0;
                bestHighBucket = curHighBucket;
                bestLowBucket = curLowBucket;
                curHighBucket = bucket - 1;
            }
        }
    }

    double median = -1;
    double txSum = 0;

    // Calculate the "average" fee of the best bucket range that met success conditions
    // Find the bucket with the median transaction and then report the average fee from that bucket
    // This is a compromise between finding the median which we can't since we don't save all tx's
    // and reporting the average which is less accurate
    for (unsigned int j = bestLowBucket; j <= bestHighBucket; j++) {
        txSum += txCtAvg[j];
    }
    if (foundAnswer && txSum != 0) {
        txSum = txSum / 2;
        for (unsigned int j = bestLowBucket; j <= bestHighBucket; j++) {
            if (txCtAvg[j] < txSum)
                txSum -= txCtAvg[j];
            else { // we're in the right bucket
                median = avg[j] / txCtAvg[j];
                break;
            }
        }
    }
    LogPrint("estimatefee", "%3d: %s needed: %12.5g from buckets %8g - %8g  Cur Bucket stats %6.2f%%  %8.1f/%8.1f\n",
             confTarget, dataTypeString, median, buckets[bestLowBucket], buckets[bestHighBucket],
             100 * nConf / totalNum, nConf, totalNum);

    return median;
}

void TxConfirmStats::Write(CAutoFile& fileout)
{
    fileout << decay;
    fileout << confAvg.size();
    fileout << buckets;
    fileout << avg;
    fileout << txCtAvg;
    for (unsigned int i = 0; i < confAvg.size(); i++) {
        fileout << confAvg[i];
    }
}

void TxConfirmStats::Read(CAutoFile& filein)
{
    // Read data file into temporary variables and do some very basic sanity checking
    std::vector<double> fileBuckets;
    std::vector<double> fileAvg;
    std::vector<std::vector<double> > fileConfAvg;
    std::vector<double> fileTxCtAvg;
    double fileDecay;
    size_t maxConfirms;

    filein >> fileDecay;
    if (fileDecay <= 0 || fileDecay >= 1)
        throw std::runtime_error("Corrupt estimates file. Decay must be between 0 and 1 (non-inclusive)");
    filein >> maxConfirms;
    if (maxConfirms <= 0 || maxConfirms > 6 * 24 * 7) // one week
        throw std::runtime_error("Corrupt estimates file.  Must maintain estimates for between 1 and 1008 (one week) confirms");
    filein >> fileBuckets;
    if (fileBuckets.size() <= 1 || fileBuckets.size() > 1000)
        throw std::runtime_error("Corrupt estimates file. Must have between 2 and 1000 fee buckets");
    filein >> fileAvg;
    if (fileAvg.size() != fileBuckets.size())
        throw std::runtime_error("Corrupt estimates file. Mismatch in fee average vector size");
    filein >> fileTxCtAvg;
    if (fileTxCtAvg.size() != fileBuckets.size())
        throw std::runtime_error("Corrupt estimates file. Mismatch in fee tx count vector size");
    for (unsigned int i = 0; i < maxConfirms; i++) {
        std::vector<double> fileConf;
        filein >> fileConf;
        if (fileConf.size() != fileBuckets.size())
            throw std::runtime_error("Corrupt estimates file. Mismatch in fee conf vector size");
        fileConfAvg.push_back(fileConf);
    }

    // Now that we've processed the entire fee estimate data file and not
    // thrown any errors, we can copy it to our data structures
    decay = fileDecay;
    buckets = fileBuckets;
    avg = fileAvg;
    confAvg = fileConfAvg;
    txCtAvg = fileTxCtAvg;
    bucketMap.clear();

    // Resize the current block variables which aren't stored in the data file
    // to match the number of confirms and buckets
    curBlockConf.resize(maxConfirms);
    for (unsigned int i = 0; i < maxConfirms; i++) {
        curBlockConf[i].resize(buckets.size());
    }
    curBlockTxCt.resize(buckets.size());
    curBlockVal.resize(buckets.size());

    for (unsigned int i = 0; i < buckets.size(); i++)
        bucketMap[buckets[i]] = i;
    if (confAvg.size() >= 16) {
        for (unsigned int j = 0; j < buckets.size(); j++) {
            LogPrint("estimatefee","%s Bucket %12.5g: %12.2f txs, %6.2f%%:1, %6.2f%%:2, %6.2f%%:3, %6.2f%%:4, %6.2f%%:8 %6.2f%%:16 avg %12.5g\n",
                     dataTypeString, buckets[j], txCtAvg[j]*(1-decay),
                     100*confAvg[1-1][j]/txCtAvg[j], 100*confAvg[2-1][j]/txCtAvg[j],
                     100*confAvg[3-1][j]/txCtAvg[j], 100*confAvg[4-1][j]/txCtAvg[j],
                     100*confAvg[8-1][j]/txCtAvg[j], 100*confAvg[16-1][j]/txCtAvg[j],
                     avg[j]/txCtAvg[j]);
        }
    }
}

CBlockPolicyEstimator::CBlockPolicyEstimator() : nBestSeenHeight(0)
{
    std::vector<double> vfeelist(FEELIST, FEELIST + sizeof(FEELIST) / sizeof(FEELIST[0]));
    feeStats.Initialize(vfeelist, MAX_BLOCK_CONFIRMS, DEFAULT_DECAY, "FeeRate");
    std::vector<double> vprilist(PRILIST, PRILIST + sizeof(PRILIST) / sizeof(PRILIST[0]));
    priStats.Initialize(vprilist, MAX_BLOCK_CONFIRMS, DEFAULT_DECAY, "Priority");
}

void CBlockPolicyEstimator::processTransaction(unsigned int nBlockHeight, const CTxMemPoolEntry& entry)
{
    if (!entry.WasClearAtEntry()) {
        // This transaction depended on other transactions in the mempool to
        // be included in a block before it was able to be included, so
        // we shouldn't include it in our calculations
        return;
    }

    // How many blocks did it take for miners to include this transaction?
    int blocksToConfirm = nBlockHeight - entry.GetHeight();
    if (blocksToConfirm <= 0) {
        // Re-org made us lose height, this should only happen if we happen
        // to re-org on a difficulty transition point: very rare!
        return;
    }

    // Fees are stored and reported as BTC-per-kb:
    CFeeRate feeRate(entry.GetFee(), entry.GetTxSize());

    // Want the priority of the tx at confirmation.  The priority when it
    // entered the mempool could easily be very small and change quickly
    double curPri = entry.GetPriority(nBlockHeight);

    // Need to characterize how high priority and high fee every transaction is
    // so we can make an estimate about why it was included in the block
    // Eventually we probably want to dynamically calculate these cutoffs
    FeePriVal feeVal, priVal;

    if (entry.GetFee() == 0)
        feeVal = zeroVal;
    else if (feeRate.GetFeePerK() <= minRelayTxFee.GetFeePerK())
        feeVal = lowVal;
    else
        feeVal = highVal;
    if (curPri < MIN_PRIORITY_VAL) //this and the feerate cutoff need to be dynamially calc'ed
        priVal = lowVal;
    else
        priVal = highVal;

    // blocksToConfirm is 1-based, so a transaction included in the earliest
    // possible block has confirmation count of 1

    // Record this as a fee estimate
    if (feeVal == highVal && priVal == lowVal) {
        feeStats.Record(blocksToConfirm, (double)feeRate.GetFeePerK());
    }
    // Record this as a priority estimate
    else if (feeVal == zeroVal || (feeVal == lowVal && priVal == highVal)) {
        priStats.Record(blocksToConfirm, curPri);
    }
}

void CBlockPolicyEstimator::processBlock(unsigned int nBlockHeight, std::vector<CTxMemPoolEntry>& entries)
{
    if (nBlockHeight <= nBestSeenHeight) {
        // Ignore side chains and re-orgs; assuming they are random
        // they don't affect the estimate.
        // And if an attacker can re-org the chain at will, then
        // you've got much bigger problems than "attacker can influence
        // transaction fees."
        return;
    }
    nBestSeenHeight = nBlockHeight;

    // Clear the current block states
    feeStats.ClearCurrent();
    priStats.ClearCurrent();

    // Repopulate the current block states
    BOOST_FOREACH (const CTxMemPoolEntry& entry, entries)
        processTransaction(nBlockHeight, entry);

    // Update all exponential averages with the current block states
    feeStats.UpdateMovingAverages();
    priStats.UpdateMovingAverages();
}


CFeeRate CBlockPolicyEstimator::estimateFee(int confTarget)
{
    // Return failure if trying to analyze a target we're not tracking
    if (confTarget <= 0 || (unsigned int)confTarget > feeStats.GetMaxConfirms())
        return CFeeRate(0);

    double median = feeStats.EstimateMedianVal(confTarget, SUFFICIENT_FEETXS, MIN_SUCCESS_PCT);

    if (median < 0)
        return CFeeRate(0);

    return CFeeRate(median);
}

double CBlockPolicyEstimator::estimatePriority(int confTarget)
{
    // Return failure if trying to analyze a target we're not tracking
    if (confTarget <= 0 || (unsigned int)confTarget > priStats.GetMaxConfirms())
        return -1;

    return priStats.EstimateMedianVal(confTarget, SUFFICIENT_PRITXS, MIN_SUCCESS_PCT);
}

void CBlockPolicyEstimator::Write(CAutoFile& fileout)
{
    fileout << nBestSeenHeight;
    feeStats.Write(fileout);
    priStats.Write(fileout);
}

void CBlockPolicyEstimator::Read(CAutoFile& filein, const CFeeRate& minRelayFee)
{
    int nFileBestSeenHeight;
    filein >> nFileBestSeenHeight;
    feeStats.Read(filein);
    priStats.Read(filein);
    nBestSeenHeight = nFileBestSeenHeight;
}
