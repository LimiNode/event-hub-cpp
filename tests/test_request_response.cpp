#include <event_hub.hpp>

#include <chrono>
#include <future>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

void expect(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Actual, typename Expected>
void expect_eq(const Actual& actual,
               const Expected& expected,
               const char* expression) {
    if (!(actual == expected)) {
        std::ostringstream stream;
        stream << expression << " failed: actual=" << actual
               << ", expected=" << expected;
        throw std::runtime_error(stream.str());
    }
}

struct EchoRequest {
    event_hub::RequestId request_id = event_hub::invalid_request_id;
    int value = 0;
};

struct EchoResult {
    event_hub::RequestId request_id = event_hub::invalid_request_id;
    int value = 0;
};

struct CustomRequest {
    event_hub::RequestId correlation_id = event_hub::invalid_request_id;
    int value = 0;
};

struct CustomResult {
    event_hub::RequestId correlation_id = event_hub::invalid_request_id;
    int value = 0;
};

struct ReplyRequest {
    int value = 0;
    event_hub::Reply<EchoResult> reply;
};

} // namespace

namespace event_hub {

template <>
struct RequestTraits<CustomRequest> {
    static RequestId get_id(const CustomRequest& event) noexcept {
        return event.correlation_id;
    }

    static void set_id(CustomRequest& event, RequestId id) noexcept {
        event.correlation_id = id;
    }
};

template <>
struct RequestTraits<CustomResult> {
    static RequestId get_id(const CustomResult& event) noexcept {
        return event.correlation_id;
    }

    static void set_id(CustomResult& event, RequestId id) noexcept {
        event.correlation_id = id;
    }
};

} // namespace event_hub

namespace {

class RequestNode final : public event_hub::EventNode {
public:
    explicit RequestNode(event_hub::EventBus& bus)
        : EventNode(bus) {}

    event_hub::RequestId start(int value, int& output) {
        EchoRequest event;
        event.value = value;

        return request<EchoRequest, EchoResult>(
            event,
            [&output](const EchoResult& result) {
                output = result.value;
            });
    }

    std::future<EchoResult> start_future(int value) {
        EchoRequest event;
        event.value = value;
        return request_future<EchoRequest, EchoResult>(event);
    }

    event_hub::RequestId allocate_request_id() {
        return next_request_id();
    }
};

class RequestModule final : public event_hub::Module {
public:
    explicit RequestModule(event_hub::EventBus& bus)
        : Module(bus) {}

