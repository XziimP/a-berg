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


// test helpers and mocks
#include "test_helpers.h"
WALLET_TEST_INIT
#include "mock_bbs_network.cpp"

// tested module
#include "wallet/client/extensions/broadcast_gateway/broadcast_router.h"
#include "wallet/client/extensions/offers_board/swap_offers_board.h"

// dependencies
#include "keykeeper/local_private_key_keeper.h"

#include <boost/filesystem.hpp>

using namespace beam;
using namespace beam::wallet;
using namespace std;

namespace
{
    using PrivateKey = ECC::Scalar::Native;
    using PublicKey = PeerID;

    const string dbFileName = "wallet.db";

    void PublishOfferNoThrow(const SwapOffersBoard& board, const SwapOffer& offer)
    {
        try {
            board.publishOffer(offer);
        }
        catch(const SwapOffersBoard::InvalidOfferException & e) {
            std::cout << offer.m_txId << e.what() << endl;
        }
        catch(const SwapOffersBoard::OfferAlreadyPublishedException & e) {
            std::cout << offer.m_txId << e.what() << endl;
        }
        catch(const SwapOffersBoard::ForeignOfferException & e) {
            std::cout << offer.m_txId << e.what() << endl;
        }
        catch(const SwapOffersBoard::ExpiredOfferException & e) {
            std::cout << offer.m_txId << e.what() << endl;
        }
    }

    /**
     *  Class to test correct notification of SwapOffersBoard observers
     */
    struct MockBoardObserver : public ISwapOffersObserver
    {
        using CheckerFunction = function<void(ChangeAction, const vector<SwapOffer>&)>;

        MockBoardObserver(CheckerFunction checker) :
            m_testChecker(checker) {};

        virtual void onSwapOffersChanged(ChangeAction action, const vector<SwapOffer>& offers) override
        {
            m_testChecker(action, offers);
        }

        CheckerFunction m_testChecker;
    };

    struct MockBroadcastListener : public IBroadcastListener
    {
        using OnMessage = function<void(ByteBuffer&)>;

        MockBroadcastListener(OnMessage func) : m_callback(func) {};

        virtual bool onMessage(uint64_t unused, ByteBuffer&& msg) override
        {
            m_callback(msg);
            return true;
        };

        OnMessage m_callback;
    };

    IWalletDB::Ptr createSqliteWalletDB()
    {
        if (boost::filesystem::exists(dbFileName))
        {
            boost::filesystem::remove(dbFileName);
        }
        ECC::NoLeak<ECC::uintBig> seed;
        seed.V = 10283UL;
        auto walletDB = WalletDB::init(dbFileName, string("pass123"), seed);
        beam::Block::SystemState::ID id = { };
        id.m_Height = 134;
        walletDB->setSystemStateID(id);
        return walletDB;
    }

    // Generate random TxID
    TxID generateTxID()
    {
        std::srand(static_cast<unsigned int>(std::time(nullptr)));
        TxID txId;
        for (uint8_t& i : txId)
        {
            i = std::rand() % 255;
        }
        return txId;
    }
    
    // Increment by 1 @id
    TxID& incrementTxID(TxID& id)
    {
        for (uint8_t& i : id)
        {
            if (i < 0xff)
            {
                ++i;
                break;
            }
        }
        return id;
    }

    // Construct SwapOffer with random tx parameters.
    SwapOffer createOffer(const TxID& txID, SwapOfferStatus s, const WalletID& pubK, AtomicSwapCoin c)
    {
        std::srand(static_cast<unsigned int>(std::time(nullptr)));
        SwapOffer o(txID, s, pubK, c);
        // mandatory parameters
        o.SetParameter(TxParameterID::AtomicSwapCoin, o.m_coin);
        o.SetParameter(TxParameterID::AtomicSwapIsBeamSide, std::rand() % 2);
        o.SetParameter(TxParameterID::Amount, Amount(std::rand() % 10000));
        o.SetParameter(TxParameterID::AtomicSwapAmount, Amount(std::rand() % 1000));
        o.SetParameter(TxParameterID::MinHeight, Height(std::rand() % 1000));
        o.SetParameter(TxParameterID::PeerResponseTime, Height(std::rand() % 500));
        o.SetParameter(TxParameterID::TransactionType, TxType::AtomicSwap);
        return o;
    }

