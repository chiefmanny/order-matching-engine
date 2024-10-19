import pandas as pd
import matplotlib.pyplot as plt

# Load CSV files
data_coarse = pd.read_csv('throughput_results_coarse.csv')
data_fine = pd.read_csv('throughput_results_fine.csv')

# Extract data_coarse test sizes
test_sizes = data_coarse['Test Size'].unique()

# Create box and whisker plots for each test size
for test_size in test_sizes:
    # Filter data for the current test size
    data_coarse_filtered = data_coarse[data_coarse['Test Size'] == test_size]['Duration (ms)']
    data_fine_filtered = data_fine[data_fine['Test Size'] == test_size]['Duration (ms)']
    
    # Create a box and whisker plot
    plt.figure(figsize=(10, 6))
    plt.boxplot([data_coarse_filtered, data_fine_filtered], labels=['Coarse Implementation', 'Fine Implementation'])
    plt.title(f'Boxplot for Test Size {test_size}')
    plt.ylabel('Duration (ms)')
    plt.grid(True)
    
    # Show the plot
    plt.show()
