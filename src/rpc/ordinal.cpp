#include "rpc/client.h"
#include "rpc/server.h"
#include "rpc/util.h"
#include "index/ordindex.h"
#include "univalue.h"
#include "util/strencodings.h"
#include "util/translation.h"

#include <iostream>

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

UniValue UnivalueFromUint32(uint32_t value)
{
    UniValue result(UniValue::VSTR);
    auto str_value = std::to_string(value);
    result.setStr(str_value);
    return result;
}

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

UniValue UnivalueFromUint64(uint64_t value)
{
    UniValue result(UniValue::VSTR);
    auto str_value = std::to_string(value);
    result.setStr(str_value);
    return result;
}

static RPCHelpMan getordinalbytxoutput()
{
    return RPCHelpMan{"getordinalbytxoutput",
                "\nReturns the ordinal ranges for a specific transaction output.\n",
                {
                    {"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The transaction ID."},
                    {"vout", RPCArg::Type::NUM, RPCArg::Optional::NO, "The output index."},
                },
                RPCResult{
                    RPCResult::Type::ARR, "", "An array of ordinal ranges for the specified output.",
                    {
                        {RPCResult::Type::OBJ, "", "", {
                            {RPCResult::Type::NUM, "start", "The starting ordinal of the range."},
                            {RPCResult::Type::NUM, "end", "The ending ordinal of the range."},
                        }},
                    }
                },
                RPCExamples{
                    HelpExampleCli("getordinalbytxoutput", "\"txid\" 0")
                  + HelpExampleRpc("getordinalbytxoutput", "\"txid\", 0")
                },
                [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    if (!g_ordindex) {
        throw JSONRPCError(RPC_METHOD_NOT_FOUND, "Ordinal index is not available.");
    }

    uint256 txid = ParseHashV(request.params[0], "txid");
    uint32_t vout = Uint32FromUniValue(request.params[1]);


    std::vector<SatoshiRange> ranges;
    if (!g_ordindex->FindOrdByTxOutput(txid, vout, ranges)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "No ordinals found for this transaction output.");
    }

    UniValue result(UniValue::VARR);
    for (const auto& range : ranges) {
        UniValue obj(UniValue::VOBJ);
        auto start = std::to_string(range.start);
        auto end = std::to_string(range.end-1);
        obj.pushKV("start", start);
        obj.pushKV("end", end); // end is exclusive to match with standard ord theory 
        result.push_back(obj);
    }
    return result;
},
    };
}

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
        obj.pushKV("txid", txid_str);
        obj.pushKV("vout", vout_str);
        result.push_back(obj);
    }
    return result;
},
    };
}

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
    result.pushKV("txid", txid_str);
    result.pushKV("vout", vout_str);
    return result;
},
    };
}



void RegisterOrdinalRPCCommands(CRPCTable& t)
{
    static const CRPCCommand commands[]{
        {"ordinal", &getordinalbytxoutput},
        {"ordinal", &gettxoutputsbyordinal},
        {"ordinal", &getordinalposition},
    };
    for (const auto& c : commands) {
        t.appendCommand(c.name, &c);
    }
}
