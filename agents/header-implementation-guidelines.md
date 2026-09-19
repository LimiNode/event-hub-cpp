# Header Implementation Guidelines

## File Ownership

- `.hpp` files own public declarations, small inline functions, templates, and
  API documentation.
- `.ipp` files may be introduced for template-heavy definitions when a public
  header becomes noisy.
- `.cpp` files may be introduced for examples, tests, or future compiled
  implementation details.

## Include Policy

- Do not use `../` in `#include` directives.
- Prefer stable public headers under `event_hub/`.
- When one header includes another header from the same directory, use the short
  relative path, for example `#include "event_bus.hpp"` instead of
  `#include "event_hub/event_bus.hpp"`.
- Keep core public headers dependency-light and standard-library-only. Optional
  integration entry points may depend on declared external feature
  dependencies when those dependencies are part of the public API.
- Include what each standalone header or public entry point uses. Aggregate-
  owned supporting headers may rely on prerequisites prepared by their
  documented entry point.

The `CalendarScheduler` subsystem is aggregate-first:

- include `<event_hub/calendar_scheduler.hpp>` as its supported entry point;
- `calendar_scheduler/types.hpp` is an aggregate-owned DTO and policy header;
- `calendar_scheduler/detail.hpp` is an aggregate-owned implementation header;
- the adjacent headers may rely on prerequisites prepared by
  `calendar_scheduler.hpp` and do not promise standalone inclusion.

Keep cross-directory project dependencies and shared aggregate prerequisites in
the aggregate entry point. Aggregate-owned headers may include an external
feature dependency that they directly expose through their declarations. Do
not add `../` traversal includes to these aggregate-owned headers.

## Umbrella Header

`include/event_hub.hpp` is the primary umbrella header. Consumers can
include it for the full library:

```cpp
#include <event_hub.hpp>
```

Leaf headers may be included directly when a consumer needs only part of the
API:

```cpp
#include <event_hub/event_bus.hpp>
#include <event_hub/event_endpoint.hpp>
```

## Public APIs

Public headers should expose concepts and contracts, not incidental
implementation structure. Keep API comments focused on guarantees, ownership,
lifetime, threading, and error behavior.
