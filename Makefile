# Kompilator i flagi
CXX = g++
CC = gcc
CXXFLAGS = -Wall -Wextra -O2 -std=c++20
CFLAGS = -Wall -Wextra -O2

# Pliki wykonywalne
CLIENT = kayles_client
SERVER = kayles_server

# Pliki obiektowe (wszystkie części składowe)
COMMON_OBJ = common.o err.o
CLIENT_OBJ = kayles_client.o $(COMMON_OBJ)
SERVER_OBJ = kayles_server.o $(COMMON_OBJ)

# Domyślny cel: buduje oba programy
all: $(CLIENT) $(SERVER)

# Budowanie klienta
$(CLIENT): $(CLIENT_OBJ)
	$(CXX) $(CXXFLAGS) $(CLIENT_OBJ) -o $(CLIENT)

# Budowanie serwera
$(SERVER): $(SERVER_OBJ)
	$(CXX) $(CXXFLAGS) $(SERVER_OBJ) -o $(SERVER)

# Kompilacja plików .cpp na .o
# Dodajemy zależność od common.hpp, żeby zmiana w nagłówku wymusiła rekompilację
%.o: %.cpp common.hpp err.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Kompilacja err.c (używamy gcc, bo to czyste C)
err.o: err.c err.h
	$(CC) $(CFLAGS) -c err.c -o err.o

# Czyszczenie plików binarnych i obiektowych
clean:
	rm -f $(CLIENT) $(SERVER) *.o

# Cel "phony" - informuje make, że to nie są nazwy plików
.PHONY: all clean