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



/** Global instances for ordinal index and configuration */
std::unique_ptr<OrdIndex> g_ordindex;
std::unique_ptr<bool> g_ordindex_prune;
std::unique_ptr<bool> g_ordindex_rewrite_spent;

/**
 * Database interface for the ordinal index.
 * Provides methods to read, write, and erase ordinal range data from LevelDB.
 */
class OrdIndex::DB : public BaseIndex::DB
{
public:
    explicit DB(size_t n_cache_size, bool f_memory = false, bool f_wipe = false);

    /** Read ordinal ranges for a specific transaction output */
    bool ReadOrdinalRanges(const uint256& txid, uint32_t vout, TxOutputSatoshiEntry& entry) const;

    /** Write ordinal ranges for a specific transaction output */
    bool WriteOrdinalRanges(const uint256& txid, uint32_t vout, const TxOutputSatoshiEntry& entry);

    /** Remove ordinal ranges for a specific transaction output */
    bool EraseOrdinalRanges(const uint256& txid, uint32_t vout);
};

/** Initialize database connection with specified cache size and options */
OrdIndex::DB::DB(size_t n_cache_size, bool f_memory, bool f_wipe) :
    BaseIndex::DB(gArgs.GetDataDirNet() / "indexes" / "ordindex", n_cache_size, f_memory, f_wipe)
{}

/** Read ordinal data from database using (txid, vout) as key */
/**
 * @brief reads the ordinal range information for a specific transaction output from the database.
 *
 * This function retrieves the ordinal range entry associated with the given transaction ID and output index.
 *
 * @param[in] txid   The transaction ID for which the ordinal range is being read.
 * @param[in] vout   The output index within the transaction.
 * @param[out] entry The ordinal range entry containing satoshi information for the output.
 * @return true if the read operation was successful, false otherwise.
 */
bool OrdIndex::DB::ReadOrdinalRanges(const uint256& txid, uint32_t vout, TxOutputSatoshiEntry& entry) const
{
    return Read(std::make_pair(DB_ORDINDEX, std::make_pair(txid, vout)), entry);
}

/** Write ordinal data to database using batch operation for efficiency */
/**
 * @brief Writes the ordinal range information for a specific transaction output to the database.
 *
 * This function creates a database batch operation to store the provided ordinal range entry
 * associated with the given transaction ID and output index. The batch is then written to the database.
 *
 * @param[in] txid   The transaction ID for which the ordinal range is being written.
 * @param[in] vout   The output index within the transaction.
 * @param[in] entry  The ordinal range entry containing satoshi information for the output.
 * @return true if the batch write to the database was successful, false otherwise.
 */
bool OrdIndex::DB::WriteOrdinalRanges(const uint256& txid, uint32_t vout, const TxOutputSatoshiEntry& entry)
{
    CDBBatch batch(*this);
    batch.Write(std::make_pair(DB_ORDINDEX, std::make_pair(txid, vout)), entry);
    return WriteBatch(batch);
}

/** Remove ordinal data from database using batch operation */
/**
 * @brief Writes the ordinal range information for a specific transaction output to the database.
 *
 * This function creates a database batch operation to store the provided ordinal range entry
 * associated with the given transaction ID and output index. The batch is then written to the database.
 *
 * @param[in] txid   The transaction ID for which the ordinal range is being written.
 * @param[in] vout   The output index within the transaction.
 * @return true if the batch write to the database was successful, false otherwise.
 */
bool OrdIndex::DB::EraseOrdinalRanges(const uint256& txid, uint32_t vout)
{
    /*CDBBatch batch(*this);
    batch.Erase(std::make_pair(DB_ORDINDEX, std::make_pair(txid, vout)));
    return WriteBatch(batch);*/
    return true;
}

/** Construct ordinal index with specified configuration */
OrdIndex::OrdIndex(std::unique_ptr<interfaces::Chain> chain, size_t n_cache_size, bool f_memory, bool f_wipe, bool f_prune)
    : BaseIndex(std::move(chain), "ordindex"),
    m_db(std::make_unique<OrdIndex::DB>(n_cache_size, f_memory, f_wipe))
{}

/** Default destructor (required due to unique_ptr with forward declaration) */
OrdIndex::~OrdIndex() = default;

/** Vector to track transaction outputs that need to be pruned after specified block delay */
std::vector<OrdDBPtr> TxOutToPrune;

/**
 * Check if a transaction contains ordinal inscription data.
 *
 * @param[in] tx The transaction to examine
 * @param[in] output_index The specific output index (currently unused - inscriptions are per-transaction)
 * @return true if inscription patterns are detected, false otherwise
 */
