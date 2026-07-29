package main

import (
	"encoding/binary"
	"flag"
	"fmt"
	"image/color"
	"io"
	"math"
	"net"
	"os"
	"sync"
	"time"

	"github.com/hajimehoshi/ebiten/v2"
	"github.com/hajimehoshi/ebiten/v2/ebitenutil"
	"github.com/hajimehoshi/ebiten/v2/vector"
)

// 입력 키 비트 (서버 Player::Update 와 동일)
//
//	0x01 up / 0x02 down / 0x04 left / 0x08 right
const (
	keyUp    = 1 << 0
	keyDown  = 1 << 1
	keyLeft  = 1 << 2
	keyRight = 1 << 3
)

// PacketId — 반드시 서버 cppServer/Protocol.h 의 enum 과 값이 일치해야 한다.
const (
	pktConnect      = 0
	pktDisconnect   = 1
	pktJoin         = 2
	pktLeave        = 3
	pktMovePlayer   = 4
	pktMoveBullet   = 5
	pktAttack       = 6
	pktRemovePlayer = 7
	pktRemoveBullet = 8
	pktHidePlayer   = 9
	pktHideBullet   = 10
	pktChat         = 11
	pktMapSnapshot  = 12
	pktDestroy      = 13
	pktObserve      = 14
	pktEnd          = 15
	pktRemoveItem   = 16
	pktPlayerStats  = 17
)

const (
	screenW = 720
	screenH = 720

	gridSize     = 100 // 서버 SpatialGrid 셀 크기와 동일 (내 주변 그리드 시각화)
	visionRadius = 280 // 전장의 안개: 이 반경 밖은 검게
)

type PlayerView struct{ x, y float32 }
type BulletView struct{ x, y float32 }
type Wall struct{ x, y, size float32 } // 월드 좌표 기준 좌상단 + 한 변 길이

type ItemView struct {
	itemType int32
	x, y     float32
}

type Game struct {
	conn net.Conn

	mu             sync.Mutex
	players        map[int32]*PlayerView
	bullets        map[int32]*BulletView
	walls          []Wall
	items          []ItemView
	cellSize       float32
	worldW, worldH float32         // 맵 필드의 월드 크기 (width*cellSize)
	wallSet        map[[2]int]bool // 벽 셀 집합 (그림자캐스팅 시야 계산용)
	cols, rows     int             // 맵 셀 개수

	// 내 스탯 (서버가 pktPlayerStats 로 내려준 값). 우측 상단 스탯창에 표시.
	moveSpeed    float32 // 현재 이동 속도 (유닛/초)
	fireInterval float32 // 현재 사격 간격 (초). 사격 속도 = 1/fireInterval
	itemCounts   [4]int  // 타입별 획득 개수 (1=사격, 2=이동)

	myID         int32
	spectateID   int32   // 관전 중 따라갈 대상 슬롯 id (없으면 -1)
	matchOver    bool    // 매치 종료 여부
	winnerID     int32   // 승자 슬롯 id (무승부면 -1)
	camX, camY   float32 // 카메라 좌상단(월드 좌표)
	lastKeys     byte
	prevMouse    bool
	disconnected bool
	errText      string

	fog *ebiten.Image // 중앙에 원형 구멍이 뚫린 검은 오버레이 (정적)
}

func (g *Game) Update() error {
	if g.disconnected {
		return nil
	}

	var keys byte
	if ebiten.IsKeyPressed(ebiten.KeyW) {
		keys |= keyUp
	}
	if ebiten.IsKeyPressed(ebiten.KeyS) {
		keys |= keyDown
	}
	if ebiten.IsKeyPressed(ebiten.KeyA) {
		keys |= keyLeft
	}
	if ebiten.IsKeyPressed(ebiten.KeyD) {
		keys |= keyRight
	}
	if keys != g.lastKeys {
		g.sendMove(keys)
		g.lastKeys = keys
	}

	mouseDown := ebiten.IsMouseButtonPressed(ebiten.MouseButtonLeft)
	if mouseDown && !g.prevMouse {
		g.mu.Lock()
		_, alive := g.players[g.myID]
		spectating := !alive && g.spectateID >= 0
		g.mu.Unlock()
		if spectating {
			g.sendObserve() // 관전 중 클릭 = 다음 대상으로 순환
		} else {
			g.sendAttack()
		}
	}
	g.prevMouse = mouseDown
	return nil
}

