# Project Structure Guide

This guide describes a recommended project layout for applications and
libraries built around `event-hub-cpp`.

The layout is not mandatory. It is a convention for keeping large modular C++
projects understandable as they grow.

The examples below use a fictional market application:

- `market_data` receives quotes and candles from external providers.
- `order_routing` sends orders to external venues.
- `risk_control` validates requests before execution.
- `notifications` sends user or system notifications.
- `persistence` stores events and domain data.
- `analytics` calculates derived metrics.

Replace these names with module names from your own domain.

## Core Idea

A module usually contains several kinds of code:

```text
data/       Pure DTOs, enums, configs, and value types.
events.hpp  Event-bus contracts and integration wrappers.
providers/  External data or service providers.
adapters/   Bridges between services and transports.
detail/     Internal implementation helpers.
module.hpp  Entry point for using the module as an executable component.
```

The key separation is:

```text
data/ contains domain data.
events.hpp contains event-bus integration contracts.
module.hpp documents and exposes the executable module/component API.
```

Do not mix these layers unless the module is intentionally tiny.

## Large Project Layout

For a large application with multiple modules, prefer one directory per module
and one broad umbrella header per module:

```text
src/
├── market_data.hpp
├── order_routing.hpp
├── risk_control.hpp
├── notifications.hpp
├── persistence.hpp
├── analytics.hpp
└── market_data/
    ├── module.hpp
    ├── events.hpp
    ├── data/
    │   ├── config.hpp
    │   ├── commands.hpp
    │   ├── config/
    │   │   ├── market_data_config.hpp
    │   │   ├── market_data_error.hpp
    │   │   └── market_data_provider_type.hpp
    │   └── commands/
    │       ├── quote_request.hpp
    │       ├── quote_response.hpp
    │       ├── candles_request.hpp
    │       ├── candles_response.hpp
    │       ├── subscribe_quotes_request.hpp
    │       └── subscribe_quotes_response.hpp
    ├── providers.hpp
    ├── providers/
    │   ├── market_data_provider.hpp
    │   ├── market_data_provider_factory.hpp
    │   ├── websocket_market_data_provider.hpp
    │   ├── rest_market_data_provider.hpp
    │   └── mock_market_data_provider.hpp
    ├── adapters/
    │   ├── market_data_event_api.hpp
    │   ├── market_data_event_api.cpp
    │   ├── market_data_http_api.hpp
    │   └── market_data_websocket_api.hpp
    ├── detail/
    │   ├── symbol_normalizer.hpp
    │   ├── retry_state.hpp
    │   └── provider_response_parser.hpp
    ├── market_data_service.hpp
    └── market_data_service.cpp
```

Use the same pattern for other modules:

```text
src/
├── <module>.hpp
└── <module>/
    ├── module.hpp
    ├── events.hpp
    ├── data/
    ├── providers/
    ├── adapters/
    └── detail/
```

This makes composition roots explicit:

```cpp
#include "market_data/module.hpp"
#include "order_routing/module.hpp"
#include "risk_control/module.hpp"
#include "notifications/module.hpp"
```

The include path itself says that the code is connecting executable modules,
not just importing unrelated DTOs.

## Standalone Module Repository Layout

When a module is developed as a separate library or repository, move public
headers into `include/` and implementation into `src/`:

```text
market-data/
├── include/
│   ├── market_data.hpp
│   └── market_data/
│       ├── module.hpp
│       ├── events.hpp
│       ├── data/
│       │   ├── config.hpp
│       │   ├── commands.hpp
│       │   ├── config/
│       │   │   ├── market_data_config.hpp
│       │   │   ├── market_data_error.hpp
│       │   │   └── market_data_provider_type.hpp
│       │   └── commands/
│       │       ├── quote_request.hpp
│       │       ├── quote_response.hpp
│       │       ├── candles_request.hpp
│       │       └── candles_response.hpp
│       ├── providers.hpp
│       ├── providers/
│       │   ├── market_data_provider.hpp
│       │   └── market_data_provider_factory.hpp
│       └── adapters/
│           └── market_data_event_api.hpp
├── src/
│   ├── market_data_service.cpp
│   ├── adapters/
│   │   └── market_data_event_api.cpp
│   └── providers/
│       ├── websocket_market_data_provider.cpp
│       ├── rest_market_data_provider.cpp
│       └── mock_market_data_provider.cpp
├── tests/
├── examples/
└── CMakeLists.txt
```

External users can choose the include level:

