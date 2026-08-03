// 헤드리스 부하 테스트 봇.
// 렌더링(ebiten) 없이 서버 프로토콜만 흉내내어, 여러 개의 가상 플레이어를
// goroutine 으로 띄워 서버에 부하를 준다.
//
//	go run ./bot -addr 127.0.0.1:5050 -n 4
//	go run ./bot -n 4 -fire
package main

import (
	"encoding/binary"
	"flag"
	"io"
	"log"
	"math"
	"math/rand"
	"net"
	"sync/atomic"
	"time"
)

// 패킷 ID — 서버 cppServer/Protocol.h 의 enum 과 값이 반드시 일치해야 한다.
const (
	pktMovePlayer = 4
	pktAttack     = 6
)

// 이동 키 비트 — 서버 Player::Update 와 동일.
const (
	keyUp    = 1 << 0
	keyDown  = 1 << 1
	keyLeft  = 1 << 2
	keyRight = 1 << 3
)

// 사격 목표 좌표 범위(월드 유닛). 서버 맵 50셀 × CELL_SIZE 50 = 2500.
// 방향 계산에만 쓰이므로 대략적이면 충분하다.
const worldSize = 2500

var connected int64 // 현재 연결에 성공해 살아있는 봇 수 (통계용)

func main() {
	addr := flag.String("addr", "localhost:5050", "서버 주소")
	n := flag.Int("n", 4, "봇 개수")
	fire := flag.Bool("fire", false, "이동뿐 아니라 사격도 할지")
	malRatio := flag.Float64("mal", 0, "악성 봇 비율 [0,1]. 완성되지 않는 거대 패킷을 흘려보내 수신 버퍼를 압박한다")
	flag.Parse()

	malN := 0
	for i := 0; i < *n; i++ {
		if rand.Float64() < *malRatio {
			malN++
			go runMaliciousBot(*addr)
		} else {
			go runBot(*addr, *fire)
		}
		time.Sleep(5 * time.Millisecond) // 접속을 살짝 분산 (동시 폭주 완화)
	}
	log.Printf("봇 %d개 기동 (악성 %d개)", *n, malN)

	// 1초마다 연결 상태 출력. 종료는 Ctrl+C.
	for range time.Tick(time.Second) {
		log.Printf("connected = %d / %d", atomic.LoadInt64(&connected), *n)
	}
}

// runBot 은 봇 하나의 수명 전체를 담당한다.
// 연결 → 수신 goroutine 분리 → 주기적으로 이동/사격 패킷 송신.
func runBot(addr string, fire bool) {
	conn, err := net.Dial("tcp", addr)
	if err != nil {
		log.Println("dial:", err)
		return
	}
	defer conn.Close()

	atomic.AddInt64(&connected, 1)
	defer atomic.AddInt64(&connected, -1)

	go readLoop(conn) // 수신은 별도 goroutine 이 전담

	moveTick := time.NewTicker(500 * time.Millisecond) // 이동 방향 전환 주기
	defer moveTick.Stop()
	fireTick := time.NewTicker(250 * time.Millisecond) // 사격 주기 (서버 쿨다운 0.25s)
	defer fireTick.Stop()

	var lastKeys byte = 0xFF // 첫 전송을 강제하기 위한 불가능한 초기값
	for {
		select {
		case <-moveTick.C:
			// 실제 클라처럼 키가 바뀔 때만 이동 패킷을 보낸다.
			if k := randomKeys(); k != lastKeys {
				if writePacket(conn, pktMovePlayer, []byte{k}) != nil {
					return // 연결 끊김
				}
				lastKeys = k
			}
		case <-fireTick.C:
			if fire {
				sendFire(conn)
			}
		}
	}
}