func (g *Game) Draw(screen *ebiten.Image) {
	// 필드 바깥은 검정. 맵 필드만 밝은 배경으로 덮는다.
	screen.Fill(color.RGBA{16, 18, 22, 255})

	g.mu.Lock()
	// 시점 대상: 내가 살아있으면 나, 죽어서 관전 중이면 관전 대상.
	followID := g.myID
	if _, ok := g.players[g.myID]; !ok && g.spectateID >= 0 {
		followID = g.spectateID
	}

	// 카메라: 시점 대상을 화면 중앙에 둔다. 위치를 모르면 직전 값을 유지.
	if me, ok := g.players[followID]; ok {
		g.camX = me.x - screenW/2
		g.camY = me.y - screenH/2
	}
	camX, camY := g.camX, g.camY
	worldW, worldH := g.worldW, g.worldH
	cellSize := g.cellSize

	// 그림자캐스팅으로 시점 대상의 시야에 들어오는 셀 집합 계산 (벽 뒤는 제외)
	var visible map[[2]int]bool
	if cellSize > 0 {
		if me, ok := g.players[followID]; ok {
			mcx := int(math.Floor(float64(me.x / cellSize)))
			mcy := int(math.Floor(float64(me.y / cellSize)))
			radCells := int(float32(visionRadius)/cellSize) + 1
			visible = g.computeVisible(mcx, mcy, radCells)
		}
	}

	// 맵 필드(밝은 배경). 스냅샷을 아직 못 받았으면 전체를 필드로 취급.
	if worldW > 0 && worldH > 0 {
		vector.DrawFilledRect(screen, -camX, -camY, worldW, worldH, color.RGBA{245, 246, 248, 255}, false)
	} else {
		screen.Fill(color.RGBA{245, 246, 248, 255})
	}

	drawGrid(screen, camX, camY, worldW, worldH)

	// 벽 (정적 지형). 화면에 걸치는 것만 그린다.
	wallColor := color.RGBA{90, 98, 110, 255}
	for _, w := range g.walls {
		sx, sy := w.x-camX, w.y-camY
		if sx+w.size < 0 || sy+w.size < 0 || sx > screenW || sy > screenH {
			continue
		}
		vector.DrawFilledRect(screen, sx, sy, w.size, w.size, wallColor, false)
	}

	for _, item := range g.items {
		sx, sy := item.x-camX, item.y-camY
		size := float32(18)
		if cellSize > 0 {
			size = cellSize * 0.35
		}
		if sx+size/2 < 0 || sy+size/2 < 0 || sx-size/2 > screenW || sy-size/2 > screenH {
			continue
		}
		vector.DrawFilledRect(screen, sx-size/2, sy-size/2, size, size, itemColor(item.itemType), false)
		vector.StrokeRect(screen, sx-size/2, sy-size/2, size, size, 2, color.RGBA{20, 24, 31, 255}, false)
	}

	for id, p := range g.players {
		sx, sy := p.x-camX, p.y-camY
		vector.DrawFilledCircle(screen, sx, sy, 13, color.RGBA{20, 24, 31, 255}, true)
		vector.DrawFilledCircle(screen, sx, sy, 10, colorFor(id), true)
		if id == followID {
			// 시점 대상(나 또는 관전 대상) 표시용 링
			vector.StrokeCircle(screen, sx, sy, 17, 2, color.RGBA{20, 24, 31, 255}, true)
		}
	}
	for _, b := range g.bullets {
		sx, sy := b.x-camX, b.y-camY
		vector.DrawFilledCircle(screen, sx, sy, 5, color.RGBA{20, 24, 31, 255}, true)
		vector.DrawFilledCircle(screen, sx, sy, 4, color.RGBA{250, 205, 70, 255}, true)
	}

	nPlayers, nBullets := len(g.players), len(g.bullets)
	myID := g.myID
	spectating := followID != g.myID
	disconnected, errText := g.disconnected, g.errText
	matchOver, winnerID := g.matchOver, g.winnerID
	moveSpeed, fireInterval := g.moveSpeed, g.fireInterval
	fireItems, moveItems := g.itemCounts[1], g.itemCounts[2]
	g.mu.Unlock()

	// 벽에 가려진(시야 밖) 셀을 어둡게 덮는다. (셀 그림자캐스팅 결과)
	if visible != nil {
		drawCellFog(screen, visible, camX, camY, cellSize)
	}

	// 전장의 안개: 중앙 원형만 남기고 나머지를 검게. (경계를 부드럽게 + 원형 시야 한계)
	screen.DrawImage(g.fog, nil)

	status := fmt.Sprintf("WASD 이동 | 좌클릭 사격 | myID: %d | players: %d | bullets: %d", myID, nPlayers, nBullets)
	if spectating {
		status = fmt.Sprintf("관전 중 (대상 id: %d) | 좌클릭: 다음 대상 | players: %d | bullets: %d", followID, nPlayers, nBullets)
	}
	if disconnected {
		status = "disconnected"
		if errText != "" {
			status += ": " + errText
		}
	}
	ebitenutil.DebugPrintAt(screen, status, 12, 12)
	if _, ok := hasMe(g, myID); !ok && !disconnected && !spectating && !matchOver {
		ebitenutil.DebugPrintAt(screen, "대기 중... 서버는 슬롯 4개가 모두 차야 매치가 시작됩니다", 12, 32)
	}

	// 우측 상단 스탯창
	drawStatPanel(screen, moveSpeed, fireInterval, fireItems, moveItems)

	// 매치 종료 배너
	if matchOver {
		vector.DrawFilledRect(screen, 0, screenH/2-40, screenW, 80, color.RGBA{0, 0, 0, 200}, false)
		var msg string
		switch {
		case winnerID < 0:
			msg = "매치 종료 — 무승부"
		case winnerID == myID:
			msg = "매치 종료 — 승리!"
		default:
			msg = fmt.Sprintf("매치 종료 — 승자 id: %d", winnerID)
		}
		ebitenutil.DebugPrintAt(screen, msg, screenW/2-len(msg)*3, screenH/2-4)
	}
}

