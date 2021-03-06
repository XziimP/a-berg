// Copyright 2019 The Beam Team
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include "wallet/core/wallet_db.h"
#include "nlohmann/json.hpp"
#include <string>

namespace beam::wallet
{
WalletAddress GenerateNewAddress(
        const IWalletDB::Ptr& walletDB,
        const std::string& label,
        WalletAddress::ExpirationStatus expirationStatus
            = WalletAddress::ExpirationStatus::OneDay,
        bool saveRequired = true);

bool ReadTreasury(ByteBuffer&, const std::string& sPath);

std::string TxIDToString(const TxID& txId);

void GetStatusResponseJson(const TxDescription& tx,
                           nlohmann::json& msg,
                           Height kernelProofHeight,
                           Height systemHeight);

}  // namespace beam::wallet