    /**
     *  Generate random offer.
     *  Create address in database. Use random TxID.
     *  return generated swap offer and address key derivation index
     */
    std::tuple<SwapOffer, uint64_t> generateTestOffer(IWalletDB::Ptr walletDB)
    {
        WalletAddress wa;
        walletDB->createAddress(wa);
        walletDB->saveAddress(wa);
        TxID txID = generateTxID();
        const auto offer = createOffer( txID,
                                        SwapOfferStatus::Pending,
                                        wa.m_walletID,
                                        AtomicSwapCoin::Bitcoin);
        return std::make_tuple(offer, wa.m_OwnID);
    }

    /**
     *  Derive key pair with specified @keyIndex
     */
    std::tuple<PublicKey, PrivateKey> deriveKeypair(IWalletDB::Ptr walletDB, uint64_t keyIndex)
    {
        PrivateKey sk;
        PublicKey pk;
        walletDB->get_MasterKdf()->DeriveKey(sk, ECC::Key::ID(keyIndex, Key::Type::Bbs));
        pk.FromSk(sk);
        return std::make_tuple(pk, sk);
    }
    
    /**
     *  Create signature for @data using key derived with specified @keyIndex
     *  return signature
     */
    ByteBuffer signData(const ByteBuffer& data, uint64_t keyIndex, IWalletDB::Ptr walletDB)
    {
        PrivateKey sk;
        std::tie(std::ignore, sk) = deriveKeypair(walletDB, keyIndex);
        SwapOfferConfirmation signHandler;
        signHandler.m_offerData = data;
        signHandler.Sign(sk);
        ByteBuffer rawSignature = toByteBuffer(signHandler.m_Signature);
        return rawSignature;
    }

    /**
     *  Create message according to protocol.
     *  Concatenate message body and signature.
     */
    ByteBuffer makeMsg(const ByteBuffer& msgRaw, const ByteBuffer& signatureRaw)
    {
        size_t size = msgRaw.size() + signatureRaw.size();
        assert(size <= UINT32_MAX);
        ByteBuffer fullMsg(size);

        auto it = std::copy(std::begin(msgRaw),
                            std::end(msgRaw),
                            std::begin(fullMsg));
        std::copy(  std::begin(signatureRaw),
                    std::end(signatureRaw),
                    it);

        return fullMsg;
    }    

    void TestProtocolHandlerSignature()
    {
        cout << endl << "Test protocol handler validating signature" << endl;

        auto storage = createSqliteWalletDB();

        OfferBoardProtocolHandler protocolHandler(storage->get_SbbsKdf(), storage);

        {
            std::cout << "Case: parsing message with invalid signature" << endl;

            const auto& [offer, keyIndex] = generateTestOffer(storage);

            const ByteBuffer msgRaw = toByteBuffer(SwapOfferToken(offer));
            auto signatureRaw = signData(msgRaw, keyIndex, storage);
            // corrupt signature
            signatureRaw.front() += 1;

            const auto finalMessage = makeMsg(msgRaw, signatureRaw);
            
            boost::optional<SwapOffer> res;
            WALLET_CHECK_NO_THROW(res = protocolHandler.parseMessage(finalMessage));
            WALLET_CHECK(!res);
        }
        {
            std::cout << "Case: parsing message with invalid public key" << endl;

            auto [offer, keyIndex] = generateTestOffer(storage);

            // changed public key another
            WalletAddress anotherAddress;
            storage->createAddress(anotherAddress);
            offer.m_publisherId = anotherAddress.m_walletID;

            const ByteBuffer msgRaw = toByteBuffer(SwapOfferToken(offer));
            const auto signatureRaw = signData(msgRaw, keyIndex, storage);
            const auto finalMessage = makeMsg(msgRaw, signatureRaw);

            boost::optional<SwapOffer> res;
            WALLET_CHECK_NO_THROW(res = protocolHandler.parseMessage(finalMessage));
            WALLET_CHECK(!res);
        }
        {
            std::cout << "Case: parsing correct message" << endl;

            const auto& [offer, keyIndex] = generateTestOffer(storage);

            const ByteBuffer msgRaw = toByteBuffer(SwapOfferToken(offer));
            const auto signatureRaw = signData(msgRaw, keyIndex, storage);
            const auto finalMessage = makeMsg(msgRaw, signatureRaw);

            boost::optional<SwapOffer> res;
            WALLET_CHECK_NO_THROW(res = protocolHandler.parseMessage(finalMessage));
            WALLET_CHECK(res);
            WALLET_CHECK(*res == offer);
        }

        cout << "Test end" << endl;
    }

