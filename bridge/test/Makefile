# Host builds (Mac): unit tests and the test server with simulated lamps.
#   make -C test           run the tests
#   make -C test serve     test server on http://127.0.0.1:8780 (LAN=1 for the Kindle)
CXX ?= clang++
JSON := ../.pio/libdeps/esp32dev/ArduinoJson/src
FLAGS := -std=c++17 -Wall -Wextra -O1 -I$(JSON)
CORE := ../src/aes.cpp ../src/awox.cpp ../src/model.cpp ../src/api.cpp
DEPS := $(CORE) ../src/*.h

.PHONY: test serve
test: model_test
	./model_test

model_test: model_test.cpp $(DEPS)
	$(CXX) $(FLAGS) -o $@ model_test.cpp $(CORE)

host_server: host_server.cpp $(DEPS)
	$(CXX) $(FLAGS) -o $@ host_server.cpp $(CORE)

serve: host_server
	./host_server $(if $(LAN),--lan,)
