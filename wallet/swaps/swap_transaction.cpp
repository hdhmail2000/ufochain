// Copyright 2018 The UFO Team
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

#include "swap_transaction.h"

#include "bitcoin/bitcoin.hpp"

#include "lock_tx_builder.h"
#include "shared_tx_builder.h"
#include "wallet/bitcoin/bitcoin_side.h"
#include "wallet/wallet.h"

using namespace ECC;

namespace ufo::wallet
{
    /// Swap Parameters 
    TxParameters InitNewSwap(const WalletID& myID, Height minHeight, Amount amount, Amount fee, AtomicSwapCoin swapCoin,
        Amount swapAmount, bool isUfoSide /*= true*/,
        Height lifetime /*= kDefaultTxLifetime*/, Height responseTime/* = kDefaultTxResponseTime*/)
    {
        TxParameters parameters(GenerateTxID());

        parameters.SetParameter(TxParameterID::TransactionType, TxType::AtomicSwap);
        parameters.SetParameter(TxParameterID::CreateTime, getTimestamp());
        parameters.SetParameter(TxParameterID::Amount, amount);
        parameters.SetParameter(TxParameterID::Fee, fee);
        parameters.SetParameter(TxParameterID::Lifetime, lifetime);

        parameters.SetParameter(TxParameterID::MinHeight, minHeight);
        parameters.SetParameter(TxParameterID::PeerResponseTime, responseTime);
        parameters.SetParameter(TxParameterID::MyID, myID);
        parameters.SetParameter(TxParameterID::IsSender, isUfoSide);
        parameters.SetParameter(TxParameterID::IsInitiator, false);

        parameters.SetParameter(TxParameterID::AtomicSwapCoin, swapCoin);
        parameters.SetParameter(TxParameterID::AtomicSwapAmount, swapAmount);
        parameters.SetParameter(TxParameterID::AtomicSwapIsUfoSide, isUfoSide);

        return parameters;
    }

    TxParameters CreateSwapParameters()
    {
        return CreateTransactionParameters(TxType::AtomicSwap, GenerateTxID())
            .SetParameter(TxParameterID::IsInitiator, false);
    }

    TxParameters AcceptSwapParameters(const TxParameters& initialParameters, const WalletID& myID)
    {
        TxParameters parameters = initialParameters;

        parameters.SetParameter(TxParameterID::PeerID, *parameters.GetParameter<WalletID>(TxParameterID::MyID));
        parameters.SetParameter(TxParameterID::MyID, myID);

        bool isUfoSide = *parameters.GetParameter<bool>(TxParameterID::AtomicSwapIsUfoSide);

        parameters.SetParameter(TxParameterID::IsSender, !isUfoSide);
        parameters.SetParameter(TxParameterID::AtomicSwapIsUfoSide, !isUfoSide);
        parameters.SetParameter(TxParameterID::IsInitiator, true);

        return parameters;
    }
    ///
    AtomicSwapTransaction::WrapperSecondSide::WrapperSecondSide(ISecondSideProvider& gateway, BaseTransaction& tx)
        : m_gateway(gateway)
        , m_tx(tx)
    {
    }

    SecondSide::Ptr AtomicSwapTransaction::WrapperSecondSide::operator -> ()
    {
        return GetSecondSide();
    }

    SecondSide::Ptr AtomicSwapTransaction::WrapperSecondSide::GetSecondSide()
    {
        if (!m_secondSide)
        {
            m_secondSide = m_gateway.GetSecondSide(m_tx);

            if (!m_secondSide)
            {
                throw UninitilizedSecondSide();
            }
        }

        return m_secondSide;
    }

    ////////////
    // Creator
    AtomicSwapTransaction::Creator::Creator(IWalletDB::Ptr walletDB)
        : m_walletDB(walletDB)
    {

    }

    void AtomicSwapTransaction::Creator::RegisterFactory(AtomicSwapCoin coinType, ISecondSideFactory::Ptr factory)
    {
        m_factories.emplace(coinType, factory);
    }

    BaseTransaction::Ptr AtomicSwapTransaction::Creator::Create(INegotiatorGateway& gateway
                                                              , IWalletDB::Ptr walletDB
                                                              , IPrivateKeyKeeper::Ptr keyKeeper
                                                              , const TxID& txID)
    {
        return BaseTransaction::Ptr(new AtomicSwapTransaction(gateway, walletDB, keyKeeper, txID, *this));
    }

    SecondSide::Ptr AtomicSwapTransaction::Creator::GetSecondSide(BaseTransaction& tx)
    {
        AtomicSwapCoin coinType = tx.GetMandatoryParameter<AtomicSwapCoin>(TxParameterID::AtomicSwapCoin);
        auto it = m_factories.find(coinType);
        if (it == m_factories.end())
        {
            throw SecondSideFactoryNotRegisteredException();
        }
        bool isUfoSide = tx.GetMandatoryParameter<bool>(TxParameterID::AtomicSwapIsUfoSide);
        return it->second->CreateSecondSide(tx, isUfoSide);
    }

    TxParameters AtomicSwapTransaction::Creator::CheckAndCompleteParameters(const TxParameters& parameters)
    {
        auto peerID = parameters.GetParameter<WalletID>(TxParameterID::PeerID);
        if (peerID)
        {
            auto receiverAddr = m_walletDB->getAddress(*peerID);
            if (receiverAddr && receiverAddr->m_OwnID)
            {
                LOG_INFO() << "Failed to initiate the atomic swap. Not able to use own address as receiver's.";
                throw FailToStartSwapException();
            }
        }
        return parameters;
    }

    AtomicSwapTransaction::AtomicSwapTransaction(INegotiatorGateway& gateway
                                               , IWalletDB::Ptr walletDB
                                               , IPrivateKeyKeeper::Ptr keyKeeper
                                               , const TxID& txID
                                               , ISecondSideProvider& secondSideProvider)
        : BaseTransaction(gateway, walletDB, keyKeeper, txID)
        , m_secondSide(secondSideProvider, *this)
    {
    }

