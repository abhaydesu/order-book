// Replays a pre-generated stream of orders through the Orderbook and reports
// throughput (orders/sec) and per-operation latency percentiles.
//
// The stream is generated against a shadow orderbook, so every cancel and
// modify targets an order that is actually resting at that point. That keeps
// the book at a steady size instead of growing, and stops cancels from being
// cheap lookups of orders that no longer exist.
//
// Usage: orderbook_bench [operations=1000000] [seed=42]

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "Orderbook.h"

namespace
{
    enum class OperationType
    {
        Add,
        Cancel,
        Modify,
    };

    // Plain parameters rather than OrderPointers: orders are mutated as they
    // fill, so every run builds fresh Order objects from these before timing.
    struct Operation
    {
        OperationType type_;
        OrderType orderType_;
        OrderId orderId_;
        Side side_;
        Price price_;
        Quantity quantity_;
    };

    using Operations = std::vector<Operation>;

    constexpr Price MidPrice = 10'000;
    constexpr int SeedLevels = 100;
    constexpr int SeedOrdersPerLevel = 10;
    constexpr Quantity SeedQuantity = 100;

    // Each resting order is equally likely to be cancelled, so the cancel share
    // grows with the book: CancelPercentAtTarget when the book holds
    // TargetBookSize orders, more above it, less below. That makes the book
    // settle at a steady size, as a real book does, instead of draining or
    // growing without bound.
    constexpr double TargetBookSize = 2'000;
    constexpr double CancelPercentAtTarget = 35;
    constexpr double MaxCancelPercent = 80;
    constexpr double ModifyPercent = 5;

    // Split of new orders by type, as cumulative percentages of adds.
    constexpr int GoodTillCancelEnd = 78;
    constexpr int FillAndKillEnd = 92;
    constexpr int FillOrKillEnd = 98;

    OrderPointer MakeOrder(const Operation& operation)
    {
        if (operation.orderType_ == OrderType::Market)
            return std::make_shared<Order>(operation.orderId_, operation.side_, operation.quantity_);

        return std::make_shared<Order>(operation.orderType_, operation.orderId_, operation.side_, operation.price_, operation.quantity_);
    }

    // Builds a book with resting bids below the mid and asks above it, so the
    // measured stream starts against realistic depth instead of an empty book.
    Operations SeedOperations()
    {
        Operations seed;
        OrderId orderId = 1;
        for (int level = 1; level <= SeedLevels; ++level)
        {
            for (int i = 0; i < SeedOrdersPerLevel; ++i)
            {
                seed.push_back({ OperationType::Add, OrderType::GoodTillCancel, orderId++, Side::Buy, MidPrice - level, SeedQuantity });
                seed.push_back({ OperationType::Add, OrderType::GoodTillCancel, orderId++, Side::Sell, MidPrice + level, SeedQuantity });
            }
        }
        return seed;
    }

    // Tracks which orders are resting in the shadow book and how much of each
    // is left, so cancels and modifies can pick a live order in O(1).
    class LiveOrders
    {
    public:
        void Add(OrderId orderId, Quantity remaining)
        {
            index_[orderId] = ids_.size();
            ids_.push_back(orderId);
            remaining_[orderId] = remaining;
        }

        void Remove(OrderId orderId)
        {
            const auto position = index_.at(orderId);
            const OrderId last = ids_.back();
            ids_[position] = last;
            index_[last] = position;
            ids_.pop_back();
            index_.erase(orderId);
            remaining_.erase(orderId);
        }

        // Returns true if the resting order is now fully filled (and removed).
        bool Fill(OrderId orderId, Quantity quantity)
        {
            auto& remaining = remaining_.at(orderId);
            remaining -= quantity;
            if (remaining != 0)
                return false;

            Remove(orderId);
            return true;
        }

        bool Empty() const { return ids_.empty(); }
        std::size_t Size() const { return ids_.size(); }

        OrderId Random(std::mt19937_64& rng) const
        {
            std::uniform_int_distribution<std::size_t> index{ 0, ids_.size() - 1 };
            return ids_[index(rng)];
        }

