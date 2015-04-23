#!/usr/bin/env python2
# Copyright (c) 2014 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

#
# Test fee estimation code
#

from decimal import ROUND_UP
from test_framework import BitcoinTestFramework
from bitcoinrpc.authproxy import AuthServiceProxy, JSONRPCException
from util import *

def small_txpuzzle_randfee(from_node, conflist, unconflist, amount, min_fee, fee_increment):
    fee = min_fee - fee_increment + Decimal(float(fee_increment)*(1.1892**random.randint(0,28))).quantize(Decimal('0.00000001'), rounding=ROUND_DOWN) #logarithmically distributed from 0-127 * fee_increment
    inputs = []
    total_in = Decimal("0.00000000")
    while total_in <= (amount + fee) and len(conflist) > 0:
        t = conflist.pop(0)
        total_in += t["amt"]
        inputs.append({ "txid" : t["txid"], "vout" : t["vout"]} )
    if total_in <= amount + fee:
        while total_in <= (amount + fee) and len(unconflist) > 0:
            t = unconflist.pop(0)
            total_in += t["amt"]
            inputs.append({ "txid" : t["txid"], "vout" : t["vout"]} )
        if total_in <= amount + fee:
            print(amount+fee,total_in)
            raise RuntimeError("Insufficient funds: need %d, have %d"%(amount+fee, total_in))
    outputs = {}
    outputs["2MySexEGVzZpRgNQ1JdjdP5bRETznm3roQ2"]=total_in - amount - fee
    outputs["2NBdpwq8Aoo1EEKEXPNrKvr5xQr3M9UfcZA"]=amount
    rawtx = from_node.createrawtransaction(inputs, outputs)
    completetx= rawtx[0:10]
    inputnum = 0
    for inp in inputs:
        completetx += rawtx[10+82*inputnum:82+82*inputnum]
        if (inp["vout"] == 0):
            script = "5175"
        else:
            script = "5275"

        completetx += "045102"
        completetx += script
        completetx += rawtx[84+82*inputnum:92+82*inputnum]
        inputnum += 1
    completetx += rawtx[10+82*inputnum:]
    txid = from_node.sendrawtransaction(completetx, True)
    unconflist.append({ "txid" : txid, "vout" : 0 , "amt" : total_in - amount - fee})
    unconflist.append({ "txid" : txid, "vout" : 1 , "amt" : amount})

    return (completetx, fee)

def initial_split(from_node, txouts):
    utxo = from_node.listunspent(0)
    inputs=[]
    outputs = {}
    t = utxo.pop()
    inputs.append({ "txid" : t["txid"], "vout" : t["vout"]})
    halfchange =  Decimal(t["amount"]/2).quantize(Decimal('0.00000001'), rounding=ROUND_DOWN)
    remchange = t["amount"] - halfchange - Decimal("0.00001000")
    outputs["2MySexEGVzZpRgNQ1JdjdP5bRETznm3roQ2"]=halfchange #P2SH of "OP_1 OP_DROP"
    outputs["2NBdpwq8Aoo1EEKEXPNrKvr5xQr3M9UfcZA"]=remchange #P2SH of "OP_2 OP_DROP"
    rawtx = from_node.createrawtransaction(inputs, outputs)
    signresult = from_node.signrawtransaction(rawtx)
    txid = from_node.sendrawtransaction(signresult["hex"], True)
    txouts.append({ "txid" : txid, "vout" : 0 , "amt" : halfchange})
    txouts.append({ "txid" : txid, "vout" : 1 , "amt" : remchange})

