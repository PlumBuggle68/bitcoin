// Copyright (c) 2017-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_INDEX_ORDINDEX_H
#define BITCOIN_INDEX_ORDINDEX_H

#include <index/base.h>

// Default configuration values for the ordinal index
static constexpr bool DEFAULT_ORDINDEX{false};           // Ordinal index disabled by default
static constexpr bool DEFAULT_ORDINDEX_PRUNE{false};     // Pruning of old spent outputs disabled by default  
static constexpr bool DEFAULT_ORDINDEX_REWRITE_SPENT{false}; // Rewriting spent status disabled by default
static constexpr uint8_t DB_ORDINDEX = 'o';              // Database key prefix for ordinal index entries

/**
 * Represents a contiguous range of satoshi ordinal numbers.
 * Ordinal theory assigns each satoshi a unique sequential number based on mining order.
 * Ranges are used for efficient storage of large consecutive ordinal sequences.
 */
struct SatoshiRange {
    uint64_t start;  // The ordinal number of the first satoshi in the range (inclusive)
    uint64_t end;    // The ordinal number after the last satoshi in the range (exclusive)

    /** Calculate the number of satoshis in this range */
    uint64_t Size() const { return end - start; }

    // Future: std::vector<Inscription> inscriptions; // Inscription data for this range
};

/**
 * Database entry containing all ordinal information for a specific transaction output.
 * This structure is serialized and stored in LevelDB with key (DB_ORDINDEX, (txid, vout)).
 */
struct TxOutputSatoshiEntry {
    std::vector<SatoshiRange> ranges;  // Ordinal ranges contained in this output
    int block_height;                  // Block height where this output was created
    bool spent = false;                // Whether this output has been spent (if rewrite_spent enabled)
    bool inscription = false;          // Whether this output contains an ordinal inscription

    /** Serialize this entry to a stream for database storage */
    template <typename Stream>
    void Serialize(Stream& s) const {
        ::Serialize(s, ranges);
        ::Serialize(s, block_height);
        ::Serialize(s, spent);
        ::Serialize(s, inscription);
    }
    
    /** Deserialize this entry from a stream when reading from database */
    template <typename Stream>
    void Unserialize(Stream& s) {
        ::Unserialize(s, ranges);
        ::Unserialize(s, block_height);
        ::Unserialize(s, spent);
        ::Unserialize(s, inscription);
    }
};

/** Serialization helper for SatoshiRange structures */
template <typename Stream>
inline void Serialize(Stream& s, const SatoshiRange& r) {
    ::Serialize(s, r.start);
    ::Serialize(s, r.end);
}

/** Deserialization helper for SatoshiRange structures */
template <typename Stream>
inline void Unserialize(Stream& s, SatoshiRange& r) {
    ::Unserialize(s, r.start);
    ::Unserialize(s, r.end);
}

/**
 * Helper structure for tracking transaction outputs that need to be pruned.
 * Used when ordinal index pruning is enabled to remove old spent outputs.
 */
struct OrdDBPtr {
    uint256 hash;        // Transaction hash
    unsigned int vout;   // Output index
    int height;          // Block height where the spending occurred
    
    OrdDBPtr(const uint256& hash, const unsigned int& vout, const int& height)
        : hash(hash), vout(vout), height(height) {}
};

/**
 * OrdIndex provides efficient lookup of Bitcoin ordinal information.
 * 
 * This index tracks the movement of individual satoshis through the blockchain
 * according to ordinal theory, where each satoshi is assigned a unique sequential
 * number based on the order it was mined. The index supports:
 * 
 * - Tracking ordinal ranges in transaction outputs
 * - Finding which outputs contain specific ordinals
 * - Detecting ordinal inscriptions in witness data
 * - Optional pruning of old spent outputs
 * - Marking spent status for ordinal position tracking
 * 
 * The index stores data in LevelDB with keys of the form (DB_ORDINDEX, (txid, vout))
 * mapping to TxOutputSatoshiEntry values containing ordinal ranges and metadata.
 */
class OrdIndex final : public BaseIndex
{
protected:
    class DB;  // Forward declaration of database interface

private:
    const std::unique_ptr<DB> m_db;  // Database connection for ordinal data

    /** Ordinal index doesn't support blockchain pruning */
    bool AllowPrune() const override { return false; }
    

protected:
    /** 
     * Process a new block and update the ordinal index.
     * Called by BaseIndex when a new block is added to the chain.
     */
    bool CustomAppend(const interfaces::BlockInfo& block_info) override;

public:
    /** Access to the underlying database (moved to public for init.cpp access) */
    BaseIndex::DB& GetDB() const override;
    
    /**
     * Constructs the ordinal index.
     * @param chain Interface to the blockchain
     * @param n_cache_size Size of the LevelDB cache
     * @param f_memory Whether to store the database in memory only
     * @param f_wipe Whether to wipe existing database on startup
     * @param f_prune Whether to enable pruning of old spent outputs
     */
    explicit OrdIndex(std::unique_ptr<interfaces::Chain> chain,
                      size_t n_cache_size,
                      bool f_memory = false,
                      bool f_wipe = false,
                      bool f_prune = false);

    /** Destructor (required due to unique_ptr to forward-declared type) */
    virtual ~OrdIndex() override;

    /**
     * Look up ordinal ranges for a specific transaction output.
     * @param[in]  tx_hash  The transaction hash to look up
     * @param[in]  vout     The output index within the transaction
     * @param[out] entry    The ordinal ranges and metadata for this output
     * @return true if the output was found and contains ordinals, false otherwise
     */
    bool FindOrdRangesByTxOutput(const uint256& tx_hash, uint32_t& vout, TxOutputSatoshiEntry& entry) const;
    
    /**
     * Find all transaction outputs containing a specific ordinal.
     * @param[in]  ordinal  The ordinal number to search for
     * @param[out] outputs  Vector of (txid, vout) pairs containing the ordinal
     * @return true if at least one output was found, false otherwise
     */
    bool FindTxOutputsByOrdinal(uint64_t ordinal, std::vector<std::pair<uint256, uint32_t>>& outputs) const;
    
    /**
     * Find the current position (most recent unspent output) of a specific ordinal.
     * This method requires the rewrite_spent option to be enabled.
     * @param[in]  ordinal  The ordinal number to search for
     * @param[out] outputs  The (txid, vout) pair of the current position
     * @return true if the ordinal was found, false otherwise
     */
    bool FindOrdPosition(uint64_t ordinal, std::pair<uint256, uint32_t>& outputs) const;

    /** Type alias for ordinal range representation (start, end) */
    using OrdinalRange = std::pair<uint64_t, uint64_t>;

    /** The highest ordinal number that has been assigned (tracks total supply) */
    uint64_t m_last_ordinal = 0;

// Forward declarations for helper functions defined in ordindex.cpp
     
};

/** Global ordinal index instance, available when -ordindex is enabled */
extern std::unique_ptr<OrdIndex> g_ordindex;

/** Global configuration flags for ordinal index behavior */
extern std::unique_ptr<bool> g_ordindex_prune;
extern std::unique_ptr<bool> g_ordindex_rewrite_spent;
#endif // BITCOIN_INDEX_ORDINDEX_H