static bool OutputContainsInscription(const CTransactionRef& tx, size_t output_index)
{
    if (tx->vin.empty() || tx->vin[0].scriptWitness.stack.empty()) {
        return false;
    }
    for(const auto& witness_item : tx->vin[0].scriptWitness.stack){
        if (witness_item.size() < 6) {
            continue;
        }
        if (witness_item[0] == OP_FALSE && witness_item[1] == OP_IF) {
            uint8_t data_length = witness_item[2];
            if (data_length >= 3 && witness_item.size() >= 3 + data_length) {
                std::string third_element(witness_item.begin() + 3, 
                                         witness_item.begin() + 3 + data_length);
                if (third_element.size() >= 3 && third_element.compare(0, 3, "ord") == 0) {
                    return true;
                }
            }
        }
    }
    return false;
}

/**
 * Allocate satoshi ranges from a pool following FIFO (first-in-first-out) order.
 * 
 * This implements the core ordinal theory algorithm where satoshis are assigned to outputs
 * in the same order they were received as inputs. The function consumes ranges from the
 * front of the pool and returns the allocated ranges plus the remaining pool.
 *
 * @param[in,out] pool Vector of available satoshi ranges (modified by this function)
 * @param[in] amount Number of satoshis to allocate from the pool
 * @return Pair containing (allocated_ranges, remaining_pool)
 */
static std::pair<std::vector<SatoshiRange>, std::vector<SatoshiRange>> SkimRanges(std::vector<SatoshiRange>& pool, uint64_t amount) {
    std::vector<SatoshiRange> result;
    size_t i = 0;
    while (amount > 0 && i < pool.size()) {
        SatoshiRange& r = pool[i];
        uint64_t available = r.Size();
        if (amount >= available) {
            result.push_back(r);
            amount -= available;
            ++i;
        } else {
            result.push_back({r.start, r.start + amount});
            r.start += amount;
            amount = 0;
        }
    }
    pool.erase(pool.begin(), pool.begin() + i);
    return {result, pool};
}

/**
 * Process a new block and update the ordinal index.
 * 
 * This is the core function that implements ordinal theory by:
 * 1. Tracking satoshi movement from transaction inputs to outputs
 * 2. Assigning ordinal numbers to newly minted satoshis in coinbase
 * 3. Handling transaction fees according to ordinal rules
 * 4. Detecting and flagging outputs containing inscriptions
 * 5. Managing database pruning and spent status tracking
 * 
 * The algorithm follows the ordinal specification:
 * - Satoshis are transferred in FIFO order from inputs to outputs
 * - Fees are collected and assigned to the coinbase transaction
 * - New satoshis are minted with sequential ordinal numbers
 * - Each output's ordinal ranges are stored in the database
 *
 * @param[in] block Block information including transactions and height
 * @return true if the block was successfully processed, false on error
 */