def resplit(from_node, txins, txouts):
    prevtxout = txins.pop()
    inputs=[]
    outputs={}
    inputs.append({ "txid" : prevtxout["txid"], "vout" : prevtxout["vout"]})
    halfchange =  Decimal(prevtxout["amt"]/2).quantize(Decimal('0.00000001'), rounding=ROUND_DOWN)
    remchange = prevtxout["amt"] - halfchange  - Decimal("0.00001000")
    outputs["2MySexEGVzZpRgNQ1JdjdP5bRETznm3roQ2"]=halfchange #P2SH of "OP_1 OP_DROP"
    outputs["2NBdpwq8Aoo1EEKEXPNrKvr5xQr3M9UfcZA"]=remchange #P2SH of "OP_2 OP_DROP"
    rawtx = from_node.createrawtransaction(inputs, outputs)
    if (prevtxout["vout"] == 0):
        script = "5175" #OP_1 OP_DROP
    else:
        script = "5275" #OP_2 OP_DRP
    completetx= rawtx[0:82] + "04" + "51" + "02" + script + rawtx[84:] #scriptsig is OP_TRUE followed by pushing on the redeem script
    txid = from_node.sendrawtransaction(completetx, True)
    txouts.append({ "txid" : txid, "vout" : 0 , "amt" : halfchange})
    txouts.append({ "txid" : txid, "vout" : 1 , "amt" : remchange})

def check_estimates(node, fees_seen, max_invalid, printestimates = True):
    all_estimates = [ node.estimatefee(i) for i in range(1,26) ]
    if printestimates:
        print(str([str(all_estimates[e-1]) for e in [1,2,3,6,15,25]]))
    delta = 1.0e-6 # account for rounding error
    last_e = max(fees_seen)
    for e in filter(lambda x: x >= 0, all_estimates):
        # Estimates should be within the bounds of what transactions fees actually were:
        if float(e)+delta < min(fees_seen) or float(e)-delta > max(fees_seen):
            raise AssertionError("Estimated fee (%f) out of range (%f,%f)"%(float(e), min(fees_seen), max(fees_seen)))
        # Estimates should be monotonically decreasing
        if float(e)-delta > last_e:
            raise AssertionError("Estimated fee (%f) larger than last fee (%f) for lower number of confirms"%(float(e),float(last_e)))
        last_e = e
    valid_estimate = False
    invalid_estimates = 0
    for e in all_estimates:
        if e >= 0:
            valid_estimate = True
        else:
            invalid_estimates += 1
        # Once we're at a high enough confirmation count that we can give an estimate
        # We should have estimates for all higher confirmation counts
        if valid_estimate and e < 0:
            raise AssertionError("Invalid estimate appears at higher confirm count than valid estimate")
    #Check on the expected number of different confirmation counts that we might not have valid estimates for
    if invalid_estimates > max_invalid:
        raise AssertionError("More than (%d) invalid estimates"%(max_invalid))
    return all_estimates