    void AtomicSwapTransaction::Cancel()
    {
        State state = GetState(kDefaultSubTxID);

        switch (state)
        {
        case State::HandlingContractTX:
            if (!IsUfoSide())
            {
                break;
            }
        case State::Initial:
        case State::BuildingUfoLockTX:
        case State::BuildingUfoRedeemTX:
        case State::BuildingUfoRefundTX:
        {
            SetNextState(State::Canceled);
            return;
        }
        default:
            break;
        }

        LOG_INFO() << GetTxID() << " You cannot cancel transaction in state: " << static_cast<int>(state);
    }

    bool AtomicSwapTransaction::Rollback(Height height)
    {
        Height proofHeight = 0;
        bool isRolledback = false;

        if (IsUfoSide())
        {
            if (GetParameter(TxParameterID::KernelProofHeight, proofHeight, SubTxIndex::UFO_REFUND_TX)
                && proofHeight > height)
            {
                SetParameter(TxParameterID::KernelProofHeight, Height(0), false, SubTxIndex::UFO_REFUND_TX);
                SetParameter(TxParameterID::KernelUnconfirmedHeight, Height(0), false, SubTxIndex::UFO_REFUND_TX);

                SetState(State::SendingUfoRefundTX);
                isRolledback = true;
            }

            if (GetParameter(TxParameterID::KernelProofHeight, proofHeight, SubTxIndex::UFO_LOCK_TX)
                && proofHeight > height)
            {
                SetParameter(TxParameterID::KernelProofHeight, Height(0), false, SubTxIndex::UFO_LOCK_TX);
                SetParameter(TxParameterID::KernelUnconfirmedHeight, Height(0), false, SubTxIndex::UFO_LOCK_TX);

                SetState(State::SendingUfoLockTX);
                isRolledback = true;
            }
        }
        else
        {
            if (GetParameter(TxParameterID::KernelProofHeight, proofHeight, SubTxIndex::UFO_REDEEM_TX) 
                && proofHeight > height)
            {
                SetParameter(TxParameterID::KernelProofHeight, Height(0), false, SubTxIndex::UFO_REDEEM_TX);
                SetParameter(TxParameterID::KernelUnconfirmedHeight, Height(0), false, SubTxIndex::UFO_REDEEM_TX);

                SetState(State::SendingUfoRedeemTX);
                isRolledback = true;
            }
        }

        if (isRolledback)
        {
            UpdateTxDescription(TxStatus::InProgress);
        }

        return isRolledback;
    }

    void AtomicSwapTransaction::SetNextState(State state)
    {
        SetState(state);
        UpdateAsync();
    }

    TxType AtomicSwapTransaction::GetType() const
    {
        return TxType::AtomicSwap;
    }

    AtomicSwapTransaction::State AtomicSwapTransaction::GetState(SubTxID subTxID) const
    {
        State state = State::Initial;
        GetParameter(TxParameterID::State, state, subTxID);
        return state;
    }

    AtomicSwapTransaction::SubTxState AtomicSwapTransaction::GetSubTxState(SubTxID subTxID) const
    {
        SubTxState state = SubTxState::Initial;
        GetParameter(TxParameterID::State, state, subTxID);
        return state;
    }

    Amount AtomicSwapTransaction::GetWithdrawFee() const
    {
        // TODO(alex.starun): implement fee calculation
        return kMinFeeInGroth;
    }

