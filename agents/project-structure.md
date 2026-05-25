# Project Structure Agent Guide

Use this guide when creating, reviewing, or refactoring projects based on
`event-hub-cpp`.

The examples use a fictional market application:

```text
market_data
order_routing
risk_control
notifications
persistence
analytics
```

These names are examples only. Do not copy them into user projects unless they
match the actual domain.

## Main Rule

Keep these concepts separate:

```text
data/       Pure DTOs, configs, enums.
events.hpp  Event-bus contracts and wrappers.
providers/  External service/backend providers.
adapters/   Bridges between services and transports.
detail/     Internal implementation helpers.
module.hpp  Entry point for using the module as an executable component.
```

Do not place event-bus wrappers into `data/` unless the project is tiny and
explicitly uses a flat structure.

## Preferred Layout In A Large Project

Use this layout for each module:

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

Example:

```text
src/
├── market_data.hpp
└── market_data/
    ├── module.hpp
    ├── events.hpp
    ├── data/
    │   ├── config.hpp
    │   ├── commands.hpp
    │   ├── config/
    │   └── commands/
    ├── providers.hpp
    ├── providers/
    ├── adapters/
    └── detail/
```

Use the same pattern for every module. Consistency is more important than
inventing a new layout per module.

## Preferred Layout For A Standalone Module Repository

Use:

```text
<module-repo>/
├── include/
│   ├── <module>.hpp
│   └── <module>/
│       ├── module.hpp
│       ├── events.hpp
│       ├── data/
│       ├── providers/
│       └── adapters/
├── src/
├── tests/
└── examples/
```

Public headers belong in `include/`. Private implementation belongs in `src/`.

## Meaning Of <module>.hpp

`<module>.hpp` is the broad public umbrella.

It can include:

```text
<module>.hpp
└── includes:
    ├── <module>/module.hpp
    ├── <module>/events.hpp
    ├── <module>/data/config.hpp
    ├── <module>/data/commands.hpp
    └── <module>/providers.hpp
```

Do not include private `detail/` headers from the broad umbrella.

## Meaning Of module.hpp

`module.hpp` is the component entry point.

It should expose the class or classes needed to instantiate, register, or
connect the module. Use it in composition roots:

```cpp
#include "market_data/module.hpp"
#include "order_routing/module.hpp"
#include "risk_control/module.hpp"
```

Do not reject `module.hpp` as unnecessary indirection in large modular projects.
It gives a stable semantic entry point.

## Required module.hpp Event API Documentation

Every module-level `module.hpp` should document the module's event API with
Doxygen comments.

A reader should understand from `module.hpp`:

- which request events the module accepts;
- which result events it publishes;
- which notification/state events it may emit;
- what each request means;
- how request/result pairs are correlated;
- whether events are synchronous, queued, or produced by async callbacks;
- what processing/lifecycle assumptions exist.

Use a concise Doxygen block near the top of `module.hpp`:

```cpp
/// \file module.hpp
/// \brief Public entry point for the market data module.
///
/// Event API:
/// - Accepts QuoteRequestedEvent to request one latest quote.
/// - Publishes QuoteReceivedEvent with the same request_id.
/// - Accepts CandlesRequestedEvent to request historical candles.
/// - Publishes CandlesReceivedEvent with the same request_id.
/// - Publishes MarketDataStatusChangedEvent when provider connectivity changes.
///
/// Request/result correlation:
/// - Request/result event pairs use event_hub::RequestId.
/// - request_id belongs to events, not to pure DTOs.
///
/// Processing:
/// - MarketDataEventApi is a passive event_hub::Module.
/// - ModuleHub::process() calls on_process(), which drives MarketDataService.
```

If the event API is large, keep a summary in `module.hpp` and put detailed
Doxygen comments on individual event structs in `events.hpp`.

## Meaning Of events.hpp

`events.hpp` contains event-bus contracts.

Events are integration contracts, not pure domain DTOs.

Good:

```cpp
struct QuoteRequest {
    Symbol symbol;
};

struct QuoteRequestedEvent {
    event_hub::RequestId request_id = event_hub::invalid_request_id;
    QuoteRequest request;
};
```

Keep event-bus metadata such as `request_id` in events, not in pure DTOs.

Use one `events.hpp` while event declarations are short. Split into
`events/*.hpp` only when the file becomes large or event groups become
independent:

```text
market_data/
├── events.hpp
└── events/
    ├── quote_requested_event.hpp
    ├── quote_received_event.hpp
    └── events.hpp
```

## Meaning Of data/commands/

Put request/response operation DTOs here:

```text
market_data/
└── data/
    └── commands/
        ├── quote_request.hpp
        ├── quote_response.hpp
        ├── candles_request.hpp
        └── candles_response.hpp
```

These DTOs should be usable without `event_hub`.

Do not put `event_hub::RequestId` into command DTOs unless correlation is truly
part of the domain.

Prefer `commands/` over `dto/`.

## Meaning Of data/config/

Put configs, errors, and simple selection enums here.

Prefer precise names:

```text
market_data/
└── data/
    └── config/
        ├── market_data_provider_type.hpp
        └── market_data_service_config.hpp
```

Do not use vague names such as `service.hpp` for an enum that only selects a
provider.

## Meaning Of providers/

Use providers for interchangeable backend implementations or external APIs.

Examples:

```text
market_data/
├── providers.hpp
└── providers/
    ├── websocket_market_data_provider.hpp
    ├── rest_market_data_provider.hpp
    ├── mock_market_data_provider.hpp
    └── market_data_provider_factory.hpp
```

Provider interfaces should stay small and close to the module domain.

## Meaning Of adapters/

Use adapters for bridges between module services and transports:

```text
adapters/
├── event-bus adapter
├── HTTP adapter
├── CLI adapter
├── Telegram adapter
└── WebSocket adapter
```

If there is only one adapter, keeping it at the module root is acceptable.
Introduce `adapters/` once there are multiple adapters or the root becomes
noisy.

## Meaning Of detail/

Use `detail/` only for private implementation helpers.

Do not expose detail types in public headers. Do not put stable DTOs into
`detail/`.

## Request/Response Rule

For request/response over `event-hub-cpp`, prefer paired events:

```text
RequestEvent -> ResultEvent
```

with the same `event_hub::RequestId`.

Keep `request_id` in events, not in pure service DTOs.

Good:

```cpp
struct QuoteRequest {
    Symbol symbol;
};

struct QuoteRequestedEvent {
    event_hub::RequestId request_id = event_hub::invalid_request_id;
    QuoteRequest request;
};
```

Avoid:

```cpp
struct QuoteRequest {
    event_hub::RequestId request_id;
    Symbol symbol;
};
```

unless the correlation id is truly part of the domain.

## Module API Rule

Inside an `event_hub::Module` subclass, prefer direct inherited methods:

```cpp
subscribe<Event>(...);
post<Event>(...);
emit_direct<Event>(...);
request<RequestEvent, ResultEvent>(...);
```

Avoid:

```cpp
endpoint().post<Event>(...);
```

unless direct endpoint access is actually needed.

## File Splitting Rule

Do not split files mechanically.

Keep one file when it is short and coherent. Split when the file becomes large,
has independent sections, or causes unnecessary include coupling.

A 90-line `events.hpp` is acceptable. A 500-line mixed DTO/event/provider file
is not.

## Mechanical Refactor Rule

When reorganizing existing modules:

1. Move files without changing behavior.
2. Add umbrella headers.
3. Update includes.
4. Keep public names stable unless the old names are misleading.
5. Run all tests.
6. Avoid mixing structural refactor with behavior changes.
