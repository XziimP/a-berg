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

#include "base_transaction.h"
#include "core/block_crypt.h"

// TODO: getrandom not available until API 28 in the Android NDK 17b
// https://github.com/boostorg/uuid/issues/76
#if defined(__ANDROID__)
#define BOOST_UUID_RANDOM_PROVIDER_DISABLE_GETRANDOM 1
#endif

#include <boost/uuid/uuid_generators.hpp>
#include <numeric>
#include "utility/logger.h"

namespace beam::wallet
{
    using namespace ECC;
    using namespace std;

    TxID GenerateTxID()
    {
        boost::uuids::uuid id = boost::uuids::random_generator()();
        TxID txID{};
        copy(id.begin(), id.end(), txID.begin());
        return txID;
    }

    TxParameters CreateTransactionParameters(TxType type, const boost::optional<TxID>& oTxId)
    {
        const auto txID = oTxId ? *oTxId : GenerateTxID();
        return TxParameters(txID)
            .SetParameter(TxParameterID::TransactionType, type)
            .SetParameter(TxParameterID::Lifetime, kDefaultTxLifetime)
            .SetParameter(TxParameterID::PeerResponseTime, kDefaultTxResponseTime)
            .SetParameter(TxParameterID::IsInitiator, true)
            .SetParameter(TxParameterID::IsSender, true)
            .SetParameter(TxParameterID::CreateTime, getTimestamp());
    }

    std::string GetFailureMessage(TxFailureReason reason)
    {
        switch (reason)
        {
#define MACRO(name, code, message) case name: return message;
            BEAM_TX_FAILURE_REASON_MAP(MACRO)
#undef MACRO
        }
        return "Unknown reason";
    }

    TransactionFailedException::TransactionFailedException(bool notify, TxFailureReason reason, const char* message)
        : std::runtime_error(message)
        , m_Notify{ notify }
        , m_Reason{ reason }
    {
    }

    bool TransactionFailedException::ShouldNofify() const
    {
        return m_Notify;
    }

    TxFailureReason TransactionFailedException::GetReason() const
    {
        return m_Reason;
    }

    const uint32_t BaseTransaction::s_ProtoVersion = 4;

    BaseTransaction::BaseTransaction(INegotiatorGateway& gateway
        , IWalletDB::Ptr walletDB
        , const TxID& txID)
        : m_Gateway{ gateway }
        , m_WalletDB{ walletDB }
        , m_ID{ txID }
    {
        assert(walletDB);
    }

    bool BaseTransaction::IsInitiator() const
    {
        if (!m_IsInitiator.is_initialized())
        {
            m_IsInitiator = GetMandatoryParameter<bool>(TxParameterID::IsInitiator);
        }
        return *m_IsInitiator;
    }

    uint32_t BaseTransaction::get_PeerVersion() const
    {
        uint32_t nVer = 0;
        GetParameter(TxParameterID::PeerProtoVersion, nVer);
        return nVer;
    }

    bool BaseTransaction::GetTip(Block::SystemState::Full& state) const
    {
        return GetGateway().get_tip(state);
    }

    void BaseTransaction::UpdateAsync()
    {
        if (!m_EventToUpdate)
        {
            GetAsyncAcontext().OnAsyncStarted();
            m_EventToUpdate = io::AsyncEvent::create(io::Reactor::get_Current(), [this, weak = this->weak_from_this()]()
            { 
                auto eventHolder = m_EventToUpdate;
                if (auto tx = weak.lock())
                {
                    Update();
                    GetAsyncAcontext().OnAsyncFinished();
                }
            });
            m_EventToUpdate->post();
        }
    }

    const TxID& BaseTransaction::GetTxID() const
    {
        return m_ID;
    }

    void BaseTransaction::Update()
    {
        AsyncContextHolder async(m_Gateway);
        try
        {
            m_EventToUpdate.reset();
            if (CheckExternalFailures())
            {
                return;
            }

            UpdateImpl();

            CheckExpired();
            SetParameter(TxParameterID::ModifyTime, getTimestamp(), true);
        }
        catch (const TransactionFailedException & ex)
        {
            LOG_ERROR() << GetTxID() << " exception msg: " << ex.what();
            OnFailed(ex.GetReason(), ex.ShouldNofify());
        }
        catch (const exception & ex)
        {
            LOG_ERROR() << GetTxID() << " exception msg: " << ex.what();
            OnFailed(TxFailureReason::Unknown);
        }
    }

    bool BaseTransaction::CanCancel() const
    {
        TxStatus status = TxStatus::Failed;
        GetParameter(TxParameterID::Status, status);

        return status == TxStatus::InProgress || status == TxStatus::Pending;
    }

