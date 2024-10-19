from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from fastapi.middleware.cors import CORSMiddleware
from fastapi.staticfiles import StaticFiles
from fastapi.responses import JSONResponse
import os
import asyncio
import threading
import time
import grpc
import matching_engine_pb2 as pb2
import matching_engine_pb2_grpc as pb2_grpc
from live_candlestick import subscribe_to_price

app = FastAPI()

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],  # Update this to the frontend's URL in production
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

# Mount the static directory to serve the candlestick chart image
app.mount("/static", StaticFiles(directory="static"), name="static")

# WebSocket route for pushing chart updates to the client
@app.websocket("/ws/chart")
async def websocket_endpoint(websocket: WebSocket):
    await websocket.accept()
    try:
        while True:
            # When the chart is updated, send the new chart URL to the client
            if os.path.exists("static/live_candlestick.png"):
                await websocket.send_json({"chartUrl": "/static/live_candlestick.png"})
            await asyncio.sleep(1)  # Adjust based on how often the chart changes
    except WebSocketDisconnect:
        print("Client disconnected")

# Generate the candlestick chart every few seconds
candlestick_chart = None

@app.on_event("startup")
async def start_watcher():
    global candlestick_chart
    # Start watching the symbol (e.g., "AAPL") with 1-second candles
    candlestick_chart = subscribe_to_price("AAPL", 1)
    
    def generate_chart():
        while True:
            candlestick_chart.save_chart("static/live_candlestick.png")
            time.sleep(1)  # Adjust this to control the chart generation frequency
    
    thread = threading.Thread(target=generate_chart)
    thread.start()

# gRPC connection setup
def get_grpc_stub():
    channel = grpc.insecure_channel("localhost:50051")
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