```cpp
#include <market_data.hpp>                 // Whole public domain.
#include <market_data/module.hpp>          // Executable module API.
#include <market_data/events.hpp>          // Event contracts only.
#include <market_data/data/commands.hpp>   // Operation DTOs only.
#include <market_data/providers.hpp>       // Provider interfaces/factories.
```

## Top-Level Umbrella Header

For a module named `market_data`, this file:

```text
src/market_data.hpp
```

or in standalone form:

```text
include/market_data.hpp
```

means:

```text
Give me the whole public market_data domain.
```

It may include:

```cpp
#pragma once

#include "market_data/module.hpp"
#include "market_data/events.hpp"
#include "market_data/data/config.hpp"
#include "market_data/data/commands.hpp"
#include "market_data/providers.hpp"
```

Do not include private `detail/` headers from the broad umbrella.

## module.hpp

`module.hpp` is the entry point for using a module as an executable component.
It is intended for composition roots and integrations that instantiate,
register, or connect modules.

Example:

```text
src/market_data/module.hpp
```

A typical `module.hpp` includes the module class and the public event contract:

```cpp
#pragma once

#include "market_data/events.hpp"
#include "market_data/adapters/market_data_event_api.hpp"
```

Usage:

```cpp
#include "market_data/module.hpp"

int main() {
    event_hub::EventBus bus;
    event_hub::ModuleHub hub(bus);

    auto market_data = std::make_shared<MarketDataEventApi>(bus);
    hub.add(market_data);
}
```

`module.hpp` should not necessarily include every provider, DTO, or detail
header. It should include what is needed to use the module as a module.

### Required Event API Documentation

Every module-level `module.hpp` should document the module's event API with
Doxygen comments. A reader should be able to open `module.hpp` and understand:

- which request events can be posted to the module;
- which result events the module publishes;
- which notification or state events the module may emit;
- what each request means;
- how request/result pairs are correlated;
- whether events are synchronous, queued, or produced by async callbacks;
- what lifecycle or processing assumptions exist.

A concise format is usually enough:

```cpp
/// \file module.hpp
/// \brief Public entry point for the market data module.
///
/// Event API:
/// - Accepts QuoteRequestedEvent to request one latest quote.
/// - Publishes QuoteReceivedEvent with the same request_id when a quote is
///   available.
/// - Accepts CandlesRequestedEvent to request historical candles.
/// - Publishes CandlesReceivedEvent with the same request_id when candles are
///   available.
/// - Publishes MarketDataStatusChangedEvent when provider connectivity changes.
///
/// Request/result correlation:
/// - All request/result event pairs use event_hub::RequestId.
/// - The request_id belongs to the event layer, not to pure DTOs.
///
/// Processing:
/// - MarketDataEventApi is a passive event_hub::Module.
/// - ModuleHub::process() calls the module's on_process(), which drives the
///   underlying MarketDataService.
```

If the module's event API becomes large, keep a summary in `module.hpp` and link
or include a dedicated `events.hpp` section with detailed Doxygen comments for
individual event structs.

## events.hpp

`events.hpp` contains event-bus contracts.

Events are not the same as DTOs.

A DTO describes the operation itself:

```cpp
struct QuoteRequest {
    Symbol symbol;
};
```

An event describes how this operation is transported through the event bus:

```cpp
struct QuoteRequestedEvent {
    event_hub::RequestId request_id = event_hub::invalid_request_id;
    QuoteRequest request;
};
```

The `request_id` is correlation metadata of the event-bus layer. It is not
business data.

Prefer:

```text
market_data/events.hpp
```

or, for larger modules:

```text
market_data/
├── events.hpp
└── events/
    ├── quote_requested_event.hpp
    ├── quote_received_event.hpp
    ├── candles_requested_event.hpp
    ├── candles_received_event.hpp
    └── events.hpp
```

Do not place event-bus wrappers into `data/` unless the module is intentionally
tiny and unlikely to grow.

## One events.hpp File or Many Event Files?

Use one `events.hpp` when:

- the module has few events;
- the file is easy to read;
- events are tightly related;
- the file is roughly under 150-200 lines.

Use an `events/` subdirectory when:

- the module has many events;
- event groups are independent;
- different consumers need only subsets of events;
- the single file becomes noisy.

Avoid premature file explosion. Split when it improves navigation.

## data/

`data/` contains pure value types:

- DTOs;
- enums;
- configs;
- small domain structs;
- serialization-friendly structures.

These types should not depend on `EventBus`, `EventEndpoint`, `ModuleHub`, or
module lifecycle.

Good examples:

