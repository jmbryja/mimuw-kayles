#include <iostream>
#include <vector>
#include <string>
#include <string_view>
#include <charconv>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netdb.h>
#include <queue>
#include <unistd.h>
#include <span>
#include <unordered_map>
#include <optional>
#include "common.hpp"
#include <chrono>
#include <cstring>

extern "C" {
    #include "err.h"
}

struct GameInstance {
    uint32_t game_id;
    std::optional<Player> player1, player2;
    GameStatus state;
    uint8_t max_pawn;
    std::vector<uint8_t> pawn_row;
    uint16_t remaining_pawns;
    std::chrono::steady_clock::time_point last_activity_p1;
    std::chrono::steady_clock::time_point last_activity_p2;
};

struct ServerConfig {
    std::optional<std::string> pawn_row;
    std::optional<std::string> address;
    std::optional<uint16_t> port;
    std::optional<int> timeout;
};

uint32_t game_id_counter = 0;

// pending_game_id is the id of the game in which a player is waiting for
// their opponent.
std::optional<uint32_t> pending_game_id;
std::unordered_map<uint32_t, GameInstance> games;

// Verifies that a pawn row is of length 1-256, starts and ends with a '1'
// and only contains 1's and 0's.
void verify_row(const std::string& row) {
    if (row.empty() || row.length() > 256) {
        fatal("Invalid pawn_row: length must be 1-256");
    }
    if (row.front() != '1' || row.back() != '1') {
        fatal("Invalid pawn_row: first and last must be '1'");
    }
    for (const char& c : row) {
        if (c != '1' && c != '0') {
            fatal("Invalid pawn_row: only '1' and '0' are allowed");
        }
    }
}

// Verifies the correctness of arguments and assigns them to ServerConfig.
ServerConfig parse_arguments(int argc, char *argv[]) {
    ServerConfig config;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg(argv[i]);
        if (i + 1 >= argc)
            fatal("Option %s requires an argument", argv[i]);

        if (arg == "-r") {
            if (config.pawn_row)
                fatal("Duplicate parameter -r");
            verify_row(argv[++i]);
            config.pawn_row = argv[i];

        } else if (arg == "-a") {
            if (config.address)
                fatal("Duplicate parameter -a");
            config.address = parse_address(argv[++i]);

        } else if (arg == "-p") {
            if (config.port)
                fatal("Duplicate parameter -p");
            config.port = parse_port(argv[++i]);

        } else if (arg == "-t") {
            if (config.timeout)
                fatal("Duplicate parameter -t");
            config.timeout = parse_timeout(argv[++i]);
            if (config.timeout < 1 || config.timeout > 99)
                fatal("Invalid timeout: must be between 1 and 99 seconds");

        } else {
            fatal("Unknown argument: %s", argv[i]);
        }
    }
    if (!config.pawn_row || !config.address || !config.port || !config.timeout)
        fatal("Missing mandatory parameters. Usage: -r -a -p -t");

    return config;
}

void check_timeouts(int timeout_seconds) {
    auto now = std::chrono::steady_clock::now();
    auto timeout_duration = std::chrono::seconds(timeout_seconds);

    for (auto it = games.begin(); it != games.end();) {
        auto& game = it->second;
        bool is_timed_out_p1 = now - game.last_activity_p1 > timeout_duration;
        bool is_timed_out_p2 = game.player2 && (now - game.last_activity_p2 > timeout_duration);

        if (game.state == GameStatus::WAITING_FOR_OPPONENT && is_timed_out_p1) {
            if (pending_game_id == game.game_id) {
                pending_game_id = std::nullopt;
            }
            games.erase(it++);
            continue;
        }

        else if (game.state == GameStatus::TURN_A || game.state == GameStatus::TURN_B) {
            if (is_timed_out_p1 && is_timed_out_p2) {
                it = games.erase(it);
                continue;
            }
            else if (is_timed_out_p1) {
                game.state = GameStatus::WIN_B;
                game.last_activity_p1 = now;
            }
            else if (is_timed_out_p2) {
                game.state = GameStatus::WIN_A;
                game.last_activity_p2 = now;
            }
        }
        else if (game.state == GameStatus::WIN_A || game.state == GameStatus::WIN_B) {

            is_timed_out_p1 = now - game.last_activity_p1 > timeout_duration;
            is_timed_out_p2 = game.player2 &&
                (now - game.last_activity_p2 > timeout_duration);

            if (is_timed_out_p1 && is_timed_out_p2) {
                it = games.erase(it);
                continue;
            }
        }
        ++it;
    }
}

// Finds the next free game_id to use returns std::nullopt if no id was found.
std::optional<uint32_t> find_next_free_id() {
    uint32_t first_id = game_id_counter;
    do {
        if (games.find(++game_id_counter) == games.end())
            return game_id_counter;
        game_id_counter++;
    } while (first_id != game_id_counter);
    return std::nullopt;
}