    void AtomicSwapTransaction::UpdateImpl()
    {
        try
        {
            CheckSubTxFailures();

            State state = GetState(kDefaultSubTxID);
            bool isUfoOwner = IsUfoSide();

            switch (state)
            {
            case State::Initial:
            {
                if (Height responseHeight = MaxHeight; !GetParameter(TxParameterID::PeerResponseHeight, responseHeight))
                {
                    Height minHeight = GetMandatoryParameter<Height>(TxParameterID::MinHeight);
                    Height responseTime = GetMandatoryParameter<Height>(TxParameterID::PeerResponseTime);
                    SetParameter(TxParameterID::PeerResponseHeight, minHeight + responseTime);
                }

                if (IsInitiator())
                {
                    if (!m_secondSide->Initialize())
                    {
                        break;
                    }

                    m_secondSide->InitLockTime();
                    SendInvitation();
                    LOG_INFO() << GetTxID() << " Invitation sent.";
                }
                else
                {
                    // TODO: refactor this
                    // hack, used for increase refCount!
                    auto secondSide = m_secondSide.GetSecondSide();

                    Height lockTime = 0;
                    if (!GetParameter(TxParameterID::AtomicSwapExternalLockTime, lockTime))
                    {
                        //we doesn't have an answer from other participant
                        UpdateOnNextTip();
                        break;
                    }

                    if (!secondSide->Initialize())
                    {
                        break;
                    }

                    if (!secondSide->ValidateLockTime())
                    {
                        LOG_ERROR() << GetTxID() << "[" << static_cast<SubTxID>(SubTxIndex::LOCK_TX) << "] " << "Lock height is unacceptable.";
                        OnSubTxFailed(TxFailureReason::InvalidTransaction, SubTxIndex::LOCK_TX, true);
                        break;
                    }
                }

                SetNextState(State::BuildingUfoLockTX);
                break;
            }
            case State::BuildingUfoLockTX:
            {
                auto lockTxState = BuildUfoLockTx();
                if (lockTxState != SubTxState::Constructed)
                {
                    UpdateOnNextTip();
                    break;
                }
                LOG_INFO() << GetTxID() << " Ufo LockTX constructed.";
                SetNextState(State::BuildingUfoRefundTX);
                break;
            }
            case State::BuildingUfoRefundTX:
            {
                auto subTxState = BuildUfoWithdrawTx(SubTxIndex::UFO_REFUND_TX, m_WithdrawTx);
                if (subTxState != SubTxState::Constructed)
                    break;

                m_WithdrawTx.reset();
                LOG_INFO() << GetTxID() << " Ufo RefundTX constructed.";
                SetNextState(State::BuildingUfoRedeemTX);
                break;
            }
            case State::BuildingUfoRedeemTX:
            {
                auto subTxState = BuildUfoWithdrawTx(SubTxIndex::UFO_REDEEM_TX, m_WithdrawTx);
                if (subTxState != SubTxState::Constructed)
                    break;

                m_WithdrawTx.reset();
                LOG_INFO() << GetTxID() << " Ufo RedeemTX constructed.";
                SetNextState(State::HandlingContractTX);
                break;
            }
            case State::HandlingContractTX:
            {
                if (!isUfoOwner)
                {
                    if (!m_secondSide->HasEnoughTimeToProcessLockTx())
                    {
                        OnFailed(NotEnoughTimeToFinishBtcTx, true);
                        break;
                    }
                    
                    if (!m_secondSide->SendLockTx())
                        break;

                    SendExternalTxDetails();

                    // Ufo LockTx: switch to the state of awaiting for proofs
                    uint8_t nCode = proto::TxStatus::Ok; // compiler workaround (ref to static const)
                    SetParameter(TxParameterID::TransactionRegistered, nCode, false, SubTxIndex::UFO_LOCK_TX);
                }
                else
                {
                    if (!m_secondSide->ConfirmLockTx())
                    {
                        UpdateOnNextTip();
                        break;
                    }
                }

                LOG_INFO() << GetTxID() << " LockTX completed.";
                SetNextState(State::SendingUfoLockTX);
                break;
            }
            case State::SendingRefundTX:
            {
                assert(!isUfoOwner);

                if (!m_secondSide->IsLockTimeExpired())
                {
                    UpdateOnNextTip();
                    break;
                }

                if (!m_secondSide->SendRefund())
                    break;

                if (!m_secondSide->ConfirmRefundTx())
                {
                    UpdateOnNextTip();
                    break;
                }

                LOG_INFO() << GetTxID() << " RefundTX completed!";
                SetNextState(State::Refunded);
                break;
            }
            case State::SendingRedeemTX:
            {
                assert(isUfoOwner);
                if (!m_secondSide->SendRedeem())
                    break;

                if (!m_secondSide->ConfirmRedeemTx())
                {
                    UpdateOnNextTip();
                    break;
                }

                LOG_INFO() << GetTxID() << " RedeemTX completed!";
                SetNextState(State::CompleteSwap);
                break;
            }
            case State::SendingUfoLockTX:
            {
                if (!m_LockTx && isUfoOwner)
                {
                    BuildUfoLockTx();
                }

                if (m_LockTx && !SendSubTx(m_LockTx, SubTxIndex::UFO_LOCK_TX))
                    break;

                if (!isUfoOwner && m_secondSide->IsLockTimeExpired())
                {
                    LOG_INFO() << GetTxID() << " Locktime is expired.";
                    SetNextState(State::SendingRefundTX);
                    break;
                }

                if (!CompleteSubTx(SubTxIndex::UFO_LOCK_TX))
                    break;

                LOG_INFO() << GetTxID() << " Ufo LockTX completed.";
                SetNextState(State::SendingUfoRedeemTX);
                break;
            }
            case State::SendingUfoRedeemTX:
            {
                if (isUfoOwner)
                {
                    UpdateOnNextTip();

                    if (IsUfoLockTimeExpired())
                    {
                        // If we already got SecretPrivateKey for RedeemTx, don't send refundTx,
                        // because it looks like we got rollback and we just should rerun TX's.
                        NoLeak<uintBig> secretPrivateKey;
                        if (!GetParameter(TxParameterID::AtomicSwapSecretPrivateKey, secretPrivateKey.V, SubTxIndex::UFO_REDEEM_TX))
                        {
                            LOG_INFO() << GetTxID() << " Ufo locktime expired.";
                            SetNextState(State::SendingUfoRefundTX);
                            break;
                        }
                    }

                    // request kernel body for getting secretPrivateKey
                    if (!GetKernelFromChain(SubTxIndex::UFO_REDEEM_TX))
                        break;

                    ExtractSecretPrivateKey();

                    // Redeem second Coin
                    SetNextState(State::SendingRedeemTX);
                }
                else
                {
                    if (!CompleteUfoWithdrawTx(SubTxIndex::UFO_REDEEM_TX))
                        break;

                    LOG_INFO() << GetTxID() << " Ufo RedeemTX completed!";
                    SetNextState(State::CompleteSwap);
                }
                break;
            }
            case State::SendingUfoRefundTX:
            {
                assert(isUfoOwner);
                if (!IsUfoLockTimeExpired())
                {
                    UpdateOnNextTip();
                    break;
                }

                if (!CompleteUfoWithdrawTx(SubTxIndex::UFO_REFUND_TX))
                    break;

                LOG_INFO() << GetTxID() << " Ufo Refund TX completed!";
                SetNextState(State::Refunded);
                break;
            }
            case State::CompleteSwap:
            {
                LOG_INFO() << GetTxID() << " Swap completed.";
                UpdateTxDescription(TxStatus::Completed);
                GetGateway().on_tx_completed(GetTxID());
                break;
            }
            case State::Canceled:
            {
                LOG_INFO() << GetTxID() << " Transaction cancelled.";
                NotifyFailure(TxFailureReason::Canceled);
                UpdateTxDescription(TxStatus::Canceled);

                RollbackTx();

                GetGateway().on_tx_completed(GetTxID());
                break;
            }
            case State::Failed:
            {
                TxFailureReason reason = TxFailureReason::Unknown;
                if (GetParameter(TxParameterID::FailureReason, reason))
                {
                    if (reason == TxFailureReason::Canceled)
                    {
                        LOG_ERROR() << GetTxID() << " Swap cancelled. The other side has cancelled the transaction.";
                    }
                    else
                    {
                        LOG_ERROR() << GetTxID() << " The other side has failed the transaction. Reason: " << GetFailureMessage(reason);
                    }
                }
                else
                {
                    LOG_ERROR() << GetTxID() << " Transaction failed.";
                }
                UpdateTxDescription(TxStatus::Failed);
                GetGateway().on_tx_completed(GetTxID());
                break;
            }

            case State::Refunded:
            {
                LOG_INFO() << GetTxID() << " Swap has not succeeded.";
                UpdateTxDescription(TxStatus::Failed);
                GetGateway().on_tx_completed(GetTxID());
                break;
            }

            default:
                break;
            }
        }
        catch (const UninitilizedSecondSide&)
        {
        }
    }