func hasMe(g *Game, myID int32) (struct{}, bool) {
	g.mu.Lock()
	_, ok := g.players[myID]
	g.mu.Unlock()
	return struct{}{}, ok
}

func (g *Game) Layout(int, int) (int, int) { return screenW, screenH }

// ---- 수신 ----

func (g *Game) receiveLoop() {
	header := make([]byte, 4)
	for {
		if _, err := io.ReadFull(g.conn, header); err != nil {
			g.setDisconnected(err)
			return
		}
		size := binary.LittleEndian.Uint16(header[0:2])
		id := binary.LittleEndian.Uint16(header[2:4])
		if size < 4 || size > 4096 {
			g.setDisconnected(fmt.Errorf("bad packet size %d", size))
			return
		}
		body := make([]byte, int(size)-4)
		if _, err := io.ReadFull(g.conn, body); err != nil {
			g.setDisconnected(err)
			return
		}
		g.handlePacket(id, body)
	}
}

func (g *Game) handlePacket(id uint16, body []byte) {
	g.mu.Lock()
	defer g.mu.Unlock()

	switch id {
	case pktConnect:
		// 서버가 접속 클라에게 자기 id 를 알려주는 경우(현재 서버는 안 보냄, 향후 대비)
		if pid, ok := readID(body); ok && g.myID < 0 {
			g.myID = pid
		}

	case pktJoin:
		// IdPacket{ int id }. 접속 직후 받는 첫 Join = 내 슬롯 idx.
		if pid, ok := readID(body); ok {
			if g.myID < 0 {
				g.myID = pid
			}
			if _, exists := g.players[pid]; !exists {
				g.players[pid] = &PlayerView{}
			}
		}

	case pktLeave:
		if pid, ok := readID(body); ok {
			delete(g.players, pid)
		}

	// 플레이어 위치 갱신 (Vec2Packet)
	case pktMovePlayer:
		if oid, x, y, ok := readVec2(body); ok {
			p := g.players[oid]
			if p == nil {
				p = &PlayerView{}
				g.players[oid] = p
			}
			p.x, p.y = x, y
		}

	// 플레이어 제거 (IdPacket)
	case pktRemovePlayer, pktHidePlayer:
		if pid, ok := readID(body); ok {
			delete(g.players, pid)
		}

	// 총알 위치 갱신 (Vec2Packet)
	case pktMoveBullet:
		if oid, x, y, ok := readVec2(body); ok {
			b := g.bullets[oid]
			if b == nil {
				b = &BulletView{}
				g.bullets[oid] = b
			}
			b.x, b.y = x, y
		}

	// 총알 제거 (IdPacket)
	case pktRemoveBullet, pktHideBullet:
		if bid, ok := readID(body); ok {
			delete(g.bullets, bid)
		}

	case pktAttack:
		if oid, x, y, ok := readVec2(body); ok {
			g.bullets[oid] = &BulletView{x: x, y: y}
		}

	case pktMapSnapshot:
		// body: cellSize(2) width(2) height(2) wallCount(2) 뒤에 wallCount×{x(2),y(2)}
		if len(body) < 8 {
			return
		}
		cellSize := float32(binary.LittleEndian.Uint16(body[0:2]))
		width := float32(binary.LittleEndian.Uint16(body[2:4]))
		height := float32(binary.LittleEndian.Uint16(body[4:6]))
		wallCount := int(binary.LittleEndian.Uint16(body[6:8]))
		off := 8
		walls := make([]Wall, 0, wallCount)
		for i := 0; i < wallCount && off+4 <= len(body); i++ {
			cx := float32(binary.LittleEndian.Uint16(body[off : off+2]))
			cy := float32(binary.LittleEndian.Uint16(body[off+2 : off+4]))
			off += 4
			walls = append(walls, Wall{x: cx * cellSize, y: cy * cellSize, size: cellSize})
		}
		g.walls = walls
		g.items = nil
		g.cellSize = cellSize
		g.worldW = width * cellSize
		g.worldH = height * cellSize
		g.cols = int(width)
		g.rows = int(height)
		g.wallSet = make(map[[2]int]bool, len(walls))
		for _, w := range walls {
			g.wallSet[[2]int{int(w.x / cellSize), int(w.y / cellSize)}] = true
		}

	case pktDestroy:
		// Vec2Packet: id(4) x(4) y(4). 서버는 파괴 지점의 월드 좌표(x,y)를 보냄.
		if dropID, x, y, ok := readVec2(body); ok {
			g.removeWallAt(x, y)
			if dropID > 0 {
				g.addItemDrop(dropID, x, y)
			}
		}

	case pktRemoveItem:
		// Vec2Packet: id(4, 미사용) x(4) y(4). 플레이어가 주운 아이템의 원본 좌표.
		// 좌표는 셀 중심으로 스냅돼 저장되므로 셀 단위로 매칭해 제거한다.
		if _, x, y, ok := readVec2(body); ok {
			g.removeItemAt(x, y)
		}

	case pktPlayerStats:
		// Vec2Packet: id=먹은 아이템 타입, x=현재 이동속도, y=현재 사격간격.
		// 서버가 "나"에게만 보낸다. 타입별 개수를 세고 현재 스탯을 갱신한다.
		if t, speed, interval, ok := readVec2(body); ok {
			if t >= 0 && int(t) < len(g.itemCounts) {
				g.itemCounts[t]++
			}
			g.moveSpeed = speed
			g.fireInterval = interval
		}

	case pktObserve:
		// IdPacket: 내가 죽어 관전자가 되었을 때(또는 대상 순환 시) 따라갈 대상 슬롯 id
		if pid, ok := readID(body); ok {
			g.spectateID = pid
		}

	case pktEnd:
		// IdPacket: 매치 종료. id = 승자 슬롯(무승부면 -1)
		if pid, ok := readID(body); ok {
			g.matchOver = true
			g.winnerID = pid
		}
	}
}