    void TestProtocolHandlerIntegration()
    {
        cout << endl << "Test protocol handler integration" << endl;

        auto storage = createSqliteWalletDB();
        OfferBoardProtocolHandler protocolHandler(storage->get_SbbsKdf(), storage);
        MockBbsNetwork mockNetwork;
        BroadcastRouter broadcastRouter(mockNetwork, mockNetwork);

        {
            std::cout << "Case: create, dispatch and parse offer" << endl;

            SwapOffer offer;
            std::tie(offer, std::ignore) = generateTestOffer(storage);
            bool executed = false;

            MockBroadcastListener testListener(
                [&executed, &offer, &protocolHandler]
                (ByteBuffer& msg)
                {
                    boost::optional<SwapOffer> res;
                    WALLET_CHECK_NO_THROW(res = protocolHandler.parseMessage(msg));
                    WALLET_CHECK(res);
                    WALLET_CHECK(*res == offer);
                    executed = true;
                });
            broadcastRouter.registerListener(BroadcastContentType::SwapOffers, &testListener);

            boost::optional<ByteBuffer> msg;
            WALLET_CHECK_NO_THROW(msg = protocolHandler.createMessage(offer, offer.m_publisherId));
            WALLET_CHECK(msg);

            broadcastRouter.sendRawMessage(BroadcastContentType::SwapOffers, *msg);

            WALLET_CHECK(executed);
        }

        cout << "Test end" << endl;
    }

    void TestMandatoryParameters()
    {
        cout << endl << "Test mandatory parameters validation" << endl;

        auto storage = createSqliteWalletDB();

        MockBbsNetwork mockNetwork;
        BroadcastRouter broadcastRouter(mockNetwork, mockNetwork);
        OfferBoardProtocolHandler protocolHandler(storage->get_SbbsKdf(), storage);
        SwapOffersBoard Alice(broadcastRouter, protocolHandler);

        WALLET_CHECK(Alice.getOffersList().size() == 0);

        SwapOffer correctOffer;
        std::tie(correctOffer, std::ignore) = generateTestOffer(storage);
        TxID txID = correctOffer.m_txId;    // used to iterate and create unique ID's
        
        size_t offersCount = 0;
        size_t count = 0;
        {
            cout << "Case: mandatory parameters presence:" << endl;
            const std::array<TxParameterID,6> mandatoryParams {
                TxParameterID::AtomicSwapCoin,
                TxParameterID::AtomicSwapIsBeamSide,
                TxParameterID::Amount,
                TxParameterID::AtomicSwapAmount,
                TxParameterID::MinHeight,
                TxParameterID::PeerResponseTime };

            for (auto parameter : mandatoryParams)
            {
                // check that offers without mandatory parameters don't appear on board
                SwapOffer o = correctOffer;
                o.m_txId = incrementTxID(txID);
                cout << "\tparameter code " << static_cast<uint32_t>(parameter) << endl;
                o.DeleteParameter(parameter);
                PublishOfferNoThrow(Alice, o);
                WALLET_CHECK_NO_THROW(count = Alice.getOffersList().size());
                WALLET_CHECK(count == offersCount);
            }
        }
        {
            cout << "Case: AtomicSwapCoin parameter validation" << endl;
            SwapOffer o = correctOffer;
            o.m_txId = incrementTxID(txID);
            o.m_coin = AtomicSwapCoin::Unknown;
            PublishOfferNoThrow(Alice, o);
            WALLET_CHECK_NO_THROW(count = Alice.getOffersList().size());
            WALLET_CHECK(count == offersCount);
        }
        {
            cout << "Case: SwapOfferStatus parameter validation" << endl;
            SwapOffer o = correctOffer;
            o.m_txId = incrementTxID(txID);
            o.m_status = static_cast<SwapOfferStatus>(static_cast<uint32_t>(SwapOfferStatus::Failed) + 1);
            PublishOfferNoThrow(Alice, o);
            WALLET_CHECK_NO_THROW(count = Alice.getOffersList().size());
            WALLET_CHECK(count == offersCount);
        }
        {
            cout << "Case: correct offer" << endl;
            SwapOffer o = correctOffer;
            o.m_txId = incrementTxID(txID);
            PublishOfferNoThrow(Alice, o);
            WALLET_CHECK(Alice.getOffersList().size() == ++offersCount);
        }
        cout << "Test end" << endl;
    }

