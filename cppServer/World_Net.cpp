#include"World.h"

void World::HandlePacket(RecvPacket& packet) {
    switch (packet.id)
    {
    case PacketId::MovePlayer:
        HandleMove(packet);
        break;
    case PacketId::Attack:
        HandleAttack(packet);
        break;
    case PacketId::Join:
        HandleJoin(packet);
        break;
    case PacketId::Leave:
        HandleLeave(packet);
        break;
    case PacketId::Observe:
        HandleObserve(packet);
        break;
    default:
        break;
    }
}

bool World::CanJoin() const {
    for (int i = 0; i < MAX_PLAYERS; ++i)
        if (slots[i].state == SlotState::Empty) return true;
    return false;
}

void World::HandleMove(RecvPacket& packet) {
    if (packet.body.size() < 1) return;
    unsigned char keys = static_cast<unsigned char>(packet.body[0]);
    int idx = FindSlotBySession(packet.sessionIndex);
    if (idx < 0 || slots[idx].state != SlotState::Playing) return;

    slots[idx].player.SetKeys(keys);
}

void World::HandleAttack(RecvPacket& packet) {
    if (packet.body.size() < sizeof(float) * 2) return;
    int idx = FindSlotBySession(packet.sessionIndex);
    if (idx < 0 || slots[idx].state != SlotState::Playing) return;
    if (!slots[idx].player.CanFire()) return;

    float targetX, targetY;
    memcpy(&targetX, packet.body.data(), sizeof(float));
    memcpy(&targetY, packet.body.data() + sizeof(float), sizeof(float));

    float px = slots[idx].player.GetX();
    float py = slots[idx].player.GetY();
    float dx = targetX - px;
    float dy = targetY - py;
    float len = std::sqrt(dx * dx + dy * dy);
    if (len == 0.0f) return;
    dx /= len;
    dy /= len;

    for (int i = 0; i < MAX_BULLETS; i++) {
        if (bullets[i].IsActive()) continue;
        bullets[i].Fire(px, py, dx, dy);
        slots[idx].player.StartFireCooldown();
        break;
    }
}

void World::HandleJoin(RecvPacket& packet) {
    int idx = FindEmptySlot();
    if (idx < 0) return;

    slots[idx].sessionIndex = packet.sessionIndex;
    slots[idx].state = SlotState::Waiting;

    IdPacket p;
    p.h.size = sizeof(IdPacket);
    p.h.id = static_cast<unsigned short>(PacketId::Join);
    p.id = idx;

    Broadcast((const char*)&p, p.h.size);
    TryStartMatch();
}

void World::HandleLeave(RecvPacket& packet) {
    int idx = FindSlotBySession(packet.sessionIndex);
    if (idx < 0) return;
    std::cout << "LEAVE" << idx << "\n";
    slots[idx].state = SlotState::Empty;
    slots[idx].sessionIndex = -1;

    RemovePlayer(idx);

    IdPacket p;
    p.h.size = sizeof(IdPacket);
    p.h.id = static_cast<unsigned short>(PacketId::Leave);
    p.id = idx;

    Broadcast((const char*)&p, p.h.size);
}

void World::HandleObserve(RecvPacket& packet) {
    int idx = FindSlotBySession(packet.sessionIndex);
    if (idx < 0) return;
    if (slots[idx].state != SlotState::Observing) return; // 관전 중일 때만 대상 순환

    int target = FindNextPlayingSlot(slots[idx].target);
    if (target < 0) return; // 순환할 다른 Playing 대상이 없음
    ObservePlayer(idx, target);
}

void World::TryStartMatch() {
    if (running) return;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (slots[i].state == SlotState::Empty) return;
    }
    for (int i = 0; i < MAX_PLAYERS; i++) {
        slots[i].state = SlotState::Playing;
    }

    std::cout << "!!!!!!!START!!!!!"<< "\n";
    running = true;
    BroadcastMapSnapshot();
}

void World::BroadcastMapSnapshot() {
    const auto& cells = map.GetCells();
    int mw = map.GetWidthCells();

    std::vector<WallPos> walls;
    for (int i = 0; i < (int)cells.size(); i++)
        if (cells[i] != 0)
            walls.push_back({ (unsigned short)(i % mw), (unsigned short)(i / mw) });

    unsigned short size = sizeof(MapSnapHeader) + (unsigned short)(walls.size() * sizeof(WallPos));
    std::vector<char> buf(size);

    auto* hdr = reinterpret_cast<MapSnapHeader*>(buf.data()); // ���� ����ȯ���� ��Ŷ ���·� ����
	hdr->h.size = size;
    hdr->h.id = (unsigned short)PacketId::MapSnapshot;
	hdr->cellSize = (unsigned short)Map::CELL_SIZE;
    hdr->width = (unsigned short)map.GetWidthCells();
    hdr->height = (unsigned short)map.GetHeightCells();
    hdr->wallCount = (unsigned short)walls.size();
    memcpy(buf.data() + sizeof(MapSnapHeader), walls.data(), walls.size() * sizeof(WallPos));

	Broadcast(buf.data(), size);
}



void World::Broadcast(const char* packet, int len) {
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        if (slots[i].state == SlotState::Empty) continue;
        int idx = slots[i].sessionIndex;
        queueSend(&g_sessions[idx], packet, len);   // 버퍼에만 쌓음 (전송은 틱 끝에)
    }
}

void World::SendTo(int idx, const char* packet, int len) {
    if (slots[idx].state == SlotState::Empty) return;
    int sessionIndex = slots[idx].sessionIndex;
    queueSend(&g_sessions[sessionIndex], packet, len);
}