    private:
        std::vector<OrderId> ids_;
        std::unordered_map<OrderId, std::size_t> index_;
        std::unordered_map<OrderId, Quantity> remaining_;
    };

    struct Workload
    {
        Operations operations_;
        std::size_t goodTillCancels_{ };
        std::size_t fillAndKills_{ };
        std::size_t fillOrKills_{ };
        std::size_t markets_{ };
        std::size_t cancels_{ };
        std::size_t modifies_{ };
        std::size_t minBookSize_{ };
        std::size_t maxBookSize_{ };
        double averageBookSize_{ };
    };

    // Limit prices sit from 10 ticks through the mid to 40 ticks behind it, so
    // about a fifth of limit orders cross and trade.
    Workload GenerateWorkload(std::size_t count, std::uint32_t seed)
    {
        std::mt19937_64 rng{ seed };
        std::uniform_int_distribution<int> percent{ 0, 99 };
        std::uniform_real_distribution<double> uniform{ 0.0, 100.0 };
        std::uniform_int_distribution<int> distanceFromMid{ -10, 40 };
        std::uniform_int_distribution<Quantity> quantity{ 1, 100 };
        std::bernoulli_distribution isBuy{ 0.5 };

        Orderbook shadow;
        LiveOrders live;
        for (const auto& operation : SeedOperations())
        {
            shadow.AddOrder(MakeOrder(operation));
            live.Add(operation.orderId_, operation.quantity_);
        }

        // Applies an add or modify to the shadow book and updates the live set
        // from the trades it produced.
        auto Submit = [&](const Operation& operation)
        {
            const auto sizeBefore = shadow.Size();
            const auto trades = operation.type_ == OperationType::Modify
                ? shadow.ModifyOrder(OrderModify{ operation.orderId_, operation.side_, operation.price_, operation.quantity_ })
                : shadow.AddOrder(MakeOrder(operation));

            if (operation.type_ == OperationType::Modify)
                live.Remove(operation.orderId_);

            Quantity incomingFilled = 0;
            std::size_t restingFilled = 0;
            for (const auto& trade : trades)
            {
                const bool incomingIsBid = trade.GetBidTrade().orderId_ == operation.orderId_;
                const auto& resting = incomingIsBid ? trade.GetAskTrade() : trade.GetBidTrade();
                incomingFilled += resting.quantity_;
                restingFilled += live.Fill(resting.orderId_, resting.quantity_) ? 1 : 0;
            }

            // Size changes by +1 if the incoming order rested, -1 per resting
            // order it filled completely, and -1 for the order a modify replaced.
            const auto removed = restingFilled + (operation.type_ == OperationType::Modify ? 1 : 0);
            const bool rested = shadow.Size() + removed == sizeBefore + 1;
            if (rested)
                live.Add(operation.orderId_, operation.quantity_ - incomingFilled);
        };

        Workload workload;
        workload.operations_.reserve(count);
        workload.minBookSize_ = shadow.Size();

        OrderId nextOrderId = SeedOperations().size() + 1;
        double bookSizeSum = 0;

        for (std::size_t i = 0; i < count; ++i)
        {
            const double roll = uniform(rng);
            const int typeRoll = percent(rng);
            const Side side = isBuy(rng) ? Side::Buy : Side::Sell;
            // Most orders rest behind the mid; those within 10 ticks on the far side cross.
            const Price price = side == Side::Buy ? MidPrice - distanceFromMid(rng) : MidPrice + distanceFromMid(rng);
            const Quantity size = quantity(rng);

            const double cancelPercent = std::min(MaxCancelPercent, CancelPercentAtTarget * live.Size() / TargetBookSize);

            Operation operation{ };
            if (!live.Empty() && roll < cancelPercent)
            {
                const OrderId orderId = live.Random(rng);
                operation = { OperationType::Cancel, OrderType::GoodTillCancel, orderId, side, price, size };
                shadow.CancelOrder(orderId);
                live.Remove(orderId);
                ++workload.cancels_;
            }
            else if (!live.Empty() && roll < cancelPercent + ModifyPercent)
            {
                operation = { OperationType::Modify, OrderType::GoodTillCancel, live.Random(rng), side, price, size };
                Submit(operation);
                ++workload.modifies_;
            }
            else
            {
                const OrderType type =
                    typeRoll < GoodTillCancelEnd ? OrderType::GoodTillCancel :
                    typeRoll < FillAndKillEnd ? OrderType::FillAndKill :
                    typeRoll < FillOrKillEnd ? OrderType::FillOrKill :
                    OrderType::Market;
                operation = { OperationType::Add, type, nextOrderId++, side, price, size };
                Submit(operation);
                ++(type == OrderType::GoodTillCancel ? workload.goodTillCancels_ :
                   type == OrderType::FillAndKill ? workload.fillAndKills_ :
                   type == OrderType::FillOrKill ? workload.fillOrKills_ :
                   workload.markets_);
            }

            workload.operations_.push_back(operation);

            const auto bookSize = shadow.Size();
            workload.minBookSize_ = std::min(workload.minBookSize_, bookSize);
            workload.maxBookSize_ = std::max(workload.maxBookSize_, bookSize);
            bookSizeSum += bookSize;

            if (live.Size() != bookSize)
            {
                std::cerr << "Live-order tracking diverged from the book at operation " << i << "\n";
                std::exit(1);
            }
        }

        workload.averageBookSize_ = bookSizeSum / count;
        return workload;
    }

