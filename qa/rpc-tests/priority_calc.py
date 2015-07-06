#!/usr/bin/env python2
# Copyright (c) 2014 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

#
# Test -reindex with CheckBlockIndex
#
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import *
import os.path

class PriorityTest(BitcoinTestFramework):

    def setup_network(self):
        self.nodes = []
        self.is_network_split = False
        self.nodes.append(start_node(0, self.options.tmpdir, ["-relaypriority=0","-debug"]))
        self.nodes.append(start_node(1, self.options.tmpdir))

    def run_test(self):
        self.nodes[1].generate(10)
        inputs = []
        coinbase_txid = self.nodes[0].getblock(self.nodes[0].getblockhash(1))['tx'][0]
        inputs.append({ "txid" : coinbase_txid, "vout" : 0} )
        outputs = {}
        outputs[self.nodes[0].getnewaddress()] = Decimal("49.999")
        rawtx = self.nodes[0].createrawtransaction(inputs, outputs)
        signtx = self.nodes[0].signrawtransaction(rawtx)
        txid = self.nodes[0].sendrawtransaction(signtx["hex"])
        inputs = []
        inputs.append({ "txid" : txid, "vout" : 0} )
        outputs = {}
        outputs[self.nodes[0].getnewaddress()] = Decimal("49.999")
        rawtx2 = self.nodes[0].createrawtransaction(inputs, outputs)
        signtx2 = self.nodes[0].signrawtransaction(rawtx2)
        txid2 = self.nodes[0].sendrawtransaction(signtx2["hex"])
        connect_nodes(self.nodes[1], 0)
        sync_blocks(self.nodes[0:2])
        if (self.nodes[0].getrawmempool(True)[txid2]["currentpriority"] != 0):
            raise AssertionError("Priority is incorrectly aging tx inputs that are still in the mempool")

if __name__ == '__main__':
    PriorityTest().main()