    void BaseTransaction::Cancel()
    {
        TxStatus s = TxStatus::Failed;
        GetParameter(TxParameterID::Status, s);
        // TODO: add CanCancel() method
        if (s == TxStatus::Pending || s == TxStatus::InProgress)
        {
            if (s == TxStatus::InProgress)
            {
                // notify about cancellation if we have started negotiations
                NotifyFailure(TxFailureReason::Canceled);

            }
            UpdateTxDescription(TxStatus::Canceled);
            RollbackTx();
            GetGateway().on_tx_completed(GetTxID());
        }
        else
        {
            LOG_INFO() << GetTxID() << " You cannot cancel transaction in state: " << static_cast<int>(s);
        }
    }

    bool BaseTransaction::Rollback(Height height)
    {
        Height proofHeight;

        if (GetParameter(TxParameterID::KernelProofHeight, proofHeight) && (proofHeight > height))
        {
            SetParameter(TxParameterID::Status, TxStatus::Registering);
            SetParameter(TxParameterID::KernelProofHeight, Height(0));
            SetParameter(TxParameterID::KernelUnconfirmedHeight, Height(0));
            return true;
        }
        return false;
    }

    void BaseTransaction::RollbackTx()
    {
        LOG_INFO() << GetTxID() << " Transaction failed. Rollback...";
        m_WalletDB->rollbackTx(GetTxID());
    }

    INegotiatorGateway& BaseTransaction::GetGateway() const
    {
        return m_Gateway;
    }

    bool BaseTransaction::CheckExpired()
    {
        TxStatus s = TxStatus::Failed;
        if (GetParameter(TxParameterID::Status, s)
            && (s == TxStatus::Failed
                || s == TxStatus::Canceled
                || s == TxStatus::Completed))
        {
            return false;
        }

        Height maxHeight = MaxHeight;
        if (!GetParameter(TxParameterID::MaxHeight, maxHeight)
            && !GetParameter(TxParameterID::PeerResponseHeight, maxHeight))
        {
            // we have no data to make decision
            return false;
        }

        uint8_t nRegistered = proto::TxStatus::Unspecified;
        Merkle::Hash kernelID;
        if (!GetParameter(TxParameterID::TransactionRegistered, nRegistered)
            || !GetParameter(TxParameterID::KernelID, kernelID))
        {
            Block::SystemState::Full state;
            if (GetTip(state) && state.m_Height > maxHeight)
            {
                LOG_INFO() << GetTxID() << " Transaction expired. Current height: " << state.m_Height << ", max kernel height: " << maxHeight;
                OnFailed(TxFailureReason::TransactionExpired);
                return true;
            }
        }
        else
        {
            Height lastUnconfirmedHeight = 0;
            if (GetParameter(TxParameterID::KernelUnconfirmedHeight, lastUnconfirmedHeight) && lastUnconfirmedHeight > 0)
            {
                if (lastUnconfirmedHeight >= maxHeight)
                {
                    LOG_INFO() << GetTxID() << " Transaction expired. Last unconfirmeed height: " << lastUnconfirmedHeight << ", max kernel height: " << maxHeight;
                    OnFailed(TxFailureReason::TransactionExpired);
                    return true;
                }
            }
        }
        return false;
    }

    bool BaseTransaction::CheckExternalFailures()
    {
        TxFailureReason reason = TxFailureReason::Unknown;
        if (GetParameter(TxParameterID::FailureReason, reason))
        {
            TxStatus s = GetMandatoryParameter<TxStatus>(TxParameterID::Status);
            if (s == TxStatus::InProgress)
            {
                OnFailed(reason);
                return true;
            }
        }
        return false;
    }

    void BaseTransaction::ConfirmKernel(const Merkle::Hash& kernelID)
    {
        UpdateTxDescription(TxStatus::Registering);
        GetGateway().confirm_kernel(GetTxID(), kernelID);
    }

    void BaseTransaction::UpdateOnNextTip()
    {
        GetGateway().UpdateOnNextTip(GetTxID());
    }

    void BaseTransaction::CompleteTx()
    {
        LOG_INFO() << GetTxID() << " Transaction completed";
        UpdateTxDescription(TxStatus::Completed);
        GetGateway().on_tx_completed(GetTxID());
    }

    void BaseTransaction::UpdateTxDescription(TxStatus s)
    {
        SetParameter(TxParameterID::Status, s, true);
    }

    void BaseTransaction::OnFailed(TxFailureReason reason, bool notify)
    {
        LOG_ERROR() << GetTxID() << " Failed. " << GetFailureMessage(reason);

        if (notify)
        {
            NotifyFailure(reason);
        }

        SetParameter(TxParameterID::FailureReason, reason, false);
        UpdateTxDescription((reason == TxFailureReason::Canceled) ? TxStatus::Canceled : TxStatus::Failed);
        RollbackTx();

        GetGateway().on_tx_completed(GetTxID());
    }