// 월드 좌표가 속한 셀의 벽을 제거한다. (호출자가 g.mu 를 잡고 있어야 함)
func (g *Game) removeWallAt(wx, wy float32) {
	if g.cellSize <= 0 {
		return
	}
	cx := int(wx / g.cellSize)
	cy := int(wy / g.cellSize)
	delete(g.wallSet, [2]int{cx, cy})
	for i, w := range g.walls {
		if int(w.x/g.cellSize) == cx && int(w.y/g.cellSize) == cy {
			g.walls = append(g.walls[:i], g.walls[i+1:]...)
			return
		}
	}
}

// 월드 좌표가 속한 셀의 아이템을 제거한다. (호출자가 g.mu 를 잡고 있어야 함)
// 서버는 원본 좌표를 보내지만 클라의 아이템은 셀 중심으로 저장돼 있으므로
// 벽 제거와 동일하게 셀 인덱스로 비교한다.
func (g *Game) removeItemAt(wx, wy float32) {
	if g.cellSize <= 0 {
		return
	}
	cx := int(wx / g.cellSize)
	cy := int(wy / g.cellSize)
	for i, it := range g.items {
		if int(it.x/g.cellSize) == cx && int(it.y/g.cellSize) == cy {
			g.items = append(g.items[:i], g.items[i+1:]...)
			return
		}
	}
}