// Sends an error message to the client containing first 12 bytes of client
// input, the 8-bit number 255 and an error index pointing to the first
// byte that could not be processed by the server.
void send_error_message(const int socket_fd, const std::vector<uint8_t>& buffer,
    const uint8_t error_index, const sockaddr_in& client_address) {

    ErrorMessage error_msg{};
    size_t bytes_to_copy = std::min(buffer.size(), sizeof(error_msg.original_bytes));
    std::memcpy(error_msg.original_bytes, buffer.data(), bytes_to_copy);
    error_msg.error_index = error_index;
    error_msg.status = 255;

    ssize_t sent_bytes = sendto(socket_fd, &error_msg, sizeof(error_msg), 0,
        reinterpret_cast<const sockaddr*>(&client_address), sizeof(client_address));
    if (sent_bytes < 0)
        syserr("sendto");

}
// Sends the state of the game back to the client.
void send_game_state(const int socket_fd, const GameInstance& game,
    const sockaddr_in& client_address) {

    GameStateHeader header;
    std::vector<uint8_t> packet(game.pawn_row.size() + sizeof(GameStateHeader));
    header.game_id = htonl(game.game_id);
    header.player_a_id = htonl(game.player1->id);
    header.player_b_id = game.player2 ? htonl(game.player2->id) : 0;
    header.max_pawn = game.max_pawn;
    header.status = static_cast<uint8_t>(game.state);

    std::memcpy(packet.data(), &header, sizeof(GameStateHeader));
    std::memcpy(packet.data() + sizeof(GameStateHeader), game.pawn_row.data(),
    game.pawn_row.size());


    ssize_t sent_bytes = sendto(socket_fd, packet.data(), packet.size(), 0,
        reinterpret_cast<const sockaddr*>(&client_address), sizeof(client_address));

    if (sent_bytes < 0)
        syserr("sendto");
}


// Handles the client join request.
void handle_join(std::vector<uint8_t> &buffer, const MsgJoin msg,
    const sockaddr_in client_address, const ServerConfig& config,
    const int socket_fd) {

    // Invalid player id.
    if (msg.player_id == 0) {
        send_error_message(socket_fd, buffer,
            offsetof(MsgJoin, player_id), client_address);
        return;
    }

    // If there is no pending game, make an instance.
    if (!pending_game_id) {
        // Game id in use
        auto game_id = find_next_free_id();
        if (!game_id) {
            return;
        }
        std::vector<uint8_t> pawn_row;
        size_t pawns_length = config.pawn_row.value().length();
        size_t num_bytes = (pawns_length + 7) / 8;
        std::vector<uint8_t> bitmap(num_bytes, 0);
        u_int16_t pawns_count = 0;

        for (size_t i = 0; i < pawns_length; ++i) {
            if (config.pawn_row.value()[i] == '1') {
                pawns_count++;
                bitmap[i / 8] |= (1 << (7 - (i % 8)));
            }
        }
        Player player = Player{msg.player_id, client_address};
        auto now = std::chrono::steady_clock::now();

        GameInstance game = GameInstance{
            game_id.value(),
            player,
            std::nullopt,
            GameStatus::WAITING_FOR_OPPONENT,
            static_cast<uint8_t>(config.pawn_row.value().length() - 1),
            std::move(bitmap),
            pawns_count,
            now,
            now,
        };
        pending_game_id = game_id;
        send_game_state(socket_fd, game, client_address);
        games.emplace(game_id.value(), std::move(game));
    }
    // There is a pending game
    else if (games[pending_game_id.value()].player1) {

        auto& game = games[pending_game_id.value()];

        game.player2 = Player{msg.player_id, client_address};
        game.state = GameStatus::TURN_B;
        const std::chrono::steady_clock::time_point now =
                std::chrono::steady_clock::now();
        game.last_activity_p1 = now;
        game.last_activity_p2 = now;
        pending_game_id = std::nullopt;
        send_game_state(socket_fd, game, client_address);
    }
    else if (games[pending_game_id.value()].player1->id == msg.player_id){
        auto& game = games[pending_game_id.value()];
        send_game_state(socket_fd, game, client_address);
    }
}

bool is_pawn_on_board(const std::vector<uint8_t>& board, const uint8_t pawn) {
    const size_t byte_index = pawn / 8;
    uint8_t bit_index = 7 - (pawn % 8);
    return board[byte_index] & (1 << bit_index);
}

