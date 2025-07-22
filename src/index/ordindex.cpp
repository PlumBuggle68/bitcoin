// Copyright (c) 2017-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <index/ordindex.h>

#include <clientversion.h>
#include <common/args.h>
#include <index/disktxpos.h>
#include <logging.h>
#include <node/blockstorage.h>
#include <validation.h>
#include <chainparams.h>
#include <chain.h>
#include <consensus/params.h>    // for Params().GetConsensus()
#include <interfaces/chain.h>     // for interfaces::BlockInfo



std::unique_ptr<OrdIndex> g_ordindex;
std::unique_ptr<bool> g_ordindex_prune;

/** Access to the ordindex database (indexes/ordindex/) */
class OrdIndex::DB : public BaseIndex::DB
{
public:
    explicit DB(size_t n_cache_size, bool f_memory = false, bool f_wipe = false);

    /// Read ordinal ranges associated with a specific transaction output.
    bool ReadOrdinalRanges(const uint256& txid, uint32_t vout, TxOutputSatoshiEntry& entry) const;

    /// Write ordinal ranges for a specific transaction output.
    bool WriteOrdinalRanges(const uint256& txid, uint32_t vout, const TxOutputSatoshiEntry& entry);

    /// Erase ordinal ranges for a specific transaction output.
    bool EraseOrdinalRanges(const uint256& txid, uint32_t vout);

};

OrdIndex::DB::DB(size_t n_cache_size, bool f_memory, bool f_wipe) :
    BaseIndex::DB(gArgs.GetDataDirNet() / "indexes" / "ordindex", n_cache_size, f_memory, f_wipe)
{}


bool OrdIndex::DB::ReadOrdinalRanges(const uint256& txid, uint32_t vout, TxOutputSatoshiEntry& entry) const
{
    return Read(std::make_pair(DB_ORDINDEX, std::make_pair(txid, vout)), entry);
}

bool OrdIndex::DB::WriteOrdinalRanges(const uint256& txid, uint32_t vout, const TxOutputSatoshiEntry& entry)
{
    CDBBatch batch(*this);
    batch.Write(std::make_pair(DB_ORDINDEX, std::make_pair(txid, vout)), entry);
    return WriteBatch(batch);
}

bool OrdIndex::DB::EraseOrdinalRanges(const uint256& txid, uint32_t vout)
{
    CDBBatch batch(*this);
    batch.Erase(std::make_pair(DB_ORDINDEX, std::make_pair(txid, vout)));
    return WriteBatch(batch);
}


OrdIndex::OrdIndex(std::unique_ptr<interfaces::Chain> chain, size_t n_cache_size, bool f_memory, bool f_wipe, bool f_prune)
    : BaseIndex(std::move(chain), "ordindex"),
    m_db(std::make_unique<OrdIndex::DB>(n_cache_size, f_memory, f_wipe))
{}

OrdIndex::~OrdIndex() = default;

// vector to hold transaction outputs that will be pruned in the future
std::vector<OrdDBPtr> TxOutToPrune;

//CustomAppend is a function that processes a block's transactions, excluding the genesis block, by
//calculating their serialized positions and storing them in a database. 
//It ensures data integrity through assertions and uses the BaseIndex::DB interface for 
//writing transaction positions.