// IdPacket body: int32 id (4바이트)
func (g *Game) addItemDrop(itemType int32, wx, wy float32) {
	if g.cellSize <= 0 {
		g.items = append(g.items, ItemView{itemType: itemType, x: wx, y: wy})
		return
	}

	cx := int(wx / g.cellSize)
	cy := int(wy / g.cellSize)
	x := float32(cx)*g.cellSize + g.cellSize/2
	y := float32(cy)*g.cellSize + g.cellSize/2
	g.items = append(g.items, ItemView{itemType: itemType, x: x, y: y})
}

func readID(body []byte) (int32, bool) {
	if len(body) < 4 {
		return 0, false
	}
	return int32(binary.LittleEndian.Uint32(body[0:4])), true
}

// Vec2Packet body: int32 id, float32 x, float32 y (12바이트)
func readVec2(body []byte) (id int32, x, y float32, ok bool) {
	if len(body) < 12 {
		return 0, 0, 0, false
	}
	id = int32(binary.LittleEndian.Uint32(body[0:4]))
	x = math.Float32frombits(binary.LittleEndian.Uint32(body[4:8]))
	y = math.Float32frombits(binary.LittleEndian.Uint32(body[8:12]))
	return id, x, y, true
}

// ---- 송신 ----

func (g *Game) sendMove(keys byte) {
	g.writePacket(pktMovePlayer, []byte{keys})
}

// 관전 대상 순환 요청. 서버가 다음 Playing 슬롯을 골라 Observe 로 되돌려준다.
func (g *Game) sendObserve() {
	g.writePacket(pktObserve, nil)
}

