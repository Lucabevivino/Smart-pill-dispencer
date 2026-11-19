import time
import requests

API_URL = "http://127.0.0.1:5000/log_pill"

# Sample dummy data representing realistic sensor events
SAMPLE_REQUESTS = [
    {
        "device_id": "ESP_001",
        "slot_id": 1,
        "clip_state": "opened",
        "sensor_timestamp": "2026-11-19T15:30:00"
    },
    {
        "device_id": "ESP_001",
        "slot_id": 1,
        "clip_state": "closed",
        "sensor_timestamp": "2026-11-19T15:30:18"
    },
    {
        "device_id": "ESP_001",
        "slot_id": 2,
        "clip_state": "opened",
        "sensor_timestamp": "2026-11-19T16:00:00"
    }
]

def main():
    print(f"Connecting to {API_URL} to send sample dummy logs...\n")
    
    for i, payload in enumerate(SAMPLE_REQUESTS, start=1):
        print(f"--- Sending Sample {i} ---")
        print(f"Payload: {payload}")
        try:
            response = requests.post(API_URL, json=payload, timeout=5)
            print(f"Status Code: {response.status_code}")
            print(f"Response: {response.json()}\n")
        except requests.exceptions.ConnectionError:
            print(f"Error: Could not connect to {API_URL}.")
            print("Please ensure 'python server.py' is running in another terminal window first!\n")
            return
        except Exception as e:
            print(f"Unexpected error: {e}\n")
            return
        
        # Brief pause between samples
        time.sleep(1)

    print("All sample requests completed successfully!")

if __name__ == "__main__":
    main()
