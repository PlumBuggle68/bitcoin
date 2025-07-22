// Copyright (c) 2017-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_INDEX_ORDINDEX_H
#define BITCOIN_INDEX_ORDINDEX_H

#include <index/base.h>

static constexpr bool DEFAULT_ORDINDEX{false};
static constexpr bool DEFAULT_ORDINDEX_PRUNE{false};
static constexpr uint8_t DB_ORDINDEX = 'o';


struct SatoshiRange {
    uint64_t start; 
    // ^ This is the ordinal number of the first satoshi in the range.
    uint64_t end;

    uint64_t Size() const { return end - start; }

    //std::vector<Inscription> Inscriptions;

};

struct TxOutputSatoshiEntry {
    std::vector<SatoshiRange> ranges;
    int block_height;

    template <typename Stream>
    void Serialize(Stream& s) const {
        ::Serialize(s, ranges);
        ::Serialize(s, block_height);
    }
    template <typename Stream>
    void Unserialize(Stream& s) {
        ::Unserialize(s, ranges);
        ::Unserialize(s, block_height);
    }
};

/*struct Inscription {
    uint64_t IntegerNotation;
    std::string Name() {
        if (IntegerNotation == 0) return "a";
        std::string result;
        uint64_t n = IntegerNotation;
        while (n > 0) {
            char c = 'a' + (n % 26);
            result = c + result;
            n /= 26;
        }
        return result;
    }
};*/

// Serialization functions for LevelDB
template <typename Stream>
inline void Serialize(Stream& s, const SatoshiRange& r) {
    ::Serialize(s, r.start);
    ::Serialize(s, r.end);
}

template <typename Stream>
inline void Unserialize(Stream& s, SatoshiRange& r) {
    ::Unserialize(s, r.start);
    ::Unserialize(s, r.end);
}

// returns two satoshi range vectors: result and pool
// result contains the ranges that were skimmed from the pool
// pool is modified and contains the ranges that were not used


struct OrdDBPtr {
    uint256 hash;
    unsigned int vout;
    int height;
    OrdDBPtr(const uint256& hash, const unsigned int& vout, const int& height)
        : hash(hash), vout(vout), height(height) {}
};




/**
 * TxIndex is used to look up transactions included in the blockchain by hash.
 * The index is written to a LevelDB database and records the filesystem
 * location of each transaction by transaction hash.
 */
class OrdIndex final : public BaseIndex
{
protected:
    class DB;

private:
    const std::unique_ptr<DB> m_db;


    bool AllowPrune() const override { return false; }
    

protected:
    // to write CBlock from block_info
    bool CustomAppend(const interfaces::BlockInfo& block_info) override;

    //BaseIndex::DB& GetDB() const override; moved to public section to allow access to "lastordinal" from init.cpp

public:

    // moved to public section to allow acces to "lastordinal" from init.cpp

    BaseIndex::DB& GetDB() const override;
    /// Constructs the index, which becomes available to be queried.
    /// contains chain state manager for building the db.
    explicit OrdIndex(std::unique_ptr<interfaces::Chain> chain,
                      size_t n_cache_size,
                      bool f_memory = false,
                      bool f_wipe = false,
                      bool f_prune = false);

    // Destructor is declared because this class contains a unique_ptr to an incomplete type.
    virtual ~OrdIndex() override;

    /// Look up a ordinal ranges in output by hash/vout.
    /// @param[in]   tx_hash  The hash of the transaction to look up.
    /// @param[out]  vout  The output index of the transaction.
    /// @param[out]  ranges  The ordinal ranges of the transaction.
    /// @return  true if successful, false otherwise
    bool FindOrdByTxOutput(const uint256& tx_hash, uint32_t& vout, std::vector<SatoshiRange>& ranges) const;
    

    using OrdinalRange = std::pair<uint64_t, uint64_t>;


    uint64_t m_last_ordinal = 0;
    
};

/// The global ordinal index, used in GetTransaction. May be null.
extern std::unique_ptr<OrdIndex> g_ordindex;
extern std::unique_ptr<bool> g_ordindex_prune;
#endif // BITCOIN_INDEX_ORDINDEX_H
