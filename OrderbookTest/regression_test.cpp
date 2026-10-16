// Regression tests for bugs found in the reference implementation
// (github.com/Tzadiko/Orderbook). Each test fails, hangs, or trips a sanitizer
// when the corresponding fix is reverted. Run with -DSANITIZE=address and
// -DSANITIZE=thread to get the full coverage.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "Orderbook.h"

namespace
{
    OrderPointer MakeOrder(OrderType type, OrderId id, Side side, Price price, Quantity quantity)
    {
        return std::make_shared<Order>(type, id, side, price, quantity);
    }
}

// Bug: MatchOrders cancelled a leftover FillAndKill order through the public
// CancelOrder, which locks ordersMutex_ again while AddOrder already holds it.
// A partly filled FillAndKill order deadlocked the book.
TEST(Regression, PartlyFilledFillAndKillDoesNotDeadlock)
{
    Orderbook orderbook;
    orderbook.AddOrder(MakeOrder(OrderType::GoodTillCancel, 1, Side::Buy, 100, 5));

    const auto trades = orderbook.AddOrder(MakeOrder(OrderType::FillAndKill, 2, Side::Sell, 100, 10));

    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].GetAskTrade().quantity_, 5u);
    EXPECT_EQ(orderbook.Size(), 0u);
}

// Bug: the destructor set the shutdown flag and notified the prune thread
// without holding the mutex, so the notification could arrive before the
// thread started waiting. The thread then slept until 16:00 and join() hung.
// The thread was also declared before the condition variable, so it could
// start using it before it was constructed.
TEST(Regression, DestructorDoesNotWaitForPruneDeadline)
{
    const auto start = std::chrono::steady_clock::now();

    for (int i = 0; i < 200; ++i)
        Orderbook orderbook;

    const auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_LT(elapsed, std::chrono::seconds(2));
}

// Bug: when a price level emptied, MatchOrders called bids_.erase(bidPrice)
// and then data_.erase(bidPrice). bidPrice is a reference to the erased map
// key, so the second call read freed memory (caught by AddressSanitizer).
TEST(Regression, EmptyingBothBestLevelsLeavesNoStaleLevelData)
{
    Orderbook orderbook;
    // Deeper levels keep data_ non-empty, so erasing by the freed key actually reads it.
    orderbook.AddOrder(MakeOrder(OrderType::GoodTillCancel, 1, Side::Buy, 99, 10));
    orderbook.AddOrder(MakeOrder(OrderType::GoodTillCancel, 2, Side::Sell, 102, 10));
    orderbook.AddOrder(MakeOrder(OrderType::GoodTillCancel, 3, Side::Buy, 100, 10));
    orderbook.AddOrder(MakeOrder(OrderType::GoodTillCancel, 4, Side::Sell, 100, 10));
    ASSERT_EQ(orderbook.Size(), 2u);

    // Stale level data at 100 would let this FillOrKill think it can fill.
    orderbook.AddOrder(MakeOrder(OrderType::GoodTillCancel, 5, Side::Sell, 101, 5));
    const auto trades = orderbook.AddOrder(MakeOrder(OrderType::FillOrKill, 6, Side::Buy, 101, 10));

    EXPECT_TRUE(trades.empty());
    EXPECT_EQ(orderbook.Size(), 3u);
}

// Bug: CancelOrderInternal took a reference to the order entry, erased the
// entry, then kept using the reference (caught by AddressSanitizer).
TEST(Regression, CancelReadsOrderAfterErasingEntry)
{
    Orderbook orderbook;
    orderbook.AddOrder(MakeOrder(OrderType::GoodTillCancel, 1, Side::Sell, 100, 10));
    orderbook.AddOrder(MakeOrder(OrderType::GoodTillCancel, 2, Side::Sell, 100, 10));

    orderbook.CancelOrder(1);

    EXPECT_EQ(orderbook.Size(), 1u);
    const auto infos = orderbook.GetOrderInfos();
    ASSERT_EQ(infos.GetAsks().size(), 1u);
    EXPECT_EQ(infos.GetAsks()[0].quantity_, 10u);
}