```text
market_data/
└── data/
    ├── config/
    │   ├── market_data_config.hpp
    │   ├── market_data_error.hpp
    │   └── market_data_provider_type.hpp
    └── commands/
        ├── quote_request.hpp
        └── quote_response.hpp
```

Bad examples for `data/`:

```text
QuoteRequestedEvent          // event-bus wrapper
MarketDataEventApi           // adapter/module integration
WebSocketMarketDataProvider  // provider implementation
```

`data/` should be reusable by services, tests, providers, adapters, and event
wrappers.

## data/commands/

Use `data/commands/` for operation payloads:

```text
market_data/
└── data/
    └── commands/
        ├── quote_request.hpp
        ├── quote_response.hpp
        ├── candles_request.hpp
        ├── candles_response.hpp
        ├── subscribe_quotes_request.hpp
        └── subscribe_quotes_response.hpp
```

These are not event-bus events. They are payload DTOs used by services and
adapters:

```cpp
m_service.request_quote_async(
    QuoteRequest{symbol},
    [](QuoteResponse response) {
        // Handle response.
    });
```

The event layer can wrap them:

```cpp
struct QuoteRequestedEvent {
    event_hub::RequestId request_id = event_hub::invalid_request_id;
    QuoteRequest request;
};
```

Prefer `commands/` over `dto/`. The name `dto/` is often too generic and tends
to become a dumping ground. `commands/` says that these structures represent
operation inputs and outputs.

## data/config/

Use `data/config/` for configuration and related enums:

```text
market_data/
└── data/
    └── config/
        ├── market_data_config.hpp
        ├── market_data_error.hpp
        └── market_data_provider_type.hpp
```

Prefer precise names. For example, if a type selects a provider/backend, prefer:

```text
market_data_provider_type.hpp
```

over:

```text
market_data_service.hpp
```

unless the type really describes a service.

Use:

```text
market_data_service_config.hpp
```

only when it is actually a configuration object for the service.

## providers/

`providers/` contains implementations that talk to external systems or provide
interchangeable behavior.

Examples:

```text
market_data/
├── providers.hpp
└── providers/
    ├── market_data_provider.hpp
    ├── market_data_provider_factory.hpp
    ├── websocket_market_data_provider.hpp
    ├── rest_market_data_provider.hpp
    └── mock_market_data_provider.hpp
```

A provider usually answers:

```text
Where do we get this data?
Which backend implementation performs this operation?
```

Use providers when:

- there are multiple external APIs;
- behavior is selected by config;
- implementation can be swapped;
- tests need fake providers.

Keep provider interfaces small and close to the module domain.

## adapters/

`adapters/` contains bridges between the module service and external
communication mechanisms.

Examples:

```text
market_data/
└── adapters/
    ├── market_data_event_api.hpp
    ├── market_data_event_api.cpp
    ├── market_data_http_api.hpp
    ├── market_data_cli_api.hpp
    └── market_data_websocket_api.hpp
```

An event adapter bridges:

```text
event_hub events <-> module service callback/DTO API
```

Typical event adapter:

```cpp
void MarketDataEventApi::on_initialize() {
    subscribe<QuoteRequestedEvent>([this](const auto& event) {
        const auto id = event.request_id;

        m_service.request_quote_async(
            event.request,
            [this, id](QuoteResponse response) {
                post<QuoteReceivedEvent>(
                    QuoteReceivedEvent{id, std::move(response)});
            });
    });
}
```

If there is only one adapter and the module is small, keeping it at module root
is acceptable:

```text
market_data/
├── market_data_event_api.hpp
└── market_data_event_api.cpp
```

Introduce `adapters/` when:

- there are two or more adapters;
- adapter files dominate the module root;
- the module exposes several integration surfaces such as event bus, HTTP, CLI,
  Telegram, or WebSocket.

## detail/

`detail/` contains implementation details that are not part of the public module
API.

Examples:

```text
market_data/
└── detail/
    ├── symbol_normalizer.hpp
    ├── retry_state.hpp
    └── provider_response_parser.hpp
```

Rules:

- Public code should not include detail headers unless it is inside the same
  module.
- Do not expose detail types in public headers.
- Do not place stable DTOs into `detail/`.

If a type becomes part of the module contract, move it out of `detail/`.

## Services, Modules, And Adapters

Keep these concepts separate.

### Service

A service contains business or application logic:

```cpp
class MarketDataService {
public:
    void request_quote_async(QuoteRequest request,
                             std::function<void(QuoteResponse)> callback);

    bool process();
};
```

