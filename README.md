# lighter_cpp_api

C++ connector library for the Lighter zk-rollup perpetuals exchange.

## Features

- REST API client for perpetual futures market data
- Historical candlestick data download with automatic pagination
- Funding rate history download and current snapshot for all markets
- Automatic discovery of all perpetual assets including delisted ones

## Requirements

- C++20 compiler
- CMake 4.0+
- Boost 1.88+ (ASIO, Beast)
- OpenSSL
- nlohmann_json
- spdlog
- magic_enum

## Building

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

## Usage Examples

### REST Client — Perpetual Assets

```cpp
#include "stonky/lighter/lighter_rest_client.h"

using namespace stonky::lighter;

RESTClient client;

// Get all active perpetual assets
auto assets = client.getPerpetualAssets();

// Include delisted assets
auto allAssets = client.getPerpetualAssets(true);

for (const auto& asset : assets) {
    std::cout << asset.symbol
              << " (market_id: " << asset.marketId
              << ", taker: " << asset.takerFee
              << ", maker: " << asset.makerFee << ")" << std::endl;
}
```

### Historical Candlestick Data

```cpp
#include "stonky/lighter/lighter_rest_client.h"

using namespace stonky::lighter;

RESTClient client;

// Download historical candles with automatic pagination
// (caller passes [from, to) once; client walks end_timestamp forward
// across however many network batches the backend needs)
auto candles = client.getHistoricalPrices(
    "BTC",
    CandleInterval::_1h,
    fromTimestamp,   // ms
    toTimestamp);    // ms

std::cout << "Downloaded " << candles.size() << " candles" << std::endl;

// By market_id instead of symbol (skips the symbol→market_id lookup)
auto candlesById = client.getHistoricalPrices(
    /*marketId=*/1,
    CandleInterval::_1h,
    fromTimestamp,
    toTimestamp);

// With callback for streaming large datasets — invoked once per
// network batch, in chronological order
client.getHistoricalPrices(
    "ETH",
    CandleInterval::_1h,
    fromTimestamp,
    toTimestamp,
    [](const std::vector<Candle>& batch) {
        for (const auto& candle : batch) {
            std::cout << "Time: " << candle.openTime
                      << " O: " << candle.open
                      << " H: " << candle.high
                      << " L: " << candle.low
                      << " C: " << candle.close
                      << " V: " << candle.baseVolume << std::endl;
        }
    });
```

### Funding Rate History

```cpp
// Historical funding rates for a single market
auto fundingRates = client.getFundingRates(
    "BTC",
    startTimestamp,
    endTimestamp);

for (const auto& fr : fundingRates) {
    std::cout << "Time: " << fr.fundingTime
              << " Symbol: " << fr.symbol
              << " Rate: " << fr.fundingRate
              << " Direction: " << fr.direction << std::endl;
}

// Snapshot of the current funding rate for every listed market
auto snapshot = client.getCurrentFundingRates();
```

## Candle Intervals

Unlike venues that cap responses at a fixed candle count, Lighter's backend serves
whatever depth it has for the requested `[from, to)` window. There is **no
documented maximum** per request — the client paginates internally using the
TradingView UDF `count_back`-authoritative convention, walking `end_timestamp`
forward until the requested window is covered. Practical history depth is
therefore bounded by whatever the Lighter backend retains for each market (not
something this client can advertise up-front).

Supported intervals (note: no 3 m, 2 h, 6 h, 8 h, 3 d, 1 w, or 1 M — narrower
set than e.g. Hyperliquid):

| Interval   | Enum Value              | API string |
|------------|-------------------------|------------|
| 1 minute   | `CandleInterval::_1m`   | `1m`       |
| 5 minutes  | `CandleInterval::_5m`   | `5m`       |
| 15 minutes | `CandleInterval::_15m`  | `15m`      |
| 30 minutes | `CandleInterval::_30m`  | `30m`      |
| 1 hour     | `CandleInterval::_1h`   | `1h`       |
| 4 hours    | `CandleInterval::_4h`   | `4h`       |
| 12 hours   | `CandleInterval::_12h`  | `12h`      |
| 1 day      | `CandleInterval::_1d`   | `1d`       |

The enum→string mapping is wired through magic_enum customisation, so the API
strings above are what hits the wire.

## Data Models

### Candle
```cpp
struct Candle {
    std::int64_t openTime;   // open time (ms)
    double open;
    double high;
    double low;
    double close;
    double baseVolume;       // volume in base asset
    double quoteVolume;      // volume in quote asset
};
```

### FundingRate
```cpp
struct FundingRate {
    int          marketId;
    std::string  symbol;
    double       fundingRate;
    std::int64_t fundingTime;  // ms
    std::string  direction;    // "long_pays_short" / "short_pays_long"
};
```

### PerpAsset
```cpp
struct PerpAsset {
    std::string symbol;          // e.g. "BTC"
    int         marketId;
    std::string marketType;
    std::string status;
    int         sizeDecimals;
    int         priceDecimals;
    double      takerFee;
    double      makerFee;
    double      minBaseAmount;
    double      minQuoteAmount;
    bool        isDelisted;
};
```

## Utility

`stonky::lighter::Lighter` exposes small helpers used by the client and
available to callers:

- `Lighter::isValidCandleResolution(int minutes, CandleInterval& out)` — map a
  resolution in minutes to a supported `CandleInterval`; returns `false` for
  unsupported resolutions.
- `Lighter::numberOfMsForCandleInterval(CandleInterval)` — interval duration in
  milliseconds (useful for window arithmetic).

## Project Structure

```
lighter-cpp-api/
├── include/stonky/lighter/
│   ├── lighter_rest_client.h     # REST API client
│   ├── lighter_http_session.h    # HTTPS session (PIMPL)
│   ├── lighter_models.h          # Data models
│   ├── lighter_enums.h           # Enumerations
│   └── lighter.h                 # Utility functions
├── src/
│   ├── lighter_rest_client.cpp
│   ├── lighter_http_session.cpp
│   ├── lighter_models.cpp
│   └── lighter.cpp
├── stonky-cpp-common/            # Common utilities submodule
└── test/
    └── main.cpp
```

CMake exports the library as the `lighter_api` target. Link consumers against
it and they pick up the public headers under `stonky/lighter/`.

## Notes and Quirks

- **No authentication.** `RESTClient` has a no-arg constructor; all endpoints
  used here are public market-data routes.
- **API host:** `mainnet.zklighter.elliot.ai`.
- **AWS WAF on CloudFront.** Lighter fronts the API with AWS WAF; there is no
  built-in client-side rate limiter, so callers should serialise requests
  rather than fan out in parallel to avoid WAF challenges.
- **Symbol → market_id resolution** is cached for the lifetime of a
  `RESTClient` instance. Pass `marketId` directly to skip the lookup.
- **Funding-rate JSON shape varies.** Lighter returns the `rate` field as a
  string on some endpoints and as a number on others; the client normalises
  both into `FundingRate::fundingRate` (double).
- **Pagination is `count_back`-authoritative** (TradingView UDF convention).
  The client handles batching by walking `end_timestamp` forward — callers
  pass `[from, to)` once and receive a chronological, half-open range.

## API Documentation

Lighter API reference: https://apidocs.lighter.xyz/

## License

MIT License - see source files for details.

## Author

Vitezslav Kot <vitezslav.kot@stonky.cz>
