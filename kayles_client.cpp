#include <iostream>
#include <vector>
#include <string>
#include <string_view>
#include <charconv>
#include <chrono>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netdb.h>
#include <optional>
#include <unistd.h>
#include <span>
#include "common.hpp"
#include <cstring>
#include <iomanip>

extern "C" {
    #include "err.h"
}

// Stores the input passed into the client program.
struct ClientConfig {
    std::optional<std::string> address;
    std::optional<uint16_t> port;
    std::optional<int> timeout;
    std::optional<std::string> message;
};

// Splits a string message with '/' signs into a vector without these signs.
std::vector<uint32_t> split_message(std::string_view msg) {
    std::vector<uint32_t> values;
    size_t start = 0;
    size_t end = msg.find('/');

    while (true) {
        std::string_view part = msg.substr(start, end - start);
        if (part.empty())
            fatal("Empty field in message");
        uint32_t val;
        auto [ptr, ec] = std::from_chars(part.data(),
            part.data() + part.size(), val);
        if (ec != std::errc{} || ptr != part.data() + part.size()) {
            fatal("Invalid number in message: %.*s", static_cast<int>(part.size()), part.data());
        }
        values.push_back(val);
        if (end == std::string_view::npos) break;
        start = end + 1;
        end = msg.find('/', start);
    }
    return values;
}

ClientConfig parse_arguments(int argc, char *argv[]) {
    ClientConfig config;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg(argv[i]);
        if (i + 1 >= argc)
            fatal("Option %s requires an argument", argv[i]);

        if (arg == "-m") {
            if (config.message)
                fatal("Duplicate parameter -m");
            config.message = argv[++i];

        } else if (arg == "-a") {
            if (config.address)
                fatal("Duplicate parameter -a");
            config.address = parse_address(argv[++i]);

        } else if (arg == "-p") {
            if (config.port)
                fatal("Duplicate parameter -p");
            int parsed_port = parse_port(argv[++i]);
            if (parsed_port < 1 || parsed_port > 65535)
                fatal("Invalid port");
            config.port = static_cast<uint16_t>(parsed_port);

        } else if (arg == "-t") {
            if (config.timeout)
                fatal("Duplicate parameter -t");
            int parsed_timeout = parse_timeout(argv[++i]);
            if (parsed_timeout < 1 || parsed_timeout > 99)
                fatal("Invalid timeout value");
            config.timeout = parsed_timeout;

        } else {
            fatal("Unknown argument: %s", argv[i]);
        }
    }
    if (!config.message || !config.address || !config.port || !config.timeout)
        fatal("Missing mandatory parameters. Usage: -a -p -m -t");

    return config;
}

// Builds a message to be sent to the server
std::vector<uint8_t> build_network_packet(const std::string &raw_msg) {
    auto values = split_message(raw_msg);
    std::vector<uint8_t> packet;
    if (values.empty())
        fatal("Invalid message");

    switch (MessageType type = static_cast<MessageType>(values.front())) {
        case MessageType::JOIN:
            if (values.size() != 2)
                fatal("MSG_JOIN usage: 0/player_id");
            MsgJoin msg_join;
            msg_join.msg_type = type;
            msg_join.player_id = htonl(values[1]);
            packet.resize(sizeof(msg_join));
            std::memcpy(packet.data(), &msg_join, sizeof(msg_join));
            break;

        case MessageType::KEEP_ALIVE:
        case MessageType::GIVE_UP:
            if (values.size() != 3)
                fatal("MSG_STATE usage: %d/player_id/game_id", type);
            MsgState msg_state;
            msg_state.msg_type = type;
            msg_state.player_id = htonl(values[1]);
            msg_state.game_id = htonl(values[2]);
            packet.resize(sizeof(msg_state));
            std::memcpy(packet.data(), &msg_state, sizeof(msg_state));
            break;
        case MessageType::MOVE_1:
        case MessageType::MOVE_2:
            if (values.size() != 4)
                fatal("MSG_MOVE usage: %d/player_id/game_id/pawn", type);
            MsgMove msg_move;
            msg_move.msg_type = type;
            msg_move.player_id = htonl(values[1]);
            msg_move.game_id = htonl(values[2]);
            msg_move.pawn = static_cast<uint8_t>(values[3]);
            packet.resize(sizeof(msg_move));
            std::memcpy(packet.data(), &msg_move, sizeof(msg_move));
            break;
        default:
            fatal("Invalid message type");
    }
    return packet;

}
void send_packet(int socket_fd, const std::vector<uint8_t>& packet) {
    ssize_t sent_bytes = send(socket_fd, packet.data(), packet.size(), 0);

    if (sent_bytes < 0) {
        syserr("send");
    }

    if (static_cast<size_t>(sent_bytes) != packet.size()) {
        fatal("Partial send: sent %zd out of %zu bytes", sent_bytes, packet.size());
    }
}

