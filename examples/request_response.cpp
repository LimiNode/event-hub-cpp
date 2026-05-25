#include <event_hub.hpp>

#include <iostream>
#include <string>

struct PriceRequest {
    event_hub::RequestId request_id = event_hub::invalid_request_id;
    std::string symbol;
};

struct PriceResult {
    event_hub::RequestId request_id = event_hub::invalid_request_id;
    std::string symbol;
    double price = 0.0;
};

struct BalanceResult {
    double balance = 0.0;
};

struct BalanceRequest {
    std::string account;
    event_hub::Reply<BalanceResult> reply;
};

int main() {
    event_hub::EventBus bus;
    event_hub::EventEndpoint client(bus);
    event_hub::EventEndpoint service(bus);

    service.subscribe_queued<PriceRequest>([&service](const PriceRequest& request) {
        PriceResult result;
        result.request_id = request.request_id;
        result.symbol = request.symbol;
        result.price = 123.45;

        // The result is posted, so it will be processed by a later process()
        // call because EventBus drains a queue snapshot.
        service.post<PriceResult>(result);
    });

    PriceRequest price_request;
    price_request.symbol = "EURUSD";

    client.request<PriceRequest, PriceResult>(
        price_request,
        [](const PriceResult& result) {
            std::cout << result.symbol << " price: " << result.price << '\n';
        });

    bus.process(); // Handles PriceRequest and posts PriceResult.
    bus.process(); // Handles PriceResult and invokes the request callback.

    service.subscribe_queued<BalanceRequest>([](const BalanceRequest& request) {
        BalanceResult result;
        result.balance = request.account == "demo" ? 10000.0 : 1000.0;

        // Reply<T> is a copyable callback-in-event shortcut. Prefer the
        // request/result pair above when the response should be observable,
        // logged, filtered, or awaited by several modules.
        request.reply(result);
    });

    BalanceRequest balance_request;
    balance_request.account = "demo";
    balance_request.reply = event_hub::Reply<BalanceResult>(
        [](const BalanceResult& result) {
            std::cout << "balance: " << result.balance << '\n';
        });

    client.post<BalanceRequest>(balance_request);
    bus.process();
}