// 마우스 클릭 지점을 월드 좌표로 변환해 사격 방향으로 보낸다(카메라 오프셋 보정).
func (g *Game) sendAttack() {
	mx, my := ebiten.CursorPosition()
	g.mu.Lock()
	worldX := float32(mx) + g.camX
	worldY := float32(my) + g.camY
	g.mu.Unlock()

	body := make([]byte, 8)
	binary.LittleEndian.PutUint32(body[0:4], math.Float32bits(worldX))
	binary.LittleEndian.PutUint32(body[4:8], math.Float32bits(worldY))
	g.writePacket(pktAttack, body)
}

func (g *Game) writePacket(id uint16, body []byte) {
	buf := make([]byte, 4+len(body))
	binary.LittleEndian.PutUint16(buf[0:2], uint16(len(buf)))
	binary.LittleEndian.PutUint16(buf[2:4], id)
	copy(buf[4:], body)
	_, _ = g.conn.Write(buf)
}

func (g *Game) setDisconnected(err error) {
	g.mu.Lock()
	defer g.mu.Unlock()
	g.disconnected = true
	if err != nil {
		g.errText = err.Error()
	}
}

// ---- 시야(FOV): 재귀 그림자캐스팅 ----

// 8개 옥탄트 변환 계수 (Björn Bergström 알고리즘)
var fovMult = [4][8]int{
	{1, 0, 0, -1, -1, 0, 0, 1},
	{0, 1, -1, 0, 0, -1, 1, 0},
	{0, 1, 1, 0, 0, -1, -1, 0},
	{1, 0, 0, 1, -1, 0, 0, -1},
}

// 셀 (cx,cy) 가 벽인지. 맵 밖은 벽으로 취급해 시야를 막는다.
func (g *Game) wallAt(cx, cy int) bool {
	if g.cols > 0 && (cx < 0 || cy < 0 || cx >= g.cols || cy >= g.rows) {
		return true
	}
	return g.wallSet[[2]int{cx, cy}]
}

// (cx,cy) 를 중심으로 radius(셀 단위) 안에서 벽에 막히지 않고 보이는 셀 집합을 구한다.
// g.mu 를 잡은 상태에서 호출해야 한다 (wallSet / cols / rows 를 읽음).
func (g *Game) computeVisible(cx, cy, radius int) map[[2]int]bool {
	visible := map[[2]int]bool{{cx, cy}: true}
	for oct := 0; oct < 8; oct++ {
		g.castLight(cx, cy, 1, 1.0, 0.0, radius,
			fovMult[0][oct], fovMult[1][oct], fovMult[2][oct], fovMult[3][oct], visible)
	}

	// 대각선 벽 틈으로 시선이 새는 것 제거.
	// 바닥 셀은 플레이어 쪽 직교 이웃 중 열려있고 보이는 셀이 하나라도 있어야 유효.
	// (판정은 삭제 전 원본 집합 기준으로 해 순서 의존성을 없앤다)
	var toRemove [][2]int
	for cell := range visible {
		x, y := cell[0], cell[1]
		if (x == cx && y == cy) || g.wallAt(x, y) {
			continue // 자기 셀과 벽면은 그대로 둔다
		}
		open := false
		if x != cx { // x축으로 플레이어에 한 칸 가까운 이웃
			nx := x + 1
			if cx < x {
				nx = x - 1
			}
			if visible[[2]int{nx, y}] && !g.wallAt(nx, y) {
				open = true
			}
		}
		if !open && y != cy { // y축으로 플레이어에 한 칸 가까운 이웃
			ny := y + 1
			if cy < y {
				ny = y - 1
			}
			if visible[[2]int{x, ny}] && !g.wallAt(x, ny) {
				open = true
			}
		}
		if !open {
			toRemove = append(toRemove, cell)
		}
	}
	for _, c := range toRemove {
		delete(visible, c)
	}
	return visible
}