    void AtomicSwapTransaction::RollbackTx()
    {
        LOG_INFO() << GetTxID() << " Rollback...";

        GetWalletDB()->rollbackTx(GetTxID());
    }

    void AtomicSwapTransaction::NotifyFailure(TxFailureReason reason)
    {
        SetTxParameter msg;
        msg.AddParameter(TxParameterID::FailureReason, reason);
        SendTxParameters(std::move(msg));
    }

    void AtomicSwapTransaction::OnFailed(TxFailureReason reason, bool notify)
    {
        LOG_ERROR() << GetTxID() << " Failed. " << GetFailureMessage(reason);

        if (notify)
        {
            NotifyFailure(reason);
        }

        SetParameter(TxParameterID::InternalFailureReason, reason, false);

        State state = GetState(kDefaultSubTxID);
        bool isUfoSide = IsUfoSide();

        switch (state)
        {
        case State::Initial:
        {
            break;
        }
        case State::BuildingUfoLockTX:
        case State::BuildingUfoRedeemTX:
        case State::BuildingUfoRefundTX:
        {
            RollbackTx();

            break;
        }
        case State::HandlingContractTX:
        {
            RollbackTx();
            
            break;
        }
        case State::SendingUfoLockTX:
        {
            if (isUfoSide)
            {
                RollbackTx();
                break;
            }
            else
            {
                SetNextState(State::SendingRefundTX);
                return;
            }
        }
        case State::SendingUfoRedeemTX:
        {
            if (isUfoSide)
            {
                assert(false && "Impossible case!");
                return;
            }
            else
            {
                SetNextState(State::SendingRefundTX);
                return;
            }
        }
        case State::SendingRedeemTX:
        {            
            if (isUfoSide)
            {
                LOG_ERROR() << GetTxID() << " Unexpected error.";
                return;
            }
            else
            {
                assert(false && "Impossible case!");
                return;
            }
            break;
        }
        default:
            return;
        }

        SetNextState(State::Failed);
    }

    bool AtomicSwapTransaction::CheckExpired()
    {
        TxFailureReason reason = TxFailureReason::Unknown;
        if (GetParameter(TxParameterID::InternalFailureReason, reason))
        {
            return false;
        }

        TxStatus s = TxStatus::Failed;
        if (GetParameter(TxParameterID::Status, s)
            && (s == TxStatus::Failed
                || s == TxStatus::Canceled
                || s == TxStatus::Completed))
        {
            return false;
        }

        Height lockTxMaxHeight = MaxHeight;
        if (!GetParameter(TxParameterID::MaxHeight, lockTxMaxHeight, SubTxIndex::UFO_LOCK_TX)
            && !GetParameter(TxParameterID::PeerResponseHeight, lockTxMaxHeight))
        {
            return false;
        }

        uint8_t nRegistered = proto::TxStatus::Unspecified;
        Merkle::Hash kernelID;
        if (!GetParameter(TxParameterID::TransactionRegistered, nRegistered, SubTxIndex::UFO_LOCK_TX)
            || !GetParameter(TxParameterID::KernelID, kernelID, SubTxIndex::UFO_LOCK_TX))
        {
            Block::SystemState::Full state;
            if (GetTip(state) && state.m_Height > lockTxMaxHeight)
            {
                LOG_INFO() << GetTxID() << " Transaction expired. Current height: " << state.m_Height << ", max kernel height: " << lockTxMaxHeight;
                OnFailed(TxFailureReason::TransactionExpired, false);
                return true;
            }
        }
        else
        {
            Height lastUnconfirmedHeight = 0;
            if (GetParameter(TxParameterID::KernelUnconfirmedHeight, lastUnconfirmedHeight, SubTxIndex::UFO_LOCK_TX) && lastUnconfirmedHeight > 0)
            {
                if (lastUnconfirmedHeight >= lockTxMaxHeight)
                {
                    LOG_INFO() << GetTxID() << " Transaction expired. Last unconfirmed height: " << lastUnconfirmedHeight << ", max kernel height: " << lockTxMaxHeight;
                    OnFailed(TxFailureReason::TransactionExpired, false);
                    return true;
                }
            }
        }
        return false;
    }

    bool AtomicSwapTransaction::CheckExternalFailures()
    {
        TxFailureReason reason = TxFailureReason::Unknown;
        if (GetParameter(TxParameterID::FailureReason, reason))
        {
            State state = GetState(kDefaultSubTxID);

            switch (state)
            {
            case State::Initial:
            {
                SetState(State::Failed);
                break;
            }
            case State::BuildingUfoLockTX:
            case State::BuildingUfoRedeemTX:
            case State::BuildingUfoRefundTX:
            {
                RollbackTx();
                SetState(State::Failed);
                break;
            }
            case State::HandlingContractTX:
            {
                if (IsUfoSide())
                {
                    RollbackTx();
                    SetState(State::Failed);
                }

                break;
            }
            case State::SendingUfoLockTX:
            {
                // nothing
                break;
            }
            case State::SendingUfoRedeemTX:
            {
                // nothing
                break;
            }
            case State::SendingRedeemTX:
            {
                // nothing
                break;
            }
            default:
                break;
            }
        }
        return false;
    }

