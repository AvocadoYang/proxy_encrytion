#!/bin/bash

DIR_PATH="/var/kenmec/tcp_cer"

if [! -f ./proxy_server]; then
    echo "Error: ./proxy_server not found"
    exit 1
fi

if [ ! -d "$DIR_PATH" ]; then
    sudo mkdir -p "$DIR_PATH"
fi


sudo cp ./proxy_server /usr/local/bin/proxy_server
sudo chmod +x /usr/local/bin/proxy_server


sudo tee /etc/systemd/system/proxy_server.service > /dev/null <<EOF
[Unit]
Description=kenmec Proxy server Server Program
After=network.target

[Service]
ExecStart=/usr/local/bin/proxy_server
Restart=always
RestartSec=5
User=root
WorkingDirectory=/usr/local/bin
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
EOF

sudo systemctl daemon-reload
sudo systemctl enable proxy_server
sudo systemctl start proxy_server