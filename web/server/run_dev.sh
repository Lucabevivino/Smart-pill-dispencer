#!/usr/bin/env bash
# run_dev.sh - Start the MedClip Unified Server (Frontend + Firebase Backend)
set -e

PORT=${1:-8000}
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$DIR"

echo "=========================================================="
echo "  Starting MedClip Server (Frontend + Cloud Firestore)    "
echo "=========================================================="

if ! command -v python3 &> /dev/null; then
    echo "Error: python3 is not installed or not in PATH."
    exit 1
fi

if ! python3 -c "import firebase_admin" &> /dev/null; then
    echo "Installing dependencies: pip install firebase-admin..."
    pip install firebase-admin
fi

echo "Local Web App: http://localhost:$PORT"
echo "Default Login: patient@medclip.local / medclip123"
echo ""
echo "To test events in a second terminal:"
echo "  cd web/server"
echo "  python3 simulate_clip.py taken A1F3B2"
echo "=========================================================="

python3 server.py --seed --port "$PORT"
