// Command relay is the Linux twin of relay/android (rete#498, contract
// protocol/RELAY-BLE.md): it opens the node's encrypted L2CAP channel on PSM 0x80
// through BlueZ and forwards each frame up as a UDP datagram from a socket kept per
// node port, and each datagram back on that socket as a frame down.
//
// It does not scan or pair: the node is named by its address, and the bond is made
// once beforehand with bluetoothctl (relay/linux/README.md). The kernel's LE connect
// waits for the node's advertising itself, so a node that turns its radio off between
// exchanges is simply reconnected when it comes back.
package main

import (
	"errors"
	"flag"
	"fmt"
	"log"
	"net"
	"strings"
	"sync"
	"time"

	"golang.org/x/sys/unix"
)

const (
	solBluetooth     = 274 // SOL_BLUETOOTH
	btSecurity       = 4   // BT_SECURITY
	btSecurityMedium = 2   // encrypted; the node demands BT_SECURITY_L2
	btRcvMTU         = 13  // BT_RCVMTU
	bdaddrLEPublic   = 1
	bdaddrLERandom   = 2
)

func main() {
	addr := flag.String("node", "", "the node's Bluetooth address, e.g. E0:44:A5:DC:EF:BB")
	random := flag.Bool("random", true, "the address is LE random static (an nRF's factory address is)")
	psm := flag.Uint("psm", 0x80, "the node's L2CAP PSM")
	mtu := flag.Uint("mtu", 4096, "the receive MTU offered to the node")
	flag.Parse()
	bd, err := parseAddr(*addr)
	if err != nil {
		log.Fatalf("-node: %v", err)
	}
	typ := uint8(bdaddrLEPublic)
	if *random {
		typ = bdaddrLERandom
	}
	for {
		t0 := time.Now()
		fd, err := dial(bd, typ, uint16(*psm), uint16(*mtu))
		if err != nil {
			log.Printf("connect: %v", err)
			time.Sleep(200 * time.Millisecond)
			continue
		}
		log.Printf("linked %s in %d ms", *addr, time.Since(t0).Milliseconds())
		up, down, err := (&session{fd: fd, udp: map[uint16]*net.UDPConn{}, hostText: map[string]string{}}).run()
		log.Printf("link down after %d up, %d down: %v", up, down, err)
	}
}

func parseAddr(s string) ([6]byte, error) {
	var a [6]byte
	p := strings.Split(s, ":")
	if len(p) != 6 {
		return a, fmt.Errorf("%q is not six octets", s)
	}
	for i, o := range p {
		if _, err := fmt.Sscanf(o, "%02x", &a[i]); err != nil {
			return a, fmt.Errorf("%q: %v", s, err)
		}
	}
	return a, nil
}

func dial(bd [6]byte, typ uint8, psm, mtu uint16) (int, error) {
	fd, err := unix.Socket(unix.AF_BLUETOOTH, unix.SOCK_SEQPACKET, unix.BTPROTO_L2CAP)
	if err != nil {
		return -1, err
	}
	fail := func(what string, err error) (int, error) { unix.Close(fd); return -1, fmt.Errorf("%s: %w", what, err) }
	if err := unix.Bind(fd, &unix.SockaddrL2{AddrType: bdaddrLEPublic}); err != nil { // any local LE adapter
		return fail("bind", err)
	}
	if err := unix.SetsockoptString(fd, solBluetooth, btSecurity, string([]byte{btSecurityMedium, 0})); err != nil {
		return fail("BT_SECURITY", err)
	}
	if err := unix.SetsockoptInt(fd, solBluetooth, btRcvMTU, int(mtu)); err != nil {
		log.Printf("BT_RCVMTU %d: %v (kept the default)", mtu, err)
	}
	if err := unix.Connect(fd, &unix.SockaddrL2{PSM: psm, Addr: bd, AddrType: typ}); err != nil {
		return fail("connect", err)
	}
	return fd, nil
}

// session is one connected channel: the reader here, one UDP receiver per node port.
type session struct {
	fd       int
	wmu      sync.Mutex // one write = one SDU
	mu       sync.Mutex
	udp      map[uint16]*net.UDPConn
	hostText map[string]string // "addr|port" -> the host text the node sent to, echoed back verbatim
	up, down int
}

func (s *session) run() (int, int, error) {
	defer s.close()
	buf := make([]byte, 65536)
	for {
		n, err := unix.Read(s.fd, buf) // SEQPACKET keeps SDU boundaries
		if err != nil {
			return s.up, s.down, err
		}
		if n == 0 {
			return s.up, s.down, errors.New("channel closed")
		}
		f, err := Decode(buf[:n])
		if err != nil {
			continue
		}
		ra, err := net.ResolveUDPAddr("udp", net.JoinHostPort(f.Host, fmt.Sprint(f.RemotePort)))
		if err != nil {
			log.Printf("host %q: %v", f.Host, err)
			continue
		}
		c, err := s.socketFor(f.LocalPort)
		if err != nil {
			return s.up, s.down, err
		}
		s.mu.Lock()
		s.hostText[key(ra)] = f.Host
		s.mu.Unlock()
		if _, err := c.WriteToUDP(f.Payload, ra); err != nil {
			log.Printf("udp %d -> %v: %v", f.LocalPort, ra, err)
			continue
		}
		s.up++
	}
}

func key(a *net.UDPAddr) string { return a.IP.String() + "|" + fmt.Sprint(a.Port) }

func (s *session) socketFor(lport uint16) (*net.UDPConn, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if c := s.udp[lport]; c != nil {
		return c, nil
	}
	c, err := net.ListenUDP("udp", nil)
	if err != nil {
		return nil, err
	}
	s.udp[lport] = c
	go s.receive(lport, c)
	return c, nil
}

func (s *session) receive(lport uint16, c *net.UDPConn) {
	buf := make([]byte, 2048)
	for {
		n, ra, err := c.ReadFromUDP(buf)
		if err != nil {
			return // closed with the session
		}
		s.mu.Lock()
		host, ok := s.hostText[key(ra)]
		s.mu.Unlock()
		if !ok {
			host = ra.IP.String()
		}
		frame, err := Frame{LocalPort: lport, RemotePort: uint16(ra.Port), Host: host, Payload: buf[:n]}.Encode()
		if err != nil {
			continue
		}
		s.wmu.Lock()
		_, err = unix.Write(s.fd, frame)
		s.wmu.Unlock()
		if err != nil {
			return // the reader sees the channel go down
		}
		s.mu.Lock()
		s.down++
		s.mu.Unlock()
	}
}

func (s *session) close() {
	s.mu.Lock()
	defer s.mu.Unlock()
	for _, c := range s.udp {
		c.Close()
	}
	unix.Close(s.fd)
}