void handle_move(bool is_move_two, const uint32_t game_id, const uint8_t pawn,
    const std::vector<uint8_t>& buffer,
    const Player &player, const int socket_fd) {

    auto gameIt = games.find(game_id);
    // game was not found.
    if (gameIt == games.end()) {
        // Finds the first bit of game_id in MsgMove
        uint8_t error_index = offsetof(MsgMove, game_id);
        send_error_message(socket_fd, buffer, error_index, player.address);
        return;
    }

    auto& game = gameIt->second;

    // Checking whether the id of the player matches any of the two ids in the
    // game and whether the turn of the player matches.
    bool is_player1 = game.player1->id == player.id;
    bool is_player2 = game.player2.has_value() && game.player2->id == player.id;
    bool turn_is_player1 = game.state == GameStatus::TURN_A && is_player1;
    bool turn_is_player2 = game.state == GameStatus::TURN_B && is_player2;

    if (!is_player1 && !is_player2) {
        uint8_t error_index = offsetof(MsgMove, player_id);
        send_error_message(socket_fd, buffer, error_index, player.address);
        return;
    }

    if (!(turn_is_player1 || turn_is_player2)) {
        send_game_state(socket_fd, game, player.address);
        return;
    }

    //Checking whether the pawn index is out of bounds.
    if (pawn > game.max_pawn || (is_move_two && (pawn + 1) > game.max_pawn) ) {
        send_game_state(socket_fd, game, player.address);
        return;
    }

    if (!is_pawn_on_board(game.pawn_row, pawn) ||
        (is_move_two && !is_pawn_on_board(game.pawn_row, pawn + 1))) {

        send_game_state(socket_fd, game, player.address);
        return;
    }

    // Setting the bit.
    game.pawn_row[pawn / 8] &= ~(1 << (7 - (pawn % 8)));
    game.remaining_pawns--;
    if (is_move_two) {
        game.remaining_pawns--;
        game.pawn_row[(pawn + 1) / 8] &= ~(1 << (7 - ((pawn + 1) % 8)));
    }


    if (game.remaining_pawns == 0)
        game.state = (game.state == GameStatus::TURN_A) ? GameStatus::WIN_A : GameStatus::WIN_B;
    // Player1 and Player2 share the same id and the game is not over yet.
    else if (is_player1 && is_player2) {
        if (game.state == GameStatus::TURN_A)
            game.state = GameStatus::TURN_B;
        else
            game.state = GameStatus::TURN_A;
    }
    else
        game.state = is_player1 ? GameStatus::TURN_B : GameStatus::TURN_A;

    send_game_state(socket_fd, game, player.address);
    auto now = std::chrono::steady_clock::now();
    if (is_player1)
        game.last_activity_p1 = now;
    if (is_player2)
        game.last_activity_p2 = now;
}


void handle_keep_alive(const int socket_fd, uint32_t game_id,
    const std::vector<uint8_t>& buffer, Player player) {

    auto game_it = games.find(game_id);
    // Game not found
    if (game_it == games.end()) {
        send_error_message(socket_fd, buffer, offsetof(MsgState, game_id), player.address);
        return;
    }

    
    auto now = std::chrono::steady_clock::now();
    // Player id matches the first player.
    if (game_it->second.player1->id == player.id) {
        game_it->second.last_activity_p1 = now;
        if (game_it->second.player2.has_value() && game_it->second.player2->id == player.id)
            game_it->second.last_activity_p2 = now;
    }
    // Player id matches the second player.
    else if (game_it->second.player2.has_value() && game_it->second.player2->id == player.id)
        game_it->second.last_activity_p2 = now;
    // Player id does not match either of the players.
    else {
        send_error_message(socket_fd, buffer, offsetof(MsgState, player_id), player.address);
        return;
    }
    send_game_state(socket_fd, game_it->second, player.address);
}

void handle_give_up(MsgState msg, std::vector<uint8_t> &buffer,
    const sockaddr_in client_address, const int socket_fd) {

    auto gameIt = games.find(msg.game_id);
    // The game was not found.
    if (gameIt == games.end()) {
        uint8_t error_index = offsetof(MsgState, game_id);
        send_error_message(socket_fd, buffer, error_index, client_address);
        return;
    }
    auto& game = gameIt->second;
    bool is_player1 = game.player1->id == msg.player_id;
    bool is_player2 = game.player2.has_value() && game.player2->id == msg.player_id;

    bool turn_player_1 = game.state == GameStatus::TURN_A;
    bool turn_player_2 = game.state == GameStatus::TURN_B;

    if (!is_player1 && !is_player2) {
        send_error_message(socket_fd, buffer, offsetof(MsgState, player_id), client_address);
        return;
    }
    if (is_player1 && turn_player_1) {
        game.state = GameStatus::WIN_B;
    }
    else if (is_player2 && turn_player_2) {
        game.state = GameStatus::WIN_A;
    }
    send_game_state(socket_fd, game, client_address);
}