// Bug: GetOrderInfos read bids_, asks_ and orders_ without taking the mutex,
// racing with any thread adding or cancelling orders (caught by ThreadSanitizer,
// and can crash on a normal build).
TEST(Regression, GetOrderInfosIsSafeDuringConcurrentWrites)
{
    Orderbook orderbook;
    std::atomic_bool done{ false };

    std::thread writer{ [&]
    {
        for (OrderId id = 1; id <= 5'000; ++id)
        {
            const Side side = id % 2 ? Side::Buy : Side::Sell;
            const Price price = side == Side::Buy ? 100 - static_cast<Price>(id % 10) : 101 + static_cast<Price>(id % 10);
            orderbook.AddOrder(MakeOrder(OrderType::GoodTillCancel, id, side, price, 10));
            if (id % 3 == 0)
                orderbook.CancelOrder(id - 1);
        }
        done = true;
    } };

    while (!done)
    {
        const auto infos = orderbook.GetOrderInfos();
        EXPECT_LE(infos.GetBids().size(), 10u);
        // Yield so the writer isn't starved of the mutex.
        std::this_thread::yield();
    }

    writer.join();
    EXPECT_GT(orderbook.Size(), 0u);
}

// Bug: per-level quantities lived in one hash map keyed by price alone, shared
// by both sides. When an incoming buy at 100 emptied the bid level at 100,
// MatchOrders erased data_[100], which also wiped the remaining ask quantity
// at 100. A later FillOrKill at that price was rejected despite enough
// liquidity. Each price level now tracks its own quantity.
TEST(Regression, FillOrKillSeesLiquidityLeftAtPriceOfEmptiedLevel)
{
    Orderbook orderbook;
    orderbook.AddOrder(MakeOrder(OrderType::GoodTillCancel, 1, Side::Sell, 100, 50));
    orderbook.AddOrder(MakeOrder(OrderType::GoodTillCancel, 2, Side::Buy, 100, 10));

    const auto trades = orderbook.AddOrder(MakeOrder(OrderType::FillOrKill, 3, Side::Buy, 100, 30));

    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].GetBidTrade().quantity_, 30u);
    const auto infos = orderbook.GetOrderInfos();
    ASSERT_EQ(infos.GetAsks().size(), 1u);
    EXPECT_EQ(infos.GetAsks()[0].quantity_, 10u);
}

// Level quantities are now maintained incrementally, so check they stay
// correct through partial fills and cancels.
TEST(Regression, LevelQuantityTracksPartialFillsAndCancels)
{
    Orderbook orderbook;
    orderbook.AddOrder(MakeOrder(OrderType::GoodTillCancel, 1, Side::Sell, 100, 30));
    orderbook.AddOrder(MakeOrder(OrderType::GoodTillCancel, 2, Side::Sell, 100, 20));
    orderbook.AddOrder(MakeOrder(OrderType::GoodTillCancel, 3, Side::Buy, 100, 40));
    orderbook.AddOrder(MakeOrder(OrderType::GoodTillCancel, 4, Side::Sell, 101, 7));
    orderbook.CancelOrder(4);
    orderbook.AddOrder(MakeOrder(OrderType::GoodTillCancel, 5, Side::Buy, 99, 15));

    const auto infos = orderbook.GetOrderInfos();
    ASSERT_EQ(infos.GetAsks().size(), 1u);
    EXPECT_EQ(infos.GetAsks()[0].price_, 100);
    EXPECT_EQ(infos.GetAsks()[0].quantity_, 10u);
    ASSERT_EQ(infos.GetBids().size(), 1u);
    EXPECT_EQ(infos.GetBids()[0].quantity_, 15u);
}