    bool AtomicSwapTransaction::CompleteUfoWithdrawTx(SubTxID subTxID)
    {
        if (!m_WithdrawTx)
        {
            BuildUfoWithdrawTx(subTxID, m_WithdrawTx);
        }

        if (m_WithdrawTx && !SendSubTx(m_WithdrawTx, subTxID))
        {
            return false;
        }

        if (!CompleteSubTx(subTxID))
        {
            return false;
        }

        return true;
    }

    AtomicSwapTransaction::SubTxState AtomicSwapTransaction::BuildUfoLockTx()
    {
        // load state
        SubTxState lockTxState = SubTxState::Initial;
        GetParameter(TxParameterID::State, lockTxState, SubTxIndex::UFO_LOCK_TX);

        bool isUfoOwner = IsUfoSide();
        Amount fee = 0;
        if (!GetParameter<Amount>(TxParameterID::Fee, fee, SubTxIndex::UFO_LOCK_TX))
        {
            // Ufo owner extract fee from main TX, receiver must get fee along with LockTX invitation
            if (isUfoOwner && lockTxState == SubTxState::Initial)
            {
                fee = GetMandatoryParameter<Amount>(TxParameterID::Fee);
                SetParameter(TxParameterID::Fee, fee, false, SubTxIndex::UFO_LOCK_TX);
            }
        }

        auto lockTxBuilder = std::make_shared<LockTxBuilder>(*this, GetAmount(), fee);

        if (!lockTxBuilder->GetInitialTxParams() && lockTxState == SubTxState::Initial)
        {
            if (isUfoOwner)
            {
                Height maxResponseHeight = 0;
                if (GetParameter(TxParameterID::PeerResponseHeight, maxResponseHeight))
                {
                    LOG_INFO() << GetTxID() << "[" << static_cast<SubTxID>(SubTxIndex::UFO_LOCK_TX) << "]"
                        << " Max height for response: " << maxResponseHeight;
                }

                lockTxBuilder->SelectInputs();
                lockTxBuilder->AddChange();
            }

            UpdateTxDescription(TxStatus::InProgress);

            lockTxBuilder->GenerateOffset();
        }

        lockTxBuilder->CreateInputs();
        if (isUfoOwner && lockTxBuilder->CreateOutputs())
        {
            return lockTxState;
        }

        lockTxBuilder->GenerateNonce();
        lockTxBuilder->LoadSharedParameters();

        if (!lockTxBuilder->UpdateMaxHeight())
        {
            OnSubTxFailed(TxFailureReason::MaxHeightIsUnacceptable, SubTxIndex::UFO_LOCK_TX, true);
            return lockTxState;
        }

        if (!lockTxBuilder->GetPeerPublicExcessAndNonce())
        {
            if (lockTxState == SubTxState::Initial && isUfoOwner)
            {
                if (!IsInitiator())
                {
                    // When swap started not from Ufo side, we should save MaxHeight
                    SetParameter(TxParameterID::MaxHeight, lockTxBuilder->GetMaxHeight(), false, SubTxIndex::UFO_LOCK_TX);
                }

                SendLockTxInvitation(*lockTxBuilder);
                SetState(SubTxState::Invitation, SubTxIndex::UFO_LOCK_TX);
                lockTxState = SubTxState::Invitation;
            }
            return lockTxState;
        }

        assert(fee);
        lockTxBuilder->CreateKernel();
        lockTxBuilder->SignPartial();

        if (lockTxState == SubTxState::Initial || lockTxState == SubTxState::Invitation)
        {
            if (!lockTxBuilder->CreateSharedUTXOProofPart2(isUfoOwner))
            {
                OnSubTxFailed(TxFailureReason::FailedToCreateMultiSig, SubTxIndex::UFO_LOCK_TX, true);
                return lockTxState;
            }

            if (!lockTxBuilder->CreateSharedUTXOProofPart3(isUfoOwner))
            {
                OnSubTxFailed(TxFailureReason::FailedToCreateMultiSig, SubTxIndex::UFO_LOCK_TX, true);
                return lockTxState;
            }

            SetState(SubTxState::Constructed, SubTxIndex::UFO_LOCK_TX);
            lockTxState = SubTxState::Constructed;

            if (!isUfoOwner)
            {
                // send part2/part3!
                SendLockTxConfirmation(*lockTxBuilder);
                return lockTxState;
            }
        }

        if (!lockTxBuilder->GetPeerSignature())
        {
            return lockTxState;
        }

        if (!lockTxBuilder->IsPeerSignatureValid())
        {
            OnSubTxFailed(TxFailureReason::InvalidPeerSignature, SubTxIndex::UFO_LOCK_TX, true);
            return lockTxState;
        }

        lockTxBuilder->FinalizeSignature();

        if (isUfoOwner)
        {
            assert(lockTxState == SubTxState::Constructed);
            // Create TX
            auto transaction = lockTxBuilder->CreateTransaction();
            TxBase::Context::Params pars;
            TxBase::Context context(pars);
            context.m_Height.m_Min = lockTxBuilder->GetMinHeight();
            if (!transaction->IsValid(context))
            {
                OnSubTxFailed(TxFailureReason::InvalidTransaction, SubTxIndex::UFO_LOCK_TX, true);
                return lockTxState;
            }

            // TODO: return
            m_LockTx = transaction;
        }

        return lockTxState;
    }