func (g *Game) castLight(cx, cy, row int, start, end float64, radius, xx, xy, yx, yy int, visible map[[2]int]bool) {
	if start < end {
		return
	}
	var newStart float64
	for j := row; j <= radius; j++ {
		dx, dy := -j-1, -j
		blocked := false
		for dx <= 0 {
			dx++
			// dx,dy 를 실제 맵 좌표로 변환
			X, Y := cx+dx*xx+dy*xy, cy+dx*yx+dy*yy
			lSlope := (float64(dx) - 0.5) / (float64(dy) + 0.5)
			rSlope := (float64(dx) + 0.5) / (float64(dy) - 0.5)
			if start < rSlope {
				continue
			} else if end > lSlope {
				break
			}
			if dx*dx+dy*dy < radius*radius {
				visible[[2]int{X, Y}] = true
			}
			if blocked {
				if g.wallAt(X, Y) {
					newStart = rSlope
					continue
				}
				blocked = false
				start = newStart
			} else if g.wallAt(X, Y) && j < radius {
				// 벽을 만남: 이 뒤로는 자식 스캔에 위임
				blocked = true
				g.castLight(cx, cy, j+1, start, lSlope, radius, xx, xy, yx, yy, visible)
				newStart = rSlope
			}
		}
		if blocked {
			break
		}
	}
}

// 시야 밖 셀을 어둡게 덮는다. 화면에 걸치는 셀만 순회.
func drawCellFog(screen *ebiten.Image, visible map[[2]int]bool, camX, camY, cs float32) {
	dark := color.RGBA{12, 13, 16, 255}
	startCX := int(math.Floor(float64(camX / cs)))
	startCY := int(math.Floor(float64(camY / cs)))
	endCX := int(math.Floor(float64((camX + screenW) / cs)))
	endCY := int(math.Floor(float64((camY + screenH) / cs)))
	for cy := startCY; cy <= endCY; cy++ {
		for cx := startCX; cx <= endCX; cx++ {
			if visible[[2]int{cx, cy}] {
				continue
			}
			sx := float32(cx)*cs - camX
			sy := float32(cy)*cs - camY
			vector.DrawFilledRect(screen, sx, sy, cs, cs, dark, false)
		}
	}
}

// 우측 상단 스탯창: 현재 이동/사격 속도와 타입별 아이템 획득 수를 표시.
func drawStatPanel(screen *ebiten.Image, moveSpeed, fireInterval float32, fireItems, moveItems int) {
	const w, h = 200, 96
	x := float32(screenW - w - 12)
	y := float32(12)
	vector.DrawFilledRect(screen, x, y, w, h, color.RGBA{0, 0, 0, 170}, false)
	vector.StrokeRect(screen, x, y, w, h, 1, color.RGBA{90, 98, 110, 255}, false)

	fireRate := float32(0)
	if fireInterval > 0 {
		fireRate = 1.0 / fireInterval
	}
	tx := int(x) + 12
	ebitenutil.DebugPrintAt(screen, "── 스탯 ──", tx, int(y)+8)
	ebitenutil.DebugPrintAt(screen, fmt.Sprintf("이동 속도 : %.0f", moveSpeed), tx, int(y)+28)
	ebitenutil.DebugPrintAt(screen, fmt.Sprintf("사격 속도 : %.1f 발/초", fireRate), tx, int(y)+44)
	ebitenutil.DebugPrintAt(screen, fmt.Sprintf("사격 아이템 : %d개", fireItems), tx, int(y)+64)
	ebitenutil.DebugPrintAt(screen, fmt.Sprintf("이동 아이템 : %d개", moveItems), tx, int(y)+80)
}

// ---- 그리기 유틸 ----

