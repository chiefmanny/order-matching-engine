import pandas as pd

# Function to clean CSV by removing rows containing any zeroes
def clean_csv(input_csv, output_csv):
    # Read the CSV file
    df = pd.read_csv(input_csv)
    
    # Remove rows containing any 0 value
    df_cleaned = df[(df != 0).all(axis=1)]
    
    # Save the cleaned DataFrame to a new CSV
    df_cleaned.to_csv(output_csv, index=False)
    print(f"Cleaned CSV saved to {output_csv}")

# Example usage
input_csv = 'AAPL_market_data.csv'  # Replace with your input CSV file path
output_csv = 'AAPL_market_data.csv'  # Output CSV file path

clean_csv(input_csv, output_csv)