// Returns 1 if there is a pawn in a given index of a pawn_row and 0 otherwise.
bool is_pawn_standing(std::span<const uint8_t> pawn_row, uint8_t pawn_index) {
    uint8_t byte_index = pawn_index / 8;
    uint8_t bit_index = pawn_index % 8;

    return (pawn_row[byte_index] >> (7 - bit_index)) & 1;
}

// Prints the server response to the standard output.
void handle_server_response(const std::vector<uint8_t>& buffer, ssize_t rec_len) {
    if (rec_len < static_cast<ssize_t>(sizeof(ErrorMessage))) {
        fatal("Invalid server response: %zd bytes", rec_len);
    }
    uint8_t status = buffer[ERROR_MSG_PAYLOAD_SIZE];
    // Handle error message.
    if (status == 255) {
        if (rec_len < static_cast<ssize_t>(sizeof(ErrorMessage))) {
            fatal("Incomplete error message received");
        }
        ErrorMessage error_msg;
        std::memcpy(&error_msg, buffer.data(), sizeof(ErrorMessage));
        std::cerr << "Error: " << error_msg.error_index << ": " << error_msg.original_bytes <<
            std::endl;
        exit(0);
    }
    else {
        GameStateHeader header;
        std::memcpy(&header, buffer.data(), sizeof(GameStateHeader));
        uint32_t game_id = ntohl(header.game_id);
        uint32_t player_a = ntohl(header.player_a_id);
        uint32_t player_b = ntohl(header.player_b_id);

        size_t expected_bit_map_len = (header.max_pawn / 8) + 1;
        // It's okay to cast, since rec_len is guaranteed to be < sizeof(ErrorMessage).
        if (static_cast<size_t>(rec_len) != sizeof(GameStateHeader) + expected_bit_map_len) {
            fatal("Invalid server response: %zd bytes", rec_len);
        }
        std::span<const uint8_t> pawn_row(buffer.data() + sizeof(GameStateHeader),
            expected_bit_map_len);
        std::cout << "Game " << game_id << " started with players " << player_a
        << " and " << player_b << "\n";
        std::cout << "Status ";
        switch(header.status) {
            case 0: std::cout << "WAITING_FOR_OPPONENT\n"; break;
            case 1: std::cout << "TURN_A\n"; break;
            case 2: std::cout << "TURN_B\n"; break;
            case 3: std::cout << "WIN_A\n"; break;
            case 4: std::cout << "WIN_B\n"; break;
            default: std::cout << "UNKNOWN (" << static_cast<int>(header.status) << ")\n";
        }
        std::cout << "Bit map: ";
        for (uint8_t i = 0; i <= header.max_pawn; ++i) {
            if (is_pawn_standing(pawn_row, i))
                std::cout << "1 ";
            else
                std::cout << "0 ";
        }
    }
    std::cout << "\n";

}

int main(int argc, char *argv[]) {

    ClientConfig config = parse_arguments(argc, argv);

    struct addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    struct addrinfo *res;
    std::string port_str = std::to_string(*config.port);
    if (getaddrinfo(config.address->c_str(), port_str.c_str(), &hints, &res) != 0)
        syserr("getaddrinfo");
    sockaddr_in server_address = *reinterpret_cast<sockaddr_in*>(res->ai_addr);
    freeaddrinfo(res);
    const Socket socket_fd(AF_INET, SOCK_DGRAM, 0);
    // Adding timeout.
    struct timeval tv;
    tv.tv_sec = *config.timeout;
    tv.tv_usec = 0;
    if (setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0)
        syserr("setsockopt");

    if (connect(socket_fd, reinterpret_cast<sockaddr*>(&server_address),
        sizeof(server_address)) < 0) {
        syserr("connect");
    }

    const std::vector<uint8_t> packet = build_network_packet(*config.message);

    auto start_time = std::chrono::steady_clock::now();
    auto timeout_duration = std::chrono::seconds(*config.timeout);
    std::vector<uint8_t> buffer(MAX_UDP_PACKET_SIZE);

    while (true) {
        send_packet(socket_fd, packet);
        ssize_t rec_len = recv(socket_fd, buffer.data(), buffer.size(), 0);
        if (rec_len >= 0) {
            handle_server_response(buffer, rec_len);
            return 0;
        }
        auto now = std::chrono::steady_clock::now();
        auto elapsed_time = now - start_time;
        if (elapsed_time > timeout_duration) {
            std::cout << "Timeout reached\n";
            return 0;
        }
    }
    return 0;
}