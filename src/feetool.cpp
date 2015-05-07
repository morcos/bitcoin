#include "core_io.h"
#include "feetool.h"
#include "streams.h"
#include "util.h"
#include "policy/fees.h"

#include <stdio.h>

void TxConfirmStat::Read(CAutoFile& filein, int version)
{
    // Read data file into temporary variables and do some very basic sanity checking
    std::vector<double> fileBuckets;
    std::vector<double> fileAvg;
    std::vector<std::vector<double> > fileConfAvg;
    std::vector<double> fileTxCtAvg;
    double fileDecay;
    size_t maxConfirms;
    size_t numBuckets;

    filein >> fileDecay;
    if (fileDecay <= 0 || fileDecay >= 1)
        throw std::runtime_error("Corrupt estimates file. Decay must be between 0 and 1 (non-inclusive)");
    if (version < 100000)
        filein >> maxConfirms;
    filein >> fileBuckets;
    numBuckets = fileBuckets.size();
    if (numBuckets <= 1 || numBuckets > 1000)
        throw std::runtime_error("Corrupt estimates file. Must have between 2 and 1000 fee/pri buckets");
    filein >> fileAvg;
    if (fileAvg.size() != numBuckets)
        throw std::runtime_error("Corrupt estimates file. Mismatch in fee/pri average bucket count");
    filein >> fileTxCtAvg;
    if (fileTxCtAvg.size() != numBuckets)
        throw std::runtime_error("Corrupt estimates file. Mismatch in tx count bucket count");
    
    if (version < 100000) {
        for (unsigned int i = 0; i < maxConfirms; i++) {
            std::vector<double> fileConf;
            filein >> fileConf;
            fileConfAvg.push_back(fileConf);
        }
        assert(maxConfirms == fileConfAvg.size());
    }
    else {
        filein >> fileConfAvg;
    }
    maxConfirms = fileConfAvg.size();
    if (maxConfirms <= 0 || maxConfirms > 6 * 24 * 7) // one week
        throw std::runtime_error("Corrupt estimates file.  Must maintain estimates for between 1 and 1008 (one week) confirms");
    for (unsigned int i = 0; i < maxConfirms; i++) {
        if (fileConfAvg[i].size() != numBuckets)
            throw std::runtime_error("Corrupt estimates file. Mismatch in fee/pri conf average bucket count");
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

    unconfTxs.resize(maxConfirms);
    for (unsigned int i = 0; i < maxConfirms; i++) {
        unconfTxs[i].resize(buckets.size());
    }
    oldUnconfTxs.resize(buckets.size());

    for (unsigned int i = 0; i < buckets.size(); i++)
        bucketMap[buckets[i]] = i;

    fprintf(stdout,"Reading estimates: %u %s buckets counting confirms up to %u blocks\n",
            numBuckets, dataTypeString.c_str(), maxConfirms);
}
void TxConfirmStat::DebugPrint(unsigned int nBlockHeight)
{
    if (confAvg.size() >= 16) {
        for (unsigned int j = 0; j < buckets.size(); j++) {
            fprintf(stdout,"%s Bucket %12.5g: %12.2f txs, %6.2f%%:1, %6.2f%%:2, %6.2f%%:3, %6.2f%%:4, %6.2f%%:8 %6.2f%%:16 avg %12.5g\n",
                    dataTypeString.c_str(), buckets[j], txCtAvg[j]*(1-decay),
                     100*confAvg[1-1][j]/txCtAvg[j], 100*confAvg[2-1][j]/txCtAvg[j],
                     100*confAvg[3-1][j]/txCtAvg[j], 100*confAvg[4-1][j]/txCtAvg[j],
                     100*confAvg[8-1][j]/txCtAvg[j], 100*confAvg[16-1][j]/txCtAvg[j],
                     avg[j]/txCtAvg[j]);
        }
    }
    // unsigned int bins = unconfTxs.size();
    // if (bins >= 2) {
    //     LogPrint("estimatefee","Mempool transaction tracking\n");
    //     LogPrint("estimatefee","%s \t0\t1\t2-max\tmax+\n",dataTypeString);
    //     for (unsigned int i=0;i<unconfTxs[0].size();i++) {
    //         unsigned int zeroct = unconfTxs[nBlockHeight%bins][i];
    //         unsigned int onect = unconfTxs[(nBlockHeight-1)%bins][i];
    //         unsigned int midct = 0;
    //         for (unsigned int j=2;j<unconfTxs.size();j++)
    //             midct += unconfTxs[(nBlockHeight-j)%bins][i];
    //         unsigned int oldct = oldUnconfTxs[i];
    //         LogPrint("estimatefee","%7.3e\t%5.1g\t%5.1g\t%5.1g\t%5.1g\n",
    //                  buckets[i],zeroct,onect,midct,oldct);
    //     }
    // }
 }
 
int main(int argc, char* argv[])
{
    CAutoFile feeEstFile(fopen(argv[1], "rb"), SER_DISK, 110000);
    try {
        int fileHeight;
        TxConfirmStat feeStats, priStats;
        int nVersionRequired, nVersionThatWrote;
        feeEstFile >> nVersionRequired >> nVersionThatWrote;
        feeEstFile >> fileHeight;
        fprintf(stdout, "Height %d  Version %d\n",fileHeight, nVersionRequired);
        feeStats.Read(feeEstFile, nVersionRequired);
        priStats.Read(feeEstFile, nVersionRequired);
        priStats.DebugPrint(fileHeight);
        feeStats.DebugPrint(fileHeight);
    }
    catch (std::exception &e) {
        fprintf(stderr,"FeeTool:: Error processing file\n");
    }
    
}
