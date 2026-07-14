/**
Lighter Execution Gateway

Licensed under the MIT License <http://opensource.org/licenses/MIT>.
SPDX-License-Identifier: MIT
Copyright (c) 2026 Vitezslav Kot <vitezslav.kot@stonky.cz>, Stonky s.r.o.
*/

#ifndef INCLUDE_STONKY_EXECUTION_LIGHTER_EXECUTION_GATEWAY_H
#define INCLUDE_STONKY_EXECUTION_LIGHTER_EXECUTION_GATEWAY_H

#include <stonky/interface/i_execution_gateway.h>
#include <cstdint>
#include <memory>

namespace stonky::execution {

/**
 * IExecutionGateway adapter for the Lighter zk-rollup perpetuals DEX.
 *
 * Quotes come from the order_book WS channel (a locally maintained book → BBO);
 * order/fill events from the account_all WS channel. Orders are zk-rollup L2
 * transactions: LighterSigner (the vendored native signer) signs a create/cancel
 * tx which RESTClient::sendTx submits. Lighter nonces are strictly sequential per
 * API key, so all order ops are serialised through one mutex with a locally
 * tracked, resync-on-failure nonce. The core sees plain base-asset qty / decimal
 * prices; contract/scaled-integer conversions stay inside the adapter.
 */
class LighterExecutionGateway final : public IExecutionGateway {
    struct P;
    std::unique_ptr<P> m_p;

public:
    /**
     * @param url e.g. "https://mainnet.zklighter.elliot.ai"
     * @param apiPrivateKey the API key private key (hex)
     * @param apiKeyIndex the registered API key slot
     * @param accountIndex the Lighter account index
     */
    LighterExecutionGateway(const std::string &url, const std::string &apiPrivateKey, int apiKeyIndex, std::int64_t accountIndex);

    ~LighterExecutionGateway() override;

    [[nodiscard]] std::string name() const override;

    void start() override;

    InstrumentSpec instrumentSpec(const std::string &symbol) override;

    void refreshInstruments() override;

    void subscribeQuotes(const std::string &symbol) override;

    void unsubscribeQuotes(const std::string &symbol) override;

    std::optional<Quote> lastQuote(const std::string &symbol) override;

    void setOrderUpdateCallback(const onOrderUpdateEvent &cb) override;

    void setFillCallback(const onFillEvent &cb) override;

    void setQuoteCallback(const onQuoteEvent &cb) override;

    void submitPostOnlyLimit(const std::string &clientOrderId, const std::string &symbol, OrderSide side, double qty, double price, bool reduceOnly) override;

    [[nodiscard]] bool supportsAmend() const override;

    void amendPrice(const std::string &clientOrderId, const std::string &symbol, double price) override;

    bool cancel(const std::string &clientOrderId, const std::string &symbol) override;

    void submitReduceOnlyMarket(const std::string &clientOrderId, const std::string &symbol, OrderSide side, double qty) override;
};

} // namespace stonky::execution

#endif // INCLUDE_STONKY_EXECUTION_LIGHTER_EXECUTION_GATEWAY_H
