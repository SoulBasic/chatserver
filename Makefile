server:	
	rm -rf ./server
	g++ ./src/Timer.cpp ./src/client.cpp ./src/httpserver.cpp ./src/main.cpp -std=c++14 -lpthread -o ./server
debug: 
	rm -rf ./server-debug
	g++ ./src/Timer.cpp ./src/client.cpp ./src/httpserver.cpp ./src/main.cpp -std=c++14 -g -lpthread -o ./server-debug

clean:
	rm -rf ./server
	rm -rf ./server-debug