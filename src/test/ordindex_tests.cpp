// Copyright (c) 2024 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>
#include <index/ordindex.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <memory>
#include <vector>
#include <string>

BOOST_AUTO_TEST_SUITE(ordindex_tests)

// Helper to create a CTransactionRef with custom witness stack
static CTransactionRef MakeTxWithWitness(const std::vector<std::vector<unsigned char>>& witness_stack) {
    CMutableTransaction mtx;
    mtx.vin.resize(1);
    mtx.vout.resize(1);
    mtx.vin[0].scriptWitness.stack = witness_stack;
    return MakeTransactionRef(mtx);
}

BOOST_AUTO_TEST_CASE(OutputContainsInscription_basic)
{
    // Case 1: No inputs
    CMutableTransaction mtx1;
    CTransactionRef tx1 = MakeTransactionRef(mtx1);
    BOOST_CHECK(!OutputContainsInscription(tx1, 0));

    // Case 2: Input, but empty witness
    CMutableTransaction mtx2;
    mtx2.vin.resize(1);
    CTransactionRef tx2 = MakeTransactionRef(mtx2);
    BOOST_CHECK(!OutputContainsInscription(tx2, 0));

    // Case 3: Witness stack, but not enough bytes
    std::vector<unsigned char> short_item = {0x00, 0x63, 0x03};
    CTransactionRef tx3 = MakeTxWithWitness({short_item});
    BOOST_CHECK(!OutputContainsInscription(tx3, 0));

    // Case 4: Proper inscription pattern (OP_FALSE, OP_IF, length, 'o','r','d',...)
    std::vector<unsigned char> inscription_item = {
        OP_FALSE, OP_IF, 0x05, 'o', 'r', 'd', 'x', 'y' // length=5, starts with 'ord'
    };
    CTransactionRef tx4 = MakeTxWithWitness({inscription_item});
    BOOST_CHECK(OutputContainsInscription(tx4, 0));

    // Case 5: Proper prefix but wrong data
    std::vector<unsigned char> wrong_item = {
        OP_FALSE, OP_IF, 0x05, 'x', 'y', 'z', 'a', 'b'
    };
    CTransactionRef tx5 = MakeTxWithWitness({wrong_item});
    BOOST_CHECK(!OutputContainsInscription(tx5, 0));
}

BOOST_AUTO_TEST_CASE(SkimRanges_basic)
{
    // Pool: [0, 10), [10, 20), [20, 30)
    std::vector<SatoshiRange> pool = {
        {0, 10}, {10, 20}, {20, 30}
    };
    // Allocate 15 sats
    auto [allocated, remaining] = SkimRanges(pool, 15);
    BOOST_CHECK_EQUAL(allocated.size(), 2); // [0,10), [10,15)
    BOOST_CHECK_EQUAL(allocated[0].start, 0);
    BOOST_CHECK_EQUAL(allocated[0].end, 10);
    BOOST_CHECK_EQUAL(allocated[1].start, 10);
    BOOST_CHECK_EQUAL(allocated[1].end, 15);
    BOOST_CHECK_EQUAL(remaining.size(), 2); // [15,20), [20,30)
    BOOST_CHECK_EQUAL(remaining[0].start, 15);
    BOOST_CHECK_EQUAL(remaining[0].end, 20);
    BOOST_CHECK_EQUAL(remaining[1].start, 20);
    BOOST_CHECK_EQUAL(remaining[1].end, 30);

    // Allocate more than available
    std::vector<SatoshiRange> pool2 = {{0, 5}};
    auto [alloc2, rem2] = SkimRanges(pool2, 10);
    BOOST_CHECK_EQUAL(alloc2.size(), 1);
    BOOST_CHECK_EQUAL(alloc2[0].start, 0);
    BOOST_CHECK_EQUAL(alloc2[0].end, 5);
    BOOST_CHECK(rem2.empty());

    // Allocate zero
    std::vector<SatoshiRange> pool3 = {{0, 5}};
    auto [alloc3, rem3] = SkimRanges(pool3, 0);
    BOOST_CHECK(alloc3.empty());
    BOOST_CHECK_EQUAL(rem3.size(), 1);
    BOOST_CHECK_EQUAL(rem3[0].start, 0);
    BOOST_CHECK_EQUAL(rem3[0].end, 5);
}

BOOST_AUTO_TEST_SUITE_END() 