It should not need to know about `EventBus` unless the whole service is
intentionally event-driven.

### Module

A module is an executable component managed by `event_hub::ModuleHub`:

```cpp
class MarketDataEventApi : public event_hub::Module {
public:
    explicit MarketDataEventApi(event_hub::EventBus& bus);

protected:
    void on_initialize() override;
    std::size_t on_process() override;
};
```

### Adapter

An adapter connects a service to a transport or integration layer:

- event bus;
- HTTP API;
- CLI;
- Telegram bot;
- WebSocket.

`MarketDataEventApi` can be both a module and an adapter.

## Request/Result Events

For request/response flows, prefer paired event types:

```text
RequestEvent -> ResultEvent
```

with the same `event_hub::RequestId`.

Example:

```cpp
struct QuoteRequestedEvent {
    event_hub::RequestId request_id = event_hub::invalid_request_id;
    QuoteRequest request;
};

struct QuoteReceivedEvent {
    event_hub::RequestId request_id = event_hub::invalid_request_id;
    QuoteResponse response;
};
```

This keeps correlation outside pure business DTOs.

The service does not need to know about `request_id`:

```cpp
m_service.request_quote_async(event.request, callback);
```

The adapter preserves the id:

```cpp
subscribe<QuoteRequestedEvent>([this](const auto& event) {
    const auto id = event.request_id;

    m_service.request_quote_async(event.request, [this, id](QuoteResponse response) {
        post<QuoteReceivedEvent>(QuoteReceivedEvent{id, std::move(response)});
    });
});
```

Prefer constructors or helper functions that make it hard to forget
`request_id`:

```cpp
struct QuoteReceivedEvent {
    event_hub::RequestId request_id = event_hub::invalid_request_id;
    QuoteResponse response;

    QuoteReceivedEvent(event_hub::RequestId id, QuoteResponse response)
        : request_id(id),
          response(std::move(response)) {}
};
```

Then:

```cpp
post<QuoteReceivedEvent>(QuoteReceivedEvent{id, std::move(response)});
```

## Direct Module API vs endpoint()

Inside `event_hub::Module` subclasses, prefer direct inherited methods:

```cpp
subscribe<MyEvent>(...);
post<MyEvent>(...);
emit_direct<MyEvent>(...);
request<MyRequestEvent, MyResultEvent>(...);
```

Avoid this unless direct endpoint access is actually needed:

```cpp
endpoint().post<MyEvent>(...);
```

`endpoint()` is an escape hatch for advanced cases:

- passing the endpoint to another object;
- using an `EventEndpoint` method not exposed by `EventNode`;
- low-level integration code.

For normal module code, direct methods are clearer.

## Naming Guidelines

Use snake_case file names:

```text
quote_request.hpp
quote_response.hpp
market_data_provider_type.hpp
market_data_event_api.hpp
```

Use domain-specific class names:

```cpp
QuoteRequest
QuoteResponse
MarketDataProviderType
MarketDataService
MarketDataEventApi
```

Prefer names that describe meaning, not implementation accidents.

Examples:

```text
market_data_provider_type.hpp   good
market_data_service.hpp         ambiguous if it is only an enum
commands/                       good for operation payloads
dto/                            too generic
```

## When To Split Files

Start simple.

One file is fine when:

- the file is short;
- types are tightly coupled;
- consumers usually need all of them;
- splitting adds only boilerplate.

Split when:

- the file exceeds roughly 150-200 lines;
- there are independent groups;
- many files include only one small part;
- merge conflicts become frequent;
- generated or template-heavy code slows builds.

Good examples:

```text
market_data/
├── events.hpp              # fine for a few short event structs
├── data/
│   └── commands/*.hpp      # useful when request/response DTOs grow
└── providers/*.hpp         # useful because providers are independent
```

## Practical Include Levels

A healthy module should allow several include levels:

```cpp
#include "market_data.hpp"
```

Whole public domain.

```cpp
#include "market_data/module.hpp"
```

Executable module API.

```cpp
#include "market_data/events.hpp"
```

Event-bus contracts.

```cpp
#include "market_data/data/commands.hpp"
```

Operation DTOs.

```cpp
#include "market_data/providers.hpp"
```

Provider interfaces and factories.

Avoid requiring users to include deep implementation files unless they really
need them.

## Mechanical Refactoring Rule

When reorganizing existing modules:

1. Move files without changing behavior.
2. Add umbrella headers.
3. Update includes.
4. Keep public names stable unless old names are misleading.
5. Run all tests.
6. Avoid mixing structural refactor with behavior changes.