    void TestCommunication()
    {
        cout << endl << "Test boards communication and notification" << endl;

        auto storage = createSqliteWalletDB();

        OfferBoardProtocolHandler protocolHandler(storage->get_SbbsKdf(), storage);
        MockBbsNetwork mockNetwork;
        BroadcastRouter broadcastRouterA(mockNetwork, mockNetwork);
        BroadcastRouter broadcastRouterB(mockNetwork, mockNetwork);
        BroadcastRouter broadcastRouterC(mockNetwork, mockNetwork);

        SwapOffersBoard Alice(broadcastRouterA, protocolHandler);
        SwapOffersBoard Bob(broadcastRouterB, protocolHandler);
        SwapOffersBoard Cory(broadcastRouterC, protocolHandler);

        WALLET_CHECK(Alice.getOffersList().size() == 0);
        WALLET_CHECK(Bob.getOffersList().size() == 0);
        WALLET_CHECK(Cory.getOffersList().size() == 0);

        SwapOffer correctOffer;
        std::tie(correctOffer, std::ignore) =  generateTestOffer(storage);
        TxID txID = correctOffer.m_txId;    // used to iterate and create unique ID's
        
        size_t offersCount = 0;
        {
            uint32_t executionCount = 0;
            MockBoardObserver testObserver(
                [&executionCount]
                (ChangeAction action, const vector<SwapOffer>& offers)
                {
                    WALLET_CHECK(action == ChangeAction::Added);
                    WALLET_CHECK(offers.size() == 1);
                    executionCount++;
                });
            Alice.Subscribe(&testObserver);
            Bob.Subscribe(&testObserver);
            Cory.Subscribe(&testObserver);
            
            cout << "Case: normal dispatch and notification" << endl;
            SwapOffer o1 = correctOffer;
            SwapOffer o2 = correctOffer;
            SwapOffer o3 = correctOffer;
            o2.m_txId = incrementTxID(txID);
            o3.m_txId = incrementTxID(txID);
            PublishOfferNoThrow(Alice, o1);
            PublishOfferNoThrow(Bob, o2);
            PublishOfferNoThrow(Cory, o3);
            offersCount += 3;
            WALLET_CHECK(Alice.getOffersList().size() == offersCount);
            WALLET_CHECK(Bob.getOffersList().size() == offersCount);
            WALLET_CHECK(Cory.getOffersList().size() == offersCount);
            WALLET_CHECK(executionCount == 9);
            {
                auto receivedOffer = Bob.getOffersList().front();
                std::array<TxParameterID,6> paramsToCompare {
                    TxParameterID::AtomicSwapCoin,
                    TxParameterID::AtomicSwapIsBeamSide,
                    TxParameterID::Amount,
                    TxParameterID::AtomicSwapAmount,
                    TxParameterID::MinHeight,
                    TxParameterID::PeerResponseTime
                };
                for (auto p : paramsToCompare)
                {
                    auto receivedValue = receivedOffer.GetParameter(p);
                    auto dispatchedValue = correctOffer.GetParameter(p);
                    WALLET_CHECK(receivedValue && dispatchedValue);
                    WALLET_CHECK(*receivedValue == *dispatchedValue);
                }
            }
            
            cout << "Case: ignore same TxID" << endl;
            SwapOffer o4 = correctOffer;
            o4.m_coin = AtomicSwapCoin::Qtum;
            PublishOfferNoThrow(Cory, o4);
            WALLET_CHECK(Alice.getOffersList().size() == offersCount);
            WALLET_CHECK(Bob.getOffersList().size() == offersCount);
            WALLET_CHECK(Cory.getOffersList().size() == offersCount);
            WALLET_CHECK(Alice.getOffersList().front().m_coin == AtomicSwapCoin::Bitcoin);
            WALLET_CHECK(executionCount == 9);

            cout << "Case: different TxID" << endl;
            o4.m_txId = incrementTxID(txID);
            o4.m_coin = AtomicSwapCoin::Qtum;
            PublishOfferNoThrow(Cory, o4);
            offersCount++;
            WALLET_CHECK(Alice.getOffersList().size() == offersCount);
            WALLET_CHECK(Bob.getOffersList().size() == offersCount);
            WALLET_CHECK(Cory.getOffersList().size() == offersCount);
            WALLET_CHECK(executionCount == 12);

            Alice.Unsubscribe(&testObserver);
            Bob.Unsubscribe(&testObserver);
            Cory.Unsubscribe(&testObserver);

            cout << "Case: unsubscribe stops notification" << endl;
            o4 = correctOffer;
            o4.m_txId = incrementTxID(txID);
            o4.m_coin = AtomicSwapCoin::Litecoin;
            PublishOfferNoThrow(Bob, o4);
            offersCount++;
            WALLET_CHECK(Alice.getOffersList().size() == offersCount);
            WALLET_CHECK(Bob.getOffersList().size() == offersCount);
            WALLET_CHECK(Cory.getOffersList().size() == offersCount);
            WALLET_CHECK(executionCount == 12);
        }
        
        {
            uint32_t execCount = 0;
            MockBoardObserver testObserver(
                [&execCount]
                (ChangeAction action, const vector<SwapOffer>& offers)
                {
                    execCount++;
                });
            Bob.Subscribe(&testObserver);
            {
                cout << "Case: no notification on new offer in status:" << endl;
                std::array<SwapOfferStatus,5> nonActiveStatuses {
                    SwapOfferStatus::InProgress,
                    SwapOfferStatus::Completed,
                    SwapOfferStatus::Canceled,
                    SwapOfferStatus::Expired,
                    SwapOfferStatus::Failed };

                for (auto s : nonActiveStatuses)
                {
                    SwapOffer o = correctOffer;
                    o.m_txId = incrementTxID(txID);
                    cout << "\tparameter " << static_cast<uint32_t>(s) << endl;
                    o.m_status = s;
                    PublishOfferNoThrow(Alice, o);
                    WALLET_CHECK(Bob.getOffersList().size() == offersCount);
                }
                WALLET_CHECK(execCount == 0);
            }
            {
                cout << "Case: notification on new offer in Pending status" << endl;
                SwapOffer o = correctOffer;
                o.m_txId = incrementTxID(txID);
                o.m_status = SwapOfferStatus::Pending;
                PublishOfferNoThrow(Alice, o);
                offersCount++;
                WALLET_CHECK(Bob.getOffersList().size() == offersCount);
                WALLET_CHECK(execCount == 1);
            }
            Bob.Unsubscribe(&testObserver);
        }
        cout << "Test end" << endl;
    }

