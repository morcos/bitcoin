#!/usr/bin/env python2
# Copyright (c) 2014 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

#
# Test tracking of orphan transactions
#

from test_framework import BitcoinTestFramework
from bitcoinrpc.authproxy import AuthServiceProxy, JSONRPCException
from util import *

# Create one-input, one-output, no-fee transaction:
class MineTest(BitcoinTestFramework):

    def setup_chain(self):
        print("Initializing test directory "+self.options.tmpdir)
        initialize_chain_clean(self.options.tmpdir, 3)
                 
    def setup_network(self):
        args = ["-checkmempool", "-debug", "-relaypriority=0", "-checkmempool=0", "-checkblockindex=0"]
        self.nodes = []
        self.nodes.append(start_node(0, self.options.tmpdir, args, timewait=300))
        self.is_network_split = False
        self.nodes[0].setgenerate(True, 400)

    def create_tx(self, inputs, amount, fee):
        node0_address = self.nodes[0].getnewaddress()
        outputs = { node0_address : amount - fee}
        rawtx = self.nodes[0].createrawtransaction(inputs, outputs)
        signresult = self.nodes[0].signrawtransaction(rawtx)
        assert_equal(signresult["complete"], True)
        return signresult["hex"]

    def run_test(self):
        utxo = self.nodes[0].listunspent()
        assert(len(utxo) == 300)
        for i in xrange(300):  #generate 100 chains of length 1 to 100
            t = utxo[i]
            amount = t["amount"]
            txid = t["txid"]
            for j in xrange(i+1):
                inputs = [{ "txid" : txid, "vout" : 0}]
                fee = random.randint(1,100)*Decimal("0.00001")
                next_tx = self.create_tx(inputs, amount, fee)
                txid = self.nodes[0].sendrawtransaction(next_tx)                
                amount -= fee

        while len(self.nodes[0].getrawmempool()) > 0:
            self.nodes[0].setgenerate(True, 1)
        


if __name__ == '__main__':
    MineTest().main()
