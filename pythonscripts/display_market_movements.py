import matplotlib.pyplot as plt
import pandas as pd

# Load the CSV data
data = pd.read_csv('AAPL_market_data.csv')

# Calculate the mark price as the average of bid and ask prices
data['mark'] = (data['bid'] + data['ask']) / 2

# Create a figure and axis objects
fig, ax1 = plt.subplots()

# Plot the bid and ask lines
ax1.plot(data['bid'], color='green', label='Bid')
ax1.plot(data['ask'], color='red', label='Ask')

# Plot the mark price line
ax1.plot(data['mark'], color='blue', label='Mark')

ax1.set_xlabel('Time (index)')
ax1.set_ylabel('Price')
ax1.legend(loc='upper left')

# Create a second y-axis for the bidSize and askSize
ax2 = ax1.twinx()

# Plot the bidSize and askSize as overlaid bar charts
width = 0.3  # width of the bars
ax2.bar(data.index - width/2, data['bidSize'], width=width, color='green', alpha=0.5, label='Bid Size')
ax2.bar(data.index + width/2, data['askSize'], width=width, color='red', alpha=0.5, label='Ask Size')
ax2.set_ylabel('Size')

# Add legends for both plots
ax2.legend(loc='upper right')

# Show the plot
plt.title('Bid/Ask Prices with Mark Price and Sizes')
plt.show()
