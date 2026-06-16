from fastapi import FastAPI, HTTPException, WebSocket, WebSocketDisconnect
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import JSONResponse
import os
import asyncio
import threading
import time
import grpc
import matching_engine_pb2 as pb2
import matching_engine_pb2_grpc as pb2_grpc
from live_candlestick import subscribe_to_price

GRPC_TARGET = os.environ.get("GRPC_SERVER_HOST", "localhost") + ":50051"

app = FastAPI()

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],  # Update this to the frontend's URL in production
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

# Global variable for candlestick chart (shared between WebSocket and chart generation)
candlestick_chart = None
candle_size = 1

@app.on_event("startup")
async def start_watcher():
    global candlestick_chart
    # Start watching the symbol (e.g., "AAPL") with 1-second candles
    candlestick_chart = subscribe_to_price("AAPL", candle_size)

# WebSocket route for pushing live candlestick data
@app.websocket("/ws/chart")
async def websocket_endpoint(websocket: WebSocket):
    await websocket.accept()
    try:
        while True:
            if candlestick_chart:
                latest_candle = candlestick_chart.get_latest_candle()
                if latest_candle:
                    await websocket.send_json(latest_candle)
            await asyncio.sleep(candle_size)  # Adjust the frequency of updates as necessary
    except WebSocketDisconnect:
        print("Client disconnected")

# gRPC connection setup
def get_grpc_stub():
    channel = grpc.insecure_channel(GRPC_TARGET)
    stub = pb2_grpc.MatchingEngineStub(channel)
    return stub

# Get level 2 data for a symbol
@app.get("/get-level2-data")
async def get_level2(symbol: str, depth: int):
    try:
        stub = get_grpc_stub()

        # Create the Level2Request message based on the depth and symbol
        request = pb2.Level2Request(symbol=symbol, side=pb2.Level2Request.BOTH, depth=depth)

        # Call the GetLevel2 method and gather the response
        level2_data = stub.GetLevel2(request)

        bids = []
        asks = []

        # Process the streamed Level2Response data
        for response in level2_data:
            for bid in response.bids:
                bids.append({
                    "price": bid.price,
                    "size": bid.size,
                    "rank": bid.rank
                })
            for ask in response.asks:
                asks.append({
                    "price": ask.price,
                    "size": ask.size,
                    "rank": ask.rank
                })

        # Return both bids and asks in the response
        return {"bids": bids, "asks": asks}

    except grpc.RpcError as e:
        print(f"gRPC error: {e}")
        raise HTTPException(status_code=500, detail="Error fetching Level 2 data")