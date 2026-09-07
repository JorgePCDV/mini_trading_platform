# Wire Protocol

All messages are newline-delimited JSON objects sent over plain TCP
sockets. Every message is a single line (no embedded newlines) terminated
by `\n`.

## Order entry: Client → Order Gateway (:6000)

### `new_order`
```json
{
  "type": "new_order",
  "client_order_id": "abc123",
  "client_id": "alice",
  "symbol": "AAPL",
  "side": "buy",
  "order_type": "limit",
  "price": 150.25,
  "qty": 10
}
```
- `order_type` is `"limit"` or `"market"`. `price` is omitted/ignored for
  market orders.
- `client_order_id` is caller-assigned and echoed back on every ack/fill so
  the client can correlate responses without tracking engine order ids.

### `cancel_order`
```json
{ "type": "cancel_order", "order_id": 42, "symbol": "AAPL" }
```
`order_id` is the engine-assigned id returned in the `ack` for the order
being cancelled.

## Order gateway → client responses

### `ack`
```json
{
  "type": "ack",
  "order_id": 42,
  "client_order_id": "abc123",
  "symbol": "AAPL",
  "status": "resting",
  "leaves_qty": 10
}
```
`status` is `"resting"` (rests on the book untouched), `"accepted"`
(partially or fully filled on arrival), or reflects the remaining
(`leaves_qty`) quantity.

### `fill`
```json
{
  "type": "fill",
  "exec_id": 7,
  "order_id": 42,
  "client_order_id": "abc123",
  "symbol": "AAPL",
  "side": "buy",
  "price": 150.25,
  "qty": 5,
  "ts": 1732650000000000000
}
```
Sent once per execution to *both* sides of a trade (the aggressor and the
resting order's owning connection, if still connected). `ts` is
nanoseconds since the Unix epoch.

### `reject`
```json
{ "type": "reject", "client_order_id": "abc123", "reason": "symbol not tradable on this venue: ZZZZ" }
```
Emitted by the order gateway (validation failures: bad symbol, rate limit,
malformed request) or the matching engine (e.g. non-positive price/qty).

### `cancel_ack` / `cancel_reject`
```json
{ "type": "cancel_ack", "order_id": 42 }
{ "type": "cancel_reject", "order_id": 42 }
```

## Market data: Matching Engine → subscribers (:5002)

Read-only feed; connect and read, no messages need to be sent.

### `trade`
```json
{
  "type": "trade",
  "symbol": "AAPL",
  "price": 150.25,
  "qty": 5,
  "aggressor_side": "buy",
  "ts": 1732650000000000000
}
```

### `book_update`
```json
{
  "type": "book_update",
  "symbol": "AAPL",
  "bids": [[150.20, 100], [150.15, 50]],
  "asks": [[150.25, 80], [150.30, 120]],
  "ts": 1732650000000000000
}
```
Top-10 price levels per side, best price first. Each level is
`[price, aggregate_qty]`.

## Trade log (logs/trades.csv)

The matching engine also appends every execution to a CSV file so the risk
engine and analytics tools can reconcile which *clients* were on each side
of a trade (something the anonymized public market data feed intentionally
does not carry):

```
ts_ns,symbol,price,qty,aggressor_side,aggressor_client,resting_client
```
