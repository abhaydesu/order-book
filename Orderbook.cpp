#include "Orderbook.h"

#include <chrono>
#include <ctime>

void Orderbook::PruneGoodForDayOrders()
{    
    using namespace std::chrono;
    const auto end = hours(16);

	while (true)
	{
		const auto now = system_clock::now();
		const auto now_c = system_clock::to_time_t(now);
		std::tm now_parts;
		localtime_r(&now_c, &now_parts);

		if (now_parts.tm_hour >= end.count())
			now_parts.tm_mday += 1;

		now_parts.tm_hour = end.count();
		now_parts.tm_min = 0;
		now_parts.tm_sec = 0;

		auto next = system_clock::from_time_t(mktime(&now_parts));
		auto till = next - now + milliseconds(100);

		{
			std::unique_lock ordersLock{ ordersMutex_ };

			if (shutdownConditionVariable_.wait_for(ordersLock, till, [this] { return shutdown_.load(std::memory_order_acquire); }))
				return;
		}

		OrderIds orderIds;

		{
			std::scoped_lock ordersLock{ ordersMutex_ };

			for (const auto& [_, entry] : orders_)
			{
				const auto& order = entry.order_;

				if (order->GetOrderType() != OrderType::GoodForDay)
					continue;

				orderIds.push_back(order->GetOrderId());
			}
		}

		CancelOrders(orderIds);
	}
}

void Orderbook::CancelOrders(OrderIds orderIds)
{
	std::scoped_lock ordersLock{ ordersMutex_ };

	for (const auto& orderId : orderIds)
		CancelOrderInternal(orderId);
}

void Orderbook::CancelOrderInternal(OrderId orderId)
{
	const auto entry = orders_.find(orderId);
	if (entry == orders_.end())
		return;

	const auto [order, iterator] = entry->second;
	orders_.erase(entry);

	if (order->GetSide() == Side::Sell)
	{
		auto price = order->GetPrice();
		auto& level = asks_.at(price);
		level.orders_.erase(iterator);
		level.quantity_ -= order->GetRemainingQuantity();
		if (level.orders_.empty())
			asks_.erase(price);
	}
	else
	{
		auto price = order->GetPrice();
		auto& level = bids_.at(price);
		level.orders_.erase(iterator);
		level.quantity_ -= order->GetRemainingQuantity();
		if (level.orders_.empty())
			bids_.erase(price);
	}
}

bool Orderbook::CanFullyFill(Side side, Price price, Quantity quantity) const
{
	if (!CanMatch(side, price))
		return false;

	// Walk the opposite side from its best price, stopping at our limit price.
	auto CanFill = [&quantity](const auto& levels, auto isBeyondLimit)
	{
		for (const auto& [levelPrice, level] : levels)
		{
			if (isBeyondLimit(levelPrice))
				break;

			if (quantity <= level.quantity_)
				return true;

			quantity -= level.quantity_;
		}

		return false;
	};

	if (side == Side::Buy)
		return CanFill(asks_, [price](Price levelPrice) { return levelPrice > price; });

	return CanFill(bids_, [price](Price levelPrice) { return levelPrice < price; });
}

bool Orderbook::CanMatch(Side side, Price price) const
{
	if (side == Side::Buy)
	{
		if (asks_.empty())
			return false;

		const auto& [bestAsk, _] = *asks_.begin();
		return price >= bestAsk;
	}
	else
	{
		if (bids_.empty())
			return false;

		const auto& [bestBid, _] = *bids_.begin();
		return price <= bestBid;
	}
}

