// Copyright (c) 2017-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <index/ordindex.h>

#include <algorithm>
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
#include <script/script.h>        // for CScript and OP_RETURN



// Helper function to check if a transaction contains ordinal inscriptions
static bool TransactionContainsOrdinals(const CTransactionRef& tx)
{
    // Check each output for OP_RETURN with "ord" tag
    for (const auto& txout : tx->vout) {
        const CScript& scriptPubKey = txout.scriptPubKey;
        
        // Check if this is an OP_RETURN output
        if (scriptPubKey.size() > 0 && scriptPubKey[0] == OP_RETURN) {
            // Look for the "ord" tag in the OP_RETURN data
            // The typical format is: OP_RETURN <pushdata> "ord" <content>
            std::vector<unsigned char> vchData;
            CScript::const_iterator pc = scriptPubKey.begin() + 1; // Skip OP_RETURN
            
            // Try to extract data from the OP_RETURN
            while (pc < scriptPubKey.end()) {
                opcodetype opcode;
                if (!scriptPubKey.GetOp(pc, opcode, vchData)) {
                    break;
                }
                
                // Check if this data chunk contains "ord"
                if (vchData.size() >= 3) {
                    std::string dataStr(vchData.begin(), vchData.end());
                    if (dataStr.find("ord") != std::string::npos) {
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

std::unique_ptr<OrdIndex> g_ordindex;
std::unique_ptr<bool> g_ordindex_prune;
std::unique_ptr<bool> g_ordindex_rewrite_spent;

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

static std::pair<std::vector<SatoshiRange>, std::vector<SatoshiRange>> SkimRanges(std::vector<SatoshiRange>& pool, uint64_t amount) {
    std::vector<SatoshiRange> result;
    size_t i = 0; // Start from the front of the pool

    while (amount > 0 && i < pool.size()) {
        SatoshiRange& r = pool[i];
        uint64_t available = r.Size();

        if (amount >= available) {
            result.push_back(r); // Use the whole range
            amount -= available;
            ++i; // Move to the next range
        } else {
            // Use a portion from the beginning of the range
            result.push_back({r.start, r.start + amount});
            r.start += amount; // Shrink the range from the beginning
            amount = 0;
        }
    }

    // Erase fully consumed ranges from the front of the pool
    pool.erase(pool.begin(), pool.begin() + i);

    return {result, pool};
}

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

    //std::cout << "Indexing block height: " << block.height << std::endl;

    uint64_t block_mint_start = m_last_ordinal;
    std::vector<SatoshiRange> fee_pool;

    // Step 1: Ensure the block has transactions

    assert(block.data);

    for (size_t tx_index = 1; tx_index < block.data->vtx.size(); ++tx_index) {
        const auto& tx = block.data->vtx[tx_index];

        // Optional: Check if transaction contains ordinals before processing
        // This is an optimization - you can enable/disable based on your needs
        bool tx_has_ordinals = TransactionContainsOrdinals(tx);
        
        if (tx_has_ordinals) {
            std::cout << "Found ordinal inscription in tx: " << tx->GetHash().ToString() << std::endl;
        }
        
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
                        m_db->EraseOrdinalRanges(txin.prevout.hash, txin.prevout.n);
                            //std::cout << "Erased ordinal ranges for " << txin.prevout.hash.ToString() << " vout " << txin.prevout.n << std::endl;
                    } else {
                        OrdDBPtr spentTX = OrdDBPtr(txin.prevout.hash, txin.prevout.n, block.height);
                        TxOutToPrune.emplace_back(spentTX);
                    }
                }else if(g_ordindex_rewrite_spent){
                    TxOutputSatoshiEntry entry{prev_entry.ranges, prev_entry.block_height};
                    entry.spent = true; // mark the entry as spent
                    m_db->EraseOrdinalRanges(txin.prevout.hash, txin.prevout.n);
                    m_db->WriteOrdinalRanges(txin.prevout.hash, txin.prevout.n, entry);
                }
            }
        }

        // Step 2b: Assign satoshi ranges to each output
        for (size_t vout_index = 0; vout_index < tx->vout.size(); ++vout_index) { // for every output in the transaction,
            uint64_t sats = tx->vout[vout_index].nValue;                          // take the amount of bitcoin (in sats) off of the pooled outputs 
            std::pair<std::vector<SatoshiRange>, std::vector<SatoshiRange>> assigned = SkimRanges(pool, sats);
            pool = assigned.second; // update the pool with the remaining ranges after assignment
            TxOutputSatoshiEntry entry{assigned.first, static_cast<int>(block.height)};
            entry.spent = false; // mark the entry as unspent
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
    //std::cout << "Total fees assigned to coinbase: " << total_fees << " satoshis" << std::endl;

    //std::cout << "Coinbase transaction hash: " << coinbase_tx->GetHash().ToString() << std::endl;

    uint64_t minted_sats = coinbase_tx->GetValueOut() - total_fees;  // subtract total fees from the coinbase output value
                                                                    //  to prevent double counting etc.
    //std::cout << "Minted satoshis in coinbase: " << minted_sats << std::endl;
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
        entry.spent = false; // mark the entry as unspent
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

bool OrdIndex::FindTxOutputsByOrdinal(uint64_t ordinal, std::vector<std::pair<uint256, uint32_t>>& outputs) const
{
    std::unique_ptr<CDBIterator> pcursor(m_db->NewIterator());

    // Start from the first entry and iterate through all
    pcursor->SeekToFirst();

    while (pcursor->Valid()) {
        std::pair<uint8_t, std::pair<uint256, uint32_t>> key;
        TxOutputSatoshiEntry entry;
        
        // Try to get the key and value
        if (pcursor->GetKey(key) && key.first == DB_ORDINDEX) {
            if (pcursor->GetValue(entry)) {
                // Check if the ordinal falls within any range in this entry
                for (const auto& range : entry.ranges) {
                    if (ordinal >= range.start && ordinal < range.end-1) { // -1 to match with ord 
                        // Found a valid range containing the ordinal
                        outputs.emplace_back(key.second.first, key.second.second);
                        break; // Found in this entry, move to next entry
                    }
                }
            }else{
                std::cout << "Failed to get value for key: " << key.second.first.ToString() << " vout: " << key.second.second << std::endl;
            }
            // If GetValue failed, this might be the "lastordinal" entry, just continue
        }else{
            std::cout << "Failed to get key for entry, possibly not a valid ordinal index entry." << std::endl;
        }
        
        pcursor->Next();
    }
    
    return !outputs.empty(); // Return true if at least one output was found
}

bool OrdIndex::FindOrdPosition(uint64_t ordinal, std::pair<uint256, uint32_t>& outputs) const
{
    std::unique_ptr<CDBIterator> pcursor(m_db->NewIterator());

    // Start from the first entry and iterate through all
    pcursor->SeekToFirst();

    while (pcursor->Valid()) {
        std::pair<uint8_t, std::pair<uint256, uint32_t>> key;
        TxOutputSatoshiEntry entry;
        
        // Try to get the key and value
        if (pcursor->GetKey(key) && key.first == DB_ORDINDEX) {
            if (pcursor->GetValue(entry)) {
                // Check if the ordinal falls within any range in this entry
                for (const auto& range : entry.ranges) {
                    if (ordinal >= range.start && ordinal < range.end-1) { // -1 to match with ord 
                        // Found a valid range containing the ordinal
                        if(!entry.spent){
                            outputs = std::make_pair(key.second.first, key.second.second);
                            return true; // Found the ordinal position
                        }
                        break; // Found in this entry, move to next entry
                    }
                }
            }
            // If GetValue failed, this might be the "lastordinal" entry, just continue
        }
        
        pcursor->Next();
    }
    
    return false; // No outputs found containing the ordinal
}