    AtomicSwapTransaction::SubTxState AtomicSwapTransaction::BuildUfoWithdrawTx(SubTxID subTxID, Transaction::Ptr& resultTx)
    {
        SubTxState subTxState = GetSubTxState(subTxID);

        Amount withdrawFee = 0;
        Amount withdrawAmount = 0;

        if (!GetParameter(TxParameterID::Amount, withdrawAmount, subTxID) ||
            !GetParameter(TxParameterID::Fee, withdrawFee, subTxID))
        {
            withdrawFee = GetWithdrawFee();
            withdrawAmount = GetAmount() - withdrawFee;

            SetParameter(TxParameterID::Amount, withdrawAmount, subTxID);
            SetParameter(TxParameterID::Fee, withdrawFee, subTxID);
        }

        bool isTxOwner = (IsUfoSide() && (SubTxIndex::UFO_REFUND_TX == subTxID)) || (!IsUfoSide() && (SubTxIndex::UFO_REDEEM_TX == subTxID));
        SharedTxBuilder builder{ *this, subTxID, withdrawAmount, withdrawFee };

        if (!builder.GetSharedParameters())
        {
            return subTxState;
        }

        // send invite to get 
        if (!builder.GetInitialTxParams() && subTxState == SubTxState::Initial)
        {
            builder.InitTx(isTxOwner);
        }

        builder.GenerateNonce();
        builder.CreateKernel();

        if (!builder.GetPeerPublicExcessAndNonce())
        {
            if (subTxState == SubTxState::Initial && isTxOwner)
            {
                SendSharedTxInvitation(builder);
                SetState(SubTxState::Invitation, subTxID);
                subTxState = SubTxState::Invitation;
            }
            return subTxState;
        }

        builder.SignPartial();

        if (!builder.GetPeerSignature())
        {
            if (subTxState == SubTxState::Initial && !isTxOwner)
            {
                // invited participant
                ConfirmSharedTxInvitation(builder);

                if (subTxID == SubTxIndex::UFO_REFUND_TX)
                {
                    SetState(SubTxState::Constructed, subTxID);
                    subTxState = SubTxState::Constructed;
                }
            }
            return subTxState;
        }

        if (subTxID == SubTxIndex::UFO_REDEEM_TX)
        {
            if (IsUfoSide())
            {
                // save SecretPublicKey
                {
                    auto peerPublicNonce = GetMandatoryParameter<Point::Native>(TxParameterID::PeerPublicNonce, subTxID);
                    Scalar::Native challenge;
                    {
                        Point::Native publicNonceNative = builder.GetPublicNonce() + peerPublicNonce;
                        Point publicNonce;
                        publicNonceNative.Export(publicNonce);

                        // Signature::get_Challenge(e, m_NoncePub, msg);
                        uintBig message;
                        builder.GetKernel().get_Hash(message);

                        Oracle() << publicNonce << message >> challenge;
                    }

                    Scalar::Native peerSignature = GetMandatoryParameter<Scalar::Native>(TxParameterID::PeerSignature, subTxID);
                    auto peerPublicExcess = GetMandatoryParameter<Point::Native>(TxParameterID::PeerPublicExcess, subTxID);

                    Point::Native pt = Context::get().G * peerSignature;

                    pt += peerPublicExcess * challenge;
                    pt += peerPublicNonce;
                    assert(!(pt == Zero));

                    Point secretPublicKey;
                    pt.Export(secretPublicKey);

                    SetParameter(TxParameterID::AtomicSwapSecretPublicKey, secretPublicKey, subTxID);
                }

                SetState(SubTxState::Constructed, subTxID);
                return SubTxState::Constructed;
            }
            else
            {
                // Send BTC side partial sign with secret
                auto partialSign = builder.GetPartialSignature();
                Scalar secretPrivateKey;
                GetParameter(TxParameterID::AtomicSwapSecretPrivateKey, secretPrivateKey.m_Value, SubTxIndex::UFO_REDEEM_TX);
                partialSign += secretPrivateKey;

                SetTxParameter msg;
                msg.AddParameter(TxParameterID::SubTxIndex, builder.GetSubTxID())
                    .AddParameter(TxParameterID::PeerSignature, partialSign);

                if (!SendTxParameters(std::move(msg)))
                {
                    OnFailed(TxFailureReason::FailedToSendParameters, false);
                    return subTxState;
                }
            }
        }

        if (!builder.IsPeerSignatureValid())
        {
            OnSubTxFailed(TxFailureReason::InvalidPeerSignature, subTxID, true);
            return subTxState;
        }

        builder.FinalizeSignature();

        SetState(SubTxState::Constructed, subTxID);
        subTxState = SubTxState::Constructed;

        if (isTxOwner)
        {
            auto transaction = builder.CreateTransaction();
            TxBase::Context::Params pars;
            TxBase::Context context(pars);
            context.m_Height.m_Min = builder.GetMinHeight();
            if (!transaction->IsValid(context))
            {
                OnSubTxFailed(TxFailureReason::InvalidTransaction, subTxID, true);
                return subTxState;
            }
            resultTx = transaction;
        }

        return subTxState;
    }

    bool AtomicSwapTransaction::SendSubTx(Transaction::Ptr transaction, SubTxID subTxID)
    {
    	uint8_t nRegistered = proto::TxStatus::Unspecified;
        if (!GetParameter(TxParameterID::TransactionRegistered, nRegistered, subTxID))
        {
            GetGateway().register_tx(GetTxID(), transaction, subTxID);
            return false;
        }

        if (proto::TxStatus::InvalidContext == nRegistered) // we have to ensure that this transaction hasn't already added to blockchain)
        {
            Height lastUnconfirmedHeight = 0;
            if (GetParameter(TxParameterID::KernelUnconfirmedHeight, lastUnconfirmedHeight) && lastUnconfirmedHeight > 0)
            {
                OnSubTxFailed(TxFailureReason::FailedToRegister, subTxID, subTxID == SubTxIndex::UFO_LOCK_TX);
                return false;
            }
        } 
        else if (proto::TxStatus::Ok != nRegistered)
        {
            OnSubTxFailed(TxFailureReason::FailedToRegister, subTxID, subTxID == SubTxIndex::UFO_LOCK_TX);
            return false;
        }

        return true;
    }

