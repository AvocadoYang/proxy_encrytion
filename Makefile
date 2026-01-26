build:
	g++ ./main.cpp ./proxy_server.cpp -O2 -g -o ./main.bin -lssl -lcrypto