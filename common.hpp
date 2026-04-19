#ifndef COMMON_HPP
#define COMMON_HPP

#include <cstdint>
#include <string_view>
#include <sys/socket.h>
#include <netinet/in.h>
#include <vector>

constexpr int MAX_UDP_PACKET_SIZE = 65535;
constexpr size_t ERROR_MSG_PAYLOAD_SIZE = 12;

// Wrapper for a socket, we use RAII.
class Socket {
    int fd;
public:
    Socket(int domain, int type, int protocol);
    ~Socket();

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    operator int() const { return fd; }

    void set_timeout(int seconds) const;
};


enum class GameStatus : uint8_t {
    WAITING_FOR_OPPONENT = 0,
    TURN_A = 1,
    TURN_B = 2,
    WIN_A = 3,
    WIN_B = 4
};

// Header for the messages sent by the server to clients any byte after the header
// belongs to the bitmap max_pawn.
struct __attribute__((packed)) GameStateHeader {
    uint32_t game_id;
    uint32_t player_a_id;
    uint32_t player_b_id;
    uint8_t status;
    uint8_t max_pawn;
};

enum class MessageType : uint8_t {
    JOIN = 0,
    MOVE_1 = 1,
    MOVE_2 = 2,
    KEEP_ALIVE = 3,
    GIVE_UP = 4
};

struct Player {
    uint32_t id;
    struct sockaddr_in address;
};

struct MsgJoin {
    MessageType msg_type;
    uint32_t player_id;
}__attribute__((packed));

struct MsgMove {
    MessageType msg_type;
    uint32_t player_id;
    uint32_t game_id;
    uint8_t pawn;
}__attribute__((packed));

struct  MsgState {
    MessageType msg_type;
    uint32_t player_id;
    uint32_t game_id;
}__attribute__((packed));

struct   ErrorMessage {
    uint8_t original_bytes[ERROR_MSG_PAYLOAD_SIZE];
    uint8_t status = 255;
    uint8_t error_index;
}__attribute__((packed));


uint16_t parse_port(std::string_view str);
int parse_timeout (const std::string& str);
std::string parse_address (const std::string& str);

#endif