    void TestLinkedTransactionChanges()
    {
        cout << endl << "Test linked transaction status changes" << endl;

        auto storage = createSqliteWalletDB();

        OfferBoardProtocolHandler protocolHandler(storage->get_SbbsKdf(), storage);
        MockBbsNetwork mockNetwork;
        BroadcastRouter broadcastRouterA(mockNetwork, mockNetwork);
        BroadcastRouter broadcastRouterB(mockNetwork, mockNetwork);

        SwapOffersBoard Alice(broadcastRouterA, protocolHandler);
        SwapOffersBoard Bob(broadcastRouterB, protocolHandler);

        SwapOffer correctOffer;
        std::tie(correctOffer, std::ignore) = generateTestOffer(storage);
        TxID txID = correctOffer.m_txId;    // used to iterate and create unique ID's

        size_t offerCount = 0;
        {
            cout << "Case: offers removed when Tx state changes to InProgress, Canceled, Failed" << endl;

            SwapOffer o1 = correctOffer;
            SwapOffer o2 = correctOffer;
            SwapOffer o3 = correctOffer;
            SwapOffer o4 = correctOffer;
            SwapOffer o5 = correctOffer;
            o1.m_txId = incrementTxID(txID);
            o2.m_txId = incrementTxID(txID);
            o3.m_txId = incrementTxID(txID);
            o4.m_txId = incrementTxID(txID);
            o5.m_txId = incrementTxID(txID);
            PublishOfferNoThrow(Alice, o1);
            PublishOfferNoThrow(Alice, o2);
            PublishOfferNoThrow(Alice, o3);
            PublishOfferNoThrow(Alice, o4);
            PublishOfferNoThrow(Alice, o5);
            offerCount += 5;
            WALLET_CHECK(Bob.getOffersList().size() == offerCount);
            WALLET_CHECK(Alice.getOffersList().size() == offerCount);

            TxDescription tx1(o1.m_txId, TxType::AtomicSwap, Amount(852), Amount(741), Height(789));
            TxDescription tx2(o2.m_txId, TxType::AtomicSwap, Amount(852), Amount(741), Height(789));
            TxDescription tx3(o3.m_txId, TxType::AtomicSwap, Amount(852), Amount(741), Height(789));
            TxDescription tx4(o4.m_txId, TxType::AtomicSwap, Amount(852), Amount(741), Height(789));
            TxDescription tx5(o4.m_txId, TxType::AtomicSwap, Amount(852), Amount(741), Height(789));
            TxDescription tx6(o4.m_txId, TxType::AtomicSwap, Amount(852), Amount(741), Height(789));
            // this TxType is ignored
            TxDescription tx7(o4.m_txId, TxType::Simple, Amount(852), Amount(741), Height(789));
            tx7.m_status = wallet::TxStatus::InProgress;
            // these states have to deactivate offer
            tx1.m_status = wallet::TxStatus::InProgress;
            tx2.m_status = wallet::TxStatus::Canceled;
            tx3.m_status = wallet::TxStatus::Failed;
            // these are ignored
            tx4.m_status = wallet::TxStatus::Pending;
            tx5.m_status = wallet::TxStatus::Completed;
            tx6.m_status = wallet::TxStatus::Registering;
            uint32_t exCount = 0;
            MockBoardObserver obsRemove([&exCount](ChangeAction action, const vector<SwapOffer>& offers) {
                        WALLET_CHECK(action == ChangeAction::Removed);
                        exCount++;
                    });
            Bob.Subscribe(&obsRemove);
            Alice.onTransactionChanged(ChangeAction::Updated, {tx5, tx4, tx1, tx3, tx2, tx6, tx7});
            Bob.Unsubscribe(&obsRemove);
            offerCount -= 3;
            WALLET_CHECK(Bob.getOffersList().size() == offerCount);
            WALLET_CHECK(Alice.getOffersList().size() == offerCount);
            WALLET_CHECK(exCount == 3);

            // cancel the remaining offers
            tx4.m_txId = o4.m_txId;
            tx4.m_status = wallet::TxStatus::Canceled;
            tx4.m_txType = TxType::AtomicSwap;
            tx5.m_txId = o5.m_txId;
            tx5.m_status = wallet::TxStatus::Canceled;
            tx5.m_txType = TxType::AtomicSwap;
            Alice.onTransactionChanged(ChangeAction::Updated, {tx4, tx5});
            offerCount -= 2;
            WALLET_CHECK(Bob.getOffersList().size() == offerCount);
            WALLET_CHECK(Alice.getOffersList().size() == offerCount);
            WALLET_CHECK(offerCount == 0);
        }

        {
            cout << "Case: offers removed when chain height growns beyond expiration" << endl;

            SwapOffer aliceOffer = correctOffer;
            SwapOffer aliceExpiredOffer = correctOffer;
            SwapOffer bobOffer = correctOffer;
            aliceOffer.m_txId = incrementTxID(txID);
            aliceExpiredOffer.m_txId = incrementTxID(txID);
            bobOffer.m_txId = incrementTxID(txID);
            PublishOfferNoThrow(Bob, bobOffer);
            PublishOfferNoThrow(Alice, aliceOffer);
            offerCount += 2;

            WALLET_CHECK(Alice.getOffersList().size() == offerCount);
            WALLET_CHECK(Bob.getOffersList().size() == offerCount);

            Block::SystemState::ID expiredHeight, nonExpiredHeight;
            auto h = aliceOffer.GetParameter<Height>(TxParameterID::MinHeight);
            auto t = aliceOffer.GetParameter<Height>(TxParameterID::PeerResponseTime);
            expiredHeight.m_Height = *h + *t;
            nonExpiredHeight.m_Height = *h + *t - Height(1);

            uint32_t exCount = 0;
            MockBoardObserver obsRemove([&exCount](ChangeAction action, const vector<SwapOffer>& offers) {
                WALLET_CHECK(action == ChangeAction::Removed);
                WALLET_CHECK(offers.front().m_status == SwapOfferStatus::Expired);
                exCount++;
            });

            Bob.Subscribe(&obsRemove);
            Bob.onSystemStateChanged(nonExpiredHeight);
            WALLET_CHECK(Alice.getOffersList().size() == offerCount);
            WALLET_CHECK(Bob.getOffersList().size() == offerCount);
            WALLET_CHECK(exCount == 0);
            Bob.Unsubscribe(&obsRemove);

            Alice.Subscribe(&obsRemove);
            Alice.onSystemStateChanged(expiredHeight);
            Alice.Unsubscribe(&obsRemove);
            WALLET_CHECK(Alice.getOffersList().size() == offerCount - 2);
            WALLET_CHECK(Bob.getOffersList().size() == offerCount);
            WALLET_CHECK(exCount == 2);

            // check expired offer 
            Alice.Subscribe(&obsRemove);
            PublishOfferNoThrow(Alice, aliceExpiredOffer);
            Alice.Unsubscribe(&obsRemove);
            WALLET_CHECK(Alice.getOffersList().size() == offerCount - 2);
            WALLET_CHECK(exCount == 2);
        }

        cout << "Test end" << endl;
    }

