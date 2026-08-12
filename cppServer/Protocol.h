#pragma once
#include<vector>

#pragma pack(push, 1)
struct PacketHeader { unsigned short size; unsigned short id; };

struct WallPos {
    unsigned short x, y;
};

struct MapSnapHeader {
    PacketHeader h;
    unsigned short cellSize;
    unsigned short width;
    unsigned short height;
    unsigned short wallCount;
};

struct IdPacket {
    PacketHeader h;
    int id;
};

struct Vec2Packet {
    PacketHeader h;
    int id;
    float x;
    float y;
};

#pragma pack(pop)

enum class AuthResult : unsigned short { 
	Ok, IdNotFound, WrongPassword, DuplicateId, AlreadyLoggedIn 
};
enum class PacketId : unsigned short {
    Connect,
    Disconnect,
    Join,
    Leave,
    MovePlayer,
    MoveBullet,
    Attack,
    RemovePlayer,
    RemoveBullet,
    HidePlayer,
    HideBullet,
    Chat,
    MapSnapshot,
    Destroy,
    Observe,
    End,
    RemoveItem,
    PlayerStats,
};

const int HEADER_SIZE = 4;

struct RecvPacket {
    int sessionIndex;
    PacketId id;
    std::vector<char> body;
};

inline IdPacket MakeIdPacket(PacketId packetId, int id) {
    IdPacket p;
    p.h.size = sizeof(IdPacket);
    p.h.id = static_cast<unsigned short>(packetId);
    p.id = id;
    return p;
}

inline Vec2Packet MakeVec2Packet(PacketId packetId, int id, float x, float y) {
    Vec2Packet p;
    p.h.size = sizeof(Vec2Packet);
    p.h.id = static_cast<unsigned short>(packetId);
    p.id = id;
    p.x = x;
    p.y = y;
    return p;
}