    bool AtomicSwapTransaction::IsUfoLockTimeExpired() const
    {
        Height lockTimeHeight = MaxHeight;
        GetParameter(TxParameterID::MinHeight, lockTimeHeight);

        Block::SystemState::Full state;

        return GetTip(state) && state.m_Height > (lockTimeHeight + kUfoLockTimeInBlocks);
    }

    bool AtomicSwapTransaction::CompleteSubTx(SubTxID subTxID)
    {
        Height hProof = 0;
        GetParameter(TxParameterID::KernelProofHeight, hProof, subTxID);
        if (!hProof)
        {
            Merkle::Hash kernelID = GetMandatoryParameter<Merkle::Hash>(TxParameterID::KernelID, subTxID);
            GetGateway().confirm_kernel(GetTxID(), kernelID, subTxID);
            return false;
        }

        if ((SubTxIndex::UFO_REDEEM_TX == subTxID) || (SubTxIndex::UFO_REFUND_TX == subTxID))
        {
            // store Coin in DB
            auto amount = GetMandatoryParameter<Amount>(TxParameterID::Amount, subTxID);
            Coin withdrawUtxo(amount);

            withdrawUtxo.m_createTxId = GetTxID();
            withdrawUtxo.m_ID = GetMandatoryParameter<Coin::ID>(TxParameterID::SharedCoinID, subTxID);

            GetWalletDB()->saveCoin(withdrawUtxo);
        }

        SetCompletedTxCoinStatuses(hProof);

        return true;
    }

    bool AtomicSwapTransaction::GetKernelFromChain(SubTxID subTxID) const
    {
        Height hProof = 0;
        GetParameter(TxParameterID::KernelProofHeight, hProof, subTxID);

        if (!hProof)
        {
            Merkle::Hash kernelID = GetMandatoryParameter<Merkle::Hash>(TxParameterID::KernelID, SubTxIndex::UFO_REDEEM_TX);
            GetGateway().get_kernel(GetTxID(), kernelID, subTxID);
            return false;
        }

        return true;
    }

    Amount AtomicSwapTransaction::GetAmount() const
    {
        if (!m_Amount.is_initialized())
        {
            m_Amount = GetMandatoryParameter<Amount>(TxParameterID::Amount);
        }
        return *m_Amount;
    }

    bool AtomicSwapTransaction::IsSender() const
    {
        if (!m_IsSender.is_initialized())
        {
            m_IsSender = GetMandatoryParameter<bool>(TxParameterID::IsSender);
        }
        return *m_IsSender;
    }

    bool AtomicSwapTransaction::IsUfoSide() const
    {
        if (!m_IsUfoSide.is_initialized())
        {
            bool isUfoSide = false;
            GetParameter(TxParameterID::AtomicSwapIsUfoSide, isUfoSide);
            m_IsUfoSide = isUfoSide;
        }
        return *m_IsUfoSide;
    }

    void AtomicSwapTransaction::SendInvitation()
    {
        auto swapAmount = GetMandatoryParameter<Amount>(TxParameterID::AtomicSwapAmount);
        auto swapCoin = GetMandatoryParameter<AtomicSwapCoin>(TxParameterID::AtomicSwapCoin);
        auto swapPublicKey = GetMandatoryParameter<std::string>(TxParameterID::AtomicSwapPublicKey);
        auto swapLockTime = GetMandatoryParameter<Timestamp>(TxParameterID::AtomicSwapExternalLockTime);
        auto lifetime = GetMandatoryParameter<Height>(TxParameterID::Lifetime);

        // send invitation
        SetTxParameter msg;
        msg.AddParameter(TxParameterID::Amount, GetAmount())
            .AddParameter(TxParameterID::Fee, GetMandatoryParameter<Amount>(TxParameterID::Fee))
            .AddParameter(TxParameterID::IsSender, !IsSender())
            .AddParameter(TxParameterID::Lifetime, lifetime)
            .AddParameter(TxParameterID::AtomicSwapAmount, swapAmount)
            .AddParameter(TxParameterID::AtomicSwapCoin, swapCoin)
            .AddParameter(TxParameterID::AtomicSwapPeerPublicKey, swapPublicKey)
            .AddParameter(TxParameterID::AtomicSwapExternalLockTime, swapLockTime)
            .AddParameter(TxParameterID::AtomicSwapIsUfoSide, !IsUfoSide())
            .AddParameter(TxParameterID::PeerProtoVersion, s_ProtoVersion);

        if (!SendTxParameters(std::move(msg)))
        {
            OnFailed(TxFailureReason::FailedToSendParameters, false);
        }
    }

    void AtomicSwapTransaction::SendExternalTxDetails()
    {
        SetTxParameter msg;
        m_secondSide->AddTxDetails(msg);

        if (!SendTxParameters(std::move(msg)))
        {
            OnFailed(TxFailureReason::FailedToSendParameters, false);
        }
    }

    void AtomicSwapTransaction::SendLockTxInvitation(const LockTxBuilder& lockBuilder)
    {
        auto swapPublicKey = GetMandatoryParameter<std::string>(TxParameterID::AtomicSwapPublicKey);

        SetTxParameter msg;
        msg.AddParameter(TxParameterID::PeerProtoVersion, s_ProtoVersion)
            .AddParameter(TxParameterID::AtomicSwapPeerPublicKey, swapPublicKey)
            .AddParameter(TxParameterID::SubTxIndex, SubTxIndex::UFO_LOCK_TX)
            .AddParameter(TxParameterID::Fee, lockBuilder.GetFee())
            .AddParameter(TxParameterID::PeerMaxHeight, lockBuilder.GetMaxHeight())
            .AddParameter(TxParameterID::PeerPublicExcess, lockBuilder.GetPublicExcess())
            .AddParameter(TxParameterID::PeerPublicNonce, lockBuilder.GetPublicNonce())
            .AddParameter(TxParameterID::PeerSharedBulletProofPart2, lockBuilder.GetRangeProofInitialPart2())
            .AddParameter(TxParameterID::PeerPublicSharedBlindingFactor, lockBuilder.GetPublicSharedBlindingFactor());

        if (!SendTxParameters(std::move(msg)))
        {
            OnFailed(TxFailureReason::FailedToSendParameters, false);
        }
    }