    IPrivateKeyKeeper2::Slot::Type BaseTransaction::GetSlotSafe(bool bAllocateIfAbsent)
    {
        IPrivateKeyKeeper2::Slot::Type iSlot = IPrivateKeyKeeper2::Slot::Invalid;
        GetParameter(TxParameterID::NonceSlot, iSlot);

        if (bAllocateIfAbsent && (IPrivateKeyKeeper2::Slot::Invalid == iSlot))
        {
            iSlot = m_WalletDB->SlotAllocate();
            SetParameter(TxParameterID::NonceSlot, iSlot);
        }

        return iSlot;
    }

    void BaseTransaction::FreeSlotSafe()
    {
        IPrivateKeyKeeper2::Slot::Type iSlot = GetSlotSafe(false);
        if (IPrivateKeyKeeper2::Slot::Invalid != iSlot)
        {
            m_WalletDB->SlotFree(iSlot);
            SetParameter(TxParameterID::NonceSlot, IPrivateKeyKeeper2::Slot::Invalid);
        }


    }

    void BaseTransaction::FreeResources()
    {
        FreeSlotSafe(); // if was used
    }

    void BaseTransaction::NotifyFailure(TxFailureReason reason)
    {
        TxStatus s = TxStatus::Failed;
        GetParameter(TxParameterID::Status, s);

        switch (s)
        {
        case TxStatus::Pending:
        case TxStatus::InProgress:
            // those are the only applicable statuses, where there's no chance tx can be valid
            break;
        default:
            return;
        }

        SetTxParameter msg;
        msg.AddParameter(TxParameterID::FailureReason, reason);
        SendTxParameters(move(msg));
    }

    IWalletDB::Ptr BaseTransaction::GetWalletDB()
    {
        return m_WalletDB;
    }

    IPrivateKeyKeeper2::Ptr BaseTransaction::get_KeyKeeperStrict()
    {
        IPrivateKeyKeeper2::Ptr ret = m_WalletDB->get_KeyKeeper();
        if (!ret)
            throw TransactionFailedException(true, TxFailureReason::NoKeyKeeper);

        return ret;
    }

    Key::IKdf::Ptr BaseTransaction::get_MasterKdfStrict()
    {
        Key::IKdf::Ptr ret = m_WalletDB->get_MasterKdf();
        if (!ret)
            throw TransactionFailedException(true, TxFailureReason::NoMasterKey);

        return ret;
    }

    void BaseTransaction::TestKeyKeeperRet(IPrivateKeyKeeper2::Status::Type n)
    {
        if (IPrivateKeyKeeper2::Status::Success != n)
            throw TransactionFailedException(true, KeyKeeperErrorToFailureReason(n));
    }

    TxFailureReason BaseTransaction::KeyKeeperErrorToFailureReason(IPrivateKeyKeeper2::Status::Type n)
    {
        if (IPrivateKeyKeeper2::Status::UserAbort == n)
            return TxFailureReason::KeyKeeperUserAbort;

        return TxFailureReason::KeyKeeperError;
    }

    IAsyncContext& BaseTransaction::GetAsyncAcontext() const
    {
        return GetGateway();
    }

    bool BaseTransaction::SendTxParameters(SetTxParameter && msg) const
    {
        msg.m_TxID = GetTxID();
        msg.m_Type = GetType();

        WalletID peerID;
        if (GetParameter(TxParameterID::MyID, msg.m_From)
            && GetParameter(TxParameterID::PeerID, peerID))
        { 
            PeerID secureWalletID = Zero, peerWalletID = Zero;
            if (GetParameter(TxParameterID::MySecureWalletID, secureWalletID) 
             && GetParameter(TxParameterID::PeerSecureWalletID, peerWalletID))
            {
                msg.AddParameter(TxParameterID::PeerSecureWalletID, secureWalletID);
            }
            GetGateway().send_tx_params(peerID, msg);
            return true;
        }
        return false;
    }

    void BaseTransaction::SetCompletedTxCoinStatuses(Height proofHeight)
    {
        std::vector<Coin> modified = GetWalletDB()->getCoinsByTx(GetTxID());
        for (auto& coin : modified)
        {
            bool bIn = (coin.m_createTxId == m_ID);
            bool bOut = (coin.m_spentTxId == m_ID);
            if (bIn || bOut)
            {
                if (bIn)
                {
                    std::setmin(coin.m_confirmHeight, proofHeight);
                    coin.m_maturity = proofHeight + Rules::get().Maturity.Std; // so far we don't use incubation for our created outputs
                }
                if (bOut)
                {
                    std::setmin(coin.m_spentHeight, proofHeight);
                }
            }
        }

        GetWalletDB()->saveCoins(modified);
    }
}