bool OrdIndex::CustomAppend(const interfaces::BlockInfo& block)
{
    
    // Phase 1: Prune old spent outputs if pruning is enabled
    // This reduces database size by removing ordinal data for old spent outputs
    if(g_ordindex_prune) 
        // Iterate through scheduled outputs to prune
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

    // Phase 2: Validate block data and initialize tracking variables
    if (block.data == nullptr) {
        return false;
    }

    // Track the starting point for newly minted ordinals in this block
    uint64_t block_mint_start = m_last_ordinal;
    
    // Pool to collect transaction fees (ordinals from unspent inputs)
    std::vector<SatoshiRange> fee_pool;

    // Phase 3: Process all non-coinbase transactions
    // These transactions consume existing ordinals and redistribute them to outputs

    assert(block.data);

    for (size_t tx_index = 1; tx_index < block.data->vtx.size(); ++tx_index) {
        const auto& tx = block.data->vtx[tx_index];
        
        // Step 3a: Collect ordinals from all transaction inputs
        // This implements the FIFO pooling mechanism of ordinal theory
        std::vector<SatoshiRange> pool;

        // Process each input and gather its ordinal ranges
        for (const auto& txin : tx->vin) {
            TxOutputSatoshiEntry prev_entry;

            // Read the ordinal ranges from the previous output being spent
            if (m_db->ReadOrdinalRanges(txin.prevout.hash, txin.prevout.n, prev_entry)) {
                // Add all ranges from this input to the pool
                for (const auto& r : prev_entry.ranges) {
                    pool.push_back(r);
                }

                // Handle database maintenance based on configuration
                if(g_ordindex_prune) {
                    // If pruning is enabled, remove old spent outputs to save space
                    if(block.height-prev_entry.block_height >= 6) {
                        //m_db->EraseOrdinalRanges(txin.prevout.hash, txin.prevout.n);
                    } else {
                        // Schedule for later pruning
                        OrdDBPtr spentTX = OrdDBPtr(txin.prevout.hash, txin.prevout.n, block.height);
                        TxOutToPrune.emplace_back(spentTX);
                    }
                }
                if(g_ordindex_rewrite_spent) {
                    // If spent tracking is enabled, mark the output as spent
                    TxOutputSatoshiEntry entry{prev_entry.ranges, prev_entry.block_height};
                    entry.spent = true;
                    entry.inscription = prev_entry.inscription; // Preserve inscription status
                    m_db->WriteOrdinalRanges(txin.prevout.hash, txin.prevout.n, entry);
                }
            } else {

                LogPrintf("Failed to read ordinal ranges for input %s:%d in tx %s\n",
                         txin.prevout.hash.ToString(), txin.prevout.n, tx->GetHash().ToString());
                std::abort();
            }
        }

        // Step 3b: Distribute pooled ordinals to transaction outputs
        // This follows FIFO order: first ordinals in the pool go to the first output
        for (size_t vout_index = 0; vout_index < tx->vout.size(); ++vout_index) {
            uint64_t sats = tx->vout[vout_index].nValue;
            
            // Allocate the required satoshis from the pool
            std::pair<std::vector<SatoshiRange>, std::vector<SatoshiRange>> assigned = SkimRanges(pool, sats);
            pool = assigned.second; // Update pool with remaining ranges
            
            // Check if this output contains inscription data
            bool output_has_inscription = OutputContainsInscription(tx, vout_index);
            if (output_has_inscription) {
                LogInfo("Found ordinal inscription in tx: %s output: %d\n", tx->GetHash().ToString(), vout_index);
            }
            
            // Create and store the ordinal entry for this output
            TxOutputSatoshiEntry entry{assigned.first, static_cast<int>(block.height)};
            entry.spent = false;
            entry.inscription = output_has_inscription;
            if(!m_db->WriteOrdinalRanges(tx->GetHash(), vout_index, entry)) {
                LogPrintf("Failed to write ordinal ranges for %s:%d\n", tx->GetHash().ToString(), vout_index);
                std::abort();
            }
        }

        // Step 3c: Collect transaction fees
        // Any remaining ordinals in the pool become fees for the miner
        std::vector<SatoshiRange> fee_taken = pool;
        fee_pool.insert(fee_pool.end(), fee_taken.begin(), fee_taken.end());
    }


    // Phase 4: Process the coinbase transaction
    // The coinbase transaction creates new ordinals and collects fees
    const auto& coinbase_tx = block.data->vtx[0];

    // Calculate total fees collected from all transactions
    uint64_t total_fees = 0;
    for (const auto& r : fee_pool) {
        total_fees += r.Size();
    }

    // Calculate new ordinals minted in this block
    // This is the block reward minus the total fees (to avoid double counting)
    uint64_t minted_sats = coinbase_tx->GetValueOut() - total_fees;
    
    // Create ranges for coinbase distribution: new ordinals + collected fees
    std::vector<SatoshiRange> coinbase_output_ranges;

    // Add newly minted ordinals first
    uint64_t mint_end = block_mint_start + minted_sats;
    coinbase_output_ranges.push_back({block_mint_start, mint_end});

    // Append collected fee ranges
    coinbase_output_ranges.insert(
        coinbase_output_ranges.end(),
        fee_pool.begin(),
        fee_pool.end()
    );

    // Distribute coinbase ordinals to outputs following FIFO order
    for (size_t vout_index = 0; vout_index < coinbase_tx->vout.size(); ++vout_index) {
        uint64_t sats = coinbase_tx->vout[vout_index].nValue;
        
        // Allocate ordinals from the coinbase pool
        std::pair<std::vector<SatoshiRange>, std::vector<SatoshiRange>> assigned = SkimRanges(coinbase_output_ranges, sats);
        coinbase_output_ranges = assigned.second;
        // Check if this coinbase output contains inscription data (rare but possible)
        bool output_has_ordinals = OutputContainsInscription(coinbase_tx, vout_index);
        if (output_has_ordinals) {
            LogInfo("Found ordinal inscription in tx: %s output: %d\n", coinbase_tx->GetHash().ToString(), vout_index);
        }
        
        // Create and store the ordinal entry for this coinbase output
        TxOutputSatoshiEntry entry{assigned.first, static_cast<int>(block.height)};
        entry.spent = false;
        entry.inscription = output_has_ordinals;
        m_db->WriteOrdinalRanges(coinbase_tx->GetHash(), vout_index, entry);
    }

    // Phase 5: Update the global ordinal counter
    // This tracks the highest ordinal number assigned so far
    m_last_ordinal = mint_end;

    LogInfo("Finished indexing block height: %d\n", block.height);

    // Persist the last ordinal counter to the database
    return m_db->Write(std::make_pair(DB_ORDINDEX, "lastordinal"), m_last_ordinal);
}