// runMaliciousBot 은 "의미를 알 수 없는 악성 패킷"을 보내는 봇이다.
// 헤더에 최대 크기(65535)를 적어 거대한 프레임을 예고한 뒤, 본문을 조금씩
// 흘려보내되 절대 프레임을 완성하지 않는다. 서버 파싱 루프는
// GetUsedSize() < header.size 조건에서 계속 대기하므로, 이 바이트들은
// 소비되지 못하고 수신 버퍼에 그대로 쌓인다.
//
//   - 링버퍼(고정 4096): 4096을 넘을 수 없어 곧 가득 참 → 스스로 세션을 끊어
//     메모리를 지켜낸다.
//   - 가변버퍼: 프레임을 담으려 계속 grow → 세션마다 최대 ~64KB가 눌러앉는다.
func runMaliciousBot(addr string) {
	conn, err := net.Dial("tcp", addr)
	if err != nil {
		log.Println("dial(mal):", err)
		return
	}
	defer conn.Close()

	atomic.AddInt64(&connected, 1)
	defer atomic.AddInt64(&connected, -1)

	go readLoop(conn) // 서버 송신 버퍼가 변수가 되지 않도록 수신은 계속 비워준다

	// 최대 크기를 주장하는 헤더. id 는 존재하지 않는 값이라 의미가 없다.
	header := make([]byte, 4)
	binary.LittleEndian.PutUint16(header[0:2], 65535)  // size = 프레임 전체 길이(최대)
	binary.LittleEndian.PutUint16(header[2:4], 0xFFFF) // id = 의미 없는 값
	if _, err := conn.Write(header); err != nil {
		return
	}

	// 본문을 야금야금 흘려보낸다. total(지금까지 보낸 바이트)이 65535에
	// 닿으면 프레임이 완성되어 서버가 소비해버리므로, 그 직전까지만 채우고 멈춘다.
	chunk := make([]byte, 256)
	total := len(header)
	tick := time.NewTicker(50 * time.Millisecond)
	defer tick.Stop()
	for range tick.C {
		if total+len(chunk) >= 65535 {
			continue // 프레임을 완성시키지 않고 버퍼에 눌러앉힌다
		}
		if _, err := conn.Write(chunk); err != nil {
			return // 서버가 (링버퍼라면) 끊었다는 신호
		}
		total += len(chunk)
	}
}

// readLoop 는 수신 스트림을 계속 비운다.
// 프레임(헤더 4B + 본문)을 읽어 그대로 버린다. 내용은 부하 테스트에 필요 없지만,
// 소켓을 꾸준히 읽어주지 않으면 서버의 송신 버퍼가 막혀 정상 클라와 달라진다.
func readLoop(conn net.Conn) {
	header := make([]byte, 4)
	body := make([]byte, 4096) // 서버 규약상 패킷 최대 4096
	for {
		if _, err := io.ReadFull(conn, header); err != nil {
			return
		}
		size := binary.LittleEndian.Uint16(header[0:2]) // [0:2] = 전체 길이
		if size < 4 || size > 4096 {
			return // 프레이밍이 깨짐
		}
		if _, err := io.ReadFull(conn, body[:size-4]); err != nil {
			return
		}
	}
}

// randomKeys 는 WASD 이동 비트 중 하나를 무작위로 고른다. 가끔 정지(0)도 섞는다.
func randomKeys() byte {
	dirs := []byte{
		keyUp, keyDown, keyLeft, keyRight,
		keyUp | keyLeft, keyUp | keyRight,
		keyDown | keyLeft, keyDown | keyRight,
		0,
	}
	return dirs[rand.Intn(len(dirs))]
}

// sendFire 는 무작위 월드 좌표를 목표로 사격 패킷을 보낸다.
// 실제 방향(정규화)은 서버가 플레이어 위치 기준으로 계산한다.
func sendFire(conn net.Conn) {
	body := make([]byte, 8)
	binary.LittleEndian.PutUint32(body[0:4], math.Float32bits(rand.Float32()*worldSize))
	binary.LittleEndian.PutUint32(body[4:8], math.Float32bits(rand.Float32()*worldSize))
	writePacket(conn, pktAttack, body)
}

// writePacket 은 [size(2) | id(2) | body] 프레임으로 감싸 전송한다.
// 서버 클라이언트의 writePacket 규약과 동일 (리틀엔디언).
func writePacket(conn net.Conn, id uint16, body []byte) error {
	buf := make([]byte, 4+len(body))
	binary.LittleEndian.PutUint16(buf[0:2], uint16(len(buf)))
	binary.LittleEndian.PutUint16(buf[2:4], id)
	copy(buf[4:], body)
	_, err := conn.Write(buf)
	return err
}