// Detects the type of client request and hands it over to the handle method.
void handle_message(std::vector<uint8_t> &buffer, const ssize_t rec_len,
                    sockaddr_in client_address, const int socket_fd, const ServerConfig &config) {

    if (rec_len < static_cast<ssize_t>(sizeof(MsgJoin))) {
        send_error_message(socket_fd, buffer, 0, client_address);
        return;
    }

    //TODO CHECK TIMEOUT

    uint8_t msg_type = buffer[0];
    bool is_move_two = true;
    switch (static_cast<MessageType>(msg_type)) {
        case MessageType::JOIN: {
            if (rec_len != static_cast<ssize_t>(sizeof(MsgJoin))) {
                send_error_message(socket_fd, buffer, 0, client_address);
                return;
            }

            MsgJoin msg;
            std::memcpy(&msg, buffer.data(), sizeof(MsgJoin));
            msg.player_id = ntohl(msg.player_id);
            handle_join(buffer, msg, client_address, config, socket_fd);
            break;
        }
        case MessageType::MOVE_1:
            is_move_two = false;
            [[fallthrough]];
        case MessageType::MOVE_2: {
            if (rec_len != static_cast<ssize_t>(sizeof(MsgMove))) {
                send_error_message(socket_fd, buffer, 0, client_address);
                return;
            }
            MsgMove msg;
            std::memcpy(&msg, buffer.data(), sizeof(MsgMove));
            msg.game_id = ntohl(msg.game_id);
            msg.player_id = ntohl(msg.player_id);
            if (msg.player_id == 0) {
                send_error_message(socket_fd, buffer, offsetof(MsgMove, player_id), client_address);
                return;
            }
            handle_move(is_move_two, msg.game_id, msg.pawn, buffer,
                Player{msg.player_id, client_address}, socket_fd);
            break;
        }
        case MessageType::KEEP_ALIVE:
            if (rec_len != static_cast<ssize_t>(sizeof(MsgState))) {
                send_error_message(socket_fd, buffer, 0, client_address);
                return;
            }
            MsgState msg;
            std::memcpy(&msg, buffer.data(), sizeof(MsgState));
            msg.game_id = ntohl(msg.game_id);
            msg.player_id = ntohl(msg.player_id);
            if (msg.player_id == 0) {
                send_error_message(socket_fd, buffer, offsetof(MsgState, player_id), client_address);
                return;
            }
            handle_keep_alive(socket_fd, msg.game_id, buffer, Player{msg.player_id, client_address});
            break;
        case MessageType::GIVE_UP:
            if (rec_len != static_cast<ssize_t>(sizeof(MsgState))) {
                send_error_message(socket_fd, buffer, 0, client_address);
                return;
            }
            std::memcpy(&msg, buffer.data(), sizeof(MsgState));
            msg.game_id = ntohl(msg.game_id);
            msg.player_id = ntohl(msg.player_id);
            if (msg.player_id == 0) {
                send_error_message(socket_fd, buffer, offsetof(MsgState, player_id), client_address);
                return;
            }
            handle_give_up(msg, buffer, client_address, socket_fd);
            break;
        default:
            send_error_message(socket_fd, buffer, 0, client_address);
            break;
    }
}

int main(int argc, char *argv[]) {

    ServerConfig config =  parse_arguments(argc, argv);

    // Create and bind a socket.
    Socket socket_fd(AF_INET, SOCK_DGRAM, 0);

    struct addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    struct addrinfo *res;
    std::string port_str = std::to_string(*config.port);
    if (getaddrinfo(config.address->c_str(), port_str.c_str(), &hints, &res) != 0)
        syserr("getaddrinfo");
    sockaddr_in server_address = *reinterpret_cast<sockaddr_in*>(res->ai_addr);
    freeaddrinfo(res);


    if (bind(socket_fd, reinterpret_cast<sockaddr*>(&server_address),
        sizeof(server_address)) < 0) {
        syserr("bind");
    }

    //Setting timeout to 0.5s
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 500000;
    if (setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO,
        &tv, sizeof(tv)) < 0) {
        syserr("setsockopt");
    }

    std::vector<uint8_t> buffer(MAX_UDP_PACKET_SIZE);


    while (true) {
        sockaddr_in client_address;
        socklen_t client_address_len = sizeof(client_address);

        ssize_t rec_len = recvfrom(socket_fd, buffer.data(), buffer.size(), 0,
                                   reinterpret_cast<sockaddr*>(&client_address), &client_address_len);

        if (rec_len < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (config.timeout.has_value())
                    check_timeouts(config.timeout.value());
                continue;
            }
            else
                syserr("recvfrom");
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(client_address.sin_addr), client_ip, INET_ADDRSTRLEN);
        check_timeouts(config.timeout.value());
        handle_message(buffer, rec_len, client_address, socket_fd, config);
    }
    return 0;
}
