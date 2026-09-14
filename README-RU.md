# event-hub-cpp

[![Лицензия: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Платформы](https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS-blue.svg)](.github/workflows/ci.yml)
[![Язык](https://img.shields.io/badge/language-C%2B%2B17%2B-orange.svg)](CMakeLists.txt)
[![Header only](https://img.shields.io/badge/header--only-yes-brightgreen.svg)](include/event_hub.hpp)
[![Пакеты](https://img.shields.io/badge/packages-CMake%20%7C%20pkg--config%20%7C%20vcpkg%20overlay-6f42c1.svg)](#установка)
![CI Windows](https://img.shields.io/github/actions/workflow/status/LimiNode/event-hub-cpp/ci.yml?branch=main&label=Windows&logo=windows)
![CI Linux](https://img.shields.io/github/actions/workflow/status/LimiNode/event-hub-cpp/ci.yml?branch=main&label=Linux&logo=linux)
![CI macOS](https://img.shields.io/github/actions/workflow/status/LimiNode/event-hub-cpp/ci.yml?branch=main&label=macOS&logo=apple)

[Read in English](README.md)

## Обзор

`event-hub-cpp` - легкая C++-шина событий для модульных приложений. Библиотека
предоставляет типизированные события, RAII-подписки, синхронную отправку,
отложенную обработку через очередь, awaiter-ы и базовые средства отмены.

Она полезна там, где модули должны публиковать команды, уведомления или
изменения состояния без прямой зависимости от реализаций друг друга.

Ключевые свойства:

- Типизированные контракты событий с callback-ами вида `void(const MyEvent&)`.
- `EventEndpoint` владеет подписками и автоматически отписывается.
- `emit_direct<T>()` отправляет событие синхронно.
- `post<T>()` кладет событие в очередь до явного `process()`.
- Опциональный `TaskManager` кладет immediate, delayed, periodic и
  future-returning задачи в ту же модель явной обработки.
- Опциональный `CalendarScheduler` добавляет daily, weekly, monthly и custom
  calendar rules, если включена интеграция с `time-shield-cpp`.
- Опциональный `RunLoop` блокирует текущий поток и обрабатывает любое число
  event bus-ов и task manager-ов.
- Опциональные внешние notifier-ы будят application event loop после появления
  queued work.
- `await_once()` и `await_each()` дают callback-ожидание событий.
- `CancellationToken` и `CancellationSource` отменяют awaiter-ы.
- Lifetime guard-ы не дают callback-у стартовать после истечения guarded owner-а.
- Header-only C++17 без обязательных внешних зависимостей.

## Структура Заголовков

| Заголовок | Назначение |
| --- | --- |
| `<event_hub.hpp>` | Основной общий заголовок для dependency-free публичного API. |
| `<event_hub/event_bus.hpp>` | Центральная шина, подписки, `emit_direct`, `post` и `process`. |
| `<event_hub/event_endpoint.hpp>` | RAII-точка подключения модуля к шине. |
| `<event_hub/event_awaiter.hpp>` | Реализация awaiter-ов и `AwaitOptions`. |
| `<event_hub/awaiter_interfaces.hpp>` | Интерфейсы cancelable awaiter-handle-ов. |
| `<event_hub/cancellation.hpp>` | Примитивы отмены token/source. |
| `<event_hub/event.hpp>` | Опциональная база события с type/name/clone metadata. |
| `<event_hub/event_listener.hpp>` | Опциональный generic-listener для наследников `Event`. |
| `<event_hub/notifier.hpp>` | `INotifier` и `SyncNotifier` для внешнего пробуждения event loop. |
| `<event_hub/run_loop.hpp>` | Блокирующий current-thread loop для зарегистрированных шин и task manager-ов. |
| `<event_hub/task.hpp>` | Move-only `Task`, `TaskContext`, `TaskId`, приоритет, periodic policy и опции задач. |
| `<event_hub/task_manager.hpp>` | Пассивный `TaskManager` для immediate, delayed и periodic задач. |
| `<event_hub/calendar_scheduler.hpp>` | Опциональный calendar scheduler на `time-shield-cpp` для daily/weekly/monthly правил. |

## Быстрый Старт

```cpp
#include <event_hub.hpp>

#include <iostream>
#include <string>

struct MyEvent {
    std::string message;
};

int main() {
    event_hub::EventBus bus;
    event_hub::EventEndpoint endpoint(bus);

    endpoint.subscribe<MyEvent>(
        event_hub::DeliveryPolicy::queued,
        [](const MyEvent& event) {
            std::cout << event.message << '\n';
        });

    endpoint.post<MyEvent>("sent from the queue");
    bus.process();

    endpoint.subscribe_direct<MyEvent>([](const MyEvent& event) {
        std::cout << event.message << '\n';
    });
    endpoint.emit_direct<MyEvent>("sent immediately");
}
```

Матрица доставки:

| Источник dispatch | direct subscriber | queued subscriber | any subscriber |
| --- | --- | --- | --- |
| `emit_direct()` | вызывается сразу | пропускается | вызывается сразу |
| `post()` + `process()` | пропускается | вызывается из `process()` | вызывается из `process()` |

События могут быть обычными C++ value type-ами. Наследуйтесь от
`event_hub::Event` только если нужны runtime metadata или клонирование:

```cpp
struct TokenFoundEvent {
    std::string source;
    std::string value;
};
```

```cpp
class ReloadRequested final : public event_hub::Event {
public:
    EVENT_HUB_EVENT(ReloadRequested)
};
```

## Awaiter-ы

`await_once()` слушает до первого подходящего события и затем сам отменяется.
`await_each()` продолжает слушать, пока его handle, endpoint, timeout или
`CancellationToken` не остановят ожидание.

```cpp
event_hub::CancellationSource source;

auto options = event_hub::AwaitOptions::timeout_ms(5000);
options.token = source.token();
options.on_timeout = [] {
    // обработать timeout
};

endpoint.await_once<MyEvent>(
    [](const MyEvent& event) { return event.message == "ready"; },
    [](const MyEvent& event) {
        // обработать первое совпадение
    },
    options);

auto stream = endpoint.await_each<MyEvent>([](const MyEvent& event) {
    // обработать каждое событие
});

stream->cancel();
```

Timeout-ы и внешняя отмена проверяются в точках `emit_direct<T>()` и `process()` с учетом `AwaitOptions::delivery`. По умолчанию awaiter-ы принимают queued delivery, поэтому event callback, timeout callback и cleanup отмены выполняются только из `process()`, если delivery явно не изменен.

Queued awaiter видит timeout и cancellation только в queued poll points.
Вызывайте `cancel()` напрямую, если отмена должна быть немедленной из любого
потока.

## TaskManager

`TaskManager` - опциональная пассивная очередь работы для приложений, которым
нужно обрабатывать не-событийные задачи из того же внешнего цикла, что и
`EventBus`. Producer-потоки могут отправлять задачи, но callback-и выполняются
только при явном вызове `process()`.

```cpp
event_hub::TaskManager tasks;

tasks.post([] {
    // выполнится в потоке, который вызовет tasks.process()
});

auto future = tasks.submit([] {
    return 42;
});

tasks.process();
assert(future.get() == 42);
```

Задачи могут быть immediate, one-shot delayed или periodic. Ready-задачи
поддерживают фиксированные приоритеты `high`, `normal` и `low`, сохраняя FIFO
внутри одного приоритета. `Task` является move-only, поэтому в C++17 можно
ставить в очередь lambda с move-only захватами.

Для абсолютного монотонного дедлайна используйте `add_task_at(...)` или
`post_at(...)` с `TaskManager::TimePoint`
(`std::chrono::steady_clock::time_point`). Это не календарная дата, а deadline,
устойчивый к изменению системного времени и DST; он хорошо сочетается с
`next_deadline()`.

Для простой постановки по wall-clock времени используйте
`add_task_at_system(...)` с `std::chrono::system_clock::time_point`. Метод
конвертирует system time в steady-clock deadline в момент постановки и не
отслеживает последующие изменения системного времени, поэтому это не
полноценный календарный scheduler. Используйте `add_task_at_system_ms(...)`,
если тот же wall-clock deadline представлен Unix epoch миллисекундами.

Periodic-задачи выполняются только из `process()`. По умолчанию первый цикл
становится ready сразу, а следующие циклы используют fixed-delay scheduling.
Используйте `post_every_after(...)` или `post_every_after_ms(...)`, чтобы
задержать первый цикл, и задайте `PeriodicTaskOptions::schedule` в
`PeriodicSchedule::fixed_rate`, если циклы должны считаться от плановых
deadline-ов, а не от времени завершения callback-а. Чтобы остановить будущие
циклы, отмените возвращенный `TaskId`. Если periodic callback бросает
исключение, эта periodic-задача останавливается, а дальше применяется обычная
exception policy `TaskManager`.

Task callback-и также могут принимать `event_hub::TaskContext&`. Context дает
id текущей задачи, `cancel()` и `reschedule_at(...)` / `reschedule_after(...)`;
это удобно для self-cancel periodic-задач и retry-like one-shot задач без
захвата `TaskId`.

Если проект собран с `-DEVENT_HUB_CPP_USE_TIME_SHIELD=ON`, опциональный header
`<event_hub/calendar_scheduler.hpp>` добавляет daily, weekly, monthly и custom
calendar rules поверх `TaskManager`. Он вычисляет следующий UTC calendar occurrence через
`time-shield-cpp`, ставит одну steady-clock задачу и после callback-а планирует
следующую. Часовой пояс по умолчанию - UTC/GMT; в опциях есть
`time_shield::ZonedClock`, UTC millisecond `now_provider`, политики missed-run
и overlap, persisted `last_due_utc_ms` и observer callback-и для событий
created/scheduled/due/callback/missed/cancelled. Для частых настроек есть
helpers `CalendarTaskOptions::in_zone(...)`,
`CalendarTaskOptions::fixed_utc_offset(...)` и
`CalendarTaskOptions::with_clock(...)`. Для time-shield NTP передайте
`time_shield::ZonedClock(zone, true)` в build-е с включенным NTP. Для named
zones локальное время в DST gap пропускается, а неоднозначное локальное время
при DST fold планируется один раз на первое UTC-вхождение.

Для читаемого wall-clock времени используйте `time_of_day(...)`, а для правил
на несколько дней - fluent schedule builders. Это время внутри настроенного
calendar day, а не текущее локальное время компьютера. `CalendarScheduler`
работает с секундной точностью для daily/weekly/monthly правил, поэтому
ненулевой аргумент миллисекунд отклоняется этими API:

```cpp
calendar.add_daily_task(event_hub::time_of_day(18, 30), [] {});

auto weekly = event_hub::WeeklySchedule{}
    .add(event_hub::MON, event_hub::time_of_day(9, 0))
    .add(event_hub::FRI, event_hub::time_of_day(18, 0));
calendar.add_weekly_task(std::move(weekly), [] {});

auto monthly = event_hub::MonthlySchedule{}
    .add(15, event_hub::time_of_day(12, 0));
calendar.add_monthly_task(std::move(monthly), [] {});
```

`TaskManager` должен жить дольше `CalendarScheduler`. Деструктор scheduler-а
отменяет queued one-shot задачи, но не ждёт callbacks, которые уже начали
выполняться. Observer может вызываться из `cancel()`, `cancel_all()` и
деструктора, поэтому callbacks и observers, которые захватывают внешние
объекты, должны защищать их lifetime на стороне пользователя.

## RunLoop

`RunLoop` - утилита для приложений, которым нужен один блокирующий цикл над
несколькими пассивными источниками. Он владеет `SyncNotifier`, регистрирует его
на добавленных `EventBus` и `TaskManager`, и обрабатывает их в текущем потоке,
пока кто-то не вызовет `request_stop()`.

```cpp
event_hub::EventBus bus;
event_hub::TaskManager tasks;
event_hub::RunLoop loop;

loop.add(bus);
loop.add(tasks);

tasks.post_after_ms(10, [&loop] {
    loop.request_stop();
});

loop.run();
```

Зарегистрированные источники не принадлежат loop-у и должны жить дольше него.
Добавляйте источники до вызова `run()` и не меняйте список источников во время
работы цикла.

## Обработка Исключений

Без exception handler-а исключения из event callback-ов пробрасываются из
`emit_direct<T>()` или `process()`. Если handler задан, dispatch передает ему
исключения и продолжает выполнять остальные callback-и и queued events:

```cpp
bus.set_exception_handler([](std::exception_ptr error) {
    try {
        if (error) {
            std::rethrow_exception(error);
        }
    } catch (const std::exception& ex) {
        // залогировать или обработать ex.what()
    }
});
```

Исключения из `AwaitOptions::on_timeout` никогда не выходят из
`poll_timeout(source) noexcept`. Они передаются в exception handler шины, если он
задан, иначе игнорируются.

`TaskManager::set_exception_handler(...)` использует такую же политику для task
callback-ов. Без handler-а `TaskManager::process()` восстанавливает еще не
стартовавшие задачи из текущего batch и пробрасывает исключение. С handler-ом
он передает исключение туда и продолжает следующие задачи.

## Установка

### Установка и `find_package`

После установки библиотеки через CMake ее можно подключить из другого CMake
проекта:

```bash
cmake -S . -B build -DEVENT_HUB_CPP_BUILD_TESTS=OFF -DEVENT_HUB_CPP_BUILD_EXAMPLES=OFF
cmake --install build --prefix /path/to/prefix
```

```cmake
cmake_minimum_required(VERSION 3.14)
project(app LANGUAGES CXX)

find_package(event-hub-cpp CONFIG REQUIRED)

add_executable(app main.cpp)
target_link_libraries(app PRIVATE event_hub::event_hub)
```

Экспортируемый target - `event_hub::event_hub`. Также доступен alias
`event-hub-cpp::event-hub-cpp`, если удобнее использовать имя пакета.

### Git Submodule С `add_subdirectory`

Добавьте репозиторий как subdirectory и подключите interface target:

```bash
git submodule add https://github.com/LimiNode/event-hub-cpp external/event-hub-cpp
```

```cmake
add_subdirectory(external/event-hub-cpp)
target_link_libraries(my_app PRIVATE event_hub::event_hub)
```

Затем подключите общий заголовок:

```cpp
#include <event_hub.hpp>
```

### vcpkg Overlay

Установите библиотеку через локальный overlay port:

```bash
vcpkg install event-hub-cpp --overlay-ports=./vcpkg-overlay/ports
```

При конфигурации своего проекта используйте toolchain vcpkg:

```bash
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake
```

После этого используются те же вызовы `find_package(event-hub-cpp CONFIG REQUIRED)`
и `target_link_libraries(... event_hub::event_hub)`.

### pkg-config

При установке также создается файл `event-hub-cpp.pc`:

```bash
c++ main.cpp -std=c++17 $(pkg-config --cflags --libs event-hub-cpp)
```

### Заметки Об Интеграции

- `event_hub::event_hub` - header-only target без внешних зависимостей по
  умолчанию.
- Потребителям нужен компилятор с поддержкой C++17 или новее.
- При подключении через `add_subdirectory` примеры и тесты по умолчанию
  выключены, если `event-hub-cpp` не является top-level проектом.
- Опциональные utilities `CalendarScheduler` используют `time-shield-cpp` из
  submodule `external/time-shield-cpp`, если собрать с
  `-DEVENT_HUB_CPP_USE_TIME_SHIELD=ON`. NTP выключен по умолчанию; включается
  через `-DEVENT_HUB_CPP_USE_TIME_SHIELD_NTP=ON`.

## Сборка И Тесты

```bash
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build
ctest --test-dir build --output-on-failure
```

В top-level сборке создаются:

- `event_hub_basic` из `examples/basic.cpp`;
- `event_hub_event_and_listener` из `examples/event_and_listener.cpp`;
- `event_hub_event_node` из `examples/event_node.cpp`;
- `event_hub_await_and_cancel` из `examples/await_and_cancel.cpp`;
- `event_hub_exception_handling` из `examples/exception_handling.cpp`;
- `event_hub_strict_lifetime` из `examples/strict_lifetime.cpp`;
- `event_hub_external_notifier` из `examples/external_notifier.cpp`;
- `event_hub_task_manager` из `examples/task_manager.cpp`;
- `event_hub_task_manager_periodic` из `examples/task_manager_periodic.cpp`;
- `event_hub_task_manager_with_bus` из `examples/task_manager_with_bus.cpp`;
- `event_hub_run_loop` из `examples/run_loop.cpp`;
- `event_hub_test_*` поведенческие тесты из `tests/test_*.cpp`.

## Проверенные Платформы

| Платформа | CI runner | Стандарты C++ |
| --- | --- | --- |
| Windows | `windows-latest` | C++17, C++20 |
| Linux | `ubuntu-latest` | C++17, C++20 |
| macOS | `macos-latest` | C++17, C++20 |

CI также проверяет подключение установленного пакета через `find_package` и
валидирует vcpkg overlay port на Linux.

## Примеры

- [basic.cpp](examples/basic.cpp) - plain value events, `subscribe`, `emit_direct`,
  `post/process`, точечная отписка, pending count и очистка очереди.
- [event_and_listener.cpp](examples/event_and_listener.cpp) - опциональное
  наследование от `event_hub::Event`, `EVENT_HUB_EVENT`, `EventListener`,
  `on_event()` и `as_ref<T>()`.
- [event_node.cpp](examples/event_node.cpp) - `EventNode` как базовый класс
  модуля с `listen`, защищенными `post`/`emit_direct`, callback `subscribe` и
  поведением unsubscribe/close.
- [await_and_cancel.cpp](examples/await_and_cancel.cpp) - `await_once()`,
  `await_each()`, ручная отмена awaiter-а, cancellation tokens и timeout
  callbacks.
- [exception_handling.cpp](examples/exception_handling.cpp) -
  `set_exception_handler(...)`, обработанные ошибки callback-ов и fail-fast
  поведение без handler-а.
- [strict_lifetime.cpp](examples/strict_lifetime.cpp) - advanced lifetime
  модуля через `std::shared_ptr` и `weak_from_this()`.
- [external_notifier.cpp](examples/external_notifier.cpp) - общий
  `SyncNotifier` для `EventBus` и `TaskManager`.
- [task_manager.cpp](examples/task_manager.cpp) - самостоятельный loop для
  `TaskManager` с priority, delayed tasks и `submit()`.
- [task_manager_periodic.cpp](examples/task_manager_periodic.cpp) -
  periodic-задачи `TaskManager` с immediate start, delayed first cycle,
  fixed-delay/fixed-rate scheduling, context self-cancel, self-reschedule и
  обработанными ошибками.
- [calendar_scheduler.cpp](examples/calendar_scheduler.cpp) - опциональные
  `CalendarScheduler` rules поверх `TaskManager`; полный пример требует
  `EVENT_HUB_CPP_USE_TIME_SHIELD=ON`.
- [calendar_scheduler_ntp.cpp](examples/calendar_scheduler_ntp.cpp) -
  настройка `CalendarScheduler` на NTP-время через time-shield; требует
  `EVENT_HUB_CPP_USE_TIME_SHIELD=ON` и
  `EVENT_HUB_CPP_USE_TIME_SHIELD_NTP=ON`.
- [calendar_scheduler_custom_time.cpp](examples/calendar_scheduler_custom_time.cpp) -
  пользовательский UTC millisecond time provider и CET-rule около перехода на
  зимнее время.
- [task_manager_with_bus.cpp](examples/task_manager_with_bus.cpp) - ручной
  shared-notifier loop для `EventBus` и `TaskManager`.
- [run_loop.cpp](examples/run_loop.cpp) - удобный `RunLoop` для одной шины и
  нескольких task manager-ов.

## Dispatch И Потоки

`post<T>()` можно вызывать из producer-потоков. `emit_direct<T>()` и `process()`
вызывают callback-и в том потоке, где были вызваны сами методы. Перед dispatch
шина копирует список callback-ов, поэтому handler может подписываться,
отписываться, публиковать события или отменять awaiter-ы.

`emit_direct<T>()` возвращает `DispatchResult` с числом подписок данного типа,
реально вызванных callback-ов и подписчиков, пропущенных из-за delivery policy.
`set_delivery_mismatch_handler(...)` задает необязательный диагностический hook
По умолчанию он сообщает только когда подписчики такого типа есть, но никто не
получил событие из-за policy filtering; используйте
`DeliveryMismatchReportMode::any_skipped` для подробной диагностики смешанных
policy.

`EventBus` и `TaskManager` не владеют потоком и не решают, сколько спать. Если
задан non-owning `INotifier`, каждый успешный `post()` вызывает `notify()` после
помещения работы в очередь. Так внешний event loop может ждать один общий
notifier, которым пользуются шина, task manager, таймеры или другие источники
работы:

```cpp
event_hub::SyncNotifier notifier;
event_hub::EventBus bus;
bus.set_notifier(&notifier);

event_hub::TaskManager tasks;
tasks.set_notifier(&notifier);

while (running) {
    // Сохраняем состояние notifier-а до обработки, чтобы не потерять wake-up,
    // который придет перед ожиданием.
    const auto generation = notifier.generation();

    std::size_t work_done = 0;
    work_done += bus.process();
    // Ограничиваем задачи за один проход, чтобы другие источники тоже работали.
    work_done += tasks.process(128);

    if (work_done != 0) {
        continue;
    }

    // Спим не дольше 1 ms, но просыпаемся раньше, если подходит delayed task.
    const auto timeout =
        tasks.recommend_wait_for_ms(1);
    if (!bus.has_pending() && !tasks.has_ready()) {
        // Если generation изменился после обработки, ожидание сразу вернется.
        notifier.wait_for(generation, timeout);
    }
}
```

`SyncNotifier` использует generation counter: если уведомление пришло между
`generation()` и `wait_for(...)`, оно не теряется, потому что ожидание сразу
увидит изменившееся поколение.

Используйте `RunLoop`, когда этот ручной паттерн нужно упаковать в маленькую
current-thread утилиту. Он использует ту же notifier-модель и не создает
worker thread.

`EventBus` должен жить дольше всех `EventEndpoint` и `EventAwaiter`, которые на
него ссылаются.

Подписки `EventEndpoint` используют внутренний lifetime guard. Если callback
уже был скопирован `dispatch()`, но guard endpoint-а истек до старта callback-а,
callback будет пропущен. `unsubscribe_all()` не ждет callback-и, которые уже
начались или уже прошли guard-check.

Для модулей, которыми владеет `std::shared_ptr`, передавайте `weak_from_this()`
как явный guard, особенно если callback захватывает состояние модуля:

```cpp
class TokenModule : public std::enable_shared_from_this<TokenModule> {
public:
    void start() {
        auto weak = weak_from_this();

        m_endpoint.subscribe<TokenFoundEvent>(
            weak,
            [weak](const TokenFoundEvent& event) {
                auto self = weak.lock();
                if (!self) {
                    return;
                }

                self->on_token_found(event);
            });
    }

private:
    void on_token_found(const TokenFoundEvent& event);

    event_hub::EventEndpoint m_endpoint;
};
```

Создавайте такие модули через `std::shared_ptr` до вызова `start()`:

```cpp
auto module = std::make_shared<TokenModule>(bus);
module->start();
```

Endpoint guard не дает callback-ам стартовать после закрытия или уничтожения
endpoint-а. Guard модуля через `weak_from_this()` защищает уже сам модуль:

```cpp
[weak](const TokenFoundEvent& event) {
    if (auto self = weak.lock()) {
        self->handle(event);
    }
}
```

Базовая модель намеренно простая:

- `post()` можно вызывать из producer-потоков.
- `process()`, `emit_direct()`, `subscribe()` и `unsubscribe()` лучше вызывать из
  application/event-loop thread.
- `EventEndpoint::~EventEndpoint()` закрывает endpoint, удаляет подписки и не
  дает новым guarded callback-ам стартовать.
- `EventEndpoint::~EventEndpoint()` не ждет callback-и, которые уже начались.

Полный пример модуля есть в
[examples/strict_lifetime.cpp](examples/strict_lifetime.cpp).

События, опубликованные из handler-ов, будут обработаны следующим вызовом
`process()`: текущий `process()` обрабатывает снимок очереди на момент старта.

Используйте `clear_pending()`, чтобы удалить события из очереди без удаления
подписок. `clear()` оставлен как compatibility wrapper для той же операции.

## Лицензия

MIT. См. [LICENSE](LICENSE).
