#include <iostream>
#include "OrderBook.hpp"

namespace {

Order makeOrder(uint64_t id, Side side, OrderType type, uint64_t price, uint32_t qty, uint64_t owner) {
    Order o{};
    o.orderId = id;
    o.price = price;
    o.timestamp = id;
    o.quantity = qty;
    o.next_idx = UINT32_MAX;
    o.prev_idx = UINT32_MAX;
    o.side = side;
    o.type = type;
    o.ownerId = owner;
    return o;
}

void printTrades(const std::vector<Trade>& trades) {
    for (const auto& t : trades) {
        std::cout << "  TRADE: buy#" << t.buyOrderId << " x sell#" << t.sellOrderId
                   << " | price=" << t.price << " qty=" << t.quantity << '\n';
    }
    if (trades.empty()) {
        std::cout << "  (no trades)\n";
    }
}

} // namespace

int main() {
    OrderBook book(64, 101, 100);

    std::cout << "Resting two sell orders (100x5, 101x5)...\n";
    printTrades(book.submit(makeOrder(1, Side::Sell, OrderType::Limit, 100, 5, /*owner*/ 100)));
    printTrades(book.submit(makeOrder(2, Side::Sell, OrderType::Limit, 101, 5, /*owner*/ 101)));

    std::cout << "Resting a sell that will be canceled before it can trade...\n";
    printTrades(book.submit(makeOrder(3, Side::Sell, OrderType::Limit, 100, 3, /*owner*/ 102)));
    book.cancel(3);

    std::cout << "Submitting a buy for 10 @ 101 (should sweep both remaining levels)...\n";
    printTrades(book.submit(makeOrder(4, Side::Buy, OrderType::Limit, 101, 10, /*owner*/ 200)));

    std::cout << "Submitting a market buy for 5 against an empty book (should be discarded)...\n";
    printTrades(book.submit(makeOrder(5, Side::Buy, OrderType::Market, 0, 5, /*owner*/ 201)));

    return 0;
}
