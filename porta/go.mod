module github.com/davidg238/porta

go 1.26.1

require (
	github.com/davidg238/rete/protocol v0.0.0
	github.com/mattn/go-sqlite3 v1.14.40
	github.com/plgd-dev/go-coap/v3 v3.8.1
	github.com/spf13/cobra v1.10.2
)

require (
	github.com/dsnet/golib/memfile v1.0.0 // indirect
	github.com/inconshreveable/mousetrap v1.1.0 // indirect
	github.com/pion/dtls/v3 v3.1.15 // indirect
	github.com/pion/logging v0.2.4 // indirect
	github.com/pion/transport/v5 v5.1.4 // indirect
	github.com/spf13/pflag v1.0.9 // indirect
	golang.org/x/crypto v0.58.0 // indirect
	golang.org/x/exp v0.0.0-20261014143641-f0251eb4aa3e // indirect
	golang.org/x/net v0.62.0 // indirect
	golang.org/x/sync v0.24.0 // indirect
	golang.org/x/sys v0.49.0 // indirect
)

replace github.com/davidg238/rete/protocol => ../protocol