    // One run's inputs: a freshly seeded book plus fresh Order objects for the
    // stream, built before the clock starts.
    struct Run
    {
        Orderbook orderbook_;
        std::vector<OrderPointer> orders_;

        explicit Run(const Operations& operations)
        {
            for (const auto& operation : SeedOperations())
                orderbook_.AddOrder(MakeOrder(operation));

            orders_.reserve(operations.size());
            for (const auto& operation : operations)
                orders_.push_back(operation.type_ == OperationType::Add ? MakeOrder(operation) : nullptr);
        }

        Trades Apply(const Operation& operation, const OrderPointer& order)
        {
            switch (operation.type_)
            {
            case OperationType::Add:
                return orderbook_.AddOrder(order);
            case OperationType::Cancel:
                orderbook_.CancelOrder(operation.orderId_);
                return { };
            case OperationType::Modify:
                return orderbook_.ModifyOrder(OrderModify{ operation.orderId_, operation.side_, operation.price_, operation.quantity_ });
            }
            return { };
        }
    };

    struct ChecksumResult
    {
        std::uint64_t hash_;
        std::size_t cancels_;
        std::size_t cancelsThatRemovedAnOrder_;
    };

    // FNV-1a over every trade, in order. Any optimisation that changes matching
    // behaviour changes this value, so it must stay identical across versions.
    // Also confirms that every cancel really removed a resting order.
    ChecksumResult RunChecksum(const Operations& operations)
    {
        Run run{ operations };

        std::uint64_t hash = 14695981039346656037ull;
        auto Mix = [&hash](std::uint64_t value)
        {
            hash ^= value;
            hash *= 1099511628211ull;
        };

        ChecksumResult result{ };
        for (std::size_t i = 0; i < operations.size(); ++i)
        {
            const auto sizeBefore = operations[i].type_ == OperationType::Cancel ? run.orderbook_.Size() : 0;

            for (const auto& trade : run.Apply(operations[i], run.orders_[i]))
            {
                Mix(trade.GetBidTrade().orderId_);
                Mix(trade.GetAskTrade().orderId_);
                Mix(static_cast<std::uint64_t>(trade.GetBidTrade().price_));
                Mix(static_cast<std::uint64_t>(trade.GetAskTrade().price_));
                Mix(trade.GetBidTrade().quantity_);
            }

            if (operations[i].type_ == OperationType::Cancel)
            {
                ++result.cancels_;
                result.cancelsThatRemovedAnOrder_ += run.orderbook_.Size() + 1 == sizeBefore ? 1 : 0;
            }
        }
        Mix(run.orderbook_.Size());

        result.hash_ = hash;
        return result;
    }

    double RunThroughput(const Operations& operations)
    {
        Run run{ operations };

        const auto start = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < operations.size(); ++i)
            run.Apply(operations[i], run.orders_[i]);
        const auto end = std::chrono::steady_clock::now();

