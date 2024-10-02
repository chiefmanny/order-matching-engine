import grpc
import matching_engine_pb2
import matching_engine_pb2_grpc
import time
import threading
import matplotlib.pyplot as plt
import matplotlib.animation as animation
from datetime import datetime
from collections import deque

class CandlestickChart:
    def __init__(self, symbol, candle_size_seconds):
        self.symbol = symbol
        self.candle_size_seconds = candle_size_seconds
        self.price_data = deque(maxlen=100)  # Store the last 100 candles
        self.candles = []  # Store candlestick data (open, high, low, close)
        self.start_time = None

        # Setup Matplotlib figure and axes
        self.fig, self.ax = plt.subplots()
        self.ax.set_title(f"Live Candlestick Chart for {symbol}")
        self.ax.set_xlabel("Time")
        self.ax.set_ylabel("Price")

    def update_candles(self, price):
        current_time = datetime.now()
        
        if self.start_time is None:
            self.start_time = current_time
        
        # Time elapsed since the last candle started
        time_elapsed = (current_time - self.start_time).total_seconds()

        if time_elapsed >= self.candle_size_seconds:
            # Form a new candle if the time elapsed is greater than or equal to the candle size
            if self.price_data:
                # Use deque to form OHLC (open, high, low, close) data
                open_price = self.price_data[0]
                high_price = max(self.price_data)
                low_price = min(self.price_data)
                close_price = self.price_data[-1]
                self.candles.append((open_price, high_price, low_price, close_price))
                self.price_data.clear()  # Reset price data for the new candle

            self.start_time = current_time

        self.price_data.append(price)

    def plot_live(self):
        def animate(i):
            self.ax.clear()

            # Extract the OHLC values for plotting
            times = list(range(len(self.candles)))
            opens = [c[0] for c in self.candles]
            highs = [c[1] for c in self.candles]
            lows = [c[2] for c in self.candles]
            closes = [c[3] for c in self.candles]

            # Plot the candlesticks
            for i in range(len(self.candles)):
                color = 'g' if closes[i] >= opens[i] else 'r'
                self.ax.plot([times[i], times[i]], [lows[i], highs[i]], color=color)  # Wick
                self.ax.plot([times[i]], [opens[i]], marker='_', color=color)  # Open
                self.ax.plot([times[i]], [closes[i]], marker='_', color=color)  # Close

            self.ax.set_title(f"Live Candlestick Chart for {self.symbol}")
            self.ax.set_xlabel("Time")
            self.ax.set_ylabel("Price")

        ani = animation.FuncAnimation(self.fig, animate, interval=1000)
        plt.show()

def subscribe_to_price(symbol, candle_size_seconds):
    # Set up a connection to the gRPC server
    channel = grpc.insecure_channel("localhost:50051")
    stub = matching_engine_pb2_grpc.MatchingEngineStub(channel)

    # Create a request for the symbol
    symbol_request = matching_engine_pb2.Symbol(symbol=symbol)

    # Start watching the symbol in a separate thread
    candlestick_chart = CandlestickChart(symbol, candle_size_seconds)

    def watch_symbol():
        for data in stub.WatchSymbol(symbol_request):
            price = (data.lastBid + data.lastAsk) / 2  # Midpoint price for the candlestick
            print(f"Received price update: {price}")
            candlestick_chart.update_candles(price)

    watch_thread = threading.Thread(target=watch_symbol)
    watch_thread.start()

    # Start live plotting of the candlesticks
    candlestick_chart.plot_live()

# Main function to run the live candlestick script
if __name__ == "__main__":
    symbol = "AAPL"
    candle_size_seconds = 1  # Candle size in seconds (e.g., 1 second per candle)
    subscribe_to_price(symbol, candle_size_seconds)
