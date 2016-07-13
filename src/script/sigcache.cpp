// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2015 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "sigcache.h"

#include "memusage.h"
#include "pubkey.h"
#include "random.h"
#include "uint256.h"
#include "util.h"

#include <boost/thread.hpp>
#include <boost/unordered_set.hpp>
#include <boost/lockfree/queue.hpp>

namespace {

/**
 * We're hashing a nonce into the entries themselves, so we don't need extra
 * blinding in the set hash computation.
 */
class CSignatureCacheHasher
{
public:
    size_t operator()(const uint256& key) const {
        return key.GetCheapHash();
    }
};

/**
 * Valid signature cache, to avoid doing expensive ECDSA signature checking
 * twice for every transaction (once when accepted into memory pool, and
 * again when accepted into the block chain)
 */
class CSignatureCache
{
private:
     //! Entries are SHA256(nonce || signature hash || public key || signature):
    uint256 nonce;
    typedef boost::unordered_set<uint256, CSignatureCacheHasher> map_type;
    map_type setValid;
    boost::shared_mutex cs_sigcache;
    boost::lockfree::queue<uint256> deleteQueue;

public:
    CSignatureCache() : deleteQueue(10000)
    {
        GetRandBytes(nonce.begin(), 32);
    }

    void
    ComputeEntry(uint256& entry, const uint256 &hash, const std::vector<unsigned char>& vchSig, const CPubKey& pubkey)
    {
        CSHA256().Write(nonce.begin(), 32).Write(hash.begin(), 32).Write(&pubkey[0], pubkey.size()).Write(&vchSig[0], vchSig.size()).Finalize(entry.begin());
    }

    bool
    Get(const uint256& entry, bool erase)
    {
        boost::shared_lock<boost::shared_mutex> lock(cs_sigcache);
        if (!setValid.count(entry))
            return false;

        if (erase)
            deleteQueue.push(entry);

        return true;
    }

    void Set(const uint256& entry)
    {
        size_t nMaxCacheSize = GetArg("-maxsigcachesize", DEFAULT_MAX_SIG_CACHE_SIZE) * ((size_t) 1 << 20);
        if (nMaxCacheSize <= 0) return;

        boost::unique_lock<boost::shared_mutex> lock(cs_sigcache);

        // Delete previous entries
        uint256 oldEntry;
        while (deleteQueue.unsynchronized_pop(oldEntry)) {
            setValid.erase(oldEntry);
        }

        while (memusage::DynamicUsage(setValid) > nMaxCacheSize)
        {
            map_type::size_type s = GetRand(setValid.bucket_count());
            map_type::local_iterator it = setValid.begin(s);
            if (it != setValid.end(s)) {
                setValid.erase(*it);
            }
        }

        setValid.insert(entry);
    }
};

}

bool CachingTransactionSignatureChecker::VerifySignature(const std::vector<unsigned char>& vchSig, const CPubKey& pubkey, const uint256& sighash) const
{
    static CSignatureCache signatureCache;

    uint256 entry;
    signatureCache.ComputeEntry(entry, sighash, vchSig, pubkey);

    if (signatureCache.Get(entry, !store)) {
        return true;
    }

    if (!TransactionSignatureChecker::VerifySignature(vchSig, pubkey, sighash))
        return false;

    if (store) {
        signatureCache.Set(entry);
    }
    return true;
}
