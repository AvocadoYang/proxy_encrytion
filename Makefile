build:
	g++ ./main.cpp ./proxy_server.cpp -O2 -g -o ./proxy_server -lssl -lcrypto