//used to append a block's transactions to the ordinals index.
bool OrdIndex::CustomAppend(const interfaces::BlockInfo& block)
{
    // Prune spent outputs older than 6 blocks
    for (auto it = TxOutToPrune.begin(); it != TxOutToPrune.end(); ) {
        TxOutputSatoshiEntry entry;
        if (m_db->ReadOrdinalRanges(it->hash, it->vout, entry)) {
            if (block.height - entry.block_height >= 6) {
                m_db->EraseOrdinalRanges(it->hash, it->vout);
                it = TxOutToPrune.erase(it);
                continue;
            }
        }
        ++it;
    }

    std::vector<std::tuple<uint256, uint32_t, std::vector<SatoshiRange>>> tx_map;


    if (block.data == nullptr) {
        return false;
    }

    std::cout << "Indexing block height: " << block.height << std::endl;

    uint64_t block_mint_start = m_last_ordinal;
    std::vector<SatoshiRange> fee_pool;

    // Step 1: Ensure the block has transactions

    assert(block.data);

    for (size_t tx_index = 1; tx_index < block.data->vtx.size(); ++tx_index) {
        const auto& tx = block.data->vtx[tx_index];

        // Step 2a: Pool all inputs
        std::vector<SatoshiRange> pool;



        //for every input in the transaction,
        for (const auto& txin : tx->vin) {
            TxOutputSatoshiEntry prev_entry;
            TxOutputSatoshiEntry prev_prev_entry;

            std::vector<SatoshiRange> prev_ranges;

            if (m_db->ReadOrdinalRanges(txin.prevout.hash, txin.prevout.n, prev_entry)) {
                prev_ranges = prev_entry.ranges;

                //take the ranges from the previous transaction output
                for (const auto& r : prev_ranges) {
                    // and add them to the pool
                    pool.push_back(r);
                }

                if(g_ordindex_prune) {
                    if(block.height-prev_entry.block_height >= 6) {
                        if(m_db->EraseOrdinalRanges(txin.prevout.hash, txin.prevout.n))
                            std::cout << "Erased ordinal ranges for " << txin.prevout.hash.ToString() << " vout " << txin.prevout.n << std::endl;
                    } else {
                        OrdDBPtr spentTX = OrdDBPtr(txin.prevout.hash, txin.prevout.n, block.height);
                        TxOutToPrune.emplace_back(spentTX);
                    }
                }
            }
        }

        // Step 2b: Assign satoshi ranges to each output
        for (size_t vout_index = 0; vout_index < tx->vout.size(); ++vout_index) { // for every output in the transaction,
            uint64_t sats = tx->vout[vout_index].nValue;                          // take the amount of bitcoin (in sats) off of the pooled outputs 
            std::pair<std::vector<SatoshiRange>, std::vector<SatoshiRange>> assigned = SkimRanges(pool, sats);
            pool = assigned.second; // update the pool with the remaining ranges after assignment
            TxOutputSatoshiEntry entry{assigned.first, static_cast<int>(block.height)};
            m_db->WriteOrdinalRanges(tx->GetHash(), vout_index, entry);        // then write the data.
        }

        // Step 2c: Calculate fee 
        std::vector<SatoshiRange> fee_taken = pool;
        fee_pool.insert(fee_pool.end(), fee_taken.begin(), fee_taken.end());
    }


    const auto& coinbase_tx = block.data->vtx[0];

    uint64_t total_fees = 0;
    for (const auto& r : fee_pool) {
        total_fees += r.Size();
    }
    std::cout << "Total fees assigned to coinbase: " << total_fees << " satoshis" << std::endl;

    std::cout << "Coinbase transaction hash: " << coinbase_tx->GetHash().ToString() << std::endl;
    
    uint64_t minted_sats = coinbase_tx->GetValueOut() - total_fees;  // subtract total fees from the coinbase output value
                                                                    //  to prevent double counting etc.
    std::cout << "Minted satoshis in coinbase: " << minted_sats << std::endl;
    std::vector<SatoshiRange> coinbase_output_ranges;


    uint64_t mint_end = block_mint_start + minted_sats;
    coinbase_output_ranges.push_back({block_mint_start, mint_end});

    // Append fee ranges
    coinbase_output_ranges.insert(
        coinbase_output_ranges.end(),
        fee_pool.begin(),
        fee_pool.end()
    );

    // Write all coinbase outputs
    for (size_t vout_index = 0; vout_index < coinbase_tx->vout.size(); ++vout_index) { // for every output in the transaction,
        uint64_t sats = coinbase_tx->vout[vout_index].nValue;                          // take the amount of bitcoin (in sats) off of the pooled outputs 
        std::pair<std::vector<SatoshiRange>, std::vector<SatoshiRange>> assigned = SkimRanges(coinbase_output_ranges, sats);
        coinbase_output_ranges = assigned.second; // update the pool with the remaining ranges after assignment
        TxOutputSatoshiEntry entry{assigned.first, static_cast<int>(block.height)};
        m_db->WriteOrdinalRanges(coinbase_tx->GetHash(), vout_index, entry);        // then write the data.
    }

    m_last_ordinal = mint_end;  // Persist this to DB later

    std::cout << "Finished indexing block height: " << block.height << std::endl;

    return m_db->Write(std::make_pair(DB_ORDINDEX, "lastordinal"), m_last_ordinal);

    // Write all (txid, position) pairs into the Ordinals index database.
}


BaseIndex::DB& OrdIndex::GetDB() const { return *m_db; }

bool OrdIndex::FindOrdByTxOutput(const uint256& tx_hash, uint32_t& vout, std::vector<SatoshiRange>& ranges) const
{
    TxOutputSatoshiEntry entry;
    m_db->ReadOrdinalRanges(tx_hash, vout, entry);
    if (entry.ranges.empty()) {
        return false; // No ordinal ranges found for this transaction
    }
    ranges = std::move(entry.ranges);
    return true;
}
