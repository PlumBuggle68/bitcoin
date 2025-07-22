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
        auto end = std::to_string(range.end);
        obj.pushKV("start", start);
        obj.pushKV("end", end);
        result.push_back(obj);
    }
    return result;
},
    };
}

void RegisterOrdinalRPCCommands(CRPCTable& t)
{
    static const CRPCCommand commands[]{
        {"ordinal", &getordinalbytxoutput},
    };
    for (const auto& c : commands) {
        t.appendCommand(c.name, &c);
    }
}
