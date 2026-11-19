import os
import glob
from flask import Flask, request, jsonify
import firebase_admin
from firebase_admin import credentials, firestore

# 1. Connect to Firebase using the downloaded JSON key
script_dir = os.path.dirname(os.path.abspath(__file__))
json_files = glob.glob(os.path.join(script_dir, "*.json"))
if json_files:
    cred = credentials.Certificate(json_files[0])
else:
    cred = credentials.Certificate(os.path.join(script_dir, "smart-pill-dispenser-c841a-firebase-adminsdk-fbsvc-1d2ee8ba65.json"))
firebase_admin.initialize_app(cred)
db = firestore.client()

app = Flask(__name__)

# 2. Create an endpoint for the ESP to send data to
@app.route('/log_pill', methods=['POST'])
def log_pill():
    # Receive data from ESP
    data = request.get_json(silent=True) or {}
    
    # Extract fields with safe defaults/handling
    payload = {
        'device_id': data.get('device_id', 'unknown'),
        'slot_id': data.get('slot_id'),
        'clip_state': data.get('clip_state'),
        'sensor_timestamp': data.get('sensor_timestamp')
    }
    
    # 3. Save the data to Firebase Firestore in a collection called 'pill_logs'
    firestore_data = dict(payload)
    firestore_data['server_timestamp'] = firestore.SERVER_TIMESTAMP
    db.collection('pill_logs').add(firestore_data)
    
    # Return saved payload (excluding the server timestamp sentinel) for easy debugging
    return jsonify({
        "status": "success",
        "data": payload,
        "saved_payload": payload
    }), 200

if __name__ == '__main__':
    # Run the server on your local network
    app.run(host='0.0.0.0', port=5000)