    void AtomicSwapTransaction::SendLockTxConfirmation(const LockTxBuilder& lockBuilder)
    {
        auto bulletProof = lockBuilder.GetSharedProof();

        SetTxParameter msg;
        msg.AddParameter(TxParameterID::PeerProtoVersion, s_ProtoVersion)
            .AddParameter(TxParameterID::SubTxIndex, SubTxIndex::UFO_LOCK_TX)
            .AddParameter(TxParameterID::PeerPublicExcess, lockBuilder.GetPublicExcess())
            .AddParameter(TxParameterID::PeerPublicNonce, lockBuilder.GetPublicNonce())
            .AddParameter(TxParameterID::PeerMaxHeight, lockBuilder.GetMaxHeight())
            .AddParameter(TxParameterID::PeerSignature, lockBuilder.GetPartialSignature())
            .AddParameter(TxParameterID::PeerOffset, lockBuilder.GetOffset())
            .AddParameter(TxParameterID::PeerSharedBulletProofPart2, lockBuilder.GetRangeProofInitialPart2())
            .AddParameter(TxParameterID::PeerSharedBulletProofPart3, bulletProof.m_Part3)
            .AddParameter(TxParameterID::PeerPublicSharedBlindingFactor, lockBuilder.GetPublicSharedBlindingFactor());

        if (!SendTxParameters(std::move(msg)))
        {
            OnFailed(TxFailureReason::FailedToSendParameters, false);
        }
    }

    void AtomicSwapTransaction::SendSharedTxInvitation(const BaseTxBuilder& builder)
    {
        SetTxParameter msg;
        msg.AddParameter(TxParameterID::SubTxIndex, builder.GetSubTxID())
            .AddParameter(TxParameterID::Amount, builder.GetAmount())
            .AddParameter(TxParameterID::Fee, builder.GetFee())
            .AddParameter(TxParameterID::MinHeight, builder.GetMinHeight())
            .AddParameter(TxParameterID::PeerPublicExcess, builder.GetPublicExcess())
            .AddParameter(TxParameterID::PeerPublicNonce, builder.GetPublicNonce());
    
        if (!SendTxParameters(std::move(msg)))
        {
            OnFailed(TxFailureReason::FailedToSendParameters, false);
        }
    }

    void AtomicSwapTransaction::ConfirmSharedTxInvitation(const BaseTxBuilder& builder)
    {
        SetTxParameter msg;
        msg.AddParameter(TxParameterID::SubTxIndex, builder.GetSubTxID())
            .AddParameter(TxParameterID::PeerPublicExcess, builder.GetPublicExcess())
            .AddParameter(TxParameterID::PeerSignature, builder.GetPartialSignature())
            .AddParameter(TxParameterID::PeerPublicNonce, builder.GetPublicNonce())
            .AddParameter(TxParameterID::PeerOffset, builder.GetOffset());

        if (!SendTxParameters(std::move(msg)))
        {
            OnFailed(TxFailureReason::FailedToSendParameters, false);
        }
    }

    void AtomicSwapTransaction::OnSubTxFailed(TxFailureReason reason, SubTxID subTxID, bool notify)
    {
        TxFailureReason previousReason;

        if (GetParameter(TxParameterID::InternalFailureReason, previousReason, subTxID) && previousReason == reason)
        {
            return;
        }

        LOG_ERROR() << GetTxID() << "[" << subTxID << "]" << " Failed. " << GetFailureMessage(reason);

        SetParameter(TxParameterID::InternalFailureReason, reason, false, subTxID);
        OnFailed(TxFailureReason::SubTxFailed, notify);
    }

    void AtomicSwapTransaction::CheckSubTxFailures()
    {
        State state = GetState(kDefaultSubTxID);
        TxFailureReason reason = TxFailureReason::Unknown;

        if ((state == State::Initial ||
            state == State::HandlingContractTX) && GetParameter(TxParameterID::InternalFailureReason, reason, SubTxIndex::LOCK_TX))
        {
            OnFailed(reason, true);
        }
    }

    void AtomicSwapTransaction::ExtractSecretPrivateKey()
    {
        auto subTxID = SubTxIndex::UFO_REDEEM_TX;
        TxKernel::Ptr kernel = GetMandatoryParameter<TxKernel::Ptr>(TxParameterID::Kernel, subTxID);

        SharedTxBuilder builder{ *this, subTxID };
        builder.GetSharedParameters();
        builder.GetInitialTxParams();
        builder.GetPeerPublicExcessAndNonce();
        builder.GenerateNonce();
        builder.CreateKernel();
        builder.SignPartial();

        Scalar::Native peerSignature = GetMandatoryParameter<Scalar::Native>(TxParameterID::PeerSignature, subTxID);
        Scalar::Native partialSignature = builder.GetPartialSignature();

        Scalar::Native fullSignature;
        fullSignature.Import(kernel->m_Signature.m_k);
        fullSignature = -fullSignature;
        Scalar::Native secretPrivateKeyNative = peerSignature + partialSignature;
        secretPrivateKeyNative += fullSignature;

        Scalar secretPrivateKey;
        secretPrivateKeyNative.Export(secretPrivateKey);

        SetParameter(TxParameterID::AtomicSwapSecretPrivateKey, secretPrivateKey.m_Value, false, UFO_REDEEM_TX);
    }

} // namespace