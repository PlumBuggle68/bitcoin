// Copyright (c) 2017-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** 
 * RPC commands for Bitcoin ordinal theory functionality.
 * 
 * This file implements JSON-RPC commands for querying ordinal information:
 * - getordinalrangesbytxoutput: Get ordinal ranges for a specific transaction output
 * - gettxoutputsbyordinal: Find all outputs containing a specific ordinal
 * - getordinalposition: Get the current position of a specific ordinal
 * 
 * These commands provide access to the ordinal index data for applications
 * implementing ordinal theory protocols and inscription tracking.
 */

#include <rpc/client.h>
#include <rpc/server.h>
#include <rpc/util.h>
#include <index/ordindex.h>
#include <univalue.h>
#include <util/strencodings.h>
#include <util/translation.h>

#include <iostream>

/** Helper function to safely convert UniValue to uint32_t */
int Uint32FromUniValue(const UniValue& value)
{
    if (value.isNum()) {
        auto strvalue = value.getValStr();
        int res = 0;
        try {
            res = std::stoi(strvalue);
        } catch (const std::invalid_argument&) {
            throw JSONRPCError(RPC_TYPE_ERROR, "Invalid parameter type");
        }
        return static_cast<uint32_t>(res);
    }
    throw JSONRPCError(RPC_TYPE_ERROR, "Invalid parameter type");
}

/** Helper function to convert uint32_t to UniValue string representation */
UniValue UnivalueFromUint32(uint32_t value)
{
    UniValue result(UniValue::VSTR);
    auto str_value = std::to_string(value);
    result.setStr(str_value);
    return result;
}

/** Helper function to safely convert UniValue to uint64_t for ordinal numbers */
uint64_t Uint64FromUniValue(const UniValue& value)
{
    if (value.isNum()) {
        auto strvalue = value.getValStr();
        uint64_t res = 0;
        try {
            res = std::stoull(strvalue);
        } catch (const std::invalid_argument&) {
            throw JSONRPCError(RPC_TYPE_ERROR, "Invalid parameter type");
        } catch (const std::out_of_range&) {
            throw JSONRPCError(RPC_TYPE_ERROR, "Parameter out of range");
        }
        return res;
    }
    throw JSONRPCError(RPC_TYPE_ERROR, "Invalid parameter type");
}

/** Helper function to convert uint64_t to UniValue string representation for large ordinal numbers */
UniValue UnivalueFromUint64(uint64_t value)
{
    UniValue result(UniValue::VSTR);
    auto str_value = std::to_string(value);
    result.setStr(str_value);
    return result;
}

/**
 * RPC command: getordinalrangesbytxoutput
 * 
 * Returns all ordinal ranges contained in a specific transaction output,
 * along with metadata about the output (spent status, inscription presence, block height).
 * 
 * This is useful for applications that need to know exactly which ordinals
 * are contained in a particular UTXO or historical transaction output.
 */
static RPCHelpMan getordinalrangesbytxoutput()
{
    return RPCHelpMan{"getordinalrangesbytxoutput",
                "\nReturns the ordinal ranges for a specific transaction output.\n",
                {
                    {"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The transaction ID."},
                    {"vout", RPCArg::Type::NUM, RPCArg::Optional::NO, "The output index."},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "An object containing ordinal ranges and metadata for the specified output.",
                    {
                        {RPCResult::Type::ARR, "ranges", "Array of ordinal ranges for this output.", {
                            {RPCResult::Type::OBJ, "", "", {
                                {RPCResult::Type::NUM, "start", "The starting ordinal of the range."},
                                {RPCResult::Type::NUM, "end", "The ending ordinal of the range."},
                            }},
                        }},
                        {RPCResult::Type::BOOL, "spent", "Whether this output has been spent."},
                        {RPCResult::Type::BOOL, "inscription", "Whether this output contains an inscription."},
                        {RPCResult::Type::NUM, "block_height", "The block height where this output was created."},
                    }
                },
                RPCExamples{
                    HelpExampleCli("getordinalrangesbytxoutput", "\"txid\" 0")
                  + HelpExampleRpc("getordinalrangesbytxoutput", "\"txid\", 0")
                },
                [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    if (!g_ordindex) {
        throw JSONRPCError(RPC_METHOD_NOT_FOUND, "Ordinal index is not available.");
    }

    uint256 txid = ParseHashV(request.params[0], "txid");
    uint32_t vout = Uint32FromUniValue(request.params[1]);

    TxOutputSatoshiEntry entry;
    if (!g_ordindex->FindOrdRangesByTxOutput(txid, vout, entry)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "No ordinals found for this transaction output. Possible OP_RETURN.");
    }

    std::vector<SatoshiRange> ranges = entry.ranges;

    UniValue result(UniValue::VOBJ);
    
    UniValue height_val;
    height_val.setInt((uint64_t)entry.block_height);
    result.pushKV("block_height", height_val);
    
    UniValue spent_val;
    spent_val.setBool(entry.spent);
    result.pushKV("spent", spent_val);
    
    UniValue inscription_val;
    inscription_val.setBool(entry.inscription);
    result.pushKV("inscription", inscription_val);
    
    UniValue rangesArray(UniValue::VARR);
    for (const auto& range : ranges) {
        UniValue obj(UniValue::VOBJ);
        auto start = std::to_string(range.start);
        auto end = std::to_string(range.end-1);
        obj.pushKV("start", UnivalueFromUint64(range.start));
        obj.pushKV("end", UnivalueFromUint64(range.end)); // end is exclusive to match with standard ord theory 
        rangesArray.push_back(obj);
    }
    result.pushKV("ranges", rangesArray);
    return result;
},
    };
}

