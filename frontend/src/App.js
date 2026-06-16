import React, { useState, useEffect } from 'react';
import Plot from 'react-plotly.js';
import './App.css';

function App() {
  const [chartData, setChartData] = useState({
    x: [],
    open: [],
    high: [],
    low: [],
    close: [],
  });
  const [level2Data, setLevel2Data] = useState({ bids: [], asks: [] });

  useEffect(() => {
    const apiBase = process.env.REACT_APP_API_HTTP_URL || 'http://localhost:8000';
    const wsBase = process.env.REACT_APP_API_WS_URL || 'ws://localhost:8000';

    const fetchLevel2Data = () => {
      fetch(`${apiBase}/get-level2-data?symbol=AAPL&depth=10`)
        .then((response) => response.json())
        .then((data) => setLevel2Data(data))
        .catch((error) => console.error('Error fetching Level 2 data:', error));
    };

    const socket = new WebSocket(`${wsBase}/ws/chart`);

    socket.onmessage = function (event) {
      const data = JSON.parse(event.data);
      setChartData(prevData => ({
        x: [...prevData.x, data.time],
        open: [...prevData.open, data.open],
        high: [...prevData.high, data.high],
        low: [...prevData.low, data.low],
        close: [...prevData.close, data.close]
      }));
    };

    socket.onerror = function (error) {
      console.error('WebSocket Error:', error);
    };

    const interval = setInterval(fetchLevel2Data, 1000);  // Sync Level 2 data with the chart update interval

    return () => {
      socket.close();
      clearInterval(interval);
    };
  }, []);

  return (
    <div className="App">
      <h1>Live Candlestick Chart and Level 2 Data</h1>

      <div className="container">
        {/* Live Candlestick Chart */}
        <div className="chart">
          <Plot
            data={[
              {
                x: chartData.x,
                open: chartData.open,
                high: chartData.high,
                low: chartData.low,
                close: chartData.close,
                type: 'candlestick',
                xaxis: 'x',
                yaxis: 'y',
              },
            ]}
            layout={{
              dragmode: 'pan',
              xaxis: {
                rangeslider: { visible: false },
                title: 'Time',
              },
              yaxis: { title: 'Price' },
            }}
            useResizeHandler
            style={{ width: '100%', height: '100%' }}
          />
        </div>

        {/* Level 2 Data: Bids and Asks */}
        <div className="level2-data">
          <div className="bids-asks">
            <div className="bids">
              <h3>Bids</h3>
              <table>
                <thead>
                  <tr>
                    <th>Rank</th>
                    <th>Size</th>
                    <th>Price</th>
                  </tr>
                </thead>
                <tbody>
                  {level2Data.bids.map((bid, index) => (
                    <tr key={index} style={{ color: 'green' }}>
                      <td>{bid.rank}</td>
                      <td>{bid.size}</td>
                      <td>{bid.price.toFixed(2)}</td>
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
    </div>
  );
}

export default App;