Trades Orderbook::MatchOrders()
{
	Trades trades;

	while (true)
	{
		if (bids_.empty() || asks_.empty())
			break;

		auto& [bidPrice, bidLevel] = *bids_.begin();
		auto& [askPrice, askLevel] = *asks_.begin();

		if (bidPrice < askPrice)
			break;

		auto& bids = bidLevel.orders_;
		auto& asks = askLevel.orders_;

		while (!bids.empty() && !asks.empty())
		{
			auto bid = bids.front();
			auto ask = asks.front();

			Quantity quantity = std::min(bid->GetRemainingQuantity(), ask->GetRemainingQuantity());

			bid->Fill(quantity);
			ask->Fill(quantity);
			bidLevel.quantity_ -= quantity;
			askLevel.quantity_ -= quantity;

			if (bid->IsFilled())
			{
				bids.pop_front();
				orders_.erase(bid->GetOrderId());
			}

			if (ask->IsFilled())
			{
				asks.pop_front();
				orders_.erase(ask->GetOrderId());
			}

			trades.push_back(Trade{
				TradeInfo{ bid->GetOrderId(), bid->GetPrice(), quantity },
				TradeInfo{ ask->GetOrderId(), ask->GetPrice(), quantity } 
				});
		}

		// Erase by iterator: bidPrice/askPrice reference the map keys being erased.
		if (bids.empty())
			bids_.erase(bids_.begin());

		if (asks.empty())
			asks_.erase(asks_.begin());
	}

	if (!bids_.empty())
	{
		auto& order = bids_.begin()->second.orders_.front();
		if (order->GetOrderType() == OrderType::FillAndKill)
			CancelOrderInternal(order->GetOrderId());
	}

	if (!asks_.empty())
	{
		auto& order = asks_.begin()->second.orders_.front();
		if (order->GetOrderType() == OrderType::FillAndKill)
			CancelOrderInternal(order->GetOrderId());
	}

	return trades;
}

Orderbook::Orderbook() : ordersPruneThread_{ [this] { PruneGoodForDayOrders(); } } { }

Orderbook::~Orderbook()
{
	{
		std::scoped_lock ordersLock{ ordersMutex_ };
		shutdown_.store(true, std::memory_order_release);
	}
	shutdownConditionVariable_.notify_one();
	ordersPruneThread_.join();
}

Trades Orderbook::AddOrder(OrderPointer order)
{
	std::scoped_lock ordersLock{ ordersMutex_ };

	if (orders_.contains(order->GetOrderId()))
		return { };

	if (order->GetOrderType() == OrderType::Market)
	{
		if (order->GetSide() == Side::Buy && !asks_.empty())
		{
			const auto& [worstAsk, _] = *asks_.rbegin();
			order->ToGoodTillCancel(worstAsk);
		}
		else if (order->GetSide() == Side::Sell && !bids_.empty())
		{
			const auto& [worstBid, _] = *bids_.rbegin();
			order->ToGoodTillCancel(worstBid);
		}
		else
			return { };
	}

	if (order->GetOrderType() == OrderType::FillAndKill && !CanMatch(order->GetSide(), order->GetPrice()))
		return { };

	if (order->GetOrderType() == OrderType::FillOrKill && !CanFullyFill(order->GetSide(), order->GetPrice(), order->GetInitialQuantity()))
		return { };

	OrderPointers::iterator iterator;

	if (order->GetSide() == Side::Buy)
	{
		auto& level = bids_[order->GetPrice()];
		level.orders_.push_back(order);
		level.quantity_ += order->GetInitialQuantity();
		iterator = std::prev(level.orders_.end());
	}
	else
	{
		auto& level = asks_[order->GetPrice()];
		level.orders_.push_back(order);
		level.quantity_ += order->GetInitialQuantity();
		iterator = std::prev(level.orders_.end());
	}

	orders_.insert({ order->GetOrderId(), OrderEntry{ order, iterator } });

	return MatchOrders();

}

void Orderbook::CancelOrder(OrderId orderId)
{
	std::scoped_lock ordersLock{ ordersMutex_ };

	CancelOrderInternal(orderId);
}

Trades Orderbook::ModifyOrder(OrderModify order)
{
	OrderType orderType;

	{
		std::scoped_lock ordersLock{ ordersMutex_ };

		const auto entry = orders_.find(order.GetOrderId());
		if (entry == orders_.end())
			return { };

		orderType = entry->second.order_->GetOrderType();
	}

	CancelOrder(order.GetOrderId());
	return AddOrder(order.ToOrderPointer(orderType));
}

std::size_t Orderbook::Size() const
{
	std::scoped_lock ordersLock{ ordersMutex_ };
	return orders_.size(); 
}

OrderbookLevelInfos Orderbook::GetOrderInfos() const
{
	std::scoped_lock ordersLock{ ordersMutex_ };

	LevelInfos bidInfos, askInfos;
	bidInfos.reserve(orders_.size());
	askInfos.reserve(orders_.size());

	for (const auto& [price, level] : bids_)
		bidInfos.push_back(LevelInfo{ price, level.quantity_ });

	for (const auto& [price, level] : asks_)
		askInfos.push_back(LevelInfo{ price, level.quantity_ });

	return OrderbookLevelInfos{ bidInfos, askInfos };

}