    void TestDelayedOfferUpdate()
    {
        cout << endl << "Test delayed offer update" << endl;

        auto storage = createSqliteWalletDB();

        OfferBoardProtocolHandler protocolHandler(storage->get_SbbsKdf(), storage);
        MockBbsNetwork mockNetwork;
        BroadcastRouter broadcastRouterA(mockNetwork, mockNetwork);
        BroadcastRouter broadcastRouterB(mockNetwork, mockNetwork);

        SwapOffersBoard Alice(broadcastRouterA, protocolHandler);
        SwapOffersBoard Bob(broadcastRouterB, protocolHandler);

        SwapOffer correctOffer;
        std::tie(correctOffer, std::ignore) = generateTestOffer(storage);

        uint32_t exCount = 0;
        MockBoardObserver observer(
            [&exCount]
            (ChangeAction action, const vector<SwapOffer>& offers)
            {
                exCount++;
            });
        {
            cout << "Case: delayed offer update broadcast to network" << endl;
            // Case when no offer exist on board.
            // Transaction steps to states InProgress and Expired or other.
            // Board doesn't know if offer exits in network and doesn't broadcast status update.
            // Offer appear on board. Offer status update has to be broadcasted.
            SwapOffer o = correctOffer;
            TxDescription tx(o.m_txId, TxType::AtomicSwap, Amount(951), Amount(753), Height(654));
            
            tx.m_status = wallet::TxStatus::InProgress;
            Alice.Subscribe(&observer);
            Alice.onTransactionChanged(ChangeAction::Updated, {tx});
            WALLET_CHECK(exCount == 0);
            WALLET_CHECK(Alice.getOffersList().size() == 0);
            WALLET_CHECK(Bob.getOffersList().size() == 0);

            tx.m_status = wallet::TxStatus::Failed;
            Alice.onTransactionChanged(ChangeAction::Updated, {tx});
            WALLET_CHECK(exCount == 0);
            WALLET_CHECK(Alice.getOffersList().size() == 0);
            WALLET_CHECK(Bob.getOffersList().size() == 0);
            
            tx.m_status = wallet::TxStatus::Canceled;
            Alice.onTransactionChanged(ChangeAction::Updated, {tx});
            WALLET_CHECK(exCount == 0);
            WALLET_CHECK(Alice.getOffersList().size() == 0);
            WALLET_CHECK(Bob.getOffersList().size() == 0);

            PublishOfferNoThrow(Bob, o);
            WALLET_CHECK(exCount == 0);
            WALLET_CHECK(Alice.getOffersList().size() == 0);
            WALLET_CHECK(Bob.getOffersList().size() == 0);
        }
        cout << "Test end" << endl;
    }

} // namespace

int main()
{
    cout << "SwapOffersBoard tests:" << endl;

    io::Reactor::Ptr mainReactor{ io::Reactor::create() };
    io::Reactor::Scope scope(*mainReactor);

    TestProtocolHandlerSignature();
    TestProtocolHandlerIntegration();

    TestMandatoryParameters();
    TestCommunication();
    TestLinkedTransactionChanges();
    TestDelayedOfferUpdate();

    boost::filesystem::remove(dbFileName);

    assert(g_failureCount == 0);
    return WALLET_CHECK_RESULT;
}