// 카메라 오프셋에 맞춰 월드 격자선을 그린다. 격자는 맵 필드 안쪽만.
func drawGrid(screen *ebiten.Image, camX, camY, worldW, worldH float32) {
	gridColor := color.RGBA{225, 228, 234, 255}
	if worldW <= 0 || worldH <= 0 {
		worldW, worldH = screenW+camX, screenH+camY // 스냅샷 전: 화면 전체
	}

	// 세로선이 그려질 화면상 y 구간 (필드 세로 범위로 클램프)
	top, bottom := clampf(-camY, 0, screenH), clampf(worldH-camY, 0, screenH)
	startX := float32(math.Floor(float64(camX/gridSize))) * gridSize
	for x := startX; x < camX+screenW; x += gridSize {
		if x < 0 || x > worldW {
			continue
		}
		sx := x - camX
		vector.StrokeLine(screen, sx, top, sx, bottom, 1, gridColor, true)
	}

	// 가로선이 그려질 화면상 x 구간
	left, right := clampf(-camX, 0, screenW), clampf(worldW-camX, 0, screenW)
	startY := float32(math.Floor(float64(camY/gridSize))) * gridSize
	for y := startY; y < camY+screenH; y += gridSize {
		if y < 0 || y > worldH {
			continue
		}
		sy := y - camY
		vector.StrokeLine(screen, left, sy, right, sy, 1, gridColor, true)
	}
}

func clampf(v, lo, hi float32) float32 {
	if v < lo {
		return lo
	}
	if v > hi {
		return hi
	}
	return v
}

func colorFor(id int32) color.Color {
	palette := []color.Color{
		color.RGBA{230, 80, 80, 255},
		color.RGBA{70, 135, 230, 255},
		color.RGBA{65, 175, 105, 255},
		color.RGBA{205, 155, 45, 255},
	}
	return palette[int(id)&3]
}

// 화면 중앙에 원형 구멍이 뚫린 검은 오버레이. 가장자리는 부드럽게 페이드.
func itemColor(itemType int32) color.Color {
	switch itemType {
	case 1:
		return color.RGBA{230, 70, 70, 255}
	case 2:
		return color.RGBA{70, 130, 235, 255}
	case 3:
		return color.RGBA{245, 205, 65, 255}
	default:
		return color.RGBA{230, 230, 230, 255}
	}
}

func makeFog() *ebiten.Image {
	img := ebiten.NewImage(screenW, screenH)
	pix := make([]byte, screenW*screenH*4)
	cx, cy := float64(screenW)/2, float64(screenH)/2
	const fade = 60.0 // 경계 페이드 폭
	for y := 0; y < screenH; y++ {
		for x := 0; x < screenW; x++ {
			dx, dy := float64(x)-cx, float64(y)-cy
			d := math.Sqrt(dx*dx + dy*dy)
			var a float64
			switch {
			case d <= visionRadius-fade:
				a = 0
			case d >= visionRadius:
				a = 255
			default:
				a = (d - (visionRadius - fade)) / fade * 255
			}
			i := (y*screenW + x) * 4
			// 프리멀티플라이드 알파: 검정이라 RGB=0
			pix[i+3] = byte(a)
		}
	}
	img.WritePixels(pix)
	return img
}

func main() {
	addr := flag.String("addr", "127.0.0.1:5050", "server address")
	flag.Parse()

	conn, err := net.DialTimeout("tcp", *addr, 3*time.Second)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	defer conn.Close()

	game := &Game{
		conn:         conn,
		players:      make(map[int32]*PlayerView),
		bullets:      make(map[int32]*BulletView),
		myID:         -1,
		spectateID:   -1,
		winnerID:     -1,
		moveSpeed:    300.0, // 서버 Player 초기값과 동일
		fireInterval: 0.25,  // 서버 Player 초기값과 동일
		fog:          makeFog(),
	}

	// 서버는 accept 시점에 자동 Join 을 처리하므로 클라에서 Join 을 또 보내지 않는다.
	go game.receiveLoop()

	ebiten.SetWindowSize(screenW, screenH)
	ebiten.SetWindowTitle("cppServer test client")

	if err := ebiten.RunGame(game); err != nil {
		panic(err)
	}
}
