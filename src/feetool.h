#include "amount.h"
#include "uint256.h"

#include <map>
#include <string>
#include <vector>

class CAutoFile;

class TxConfirmStat
{
private:
    //Define the buckets we will group transactions into (both fee buckets and priority buckets)
    std::vector<double> buckets;              // The upper-bound of the range for the bucket (inclusive)
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
    // and calculate the total for the current block to update the moving average
    std::vector<double> curBlockVal;

    // Combine the conf counts with tx counts to calculate the confirmation % for each Y,X
    // Combine the total value with the tx counts to calculate the avg fee/priority per bucket

    std::string dataTypeString;
    double decay;

    // Mempool counts of outstanding transactions
    // For each bucket X, track the number of transactions in the mempool
    // that are unconfirmed for each possible confirmation value Y
    std::vector<std::vector<int> > unconfTxs;  //unconfTxs[Y][X]
    // transactions still unconfirmed after MAX_CONFIRMS for each bucket
    std::vector<int> oldUnconfTxs;

public:
    /**
     * Read saved state of estimation data from a file and replace all internal data structures and
     * variables with this state.
     */
    void Read(CAutoFile& filein, int version);
    void DebugPrint(unsigned int height);
};
