import React, { useState, useEffect } from 'react';
import './App.css';  // Import the new stylesheet for custom styles

function App() {
  const [chartUrl, setChartUrl] = useState('');
  const [level2Data, setLevel2Data] = useState({ bids: [], asks: [] });

  // Unified interval for both chart and level 2 data updates
  useEffect(() => {
    const fetchData = () => {
      // Fetch chart URL via WebSocket
      const socket = new WebSocket('ws://localhost:8000/ws/chart');
      socket.onmessage = function (event) {
        const data = JSON.parse(event.data);
        setChartUrl(`http://localhost:8000${data.chartUrl}?t=${new Date().getTime()}`);
      };
      socket.onerror = function (error) {
        console.error('WebSocket Error:', error);
      };

      // Fetch Level 2 data via HTTP request
      fetch('http://localhost:8000/get-level2-data?symbol=AAPL&depth=5')
        .then((response) => response.json())
        .then((data) => setLevel2Data(data))
        .catch((error) => console.error('Error fetching Level 2 data:', error));

      return () => socket.close(); // Clean up WebSocket connection
    };

    fetchData();  // Initial fetch
    const interval = setInterval(fetchData, 1000);  // Unified interval for both chart and Level 2 data

    return () => clearInterval(interval);  // Clean up interval on component unmount
  }, []);

  return (
    <div className="App">
      <h1>Live Candlestick Chart and Level 2 Data</h1>
      <div className="container">
        {/* Live Candlestick Chart */}
        <div className="chart">
          {chartUrl ? <img src={chartUrl} alt="Live Candlestick Chart" /> : <p>Loading chart...</p>}
        </div>

        {/* Level 2 Data */}
        <div className="level2-data">
          <div className="bids">
            <h3>Bids</h3>
            <table>
              <thead>
                <tr>
                  <th>Price</th>
                  <th>Size</th>
                  <th>Rank</th>
                </tr>
              </thead>
              <tbody>
                {level2Data.bids.map((bid, index) => (
                  <tr key={index} style={{ color: 'green' }}>
                    <td>{bid.price.toFixed(2)}</td>
                    <td>{bid.size}</td>
                    <td>{bid.rank}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>

          <div className="asks">
            <h3>Asks</h3>
            <table>
              <thead>
                <tr>
                  <th>Price</th>
                  <th>Size</th>
                  <th>Rank</th>
                </tr>
              </thead>
              <tbody>
                {level2Data.asks.map((ask, index) => (
                  <tr key={index} style={{ color: 'red' }}>
                    <td>{ask.price.toFixed(2)}</td>
                    <td>{ask.size}</td>
                    <td>{ask.rank}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        </div>
      </div>
    </div>
  );
}

export default App;
