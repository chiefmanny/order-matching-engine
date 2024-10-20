import grpc
import matching_engine_pb2
import matching_engine_pb2_grpc
import time
import threading
from datetime import datetime
from collections import deque

class CandlestickChart:
    def __init__(self, symbol, candle_size_seconds):
        self.symbol = symbol
        self.candle_size_seconds = candle_size_seconds
        self.price_data = deque(maxlen=100)  # Store the last 100 candles
        self.candles = []  # Store candlestick data (open, high, low, close)
        self.start_time = None

    def update_candles(self, price):
        current_time = datetime.now()
        if self.start_time is None:
            self.start_time = current_time
        time_elapsed = (current_time - self.start_time).total_seconds()
        if time_elapsed >= self.candle_size_seconds:
            if self.price_data:
                open_price = self.price_data[0]
                high_price = max(self.price_data)
                low_price = min(self.price_data)
                close_price = self.price_data[-1]
                self.candles.append((open_price, high_price, low_price, close_price))
                self.price_data.clear()
            self.start_time = current_time
        self.price_data.append(price)

    def get_latest_candle(self):
        """Returns the latest candlestick data: open, high, low, close"""
        if self.candles:
            return {
                "time": datetime.utcnow().isoformat(),
                "open": self.candles[-1][0],
                "high": self.candles[-1][1],
                "low": self.candles[-1][2],
                "close": self.candles[-1][3]
            }
        else:
            return None

def subscribe_to_price(symbol, candle_size_seconds):
    channel = grpc.insecure_channel("localhost:50051")
    stub = matching_engine_pb2_grpc.MatchingEngineStub(channel)
    symbol_request = matching_engine_pb2.Symbol(symbol=symbol)
    candlestick_chart = CandlestickChart(symbol, candle_size_seconds)

    def watch_symbol():
        for data in stub.WatchSymbol(symbol_request):
            price = (data.lastBid + data.lastAsk) / 2  # Midpoint price
            candlestick_chart.update_candles(price)

    watch_thread = threading.Thread(target=watch_symbol)
    watch_thread.start()

    return candlestick_chart