/**
 * RPC command: gettxoutputsbyordinal
 * 
 * Finds all transaction outputs that contain a specific ordinal number.
 * This command is useful for tracking the history of a particular ordinal
 * as it moves through different transactions.
 * 
 * Note: If pruning is enabled (-ordinalindexprune), only unspent outputs
 * will be returned as old spent outputs are removed from the database.
 */
static RPCHelpMan gettxoutputsbyordinal()
{
    return RPCHelpMan{"gettxoutputsbyordinal",
                "\nReturns the transaction outputs that contain a specific ordinal (in no specific order). If -ordinalindexprune is enabled, only unspent outputs will be returned.\n",
                {
                    {"ordinal", RPCArg::Type::NUM, RPCArg::Optional::NO, "The ordinal number to search for."},
                },
                RPCResult{
                    RPCResult::Type::ARR, "", "An array of transaction outputs containing the specified ordinal.",
                    {
                        {RPCResult::Type::OBJ, "", "", {
                            {RPCResult::Type::STR_HEX, "txid", "The transaction ID."},
                            {RPCResult::Type::NUM, "vout", "The output index."},
                        }},
                    }
                },
                RPCExamples{
                    HelpExampleCli("gettxoutputsbyordinal", "123456789")
                  + HelpExampleRpc("gettxoutputsbyordinal", "123456789")
                },
                [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    if (!g_ordindex) {
        throw JSONRPCError(RPC_METHOD_NOT_FOUND, "Ordinal index is not available.");
    }

    uint64_t ordinal = Uint64FromUniValue(request.params[0]);

    std::vector<std::pair<uint256, uint32_t>> outputs;
    if(!g_ordindex->FindTxOutputsByOrdinal(ordinal, outputs)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "error attempting function FindTxOutputsByOrdinal");
    }

    UniValue result(UniValue::VARR);
    for (const auto& [txid, vout] : outputs) {
        UniValue obj(UniValue::VOBJ);
        auto txid_str = txid.ToString();
        auto vout_str = UnivalueFromUint32(vout);
        UniValue txid_val;
        txid_val.setStr(txid_str);
        obj.pushKV("txid", txid_val);
        obj.pushKV("vout", vout_str);
        result.push_back(obj);
    }
    return result;
},
    };
}

/**
 * RPC command: getordinalposition
 * 
 * Returns the current position (most recent unspent output) of a specific ordinal.
 * This command requires the -ordindexrewritespent option to be enabled, as it
 * needs to track which outputs have been spent to determine the current position.
 * 
 * This is useful for applications that need to find where a specific ordinal
 * currently resides in the UTXO set, such as wallet applications or block explorers.
 */
static RPCHelpMan getordinalposition()
{
    return RPCHelpMan{"getordinalposition",
                "\nReturns the current position (most recent transaction output) of a specific ordinal.\n",
                {
                    {"ordinal", RPCArg::Type::NUM, RPCArg::Optional::NO, "The ordinal number to search for."},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "The transaction output containing the specified ordinal.",
                    {
                        {RPCResult::Type::STR_HEX, "txid", "The transaction ID."},
                        {RPCResult::Type::NUM, "vout", "The output index."},
                    }
                },
                RPCExamples{
                    HelpExampleCli("getordinalposition", "123456789")
                  + HelpExampleRpc("getordinalposition", "123456789")
                },
                [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    if(!g_ordindex_rewrite_spent){
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Ordinal index rewrite spent is not enabled. Please enable it with -ordindexrewritespent.");
    }
    if (!g_ordindex) {
        throw JSONRPCError(RPC_METHOD_NOT_FOUND, "Ordinal index is not available.");
    }

    uint64_t ordinal = Uint64FromUniValue(request.params[0]);

    std::pair<uint256, uint32_t> outputs;
    if(!g_ordindex->FindOrdPosition(ordinal, outputs)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Ordinal not found or error attempting function FindOrdPosition");
    }

    // FindOrdPosition should return exactly one output (the most recent)
    if ((outputs.first.IsNull() && outputs.second == 0)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "No current position found for the specified ordinal");
    }

    UniValue result(UniValue::VOBJ);
    auto txid_str = outputs.first.ToString();
    auto vout_str = UnivalueFromUint32(outputs.second);
    UniValue txid_val;
    txid_val.setStr(txid_str);
    result.pushKV("txid", txid_val);
    result.pushKV("vout", vout_str);
    return result;
},
    };
}

/**
 * Register all ordinal-related RPC commands with the RPC server.
 * @param t Reference to the RPC command table
 */
void RegisterOrdinalRPCCommands(CRPCTable& t)
{
    static const CRPCCommand commands[]{
        {"ordinal", &getordinalrangesbytxoutput},
        {"ordinal", &gettxoutputsbyordinal},
        {"ordinal", &getordinalposition},
    };
    for (const auto& c : commands) {
        t.appendCommand(c.name, &c);
    }
}