/** Provide access to the underlying database for initialization code */
BaseIndex::DB& OrdIndex::GetDB() const { return *m_db; }

/**
 * Find ordinal ranges for a specific transaction output.
 * This is the primary lookup method used by RPC commands.
 * 
 * @param[in] tx_hash Hash of the transaction to look up
 * @param[in] vout Output index within the transaction
 * @param[out] entry Reference to store the retrieved ordinal data
 * @return true if the output exists and contains ordinals, false otherwise
 */
bool OrdIndex::FindOrdRangesByTxOutput(const uint256& tx_hash, uint32_t& vout, TxOutputSatoshiEntry& entry) const
{
    return m_db->ReadOrdinalRanges(tx_hash, vout, entry);
}

/**
 * Find all transaction outputs that contain a specific ordinal number.
 * 
 * This function scans the entire ordinal database to find outputs containing
 * the specified ordinal. It's used by the gettxoutputsbyordinal RPC command.
 * 
 * @param[in] ordinal The ordinal number to search for
 * @param[out] outputs Vector to store the results as (txid, vout) pairs
 * @return true if at least one output was found, false otherwise
 */
bool OrdIndex::FindTxOutputsByOrdinal(uint64_t ordinal, std::vector<std::pair<uint256, uint32_t>>& outputs) const
{
    std::unique_ptr<CDBIterator> pcursor(m_db->NewIterator());

    // Iterate through all database entries
    pcursor->SeekToFirst();

    while (pcursor->Valid()) {
        std::pair<uint8_t, std::pair<uint256, uint32_t>> key;
        TxOutputSatoshiEntry entry;
        
        // Read the database key and value
        if (pcursor->GetKey(key) && key.first == DB_ORDINDEX) {
            if (pcursor->GetValue(entry)) {
                // Check if the ordinal falls within any range in this output
                for (const auto& range : entry.ranges) {
                    if (ordinal >= range.start && ordinal < range.end) {
                        // Found the ordinal in this output
                        outputs.emplace_back(key.second.first, key.second.second);
                        break; // Move to next entry
                    }
                }
            } else {
                LogWarning("Failed to get value for key: %s vout: %d\n", key.second.first.ToString(), key.second.second);
            }
        } else {
            LogWarning("Failed to get key for entry, possibly not a valid ordinal index entry.\n");
        }
        
        pcursor->Next();
    }
    
    return !outputs.empty();
}

/**
 * Find the current position (most recent unspent output) of a specific ordinal.
 * 
 * This function requires the rewrite_spent option to be enabled, as it needs
 * to distinguish between spent and unspent outputs to determine the current position.
 * It returns the most recent unspent output containing the ordinal.
 * 
 * @param[in] ordinal The ordinal number to search for
 * @param[out] outputs Reference to store the result as (txid, vout) pair
 * @return true if the ordinal's current position was found, false otherwise
 */
bool OrdIndex::FindOrdPosition(uint64_t ordinal, std::pair<uint256, uint32_t>& outputs) const
{
    std::unique_ptr<CDBIterator> pcursor(m_db->NewIterator());

    // Iterate through all database entries to find unspent outputs
    pcursor->SeekToFirst();

    while (pcursor->Valid()) {
        std::pair<uint8_t, std::pair<uint256, uint32_t>> key;
        TxOutputSatoshiEntry entry;
        
        // Read the database key and value
        if (pcursor->GetKey(key) && key.first == DB_ORDINDEX) {
            if (pcursor->GetValue(entry)) {
                // Only consider unspent outputs for current position 
                if (!entry.spent) {
                    // Check if the ordinal falls within any range in this unspent output
                    for (const auto& range : entry.ranges) {
                        if (ordinal >= range.start && ordinal < range.end) {
                            // Found the ordinal in this unspent output
                            outputs = std::make_pair(key.second.first, key.second.second);
                            return true; // Return the current position
                        }
                    }
                }
            }
        }
        
        pcursor->Next();
    }
    
    return false; // Ordinal not found in any unspent output
}

//🪲