    event_hub::RequestId start(int value, int& output) {
        EchoRequest event;
        event.value = value;

        return request<EchoRequest, EchoResult>(
            event,
            [&output](const EchoResult& result) {
                output = result.value;
            });
    }
};

void add_echo_service(event_hub::EventEndpoint& service) {
    service.subscribe_queued<EchoRequest>([&service](const EchoRequest& request) {
        EchoResult result;
        result.request_id = request.request_id;
        result.value = request.value * 2;
        service.post<EchoResult>(result);
    });
}

void test_request_callback() {
    event_hub::EventBus bus;
    event_hub::EventEndpoint client(bus);
    event_hub::EventEndpoint service(bus);
    add_echo_service(service);

    int output = 0;
    EchoRequest event;
    event.value = 21;

    const auto id = client.request<EchoRequest, EchoResult>(
        event,
        [&output](const EchoResult& result) {
            output = result.value;
        });

    expect(id != event_hub::invalid_request_id, "request id must be valid");
    expect_eq(bus.process(), 1U, "first process count");
    expect_eq(output, 0, "output before result");
    expect_eq(bus.process(), 1U, "second process count");
    expect_eq(output, 42, "output after result");
}

void test_request_future() {
    event_hub::EventBus bus;
    event_hub::EventEndpoint client(bus);
    event_hub::EventEndpoint service(bus);
    add_echo_service(service);

    EchoRequest event;
    event.value = 7;

    auto future = client.request_future<EchoRequest, EchoResult>(event);

    expect_eq(bus.process(), 1U, "first future process count");
    expect(future.wait_for(std::chrono::milliseconds(0)) ==
               std::future_status::timeout,
           "future must not be ready before result event is processed");

    expect_eq(bus.process(), 1U, "second future process count");
    const auto result = future.get();
    expect_eq(result.value, 14, "future result value");
}

void test_request_future_timeout() {
    event_hub::EventBus bus;
    event_hub::EventEndpoint client(bus);

    EchoRequest event;
    event.value = 1;

    bool timeout_called = false;
    auto options = event_hub::AwaitOptions::timeout_ms(1);
    options.on_timeout = [&timeout_called] {
        timeout_called = true;
    };

    auto future = client.request_future<EchoRequest, EchoResult>(event, options);

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(250);
    while (future.wait_for(std::chrono::milliseconds(0)) !=
           std::future_status::ready) {
        (void)bus.process();
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error("request_future timeout test did not complete");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    expect(timeout_called, "timeout callback must be called");

    try {
        (void)future.get();
        throw std::runtime_error("request_future must throw after timeout");
    } catch (const event_hub::RequestTimeoutError& error) {
        expect(error.request_id() != event_hub::invalid_request_id,
               "timeout error request id must be valid");
    }
}

void test_request_future_cancellation() {
    event_hub::EventBus bus;
    event_hub::EventEndpoint client(bus);
    event_hub::CancellationSource source;

    EchoRequest event;
    event.value = 3;
    auto options = event_hub::AwaitOptions{};
    options.token = source.token();

    auto future = client.request_future<EchoRequest, EchoResult>(
        event, std::move(options));
    source.cancel();

    // Cancellation is cooperative and observed at the bus poll point.
    (void)bus.process();
    expect(future.wait_for(std::chrono::milliseconds(0)) ==
               std::future_status::ready,
           "future must become ready after cancellation is polled");

    try {
        (void)future.get();
        throw std::runtime_error("request_future must throw after cancellation");
    } catch (const event_hub::RequestCancelledError& error) {
        expect(error.request_id() != event_hub::invalid_request_id,
               "cancelled request id must be valid");
    }
}

void test_request_operations_after_close() {
    event_hub::EventBus bus;
    event_hub::EventEndpoint client(bus);
    client.close();

    EchoRequest event;
    event.value = 1;
    auto future = client.request_future<EchoRequest, EchoResult>(event);
    expect(future.wait_for(std::chrono::milliseconds(0)) ==
               std::future_status::ready,
           "closed endpoint future must be ready");
    try {
        (void)future.get();
        throw std::runtime_error("closed endpoint future must throw");
    } catch (const event_hub::EventEndpointClosedError&) {
    }

    expect(client.request<EchoRequest, EchoResult>(
                event, [](const EchoResult&) {}) ==
               event_hub::invalid_request_id,
           "request on a closed endpoint must be rejected");
}

void test_request_traits_custom_field() {
    event_hub::EventBus bus;
    event_hub::EventEndpoint client(bus);
    event_hub::EventEndpoint service(bus);

    service.subscribe_queued<CustomRequest>([&service](const CustomRequest& request) {
        CustomResult result;
        result.correlation_id = request.correlation_id;
        result.value = request.value + 3;
        service.post<CustomResult>(result);
    });

    int output = 0;
    CustomRequest event;
    event.value = 4;

    const auto id = client.request<CustomRequest, CustomResult>(
        event,
        [&output](const CustomResult& result) {
            output = result.value;
        });

    expect(id != event_hub::invalid_request_id, "custom request id must be valid");
    expect_eq(bus.process(), 1U, "first custom process count");
    expect_eq(bus.process(), 1U, "second custom process count");
    expect_eq(output, 7, "custom result output");
}

void test_reply_callback_in_event() {
    event_hub::EventBus bus;
    event_hub::EventEndpoint client(bus);
    event_hub::EventEndpoint service(bus);

    service.subscribe_queued<ReplyRequest>([](const ReplyRequest& request) {
        EchoResult result;
        result.value = request.value * 3;
        request.reply(result);
    });

    int output = 0;
    ReplyRequest event;
    event.value = 5;
    event.reply = event_hub::Reply<EchoResult>(
        [&output](const EchoResult& result) {
            output = result.value;
        });

    client.post<ReplyRequest>(event);
    expect_eq(bus.process(), 1U, "reply process count");
    expect_eq(output, 15, "reply output");

    auto copy = event.reply;
    EchoResult result;
    result.value = 2;
    copy(result);
    expect_eq(output, 2, "copied reply output");

    copy.reset();
    expect(!copy, "reset reply must be empty");
}

void test_event_node_request_helpers() {
    event_hub::EventBus bus;
    event_hub::EventEndpoint service(bus);
    add_echo_service(service);
    RequestNode node(bus);

    int output = 0;
    const auto id = node.start(6, output);

    expect(id != event_hub::invalid_request_id, "node request id must be valid");
    expect(node.allocate_request_id() != id, "node request ids must be unique");
    expect_eq(bus.process(), 1U, "first node process count");
    expect_eq(bus.process(), 1U, "second node process count");
    expect_eq(output, 12, "node output");

    auto future = node.start_future(8);
    expect_eq(bus.process(), 1U, "first node future process count");
    expect_eq(bus.process(), 1U, "second node future process count");
    expect_eq(future.get().value, 16, "node future output");
}

void test_module_inherits_request_helpers() {
    event_hub::EventBus bus;
    event_hub::EventEndpoint service(bus);
    add_echo_service(service);
    RequestModule module(bus);

    int output = 0;
    const auto id = module.start(9, output);

    expect(id != event_hub::invalid_request_id, "module request id must be valid");
    expect_eq(bus.process(), 1U, "first module process count");
    expect_eq(bus.process(), 1U, "second module process count");
    expect_eq(output, 18, "module output");
}

} // namespace

int main() {
    test_request_callback();
    test_request_future();
    test_request_future_timeout();
    test_request_future_cancellation();
    test_request_operations_after_close();
    test_request_traits_custom_field();
    test_reply_callback_in_event();
    test_event_node_request_helpers();
    test_module_inherits_request_helpers();
    return 0;
}