        return std::chrono::duration<double>(end - start).count();
    }

    std::vector<std::int64_t> RunLatency(const Operations& operations)
    {
        Run run{ operations };

        std::vector<std::int64_t> latencies;
        latencies.reserve(operations.size());

        for (std::size_t i = 0; i < operations.size(); ++i)
        {
            const auto start = std::chrono::steady_clock::now();
            run.Apply(operations[i], run.orders_[i]);
            const auto end = std::chrono::steady_clock::now();
            latencies.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
        }

        std::sort(latencies.begin(), latencies.end());
        return latencies;
    }

    // Smallest non-zero step of steady_clock. On Apple Silicon this is ~42 ns,
    // so latencies are only accurate to that granularity.
    std::int64_t ClockResolution()
    {
        std::int64_t resolution = std::numeric_limits<std::int64_t>::max();
        for (int i = 0; i < 100'000; ++i)
        {
            const auto a = std::chrono::steady_clock::now();
            auto b = std::chrono::steady_clock::now();
            while (b == a)
                b = std::chrono::steady_clock::now();
            resolution = std::min<std::int64_t>(resolution, std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
        }
        return resolution;
    }

    std::int64_t Percentile(const std::vector<std::int64_t>& sorted, double percentile)
    {
        const auto index = static_cast<std::size_t>(percentile / 100.0 * (sorted.size() - 1));
        return sorted[index];
    }
}

int main(int argc, char** argv)
{
    const std::size_t count = argc > 1 ? std::stoull(argv[1]) : 1'000'000;
    const std::uint32_t seed = argc > 2 ? static_cast<std::uint32_t>(std::stoul(argv[2])) : 42;

    const auto workload = GenerateWorkload(count, seed);
    const auto& operations = workload.operations_;

    auto Share = [count](std::size_t n) { return 100.0 * n / count; };
    std::cout << std::fixed << std::setprecision(1);
    std::cout << "Operations: " << count << ", seed " << seed << "\n";
    std::cout << "Mix:        " << Share(workload.goodTillCancels_) << "% GTC, "
              << Share(workload.fillAndKills_) << "% FAK, "
              << Share(workload.fillOrKills_) << "% FOK, "
              << Share(workload.markets_) << "% market, "
              << Share(workload.cancels_) << "% cancel, "
              << Share(workload.modifies_) << "% modify\n";
    std::cout << "Seed book:  " << SeedLevels * SeedOrdersPerLevel * 2 << " resting orders over " << SeedLevels * 2 << " price levels\n";
    std::cout << std::fixed << std::setprecision(0);
    std::cout << "Book size:  " << workload.averageBookSize_ << " resting orders on average (min "
              << workload.minBookSize_ << ", max " << workload.maxBookSize_ << ")\n";

    const auto checksum = RunChecksum(operations);
    std::cout << "Cancels:    " << checksum.cancelsThatRemovedAnOrder_ << " of " << checksum.cancels_ << " removed a resting order\n";
    std::cout << "Checksum:   " << std::hex << checksum.hash_ << std::dec << "\n\n";

    // Throughput is measured without per-operation timers so clock calls don't skew it.
    // Median of 5 runs, since a single run varies by a few percent.
    std::vector<double> seconds;
    for (int i = 0; i < 5; ++i)
        seconds.push_back(RunThroughput(operations));
    std::sort(seconds.begin(), seconds.end());
    const double median = seconds[seconds.size() / 2];
    std::cout << "Throughput: " << count / median << " ops/sec"
              << std::setprecision(3) << " (" << median << " s, median of 5 runs)\n\n";

    const auto latencies = RunLatency(operations);
    std::cout << "Latency per operation (ns, clock resolution " << ClockResolution() << " ns):\n";
    for (double p : { 50.0, 90.0, 99.0, 99.9 })
        std::cout << "  p" << std::setprecision(p == 99.9 ? 1 : 0) << p << ": " << Percentile(latencies, p) << "\n";
    std::cout << "  max:  " << latencies.back() << "\n";

    return 0;
}
