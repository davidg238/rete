module github.com/davidg238/porta

go 1.26.1

require (
	github.com/davidg238/rete/protocol v0.0.0
	github.com/mattn/go-sqlite3 v1.14.40
	github.com/spf13/cobra v1.10.2
)

require (
	github.com/inconshreveable/mousetrap v1.1.0 // indirect
	github.com/spf13/pflag v1.0.9 // indirect
)

replace github.com/davidg238/rete/protocol => ../protocol