class EstimateFeeTest(BitcoinTestFramework):

    def setup_network(self):
        self.nodes = []
        #Use node0 to mine blocks for input splitting
        self.nodes.append(start_node(0, self.options.tmpdir, ["-maxorphantx=1000", "-relaypriority=0", "-whitelist=127.0.0.1"]))

        print("This test is time consuming, please be patient")
        print("Splitting inputs to small size so we can generate low priority tx's")
        self.txouts = []
        self.txouts2 = []
        #Split a coinbase into two tranaction puzzle outputs
        initial_split(self.nodes[0],self.txouts)

        #Mine
        while (len(self.nodes[0].getrawmempool()) > 0):
            self.nodes[0].generate(1)

        #Repeatedly split those 2 outputs, doubling twice for each rep
        #Use txouts to monitor the available utxo, since these won't be tracked in wallet
        reps = 0
        while (reps < 5):
            #Double txouts to txouts2
            while (len(self.txouts)>0):
                resplit(self.nodes[0],self.txouts,self.txouts2)
            while (len(self.nodes[0].getrawmempool()) > 0):
                self.nodes[0].generate(1)
            #Double txouts2 to txouts
            while (len(self.txouts2)>0):
                resplit(self.nodes[0],self.txouts2,self.txouts)
            while (len(self.nodes[0].getrawmempool()) > 0):
                self.nodes[0].generate(1)
            reps += 1
        print("Finished splitting")

        # Now we can connect the other nodes, didn't want to connect them earlier
        # So the estimates would not be affected by the splitting transactions
        # Node1 mines small blocks but that are bigger than the expected transaction rate, and allows free transactions.
        # NOTE: the CreateNewBlock code starts counting block size at 1,000 bytes,
        # (17k is room enough for 110 or so transactions)
        self.nodes.append(start_node(1, self.options.tmpdir,
                                     ["-blockprioritysize=1500", "-blockmaxsize=18000", "-maxorphantx=1000", "-relaypriority=0", "-debug=estimatefee"]))
        connect_nodes(self.nodes[1], 0)

        # Node2 is a stingy miner, that
        # produces too small blocks (room for only 70 or so transactions)
        node2args = ["-blockprioritysize=0", "-blockmaxsize=12000", "-maxorphantx=1000", "-relaypriority=0"]

        self.nodes.append(start_node(2, self.options.tmpdir, node2args))
        connect_nodes(self.nodes[0], 2)
        connect_nodes(self.nodes[2], 1)

        self.is_network_split = False
        self.sync_all()

    def transact_and_mine(self, numblocks, mining_node):
        min_fee = Decimal("0.00001")
        #We will now mine numblocks blocks generating on average 100 transactions between each block
        #We shuffle our confirmed txout set before each set of transactions
        #small_txpuzzle_randfee will use the transactions that have inputs already in the chain when possible
        #resorting to tx's that depend on the mempool when those run out
        for i in range(numblocks):
            random.shuffle(self.confutxo)
            for j in range(random.randrange(100-50,100+50)):
                from_index = random.randint(1,2)
                (txhex, fee) = small_txpuzzle_randfee(self.nodes[from_index], self.confutxo, self.memutxo, Decimal("0.005"), min_fee, min_fee)
                tx_kbytes = (len(txhex)/2)/1000.0
                self.fees_per_kb.append(float(fee)/tx_kbytes)
            sync_mempools(self.nodes[0:3],.1)
            mined = mining_node.getblock(mining_node.generate(1)[0],True)["tx"]
            sync_blocks(self.nodes[0:3],.1)
            #update which txouts are confirmed
            newmem = []
            for utx in self.memutxo:
                if utx["txid"] in mined:
                    self.confutxo.append(utx)
                else:
                    newmem.append(utx)
            self.memutxo = newmem

    def run_test(self):
        self.fees_per_kb = []
        self.memutxo = []
        self.confutxo=self.txouts #Start with the set of confirmed txouts after splitting
        print("Checking estimates after 1/2/3/6/15/25")
        print("Creating transactions and mining them with a huge block size")
        #Create transactions and mine 20 big blocks with node 0 such that the mempool is always emptied
        self.transact_and_mine(30, self.nodes[0])
        check_estimates(self.nodes[1],self.fees_per_kb, 1)

        print("Creating transactions and mining them with a block size that can't keep up")
        #Create transactions and mine 30 small blocks with node 2, but create txs faster than we can mine
        self.transact_and_mine(20, self.nodes[2])
        check_estimates(self.nodes[1],self.fees_per_kb, 3)

        print("Creating transactions and mining them at a block size that is just big enough")
        # Generate transactions while mining 40 more blocks, this time with node1
        # which mines blocks with capacity just above the rate that transactions are being created
        self.transact_and_mine(40, self.nodes[1])
        check_estimates(self.nodes[1],self.fees_per_kb, 2)

        # Finish by mining a normal-sized block:
        while len(self.nodes[1].getrawmempool()) > 0:
            self.nodes[1].generate(1)

        sync_blocks(self.nodes[0:3],.1)
        print("Final estimates after emptying mempools")
        check_estimates(self.nodes[1],self.fees_per_kb, 2)

if __name__ == '__main__':
    EstimateFeeTest().main()
