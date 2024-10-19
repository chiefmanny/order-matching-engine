import grpc
import matching_engine_pb2
import matching_engine_pb2_grpc
import time
import threading
import matplotlib
matplotlib.use('Agg')  # Use non-interactive Agg backend
import matplotlib.pyplot as plt
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

    def save_chart(self, filepath="static/live_candlestick.png"):
        fig, ax = plt.subplots()
        times = list(range(len(self.candles)))
        opens = [c[0] for c in self.candles]
        highs = [c[1] for c in self.candles]
        lows = [c[2] for c in self.candles]
        closes = [c[3] for c in self.candles]

        for i in range(len(self.candles)):
            color = 'g' if closes[i] >= opens[i] else 'r'
            ax.plot([times[i], times[i]], [lows[i], highs[i]], color=color)  # Wick
            ax.plot([times[i]], [opens[i]], marker='_', color=color)  # Open
            ax.plot([times[i]], [closes[i]], marker='_', color=color)  # Close

        ax.set_title(f"Live Candlestick Chart for {self.symbol}")
        ax.set_xlabel("Time")
        ax.set_ylabel("Price")
        plt.savefig(filepath)
        plt.close()

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